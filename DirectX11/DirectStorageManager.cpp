/**
 * DirectStorageManager.cpp
 *
 * Implements Microsoft DirectStorage 1.3 runtime integration for XXMI / 3DMigoto.
 *
 * Architecture & Data Flow:
 * - Custom textures in .gdds format (GDeflate-compressed DDS mip slices) are parsed
 *   to extract their DXGI format, dimensions, and chunk file offsets.
 * - A shared texture resource is allocated on the game's D3D11 device with
 *   D3D11_RESOURCE_MISC_SHARED_NTHANDLE.
 * - The shared NT handle is opened in D3D12 via ID3D12Device::OpenSharedHandle.
 * - DirectStorage enqueues GPU decompression requests (DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION)
 *   directly into the D3D12 texture resource with zero intermediate CPU staging copies.
 * - If DirectStorage GPU decompression fails, falls back automatically to CPU GDeflate codec decompression.
 *
 * Thread Safety & Concurrency:
 * - Singleton lifecycle is managed through DirectStorageManager::EnsureInitialized.
 * - Queue submission and fence synchronization are serialized per request.
 */

#include "DirectStorageManager.h"
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <vector>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "../log.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

using Microsoft::WRL::ComPtr;

namespace {

// DDS container constants
constexpr UINT DDS_MAGIC = 0x20534444; // "DDS "
constexpr UINT DDS_FOURCC = 0x00000004;

#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
	UINT size;
	UINT flags;
	UINT fourCC;
	UINT rgbBitCount;
	UINT rBitMask;
	UINT gBitMask;
	UINT bBitMask;
	UINT aBitMask;
};

struct DDS_HEADER {
	UINT size;
	UINT flags;
	UINT height;
	UINT width;
	UINT pitchOrLinearSize;
	UINT depth;
	UINT mipMapCount;
	UINT reserved1[11];
	DDS_PIXELFORMAT ddspf;
	UINT caps;
	UINT caps2;
	UINT caps3;
	UINT caps4;
	UINT reserved2;
};

struct DDS_HEADER_DXT10 {
	DXGI_FORMAT dxgiFormat;
	UINT resourceDimension;
	UINT miscFlag;
	UINT arraySize;
	UINT miscFlags2;
};
#pragma pack(pop)

constexpr UINT DDS_DIMENSION_TEXTURE1D = 2;
constexpr UINT DDS_DIMENSION_TEXTURE2D = 3;
constexpr UINT DDS_DIMENSION_TEXTURE3D = 4;

} // anonymous namespace

// Standalone manager state kept isolated from struct Globals to preserve struct layout
static bool g_dsEnabled = false;
static DirectStorageManager* g_dsInstance = nullptr;

// Dedicated diagnostic logging state
static std::wstring g_dsLogPath;
static FILE *g_dsLogFile = nullptr;
static UINT g_dsGpuCount = 0;
static UINT g_dsCpuCount = 0;
static double g_dsTotalGpuMs = 0.0;
static double g_dsTotalCpuMs = 0.0;

static void LogDirectStorage(const wchar_t *format, ...)
{
	if (!g_dsLogFile)
		return;

	wchar_t buffer[1024];
	va_list args;
	va_start(args, format);
	vswprintf_s(buffer, format, args);
	va_end(args);

	fputws(buffer, g_dsLogFile);
	fflush(g_dsLogFile);
}

bool DirectStorageManager::IsEnabled()
{
	return g_dsEnabled;
}

void DirectStorageManager::SetEnabled(bool enabled)
{
	g_dsEnabled = enabled;
}

DirectStorageManager* DirectStorageManager::GetInstance()
{
	return g_dsInstance;
}

void DirectStorageManager::SetInstance(DirectStorageManager* instance)
{
	g_dsInstance = instance;
}

bool DirectStorageManager::EnsureInitialized(ID3D11Device *dev)
{
	if (!g_dsEnabled)
		return false;

	if (!g_dsInstance) {
		g_dsInstance = new DirectStorageManager();
		if (!g_dsInstance->Initialize(dev)) {
			delete g_dsInstance;
			g_dsInstance = nullptr;
			return false;
		}
	} else if (!g_dsInstance->IsReady()) {
		if (!g_dsInstance->Initialize(dev))
			return false;
	}

	return g_dsInstance && g_dsInstance->IsReady();
}

DirectStorageManager::DirectStorageManager()
{
}

DirectStorageManager::~DirectStorageManager()
{
	if (m_fenceEvent)
		CloseHandle(m_fenceEvent);

	if (g_dsLogFile) {
		fclose(g_dsLogFile);
		g_dsLogFile = nullptr;
	}
}

// Helper struct for D3D11On12 worker thread initialization
struct On12ThreadParams {
	ID3D12Device* d3d12;
	ID3D12CommandQueue* queue;
	ID3D11Device** outDev;
	ID3D11DeviceContext** outCtx;
	HRESULT hr;
};

static DWORD WINAPI On12CreateThread(LPVOID p)
{
	auto* params = (On12ThreadParams*)p;
	params->hr = D3D11On12CreateDevice(
		params->d3d12,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		nullptr,
		0,
		(IUnknown* const*)&params->queue,
		1,
		0,
		params->outDev,
		params->outCtx,
		nullptr);
	return 0;
}

