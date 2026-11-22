#include "MCPBridge.h"

#include <Windows.h>
#include <sddl.h>
#include <stdint.h>

#include <sstream>
#include <string>
#include <vector>

#include "D3D11Wrapper.h"
#include "Globals.h"
#include "HackerDevice.h"
#include "Hunting.h"
#include "log.h"
#include "version.h"

#pragma comment(lib, "advapi32.lib")

namespace {

constexpr uint32_t MCP_MAGIC = 0x3150434D; // "MCP1" in little-endian byte order.
constexpr uint16_t MCP_PROTOCOL_VERSION = 1;
constexpr uint32_t MCP_MAX_PAYLOAD = 1024 * 16;
constexpr DWORD MCP_RENDER_WAIT_MS = 1500;
constexpr DWORD MCP_CLIENT_IO_TIMEOUT_MS = 2000;

enum class MCPCommand : uint16_t {
	STATUS = 1,
	FRAME_ANALYSIS = 2,
	RELOAD_CONFIG = 3,
	RELOAD_SHADERS = 4,
	PING = 5,
};

struct MCPRequestHeader {
	uint32_t magic;
	uint16_t version;
	uint16_t command;
	uint32_t request_id;
	uint32_t payload_size;
};

struct MCPResponseHeader {
	uint32_t magic;
	uint32_t status;
	uint32_t request_id;
	uint32_t payload_size;
};

struct MCPRequest {
	MCPCommand command = MCPCommand::PING;
	uint32_t request_id = 0;
	std::string payload;
};

struct MCPResponse {
	uint32_t status = 0;
	uint32_t request_id = 0;
	std::string payload;
};

enum MCPStatus : uint32_t {
	MCP_STATUS_OK = 0,
	MCP_STATUS_BAD_REQUEST = 1,
	MCP_STATUS_UNSUPPORTED = 2,
	MCP_STATUS_BUSY = 3,
	MCP_STATUS_NOT_READY = 4,
	MCP_STATUS_FAILED = 5,
	MCP_STATUS_TIMEOUT = 6,
	MCP_STATUS_STOPPING = 7,
};

static_assert(sizeof(MCPRequestHeader) == 16, "MCP request header must remain 16 bytes");
static_assert(sizeof(MCPResponseHeader) == 16, "MCP response header must remain 16 bytes");

SRWLOCK g_request_lock = SRWLOCK_INIT;
SRWLOCK g_response_lock = SRWLOCK_INIT;
SRWLOCK g_lifecycle_lock = SRWLOCK_INIT;

MCPRequest g_request;
MCPResponse g_response;

volatile LONG g_pending_request = 0;
volatile LONG g_request_state = 0; // 0 = pending, 1 = abandoned, 2 = claimed.
volatile LONG g_started = 0;
volatile LONG g_reload_shaders_pending = 0;
HANDLE g_pipe_thread = NULL;
HANDLE g_stop_event = NULL;
HANDLE g_response_event = NULL;
HANDLE g_request_claimed_event = NULL;
HANDLE g_slot_free_event = NULL;

std::wstring g_pipe_name;
std::string g_instance_id;
std::string g_last_reload_shaders_result = "none";
std::string g_last_reload_shaders_error;
int g_capture_restore_hunting = -1;

std::wstring CurrentUserSidString()
{
	HANDLE token = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		return {};

	DWORD size = 0;
	GetTokenInformation(token, TokenUser, nullptr, 0, &size);
	if (!size) {
		CloseHandle(token);
		return {};
	}

	std::vector<uint8_t> buffer(size);
	if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
		CloseHandle(token);
		return {};
	}
	CloseHandle(token);

	TOKEN_USER *user = reinterpret_cast<TOKEN_USER *>(buffer.data());
	LPWSTR sid_string = nullptr;
	if (!ConvertSidToStringSidW(user->User.Sid, &sid_string))
		return {};

	std::wstring result = sid_string;
	LocalFree(sid_string);
	return result;
}

std::string WideToUtf8(const std::wstring &value)
{
	if (value.empty())
		return {};

	int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
			nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};

	std::string result(static_cast<size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
			&result[0], size, nullptr, nullptr);
	return result;
}

std::wstring ModulePathWide(HMODULE module)
{
	std::vector<wchar_t> buffer(MAX_PATH);
	while (true) {
		DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (length == 0)
			return {};
		if (length < buffer.size())
			return std::wstring(buffer.data(), length);
		buffer.resize(buffer.size() * 2);
	}
}

std::string ModulePath(HMODULE module)
{
	return WideToUtf8(ModulePathWide(module));
}

std::wstring ModuleDirectory(HMODULE module)
{
	std::wstring path = ModulePathWide(module);
	size_t separator = path.find_last_of(L"\\/");
	if (separator == std::wstring::npos)
		return {};
	return path.substr(0, separator);
}

void AppendField(std::ostringstream &stream, const char *key, const std::string &value)
{
	stream << key << '=' << value << '\n';
}

void AppendField(std::ostringstream &stream, const char *key, const std::wstring &value)
{
	AppendField(stream, key, WideToUtf8(value));
}

std::string BuildStatusPayload()
{
	std::ostringstream stream;
	std::wstring executable_path = ModulePathWide(nullptr);
	std::wstring dll_directory = ModuleDirectory(migoto_handle);
	std::wstring config_path = dll_directory.empty() ? L"" : dll_directory + L"\\d3dx.ini";
	std::wstring log_path = dll_directory.empty() ? L"" : dll_directory + L"\\d3d11_log.txt";

	CriticalSectionGuard(&G->mCriticalSection);

	AppendField(stream, "protocol", std::to_string(MCP_PROTOCOL_VERSION));
	AppendField(stream, "instance", g_instance_id);
	AppendField(stream, "pid", std::to_string(GetCurrentProcessId()));
	AppendField(stream, "dll_version", VER_FILE_VERSION_STR);
	AppendField(stream, "process_path", executable_path);
	AppendField(stream, "dll_path", ModulePath(migoto_handle));
	AppendField(stream, "config_path", config_path);
	AppendField(stream, "log_path", log_path);
	AppendField(stream, "analysis_root", dll_directory);
	AppendField(stream, "log_enabled", LogFile ? "1" : "0");
	AppendField(stream, "shader_path", std::wstring(G->SHADER_PATH));
	AppendField(stream, "analysis_path", std::wstring(G->ANALYSIS_PATH));
	AppendField(stream, "frame", std::to_string(G->frame_no));
	AppendField(stream, "fps", std::to_string(G->gFPSCounter.GetFPS()));
	AppendField(stream, "frame_time_ms", std::to_string(G->gFrameTime * 1000.0f));
	AppendField(stream, "uptime_s", std::to_string(G->gTime));
	AppendField(stream, "hunting", std::to_string(G->hunting));
	AppendField(stream, "config_initialized", G->gConfigInitialized ? "1" : "0");
	AppendField(stream, "config_reload_pending", G->gReloadConfigPending ? "1" : "0");
	AppendField(stream, "config_generation", std::to_string(G->config_generation));
	AppendField(stream, "reload_shaders_pending", g_reload_shaders_pending ? "1" : "0");
	AppendField(stream, "last_reload_shaders_result", g_last_reload_shaders_result);
	AppendField(stream, "last_reload_shaders_error", g_last_reload_shaders_error);
	AppendField(stream, "capture_restore_hunting", std::to_string(g_capture_restore_hunting));
	AppendField(stream, "analysis_active", G->analyse_frame ? "1" : "0");
	AppendField(stream, "analysis_number", std::to_string(G->analyse_frame_no));
	AppendField(stream, "screen_width", std::to_string(G->SCREEN_WIDTH));
	AppendField(stream, "screen_height", std::to_string(G->SCREEN_HEIGHT));
	AppendField(stream, "shader_overrides", std::to_string(G->mShaderOverrideMap.size()));
	AppendField(stream, "texture_overrides", std::to_string(G->mTextureOverrideMap.size()));
	AppendField(stream, "reloaded_shaders", std::to_string(G->mReloadedShaders.size()));
	AppendField(stream, "registered_shaders", std::to_string(G->mShaders.size()));

	std::ostringstream selected;
	selected << std::hex << G->mSelectedVertexShader << ',' << G->mSelectedPixelShader;
	AppendField(stream, "selected_shaders", selected.str());

	return stream.str();
}