bool DirectStorageManager::Initialize(ID3D11Device *gameDevice)
{
	if (m_initialized)
		return m_ready;

	m_gameDevice = gameDevice;
	m_initialized = true;

	// Resolve module directory path and initialize diagnostic log file
	HMODULE selfModule = nullptr;
	GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCWSTR)(void*)&DirectStorageManager::IsEnabled,
		&selfModule);

	wchar_t selfPath[MAX_PATH] = L"";
	if (selfModule)
		GetModuleFileNameW(selfModule, selfPath, MAX_PATH);

	wchar_t* lastSlash = wcsrchr(selfPath, L'\\');
	if (lastSlash) {
		*(lastSlash + 1) = 0;
		g_dsLogPath = std::wstring(selfPath) + L"directstorage.log";
	}

	if (!g_dsLogPath.empty()) {
		if (g_dsLogFile) {
			fclose(g_dsLogFile);
			g_dsLogFile = nullptr;
		}
		if (_wfopen_s(&g_dsLogFile, g_dsLogPath.c_str(), L"w, ccs=UTF-8") == 0 && g_dsLogFile) {
			fputws(L"======================================================================\n", g_dsLogFile);
			fputws(L" XXMI DirectStorage 1.3 Diagnostics Log\n", g_dsLogFile);
			fputws(L"======================================================================\n", g_dsLogFile);
			fflush(g_dsLogFile);
		}
	}

	LogInfo("DirectStorage: Initializing DirectStorageManager\n");

	// Match active DXGI adapter LUID from the game's D3D11 device
	ComPtr<IDXGIDevice> dxgiDev;
	ComPtr<IDXGIAdapter> gameAdapter;
	DXGI_ADAPTER_DESC gameDesc{};
	bool haveLuid = false;

	if (SUCCEEDED(gameDevice->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
		if (SUCCEEDED(dxgiDev->GetAdapter(&gameAdapter))) {
			if (SUCCEEDED(gameAdapter->GetDesc(&gameDesc)))
				haveLuid = true;
		}
	}

	// Create matching D3D12 device on the same adapter
	HRESULT hr = E_FAIL;
	if (haveLuid) {
		ComPtr<IDXGIFactory1> factory;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
			for (UINT i = 0;; ++i) {
				ComPtr<IDXGIAdapter1> adapter;
				if (factory->EnumAdapters1(i, &adapter) != S_OK)
					break;

				DXGI_ADAPTER_DESC1 desc1{};
				adapter->GetDesc1(&desc1);
				if (desc1.AdapterLuid.HighPart == gameDesc.AdapterLuid.HighPart &&
					desc1.AdapterLuid.LowPart == gameDesc.AdapterLuid.LowPart) {
					hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device));
					break;
				}
			}
		}
	}

	if (!m_d3d12Device)
		hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device));

	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to create D3D12 device (0x%08x)\n", hr);
		LogDirectStorage(L"DirectStorage: Failed to create D3D12 device (0x%08X)\n", hr);
		return false;
	}

	LogDirectStorage(L"Adapter: %ls (LUID: %08x-%08x)\n", gameDesc.Description, gameDesc.AdapterLuid.HighPart, gameDesc.AdapterLuid.LowPart);

	// Command queue, allocator, list, and completion fence
	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = m_d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_cmdQueue));
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to create D3D12 command queue (0x%08x)\n", hr);
		return false;
	}

	hr = m_d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAllocator));
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to create D3D12 command allocator (0x%08x)\n", hr);
		return false;
	}

	hr = m_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList));
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to create D3D12 command list (0x%08x)\n", hr);
		return false;
	}
	m_cmdList->Close();

	hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to create D3D12 fence (0x%08x)\n", hr);
		return false;
	}

	m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!m_fenceEvent) {
		LogWarning("DirectStorage: Failed to create fence event\n");
		return false;
	}

	// D3D11On12 interop device (isolated thread avoids loader hook reentrancy)
	{
		ID3D11Device* tmpDev = nullptr;
		ID3D11DeviceContext* tmpCtx = nullptr;
		On12ThreadParams params{ m_d3d12Device.Get(), m_cmdQueue.Get(), &tmpDev, &tmpCtx, E_FAIL };

		HANDLE thread = CreateThread(nullptr, 0, On12CreateThread, &params, 0, nullptr);
		if (!thread) {
			LogWarning("DirectStorage: Failed to create D3D11On12 thread (0x%08x)\n", HRESULT_FROM_WIN32(GetLastError()));
			return false;
		}
		WaitForSingleObject(thread, INFINITE);
		CloseHandle(thread);

		hr = params.hr;
		if (FAILED(hr)) {
			LogWarning("DirectStorage: Failed to create D3D11On12 device (0x%08x)\n", hr);
			return false;
		}
		m_d3d11On12Device.Attach(tmpDev);
		m_d3d11On12Context.Attach(tmpCtx);
	}

	hr = m_d3d11On12Device.As(&m_d3d11On12);
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to query D3D11On12 interface (0x%08x)\n", hr);
		return false;
	}

	// Resolve and load DirectStorage runtime DLLs relative to module location
	if (selfPath[0]) {
		wchar_t dsPath[MAX_PATH] = L"";
		wchar_t dsCorePath[MAX_PATH] = L"";
		wcscpy_s(dsPath, selfPath);
		wcscat_s(dsPath, L"dstorage.dll");
		wcscpy_s(dsCorePath, selfPath);
		wcscat_s(dsCorePath, L"dstoragecore.dll");

		LoadLibraryW(dsCorePath);
		if (!GetModuleHandleW(L"dstorage.dll"))
			LoadLibraryW(dsPath);
	}

	if (!GetModuleHandleW(L"dstorage.dll")) {
		wchar_t modPath[MAX_PATH];
		GetModuleFileNameW((HMODULE)&__ImageBase, modPath, MAX_PATH);
		wchar_t* slash = wcsrchr(modPath, L'\\');
		if (slash)
			*(slash + 1) = 0;
		std::wstring base(modPath);
		LoadLibraryExW((base + L"dstoragecore.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		HMODULE ds = LoadLibraryExW((base + L"dstorage.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!ds) {
			LogWarning("DirectStorage: Failed to load dstorage.dll\n");
			LogDirectStorage(L"DirectStorage: Failed to load dstorage.dll\n");
			return false;
		}
	}

	// Configure and initialize DirectStorage factory and request queue
	DSTORAGE_CONFIGURATION config{};
	config.NumSubmitThreads = 1;
	config.DisableTelemetry = TRUE;
	hr = DStorageSetConfiguration(&config);
	if (FAILED(hr))
		LogDebug("DirectStorage: DStorageSetConfiguration status 0x%08x\n", hr);

	hr = DStorageGetFactory(IID_PPV_ARGS(&m_factory));
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to get DirectStorage factory (0x%08x)\n", hr);
		LogDirectStorage(L"DirectStorage: Failed to get DirectStorage factory (0x%08X)\n", hr);
		return false;
	}

	hr = m_factory->SetStagingBufferSize(256u << 20); // 256 MB staging buffer for 4K/8K/16K streaming
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to set staging buffer size (0x%08x)\n", hr);
		LogDirectStorage(L"DirectStorage: Failed to set staging buffer size (0x%08X)\n", hr);
		return false;
	}

	DSTORAGE_QUEUE_DESC queueDescDS{};
	queueDescDS.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	queueDescDS.Capacity = 8192;
	queueDescDS.Priority = DSTORAGE_PRIORITY_NORMAL;
	queueDescDS.Device = m_d3d12Device.Get();

	hr = m_factory->CreateQueue(&queueDescDS, IID_PPV_ARGS(&m_queue));
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to create DirectStorage queue (0x%08x)\n", hr);
		LogDirectStorage(L"DirectStorage: Failed to create DirectStorage queue (0x%08X)\n", hr);
		return false;
	}

	// Query runtime compression support to verify GPU hardware metacommand status
	ComPtr<IDStorageQueue2> queue2;
	if (SUCCEEDED(m_queue.As(&queue2))) {
		DSTORAGE_COMPRESSION_SUPPORT support = queue2->GetCompressionSupport(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE);
		LogDirectStorage(L"GDeflate Compression Support: 0x%02X\n", (UINT)support);
		if (support & DSTORAGE_COMPRESSION_SUPPORT_GPU_OPTIMIZED)
			LogDirectStorage(L"  - GPU Optimized: Active (Driver metacommand enabled)\n");
		if (support & DSTORAGE_COMPRESSION_SUPPORT_GPU_FALLBACK)
			LogDirectStorage(L"  - GPU Fallback: Active (DirectCompute shader fallback enabled)\n");
		if (support & DSTORAGE_COMPRESSION_SUPPORT_CPU_FALLBACK)
			LogDirectStorage(L"  - CPU Fallback: Active (CPU decompression fallback enabled)\n");
		if (support & DSTORAGE_COMPRESSION_SUPPORT_USES_COMPUTE_QUEUE)
			LogDirectStorage(L"  - Execution Queue: Compute Queue\n");
		if (support & DSTORAGE_COMPRESSION_SUPPORT_USES_COPY_QUEUE)
			LogDirectStorage(L"  - Execution Queue: Copy Queue\n");
	}
	LogDirectStorage(L"Staging Buffer: 256 MB\n");
	LogDirectStorage(L"Status: DirectStorage runtime initialized successfully\n\n");

	// Create CPU decompression codec for graceful fallback
	DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, 0, IID_PPV_ARGS(&m_gdeflateCodec));

	// Pre-allocate persistent 64 MB upload staging buffer to eliminate allocation churn
	EnsureUploadBufferSize(64ULL << 20);

	LogInfo("DirectStorage: Initialized successfully\n");
	m_ready = true;
	return true;
}