MCPResponse MakeErrorResponse(uint32_t request_id, uint32_t status,
		const char *code, const std::string &message)
{
	MCPResponse response;
	response.status = status;
	response.request_id = request_id;
	std::ostringstream payload;
	AppendField(payload, "error", std::string(code));
	AppendField(payload, "error_message", message);
	response.payload = payload.str();
	return response;
}

MCPResponse ExecuteRequest(const MCPRequest &request, HackerDevice *device)
{
	MCPResponse response;
	response.request_id = request.request_id;
	response.status = MCP_STATUS_OK;

	std::ostringstream payload;
	switch (request.command) {
	case MCPCommand::PING:
		AppendField(payload, "pong", "1");
		response.payload = payload.str();
		return response;

	case MCPCommand::STATUS:
		response.payload = BuildStatusPayload();
		return response;

	case MCPCommand::FRAME_ANALYSIS: {
		int previous_hunting = static_cast<int>(G->hunting);
		bool changed_hunting = G->hunting != HUNTING_MODE_ENABLED;
		if (changed_hunting)
			G->hunting = HUNTING_MODE_ENABLED;

		std::string error;
		if (!MCPTriggerFrameAnalysis(device, &error)) {
			if (changed_hunting)
				G->hunting = previous_hunting;
			uint32_t status = MCP_STATUS_FAILED;
			if (error == "busy")
				status = MCP_STATUS_BUSY;
			else if (error == "hunting_disabled" ||
					error == "frame_analysis_context_missing")
				status = MCP_STATUS_NOT_READY;
			return MakeErrorResponse(request.request_id, status, error.c_str(), error);
		}
		g_capture_restore_hunting = changed_hunting ? previous_hunting : -1;
		AppendField(payload, "command", "frame_analysis");
		AppendField(payload, "result", "accepted");
		AppendField(payload, "analysis_path", std::wstring(G->ANALYSIS_PATH));
		AppendField(payload, "hunting_restore_mode",
				std::to_string(g_capture_restore_hunting));
		response.payload = payload.str();
		return response;
	}

	case MCPCommand::RELOAD_CONFIG:
		if (G->gReloadConfigPending)
			return MakeErrorResponse(request.request_id, MCP_STATUS_BUSY,
					"busy", "configuration reload is already pending");
		G->gReloadConfigPending = true;
		AppendField(payload, "command", "reload_config");
		AppendField(payload, "result", "queued");
		AppendField(payload, "config_generation", std::to_string(G->config_generation));
		response.payload = payload.str();
		return response;

	case MCPCommand::RELOAD_SHADERS: {
		if (InterlockedCompareExchange(&g_reload_shaders_pending, 1, 0) != 0)
			return MakeErrorResponse(request.request_id, MCP_STATUS_BUSY,
					"busy", "shader reload is already pending");
		AppendField(payload, "command", "reload_shaders");
		AppendField(payload, "result", "queued");
		response.payload = payload.str();
		return response;
	}

	default:
		return MakeErrorResponse(request.request_id, MCP_STATUS_UNSUPPORTED,
				"unsupported_command", "unsupported MCP command");
	}
}

enum class PipeIoResult {
	COMPLETED,
	STOPPED,
	TIMED_OUT,
	FAILED,
};

void CancelAndWaitForPipeIo(HANDLE pipe, OVERLAPPED &overlapped)
{
	if (!CancelIoEx(pipe, &overlapped) && GetLastError() != ERROR_NOT_FOUND) {
		LogWarning("MCP bridge failed to cancel pipe I/O: %lu\n", GetLastError());
		DisconnectNamedPipe(pipe);
	}

	WaitForSingleObject(overlapped.hEvent, INFINITE);
	DWORD ignored = 0;
	GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
}

PipeIoResult WaitForPipeIo(HANDLE pipe, OVERLAPPED &overlapped,
		DWORD timeout_ms, DWORD &bytes_transferred)
{
	HANDLE waits[] = {g_stop_event, overlapped.hEvent};
	DWORD wait = WaitForMultipleObjects(2, waits, FALSE, timeout_ms);
	if (wait == WAIT_OBJECT_0) {
		CancelAndWaitForPipeIo(pipe, overlapped);
		return PipeIoResult::STOPPED;
	}
	if (wait == WAIT_OBJECT_0 + 1) {
		return GetOverlappedResult(pipe, &overlapped, &bytes_transferred, FALSE) ?
			PipeIoResult::COMPLETED : PipeIoResult::FAILED;
	}
	if (wait == WAIT_TIMEOUT) {
		CancelAndWaitForPipeIo(pipe, overlapped);
		return PipeIoResult::TIMED_OUT;
	}
	return PipeIoResult::FAILED;
}

bool ReadPipeExact(HANDLE pipe, HANDLE io_event, uint8_t *buffer, DWORD size,
		DWORD &error_code)
{
	DWORD offset = 0;
	while (offset < size) {
		ResetEvent(io_event);
		OVERLAPPED overlapped = {};
		overlapped.hEvent = io_event;

		DWORD bytes_read = 0;
		BOOL result = ReadFile(pipe, buffer + offset, size - offset, &bytes_read, &overlapped);
		if (!result) {
			error_code = GetLastError();
			if (error_code != ERROR_IO_PENDING)
				return false;
			if (WaitForPipeIo(pipe, overlapped, MCP_CLIENT_IO_TIMEOUT_MS, bytes_read) !=
					PipeIoResult::COMPLETED) {
				error_code = GetLastError();
				return false;
			}
		}
		if (!bytes_read) {
			error_code = ERROR_BROKEN_PIPE;
			return false;
		}
		offset += bytes_read;
	}
	error_code = ERROR_SUCCESS;
	return true;
}

bool WritePipeExact(HANDLE pipe, HANDLE io_event, const uint8_t *buffer, DWORD size,
		DWORD &error_code)
{
	DWORD offset = 0;
	while (offset < size) {
		ResetEvent(io_event);
		OVERLAPPED overlapped = {};
		overlapped.hEvent = io_event;

		DWORD bytes_written = 0;
		BOOL result = WriteFile(pipe, buffer + offset, size - offset, &bytes_written, &overlapped);
		if (!result) {
			error_code = GetLastError();
			if (error_code != ERROR_IO_PENDING)
				return false;
			if (WaitForPipeIo(pipe, overlapped, MCP_CLIENT_IO_TIMEOUT_MS, bytes_written) !=
					PipeIoResult::COMPLETED) {
				error_code = GetLastError();
				return false;
			}
		}
		if (!bytes_written) {
			error_code = ERROR_BROKEN_PIPE;
			return false;
		}
		offset += bytes_written;
	}
	error_code = ERROR_SUCCESS;
	return true;
}

bool ReadPipeMessage(HANDLE pipe, HANDLE io_event, std::vector<uint8_t> &message)
{
	MCPRequestHeader header;
	DWORD error_code = ERROR_SUCCESS;
	if (!ReadPipeExact(pipe, io_event, reinterpret_cast<uint8_t *>(&header),
			sizeof(header), error_code)) {
		LogWarning("MCP bridge failed to read request header: %lu\n", error_code);
		return false;
	}
	if (header.payload_size > MCP_MAX_PAYLOAD) {
		LogWarning("MCP bridge rejected oversized request payload: %lu\n", header.payload_size);
		return false;
	}

	message.resize(sizeof(header) + header.payload_size);
	memcpy(message.data(), &header, sizeof(header));
	if (header.payload_size &&
			!ReadPipeExact(pipe, io_event, message.data() + sizeof(header),
				header.payload_size, error_code)) {
		LogWarning("MCP bridge failed to read request payload: %lu\n", error_code);
		return false;
	}
	return true;
}

bool WritePipeMessage(HANDLE pipe, HANDLE io_event, const MCPResponse &response)
{
	if (response.payload.size() > MCP_MAX_PAYLOAD)
		return false;

	MCPResponseHeader header;
	header.magic = MCP_MAGIC;
	header.status = response.status;
	header.request_id = response.request_id;
	header.payload_size = static_cast<uint32_t>(response.payload.size());

	std::vector<uint8_t> message(sizeof(header) + response.payload.size());
	memcpy(message.data(), &header, sizeof(header));
	if (!response.payload.empty())
		memcpy(message.data() + sizeof(header), response.payload.data(), response.payload.size());

	DWORD error_code = ERROR_SUCCESS;
	if (!WritePipeExact(pipe, io_event, message.data(),
			static_cast<DWORD>(message.size()), error_code)) {
		LogWarning("MCP bridge failed to write response: %lu\n", error_code);
		return false;
	}
	return true;
}