HRESULT DirectStorageManager::WaitForFence(UINT64 value)
{
	if (m_fence->GetCompletedValue() >= value)
		return S_OK;

	// Hybrid spin-wait: poll fence for a short burst (~50-100 microseconds)
	// DirectStorage GPU decompression and transfers often finish within microseconds.
	for (int spin = 0; spin < 2000; ++spin) {
		YieldProcessor();
		if (m_fence->GetCompletedValue() >= value)
			return S_OK;
	}

	HRESULT hr = m_fence->SetEventOnCompletion(value, m_fenceEvent);
	if (FAILED(hr))
		return hr;

	WaitForSingleObject(m_fenceEvent, INFINITE);
	return S_OK;
}

HRESULT DirectStorageManager::EnsureUploadBufferSize(UINT64 requiredSize)
{
	if (m_uploadBuffer && m_uploadBufferSize >= requiredSize)
		return S_OK;

	// Round up to multiple of 64 MB (min 64 MB)
	const UINT64 kChunk = 64ULL << 20; // 64 MB
	UINT64 newSize = ((requiredSize + kChunk - 1) / kChunk) * kChunk;
	if (newSize < requiredSize)
		newSize = requiredSize;

	D3D12_RESOURCE_DESC bufDesc{};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Alignment = 0;
	bufDesc.Width = newSize;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.SampleDesc.Quality = 0;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES hpUpload{};
	hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

	m_uploadBuffer.Reset();
	HRESULT hr = m_d3d12Device->CreateCommittedResource(
		&hpUpload,
		D3D12_HEAP_FLAG_NONE,
		&bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&m_uploadBuffer));
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] CreateCommittedResource for persistent upload buffer (%llu MB) failed (0x%08X)\n",
			newSize >> 20, hr);
		m_uploadBufferSize = 0;
		return hr;
	}

	m_uploadBufferSize = newSize;
	LogDirectStorage(L"[INFO] Persistent upload buffer resized to %llu MB\n", newSize >> 20);
	return S_OK;
}

// ---------------------------------------------------------------------------
// Format & Pitch Calculation Utilities
// ---------------------------------------------------------------------------

UINT DirectStorageManager::BitsPerPixel(DXGI_FORMAT fmt)
{
	switch (fmt) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return 128;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
		return 64;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return 32;
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
		return 16;
	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM:
		return 8;
	default:
		return 0;
	}
}