bool ParseRequest(const std::vector<uint8_t> &message, MCPRequest &request)
{
	if (message.size() < sizeof(MCPRequestHeader))
		return false;

	MCPRequestHeader header;
	memcpy(&header, message.data(), sizeof(header));
	if (header.magic != MCP_MAGIC || header.version != MCP_PROTOCOL_VERSION)
		return false;
	if (header.payload_size != message.size() - sizeof(header))
		return false;
	if (header.payload_size > MCP_MAX_PAYLOAD)
		return false;

	request.command = static_cast<MCPCommand>(header.command);
	request.request_id = header.request_id;
	request.payload.assign(reinterpret_cast<const char *>(message.data() + sizeof(header)),
			header.payload_size);
	return true;
}

MCPResponse QueueRequestAndWait(const MCPRequest &request, bool &abandoned)
{
	abandoned = false;
	ResetEvent(g_response_event);
	ResetEvent(g_request_claimed_event);
	ResetEvent(g_slot_free_event);

	AcquireSRWLockExclusive(&g_request_lock);
	g_request = request;
	InterlockedExchange(&g_request_state, 0);
	ReleaseSRWLockExclusive(&g_request_lock);

	InterlockedExchange(&g_pending_request, 1);

	HANDLE waits[] = {g_response_event, g_request_claimed_event, g_stop_event};
	DWORD wait = WaitForMultipleObjects(3, waits, FALSE, MCP_RENDER_WAIT_MS);
	if (wait == WAIT_OBJECT_0) {
		AcquireSRWLockExclusive(&g_response_lock);
		MCPResponse response = g_response;
		ReleaseSRWLockExclusive(&g_response_lock);
		return response;
	}

	if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_TIMEOUT) {
		if (wait == WAIT_TIMEOUT) {
			LONG previous = InterlockedCompareExchange(&g_request_state, 1, 0);
			if (previous == 2)
				wait = WAIT_OBJECT_0 + 1;
		}
		if (wait == WAIT_OBJECT_0 + 1) {
			HANDLE execute_waits[] = {g_response_event, g_stop_event};
			DWORD execute_wait = WaitForMultipleObjects(2, execute_waits, FALSE, INFINITE);
			if (execute_wait == WAIT_OBJECT_0) {
				AcquireSRWLockExclusive(&g_response_lock);
				MCPResponse response = g_response;
				ReleaseSRWLockExclusive(&g_response_lock);
				return response;
			}
			abandoned = true;
			return MakeErrorResponse(request.request_id, MCP_STATUS_STOPPING,
					"stopping", "MCP bridge is stopping");
		}
	}

	abandoned = true;
	InterlockedCompareExchange(&g_request_state, 1, 0);
	if (wait == WAIT_OBJECT_0 + 2)
		return MakeErrorResponse(request.request_id, MCP_STATUS_STOPPING,
				"stopping", "MCP bridge is stopping");
	return MakeErrorResponse(request.request_id, MCP_STATUS_TIMEOUT,
			"render_timeout", "the game did not reach a frame boundary before the timeout");
}

bool HandlePipeClient(HANDLE pipe, HANDLE io_event)
{
	std::vector<uint8_t> message;
	if (!ReadPipeMessage(pipe, io_event, message))
		return false;

	MCPRequest request;
	MCPResponse response;
	bool abandoned = false;
	if (!ParseRequest(message, request)) {
		LogWarning("MCP bridge rejected malformed request\n");
		response = MakeErrorResponse(0, MCP_STATUS_BAD_REQUEST,
				"bad_request", "invalid MCP request header or payload size");
	} else {
		response = QueueRequestAndWait(request, abandoned);
	}

	bool write_ok = WritePipeMessage(pipe, io_event, response);
	if (write_ok && !FlushFileBuffers(pipe)) {
		LogWarning("MCP bridge failed to flush response: %lu\n", GetLastError());
		write_ok = false;
	}
	HANDLE waits[] = {g_stop_event, g_slot_free_event};
	WaitForMultipleObjects(2, waits, FALSE, INFINITE);
	return write_ok;
}