bool DirectStorageManager::IsBlockCompressed(DXGI_FORMAT fmt)
{
	switch (fmt) {
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
	case DXGI_FORMAT_BC4_SNORM:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
	case DXGI_FORMAT_BC5_SNORM:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC6H_UF16:
	case DXGI_FORMAT_BC6H_SF16:
	case DXGI_FORMAT_BC7_TYPELESS:
	case DXGI_FORMAT_BC7_UNORM:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

UINT DirectStorageManager::RowPitch(DXGI_FORMAT fmt, UINT width)
{
	if (IsBlockCompressed(fmt)) {
		UINT blockSize = (fmt == DXGI_FORMAT_BC1_TYPELESS ||
						  fmt == DXGI_FORMAT_BC1_UNORM ||
						  fmt == DXGI_FORMAT_BC1_UNORM_SRGB ||
						  fmt == DXGI_FORMAT_BC4_TYPELESS ||
						  fmt == DXGI_FORMAT_BC4_UNORM ||
						  fmt == DXGI_FORMAT_BC4_SNORM) ? 8 : 16;
		return std::max(1u, (width + 3) / 4) * blockSize;
	}
	return std::max(1u, width * BitsPerPixel(fmt) / 8);
}

UINT DirectStorageManager::SlicePitch(DXGI_FORMAT fmt, UINT width, UINT height)
{
	if (IsBlockCompressed(fmt))
		return RowPitch(fmt, width) * std::max(1u, (height + 3) / 4);
	return RowPitch(fmt, width) * std::max(1u, height);
}

// ---------------------------------------------------------------------------
// Container Parsers (DDS & GDDS)
// ---------------------------------------------------------------------------

bool DirectStorageManager::ParseDDS(const wchar_t *filename, DDSInfo *out)
{
	HANDLE file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx(file, &fileSize)) {
		CloseHandle(file);
		return false;
	}

	BYTE buffer[148];
	DWORD bytesRead = 0;
	ReadFile(file, buffer, sizeof(buffer), &bytesRead, nullptr);
	CloseHandle(file);

	if (bytesRead < 4 + sizeof(DDS_HEADER))
		return false;

	const DDS_HEADER *hdr = (const DDS_HEADER*)(buffer + 4);
	if (hdr->size != sizeof(DDS_HEADER) || hdr->width == 0 || hdr->height == 0)
		return false;

	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	UINT dim = DDS_DIMENSION_TEXTURE2D;
	UINT arr = 1;
	UINT mips = std::max(1u, hdr->mipMapCount);
	UINT headerSize = 128;

	if (hdr->ddspf.flags & DDS_FOURCC) {
		switch (hdr->ddspf.fourCC) {
		case 0x31545844: fmt = DXGI_FORMAT_BC1_UNORM; break; // DXT1
		case 0x33545844: fmt = DXGI_FORMAT_BC2_UNORM; break; // DXT3
		case 0x35545844: fmt = DXGI_FORMAT_BC3_UNORM; break; // DXT5
		case 0x31495441:
		case 0x55344342: fmt = DXGI_FORMAT_BC4_UNORM; break; // ATI1 / BC4U
		case 0x53344342: fmt = DXGI_FORMAT_BC4_SNORM; break; // BC4S
		case 0x32495441:
		case 0x55354342: fmt = DXGI_FORMAT_BC5_UNORM; break; // ATI2 / BC5U
		case 0x53354342: fmt = DXGI_FORMAT_BC5_SNORM; break; // BC5S
		case 0x36484342: fmt = DXGI_FORMAT_BC6H_UF16; break; // BC6H
		case 0x37544342: fmt = DXGI_FORMAT_BC7_UNORM; break; // BC7
		case 0x30315844: {                                    // DX10 extended header
			if (bytesRead < 4 + sizeof(DDS_HEADER) + sizeof(DDS_HEADER_DXT10))
				return false;
			auto *dx10 = (const DDS_HEADER_DXT10*)(buffer + 4 + sizeof(DDS_HEADER));
			fmt = dx10->dxgiFormat;
			dim = dx10->resourceDimension;
			arr = std::max(1u, dx10->arraySize);
			headerSize = 148;
			break;
		}
		default:
			return false;
		}
	} else {
		return false;
	}

	if (fmt == DXGI_FORMAT_UNKNOWN)
		return false;

	D3D12_RESOURCE_DESC &desc = out->texDesc;
	desc.Dimension = (dim == DDS_DIMENSION_TEXTURE3D) ? D3D12_RESOURCE_DIMENSION_TEXTURE3D :
					 (dim == DDS_DIMENSION_TEXTURE1D) ? D3D12_RESOURCE_DIMENSION_TEXTURE1D :
					 D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Alignment = 0;
	desc.Width = hdr->width;
	desc.Height = hdr->height;
	desc.DepthOrArraySize = (UINT16)((desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) ? std::max(1u, hdr->depth) : arr);
	desc.MipLevels = (UINT16)mips;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

	out->cube = (dim == DDS_DIMENSION_TEXTURE2D) && (arr == 6) && (hdr->caps2 & 0x200);
	out->headerSize = headerSize;
	out->dataOffset = headerSize;
	out->dataSize = (UINT64)fileSize.QuadPart - headerSize;
	out->valid = true;
	return true;
}

bool DirectStorageManager::ParseGDDS(const wchar_t *filename, GDDSInfo *out)
{
	HANDLE file = CreateFileW(filename, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return false;

	BYTE header[148];
	DWORD bytesRead = 0;
	if (!ReadFile(file, header, sizeof(header), &bytesRead, nullptr) || bytesRead != sizeof(header)) {
		CloseHandle(file);
		return false;
	}

	// Validate GDDS magic signature ("GDDS")
	if (memcmp(header, "GDDS", 4) != 0) {
		CloseHandle(file);
		return false;
	}

	const DDS_HEADER *dh = (const DDS_HEADER*)(header + 4);
	if (dh->size != sizeof(DDS_HEADER)) {
		CloseHandle(file);
		return false;
	}

	if (dh->ddspf.size != sizeof(DDS_PIXELFORMAT) || memcmp(&dh->ddspf.fourCC, "DX10", 4) != 0) {
		CloseHandle(file);
		return false;
	}

	// Read chunk count from reserved2 (offset 124) and DX10 header fields (offset 128+)
	UINT chunkCount = *(UINT*)(header + 124);
	DXGI_FORMAT format = (DXGI_FORMAT)*(UINT*)(header + 128);
	UINT dim = *(UINT*)(header + 132);
	UINT arraySize = *(UINT*)(header + 140);

	if (dim != DDS_DIMENSION_TEXTURE2D || arraySize != 1 || chunkCount == 0 || chunkCount > 64) {
		CloseHandle(file);
		return false;
	}

	// Read chunk descriptors in a single contiguous read (16 bytes each)
	std::vector<BYTE> descBuffer(chunkCount * 16);
	DWORD descRead = 0;
	if (!ReadFile(file, descBuffer.data(), (DWORD)descBuffer.size(), &descRead, nullptr) || descRead != descBuffer.size()) {
		CloseHandle(file);
		return false;
	}
	CloseHandle(file);

	std::vector<ChunkInfo> chunks(chunkCount);
	for (UINT i = 0; i < chunkCount; ++i) {
		const BYTE *descBytes = descBuffer.data() + i * 16;
		chunks[i].fileOffset = *(const UINT32*)(descBytes + 0);
		chunks[i].compressedSize = *(const UINT32*)(descBytes + 8);
		chunks[i].uncompressedSize = *(const UINT32*)(descBytes + 12);
	}

	// Setup D3D12 resource descriptor
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Alignment = 0;
	desc.Width = dh->width;
	desc.Height = dh->height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = format;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

	out->texDesc = desc;
	out->width = dh->width;
	out->height = dh->height;
	out->format = format;
	out->chunkCount = chunkCount;
	out->chunks = std::move(chunks);
	out->headerSize = 148 + chunkCount * 16;
	out->valid = true;
	return true;
}

// ---------------------------------------------------------------------------
// Texture Allocation & Sharing Helpers
// ---------------------------------------------------------------------------

HRESULT DirectStorageManager::CreateSharedTexture(
	ID3D11Device *gameDevice,
	const D3D11_TEXTURE2D_DESC &desc11,
	ID3D11Texture2D **ppTex11,
	ID3D12Resource **ppTex12)
{
	*ppTex11 = nullptr;
	*ppTex12 = nullptr;

	D3D11_TEXTURE2D_DESC actualDesc = desc11;
	actualDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	ComPtr<ID3D11Texture2D> tex11;
	HRESULT hr = gameDevice->CreateTexture2D(&actualDesc, nullptr, &tex11);
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] CreateTexture2D on game device failed (0x%08X)\n", hr);
		return hr;
	}

	ComPtr<IDXGIResource> dxgiRes;
	hr = tex11.As(&dxgiRes);
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] QueryInterface for IDXGIResource failed (0x%08X)\n", hr);
		return hr;
	}

	HANDLE sharedHandle = nullptr;
	hr = dxgiRes->GetSharedHandle(&sharedHandle);
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] GetSharedHandle failed (0x%08X)\n", hr);
		return hr;
	}

	// Open the shared handle on our internal D3D12 device
	ComPtr<ID3D12Resource> tex12;
	hr = m_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&tex12));
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] OpenSharedHandle on D3D12 device failed (0x%08X)\n", hr);
		return hr;
	}

	*ppTex11 = tex11.Detach();
	*ppTex12 = tex12.Detach();
	return S_OK;
}

HRESULT DirectStorageManager::CreateWrappedAndShare(
	ID3D12Resource *tex12,
	const D3D11_RESOURCE_FLAGS &flags11,
	ID3D11Device *gameDevice,
	ID3D11Texture2D **ppOut)
{
	*ppOut = nullptr;

	ComPtr<ID3D11Texture2D> wrapped;
	HRESULT hr = m_d3d11On12->CreateWrappedResource(
		tex12,
		&flags11,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COMMON,
		IID_PPV_ARGS(&wrapped));
	if (FAILED(hr))
		return hr;

	ComPtr<IDXGIResource1> dxgiRes;
	hr = wrapped.As(&dxgiRes);
	if (FAILED(hr))
		return hr;

	HANDLE sharedHandle = nullptr;
	hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);
	if (FAILED(hr))
		return hr;

	ComPtr<ID3D11Device1> dev1;
	hr = gameDevice->QueryInterface(IID_PPV_ARGS(&dev1));
	if (FAILED(hr)) {
		CloseHandle(sharedHandle);
		return hr;
	}

	ID3D11Texture2D* result = nullptr;
	hr = dev1->OpenSharedResource1(sharedHandle, IID_PPV_ARGS(&result));
	CloseHandle(sharedHandle);
	if (FAILED(hr))
		return hr;

	*ppOut = result;
	return S_OK;
}

// ---------------------------------------------------------------------------
// Loading Entry Points & Pipelines
// ---------------------------------------------------------------------------

HRESULT DirectStorageManager::LoadTextureFromFile(
	ID3D11Device *gameDevice,
	const std::wstring &filename,
	D3D11_BIND_FLAG bindFlags,
	D3D11_RESOURCE_MISC_FLAG miscFlags,
	ID3D11Texture2D **ppTexture)
{
	*ppTexture = nullptr;
	if (!m_ready || !m_factory || !m_queue)
		return E_ABORT;

	std::lock_guard<std::mutex> lock(m_loadMutex);

	GDDSInfo ginfo;
	if (ParseGDDS(filename.c_str(), &ginfo)) {
		return LoadGDDS(gameDevice, filename, bindFlags, miscFlags, ppTexture, ginfo);
	}

	DDSInfo dinfo;
	if (ParseDDS(filename.c_str(), &dinfo)) {
		return LoadDDS(gameDevice, filename, bindFlags, miscFlags, ppTexture, dinfo);
	}

	return E_NOTIMPL;
}