DWORD WINAPI PipeThreadProc(void *)
{
	HANDLE io_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!io_event)
		return 0;

	PSECURITY_DESCRIPTOR security_descriptor = nullptr;
	SECURITY_ATTRIBUTES security_attributes = {};
	std::wstring user_sid = CurrentUserSidString();
	wchar_t sddl[256];
	if (user_sid.empty() ||
			swprintf_s(sddl, L"D:P(A;;GA;;;%ls)(A;;GA;;;SY)S:(ML;;NW;;;ME)",
				user_sid.c_str()) < 0) {
		LogWarning("MCP bridge could not build its pipe security descriptor\n");
		CloseHandle(io_event);
		return 0;
	}
	// Current user and LocalSystem only; the Medium label allows non-elevated Codex
	// to connect to a pipe created by the elevated game process.
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl, SDDL_REVISION_1, &security_descriptor, nullptr)) {
		LogWarning("MCP bridge failed to create pipe security descriptor: %lu\n", GetLastError());
		CloseHandle(io_event);
		return 0;
	}
	security_attributes.nLength = sizeof(security_attributes);
	security_attributes.lpSecurityDescriptor = security_descriptor;

	while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
		HANDLE pipe = CreateNamedPipeW(
				g_pipe_name.c_str(),
				PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
				PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
					PIPE_REJECT_REMOTE_CLIENTS,
				1,
				MCP_MAX_PAYLOAD + sizeof(MCPResponseHeader),
				MCP_MAX_PAYLOAD + sizeof(MCPRequestHeader),
				0,
				&security_attributes);
		if (pipe == INVALID_HANDLE_VALUE) {
			LogWarning("MCP bridge failed to create named pipe: %lu\n", GetLastError());
			Sleep(1000);
			continue;
		}

		ResetEvent(io_event);
		OVERLAPPED connect_overlapped = {};
		connect_overlapped.hEvent = io_event;
		BOOL connected = ConnectNamedPipe(pipe, &connect_overlapped);
		DWORD connect_error = connected ? ERROR_SUCCESS : GetLastError();
		if (!connected && connect_error == ERROR_IO_PENDING) {
			DWORD ignored = 0;
			connected = WaitForPipeIo(pipe, connect_overlapped, INFINITE, ignored) ==
				PipeIoResult::COMPLETED;
			connect_error = connected ? ERROR_SUCCESS : GetLastError();
		} else if (!connected && connect_error == ERROR_PIPE_CONNECTED) {
			connected = TRUE;
		}
		if (!connected) {
			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
			continue;
		}

		if (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT)
			HandlePipeClient(pipe, io_event);

		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
	}

	CloseHandle(io_event);
	if (security_descriptor)
		LocalFree(security_descriptor);
	return 0;
}

} // namespace

void MCP_Start(bool enabled)
{
	if (!enabled) {
		LogInfo("MCP bridge disabled by d3dx.ini\n");
		return;
	}

	AcquireSRWLockExclusive(&g_lifecycle_lock);
	if (g_started) {
		ReleaseSRWLockExclusive(&g_lifecycle_lock);
		return;
	}
	g_started = 1;
	InterlockedExchange(&g_pending_request, 0);
	InterlockedExchange(&g_request_state, 0);
	InterlockedExchange(&g_reload_shaders_pending, 0);

	FILETIME creation_time = {};
	FILETIME exit_time = {};
	FILETIME kernel_time = {};
	FILETIME user_time = {};
	GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time);

	DWORD pid = GetCurrentProcessId();
	uint32_t start_low = creation_time.dwLowDateTime;
	char instance[32];
	sprintf_s(instance, "%lu-%08X", pid, start_low);
	g_instance_id = instance;

	wchar_t pipe_name[128];
	swprintf_s(pipe_name, L"\\\\.\\pipe\\xxmi-mcp-v1-%hs", instance);
	g_pipe_name = pipe_name;

	g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_response_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_request_claimed_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_slot_free_event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
	if (!g_stop_event || !g_response_event || !g_request_claimed_event || !g_slot_free_event) {
		LogWarning("MCP bridge failed to create synchronization objects: %lu\n", GetLastError());
		g_started = 0;
		if (g_stop_event) {
			CloseHandle(g_stop_event);
			g_stop_event = NULL;
		}
		if (g_response_event) {
			CloseHandle(g_response_event);
			g_response_event = NULL;
		}
		if (g_request_claimed_event) {
			CloseHandle(g_request_claimed_event);
			g_request_claimed_event = NULL;
		}
		if (g_slot_free_event) {
			CloseHandle(g_slot_free_event);
			g_slot_free_event = NULL;
		}
		ReleaseSRWLockExclusive(&g_lifecycle_lock);
		return;
	}

	g_pipe_thread = CreateThread(nullptr, 0, PipeThreadProc, nullptr, 0, nullptr);
	if (!g_pipe_thread) {
		LogWarning("MCP bridge failed to create pipe thread: %lu\n", GetLastError());
		g_started = 0;
		CloseHandle(g_stop_event);
		g_stop_event = NULL;
		CloseHandle(g_response_event);
		g_response_event = NULL;
		CloseHandle(g_request_claimed_event);
		g_request_claimed_event = NULL;
		CloseHandle(g_slot_free_event);
		g_slot_free_event = NULL;
		ReleaseSRWLockExclusive(&g_lifecycle_lock);
		return;
	}

	LogInfoW(L"MCP bridge listening on %ls\n", g_pipe_name.c_str());
	ReleaseSRWLockExclusive(&g_lifecycle_lock);
}

void MCP_Stop()
{
	if (g_stop_event)
		SetEvent(g_stop_event);

	if (g_pipe_thread) {
		WaitForSingleObject(g_pipe_thread, INFINITE);
	}

	AcquireSRWLockExclusive(&g_lifecycle_lock);
	if (!g_started) {
		ReleaseSRWLockExclusive(&g_lifecycle_lock);
		return;
	}
	g_started = 0;

	if (g_pipe_thread) {
		CloseHandle(g_pipe_thread);
		g_pipe_thread = NULL;
	}
	if (g_slot_free_event) {
		CloseHandle(g_slot_free_event);
		g_slot_free_event = NULL;
	}
	if (g_response_event) {
		CloseHandle(g_response_event);
		g_response_event = NULL;
	}
	if (g_request_claimed_event) {
		CloseHandle(g_request_claimed_event);
		g_request_claimed_event = NULL;
	}
	if (g_stop_event) {
		CloseHandle(g_stop_event);
		g_stop_event = NULL;
	}
	ReleaseSRWLockExclusive(&g_lifecycle_lock);
}

void MCP_RunFrameActions(HackerDevice *device)
{
	if (!InterlockedExchange(&g_pending_request, 0))
		return;

	AcquireSRWLockShared(&g_lifecycle_lock);
	if (!g_started || !g_response_event) {
		ReleaseSRWLockShared(&g_lifecycle_lock);
		return;
	}

	MCPRequest request;
	AcquireSRWLockExclusive(&g_request_lock);
	request = g_request;
	ReleaseSRWLockExclusive(&g_request_lock);

	bool abandoned = InterlockedCompareExchange(&g_request_state, 2, 0) != 0;
	if (!abandoned)
		SetEvent(g_request_claimed_event);
	if (!abandoned) {
		MCPResponse response = ExecuteRequest(request, device);
		AcquireSRWLockExclusive(&g_response_lock);
		g_response = response;
		ReleaseSRWLockExclusive(&g_response_lock);
		SetEvent(g_response_event);
	}
	InterlockedExchange(&g_request_state, 0);
	SetEvent(g_slot_free_event);
	ReleaseSRWLockShared(&g_lifecycle_lock);
}

void MCP_RunDeferredActions(HackerDevice *device)
{
	if (InterlockedExchange(&g_reload_shaders_pending, 0) == 0)
		return;

	AcquireSRWLockShared(&g_lifecycle_lock);
	if (!g_started) {
		ReleaseSRWLockShared(&g_lifecycle_lock);
		return;
	}

	std::string error;
	if (MCPReloadShaderFixes(device, &error)) {
		g_last_reload_shaders_result = "ok";
		g_last_reload_shaders_error.clear();
	} else {
		g_last_reload_shaders_result = "failed";
		g_last_reload_shaders_error = error;
	}
	ReleaseSRWLockShared(&g_lifecycle_lock);
}

void MCP_AfterFrameAnalysis()
{
	if (g_capture_restore_hunting < 0)
		return;

	AcquireSRWLockShared(&g_lifecycle_lock);
	if (g_started) {
		G->hunting = static_cast<UINT>(g_capture_restore_hunting);
		LogInfo("> MCP restored hunting mode to %d\n", G->hunting);
	}
	g_capture_restore_hunting = -1;
	ReleaseSRWLockShared(&g_lifecycle_lock);
}