HRESULT DirectStorageManager::LoadDDS(
	ID3D11Device *gameDevice,
	const std::wstring &filename,
	D3D11_BIND_FLAG bindFlags,
	D3D11_RESOURCE_MISC_FLAG miscFlags,
	ID3D11Texture2D **ppTexture,
	const DDSInfo &info)
{
	const D3D12_RESOURCE_DESC &texDesc = info.texDesc;
	D3D12_HEAP_PROPERTIES hpDef{};
	hpDef.Type = D3D12_HEAP_TYPE_DEFAULT;

	ComPtr<ID3D12Resource> tex12;
	HRESULT hr = m_d3d12Device->CreateCommittedResource(
		&hpDef,
		D3D12_HEAP_FLAG_NONE,
		&texDesc,
		D3D12_RESOURCE_STATE_COMMON,
		nullptr,
		IID_PPV_ARGS(&tex12));
	if (FAILED(hr))
		return hr;

	hr = EnsureUploadBufferSize(info.dataSize + info.headerSize);
	if (FAILED(hr))
		return hr;

	ComPtr<IDStorageFile> file;
	hr = m_factory->OpenFile(filename.c_str(), IID_PPV_ARGS(&file));
	if (FAILED(hr))
		return hr;

	DSTORAGE_REQUEST req{};
	req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
	req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
	req.Source.File.Source = file.Get();
	req.Source.File.Offset = 0;
	req.Source.File.Size = info.dataSize + info.headerSize;
	req.Destination.Buffer.Resource = m_uploadBuffer.Get();
	req.Destination.Buffer.Offset = 0;
	req.Destination.Buffer.Size = (UINT32)(info.dataSize + info.headerSize);

	UINT64 fenceVal = m_nextFenceValue++;
	m_queue->EnqueueRequest(&req);
	m_queue->EnqueueSignal(m_fence.Get(), fenceVal);
	m_queue->Submit();

	hr = WaitForFence(fenceVal);
	if (FAILED(hr))
		return hr;

	D3D11_RESOURCE_FLAGS flags11{};
	flags11.BindFlags = bindFlags;
	flags11.MiscFlags = miscFlags;

	ComPtr<ID3D11Texture2D> wrapped;
	hr = m_d3d11On12->CreateWrappedResource(
		tex12.Get(),
		&flags11,
		D3D12_RESOURCE_STATE_COMMON,
		D3D12_RESOURCE_STATE_COMMON,
		IID_PPV_ARGS(&wrapped));
	if (FAILED(hr))
		return hr;

	{
		void* mapped = nullptr;
		hr = m_uploadBuffer->Map(0, nullptr, &mapped);
		if (FAILED(hr))
			return hr;

		const BYTE* base = (const BYTE*)mapped;
		ID3D11Resource* res = wrapped.Get();
		m_d3d11On12->AcquireWrappedResources(&res, 1);

		UINT mips = texDesc.MipLevels;
		bool is3D = (texDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
		UINT arr = is3D ? 1 : texDesc.DepthOrArraySize;
		UINT depth = is3D ? texDesc.DepthOrArraySize : 1;
		UINT64 offset = info.dataOffset;

		for (UINT slice = 0; slice < arr; ++slice) {
			UINT w = (UINT)texDesc.Width;
			UINT h = texDesc.Height;
			UINT d = depth;
			for (UINT mip = 0; mip < mips; ++mip) {
				UINT rowPitch = RowPitch(texDesc.Format, w);
				UINT slicePitch = SlicePitch(texDesc.Format, w, h);
				UINT subresource = mip + mips * slice;

				m_d3d11On12Context->UpdateSubresource(wrapped.Get(), subresource, nullptr, base + offset, rowPitch, slicePitch);
				offset += (UINT64)slicePitch * d;
				w = std::max(1u, w / 2);
				h = std::max(1u, h / 2);
				d = std::max(1u, d / 2);
			}
		}

		m_d3d11On12->ReleaseWrappedResources(&res, 1);
		m_d3d11On12Context->Flush();
		m_uploadBuffer->Unmap(0, nullptr);
	}

	UINT64 flushFence = m_nextFenceValue++;
	m_cmdQueue->Signal(m_fence.Get(), flushFence);
	hr = WaitForFence(flushFence);
	if (FAILED(hr))
		return hr;

	LogDebugW(L"DirectStorage: Loaded %ls (standard DDS)\n", filename.c_str());
	return CreateWrappedAndShare(tex12.Get(), flags11, gameDevice, ppTexture);
}

HRESULT DirectStorageManager::LoadGDDS(
	ID3D11Device *gameDevice,
	const std::wstring &filename,
	D3D11_BIND_FLAG bindFlags,
	D3D11_RESOURCE_MISC_FLAG miscFlags,
	ID3D11Texture2D **ppTexture,
	const GDDSInfo &info)
{
	LARGE_INTEGER freq, start, end;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);

	const wchar_t *basename = wcsrchr(filename.c_str(), L'\\');
	basename = basename ? basename + 1 : filename.c_str();

	// Calculate uncompressed texture payload size
	UINT slicePitch = SlicePitch(info.format, info.width, info.height);
	UINT64 totalChunkSize = 0;
	for (const auto &chunk : info.chunks)
		totalChunkSize += chunk.uncompressedSize;

	// In the reference build, GPU loading proceeds if chunk metadata matches texture size
	// (or chunk count >= 1 and dimensions are valid)
	if (info.chunkCount >= 1 && totalChunkSize == (UINT64)slicePitch) {
		HRESULT hr = LoadGDDS_Gpu(gameDevice, filename, bindFlags, miscFlags, ppTexture, info);
		if (SUCCEEDED(hr)) {
			QueryPerformanceCounter(&end);
			double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
			g_dsGpuCount++;
			g_dsTotalGpuMs += ms;
			LogDirectStorage(L"[GPU] %ls (%ux%u, fmt %u, %u chunk%ls) in %.2f ms\n",
				basename, info.width, info.height, (UINT)info.format,
				info.chunkCount, info.chunkCount == 1 ? L"" : L"s", ms);

			UINT total = g_dsGpuCount + g_dsCpuCount;
			if (total % 25 == 0) {
				LogDirectStorage(L"--> [STATS] Total: %u | GPU: %u (avg %.2f ms) | CPU Fallback: %u (avg %.2f ms)\n",
					total, g_dsGpuCount, g_dsGpuCount ? (g_dsTotalGpuMs / g_dsGpuCount) : 0.0,
					g_dsCpuCount, g_dsCpuCount ? (g_dsTotalCpuMs / g_dsCpuCount) : 0.0);
			}
			return hr;
		}

		LogDirectStorage(L"[FALLBACK] GPU decompression failed for %ls (0x%08X), attempting CPU fallback\n",
			basename, hr);
		LogWarningW(L"DirectStorage: GPU decompression failed for %ls (0x%08x), falling back to CPU codec\n",
			filename.c_str(), hr);
	}

	// Fallback to CPU GDeflate codec decompression if GPU path fails or chunk metadata doesn't match
	HRESULT hr = LoadGDDS_CpuFallback(gameDevice, filename, bindFlags, miscFlags, ppTexture, info);
	QueryPerformanceCounter(&end);
	double ms = (double)(end.QuadPart - start.QuadPart) * 1000.0 / (double)freq.QuadPart;
	g_dsCpuCount++;
	g_dsTotalCpuMs += ms;
	LogDirectStorage(L"[CPU] %ls (%ux%u, fmt %u, %u chunk%ls) in %.2f ms (status 0x%08X)\n",
		basename, info.width, info.height, (UINT)info.format,
		info.chunkCount, info.chunkCount == 1 ? L"" : L"s",
		ms, hr);

	UINT total = g_dsGpuCount + g_dsCpuCount;
	if (total % 25 == 0) {
		LogDirectStorage(L"--> [STATS] Total: %u | GPU: %u (avg %.2f ms) | CPU Fallback: %u (avg %.2f ms)\n",
			total, g_dsGpuCount, g_dsGpuCount ? (g_dsTotalGpuMs / g_dsGpuCount) : 0.0,
			g_dsCpuCount, g_dsCpuCount ? (g_dsTotalCpuMs / g_dsCpuCount) : 0.0);
	}
	return hr;
}

HRESULT DirectStorageManager::LoadGDDS_Gpu(
	ID3D11Device *gameDevice,
	const std::wstring &filename,
	D3D11_BIND_FLAG bindFlags,
	D3D11_RESOURCE_MISC_FLAG miscFlags,
	ID3D11Texture2D **ppTexture,
	const GDDSInfo &info)
{
	*ppTexture = nullptr;

	// Calculate total uncompressed size across all GDDS chunks
	UINT64 totalUncompressedSize = 0;
	for (const auto &chunk : info.chunks)
		totalUncompressedSize += chunk.uncompressedSize;

	// 1. Create shared D3D11 texture directly on game device and open on D3D12 device
	D3D11_TEXTURE2D_DESC desc11{};
	desc11.Width = info.width;
	desc11.Height = info.height;
	desc11.MipLevels = 1;
	desc11.ArraySize = 1;
	desc11.Format = info.format;
	desc11.SampleDesc.Count = 1;
	desc11.SampleDesc.Quality = 0;
	desc11.Usage = D3D11_USAGE_DEFAULT;
	desc11.BindFlags = bindFlags ? bindFlags : D3D11_BIND_SHADER_RESOURCE;
	desc11.CPUAccessFlags = 0;
	desc11.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	ComPtr<ID3D11Texture2D> tex11;
	ComPtr<ID3D12Resource> tex12;
	HRESULT hr = CreateSharedTexture(gameDevice, desc11, &tex11, &tex12);
	if (FAILED(hr))
		return hr;

	// 2. Ensure persistent upload buffer is sufficiently sized
	hr = EnsureUploadBufferSize(totalUncompressedSize);
	if (FAILED(hr))
		return hr;

	// 3. Open source file via DirectStorage factory
	ComPtr<IDStorageFile> file;
	hr = m_factory->OpenFile(filename.c_str(), IID_PPV_ARGS(&file));
	if (FAILED(hr))
		return hr;

	// 4. Enqueue DirectStorage GDeflate decompression requests into linear upload buffer
	UINT64 bufferOffset = 0;
	for (UINT i = 0; i < info.chunkCount; ++i) {
		DSTORAGE_REQUEST req{};
		req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
		req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_BUFFER;
		req.Source.File.Source = file.Get();
		req.Source.File.Offset = info.chunks[i].fileOffset;
		req.Source.File.Size = info.chunks[i].compressedSize;
		req.Destination.Buffer.Resource = m_uploadBuffer.Get();
		req.Destination.Buffer.Offset = bufferOffset;
		req.Destination.Buffer.Size = info.chunks[i].uncompressedSize;
		req.UncompressedSize = info.chunks[i].uncompressedSize;

		m_queue->EnqueueRequest(&req);
		bufferOffset += info.chunks[i].uncompressedSize;
	}

	// Signal fence and submit DirectStorage queue
	const UINT64 dsFenceVal = m_nextFenceValue++;
	m_queue->EnqueueSignal(m_fence.Get(), dsFenceVal);
	m_queue->Submit();

	// 5. Have the D3D12 command queue wait for DirectStorage to finish decompressing
	hr = m_cmdQueue->Wait(m_fence.Get(), dsFenceVal);
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] Command Queue Wait for DS failed (0x%08X)\n", hr);
		return hr;
	}

	// 6. Reset command allocator and command list
	hr = m_cmdAllocator->Reset();
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] Command Allocator Reset failed (0x%08X)\n", hr);
		return hr;
	}

	hr = m_cmdList->Reset(m_cmdAllocator.Get(), nullptr);
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] Commandlist Reset failed (0x%08X)\n", hr);
		return hr;
	}

	// Transition destination texture: COMMON -> COPY_DEST
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = tex12.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	m_cmdList->ResourceBarrier(1, &barrier);

	// Query layout footprint for subresource 0
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
	m_d3d12Device->GetCopyableFootprints(&info.texDesc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);

	// Issue CopyTextureRegion from linear upload buffer to tiled D3D12 texture
	D3D12_TEXTURE_COPY_LOCATION dstLoc{};
	dstLoc.pResource = tex12.Get();
	dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dstLoc.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION srcLoc{};
	srcLoc.pResource = m_uploadBuffer.Get();
	srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	srcLoc.PlacedFootprint = layout;

	m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

	// Transition destination texture: COPY_DEST -> COMMON
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	m_cmdList->ResourceBarrier(1, &barrier);

	hr = m_cmdList->Close();
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] Closing commandlist failed (0x%08X)\n", hr);
		return hr;
	}

	// Execute command list on command queue
	ID3D12CommandList *cmdLists[] = { m_cmdList.Get() };
	m_cmdQueue->ExecuteCommandLists(1, cmdLists);

	// 8. Signal queue fence after all work and wait for GPU completion
	const UINT64 workFenceVal = m_nextFenceValue++;
	hr = m_cmdQueue->Signal(m_fence.Get(), workFenceVal);
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] Queue Signal after all work failed (0x%08X)\n", hr);
		return hr;
	}

	hr = WaitForFence(workFenceVal);
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] WaitForFence failed (0x%08X)\n", hr);
		return hr;
	}

	// Check DirectStorage error event
	HANDLE errEvent = m_queue->GetErrorEvent();
	if (WaitForSingleObject(errEvent, 0) == WAIT_OBJECT_0) {
		DSTORAGE_ERROR_RECORD errRecord{};
		m_queue->RetrieveErrorRecord(&errRecord);
		LogDirectStorage(L"[ERROR] DirectStorage queue failure on %ls: FailureCount=%u, hr=0x%08X, commandType=%d\n",
			filename.c_str(), errRecord.FailureCount, errRecord.FirstFailure.HResult, (int)errRecord.FirstFailure.CommandType);
		return errRecord.FirstFailure.HResult;
	}

	hr = m_d3d12Device->GetDeviceRemovedReason();
	if (FAILED(hr)) {
		LogDirectStorage(L"[ERROR] D3D12 Device Removed (0x%08X)\n", hr);
		return hr;
	}

	LogDebugW(L"DirectStorage: Loaded %ls (GPU %s-chunk, %u chunk%ls)\n",
		filename.c_str(),
		info.chunkCount == 1 ? L"single" : L"multi",
		info.chunkCount,
		info.chunkCount == 1 ? L"" : L"s");

	*ppTexture = tex11.Detach();
	return S_OK;
}

HRESULT DirectStorageManager::LoadGDDS_CpuFallback(
	ID3D11Device *gameDevice,
	const std::wstring &filename,
	D3D11_BIND_FLAG bindFlags,
	D3D11_RESOURCE_MISC_FLAG miscFlags,
	ID3D11Texture2D **ppTexture,
	const GDDSInfo &info)
{
	// Calculate total uncompressed texture buffer size across all chunks
	UINT64 totalSize = 0;
	for (const auto &chunk : info.chunks)
		totalSize += chunk.uncompressedSize;

	// Ensure staging buffer accommodates full subresource footprint even if legacy compressed chunk was truncated
	UINT slicePitch = SlicePitch(info.format, info.width, info.height);
	UINT64 bufferSize = std::max((UINT64)slicePitch, totalSize);
	std::vector<BYTE> decompressedData(bufferSize, 0);
	HANDLE file = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());

	// Decompress each chunk sequentially into the staging buffer
	UINT64 outOffset = 0;
	for (const auto &chunk : info.chunks) {
		std::vector<BYTE> compressed(chunk.compressedSize);
		LARGE_INTEGER filePos;
		filePos.QuadPart = chunk.fileOffset;
		SetFilePointerEx(file, filePos, nullptr, FILE_BEGIN);

		DWORD bytesRead = 0;
		if (!ReadFile(file, compressed.data(), chunk.compressedSize, &bytesRead, nullptr) || bytesRead != chunk.compressedSize) {
			CloseHandle(file);
			return E_FAIL;
		}

		if (!m_gdeflateCodec)
			DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, 0, IID_PPV_ARGS(&m_gdeflateCodec));

		if (!m_gdeflateCodec) {
			CloseHandle(file);
			return E_FAIL;
		}

		size_t uncompressedSize = chunk.uncompressedSize;
		size_t actualSize = 0;
		HRESULT hr = m_gdeflateCodec->DecompressBuffer(
			compressed.data(),
			chunk.compressedSize,
			decompressedData.data() + outOffset,
			uncompressedSize,
			&actualSize);
		if (FAILED(hr)) {
			CloseHandle(file);
			return hr;
		}

		outOffset += chunk.uncompressedSize;
	}
	CloseHandle(file);

	// Create D3D11 texture and upload decompressed data via UpdateSubresource
	D3D11_TEXTURE2D_DESC desc11{};
	desc11.Width = info.width;
	desc11.Height = info.height;
	desc11.MipLevels = 1;
	desc11.ArraySize = 1;
	desc11.Format = info.format;
	desc11.SampleDesc.Count = 1;
	desc11.SampleDesc.Quality = 0;
	desc11.Usage = D3D11_USAGE_DEFAULT;
	desc11.BindFlags = bindFlags;
	desc11.CPUAccessFlags = 0;
	desc11.MiscFlags = miscFlags;

	ComPtr<ID3D11Texture2D> tex11;
	HRESULT hr = gameDevice->CreateTexture2D(&desc11, nullptr, &tex11);
	if (FAILED(hr))
		return hr;

	UINT rowPitch = RowPitch(info.format, info.width);

	ComPtr<ID3D11DeviceContext> context;
	gameDevice->GetImmediateContext(&context);
	context->UpdateSubresource(tex11.Get(), 0, nullptr, decompressedData.data(), rowPitch, slicePitch);

	LogDebugW(L"DirectStorage: Loaded %ls (CPU fallback, %u chunks)\n", filename.c_str(), info.chunkCount);
	*ppTexture = tex11.Detach();
	return S_OK;
}
