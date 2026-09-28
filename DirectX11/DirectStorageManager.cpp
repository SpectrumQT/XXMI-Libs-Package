#include "DirectStorageManager.h"
#include <cstdio>
#include <algorithm>
#include <vector>
#include <wrl/client.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "../log.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT DDS_MAGIC = 0x20534444;
constexpr UINT DDS_FOURCC = 0x00000004;
constexpr UINT DDS_RGBA = 0x00000041;
constexpr UINT DDS_RGB = 0x00000040;

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

constexpr UINT DDS_DIMENSION_TEXTURE1D = 2;
constexpr UINT DDS_DIMENSION_TEXTURE2D = 3;
constexpr UINT DDS_DIMENSION_TEXTURE3D = 4;
}

// Standalone manager state kept isolated from struct Globals to preserve struct layout
static bool g_dsEnabled = false;
static DirectStorageManager* g_dsInstance = nullptr;

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
}

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
	LogInfo("DirectStorage: Initializing\n");

	// Create D3D12 device matching the active DXGI adapter LUID
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
		return false;
	}

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

	// Create D3D11On12 interop device on a helper thread to avoid loader hook reentrancy
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

	// Resolve DirectStorage DLLs relative to our module location
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
			return false;
		}
	}

	DSTORAGE_CONFIGURATION config{};
	config.NumSubmitThreads = 1;
	hr = DStorageSetConfiguration(&config);
	if (FAILED(hr))
		LogDebug("DirectStorage: DStorageSetConfiguration status 0x%08x\n", hr);

	hr = DStorageGetFactory(IID_PPV_ARGS(&m_factory));
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to get DirectStorage factory (0x%08x)\n", hr);
		return false;
	}

	hr = m_factory->SetStagingBufferSize(64u << 20);
	if (FAILED(hr)) {
		LogWarning("DirectStorage: Failed to set staging buffer size (0x%08x)\n", hr);
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
		return false;
	}

	DStorageCreateCompressionCodec(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE, 0, IID_PPV_ARGS(&m_gdeflateCodec));

	LogInfo("DirectStorage: Initialized successfully\n");
	m_ready = true;
	return true;
}

HRESULT DirectStorageManager::WaitForFence(UINT64 value)
{
	if (m_fence->GetCompletedValue() >= value)
		return S_OK;

	HRESULT hr = m_fence->SetEventOnCompletion(value, m_fenceEvent);
	if (FAILED(hr))
		return hr;

	WaitForSingleObject(m_fenceEvent, INFINITE);
	return S_OK;
}

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

	if (fileSize.QuadPart < (LONG64)sizeof(DDS_HEADER) + 4) {
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
		case 0x31545844: fmt = DXGI_FORMAT_BC1_UNORM; break;
		case 0x33545844: fmt = DXGI_FORMAT_BC2_UNORM; break;
		case 0x35545844: fmt = DXGI_FORMAT_BC3_UNORM; break;
		case 0x31495441:
		case 0x55344342: fmt = DXGI_FORMAT_BC4_UNORM; break;
		case 0x53344342: fmt = DXGI_FORMAT_BC4_SNORM; break;
		case 0x32495441:
		case 0x55354342: fmt = DXGI_FORMAT_BC5_UNORM; break;
		case 0x53354342: fmt = DXGI_FORMAT_BC5_SNORM; break;
		case 0x36484342: fmt = DXGI_FORMAT_BC6H_UF16; break;
		case 0x37544342: fmt = DXGI_FORMAT_BC7_UNORM; break;
		case 0x30315844: {
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

	if (memcmp(header, "GDDS", 4) != 0) {
		CloseHandle(file);
		return false;
	}

	const DDS_HEADER *dh = (const DDS_HEADER*)(header + 4);
	if (dh->size != sizeof(DDS_HEADER)) {
		CloseHandle(file);
		return false;
	}

	if (dh->ddspf.size != 32 || memcmp(&dh->ddspf.fourCC, "DX10", 4) != 0) {
		CloseHandle(file);
		return false;
	}

	UINT chunkCount = *(UINT*)(header + 124);
	DXGI_FORMAT format = (DXGI_FORMAT)*(UINT*)(header + 128);
	UINT dim = *(UINT*)(header + 132);
	UINT arraySize = *(UINT*)(header + 140);

	if (dim != DDS_DIMENSION_TEXTURE2D || arraySize != 1 || chunkCount == 0 || chunkCount > 64) {
		CloseHandle(file);
		return false;
	}

	std::vector<ChunkInfo> chunks(chunkCount);
	for (UINT i = 0; i < chunkCount; ++i) {
		BYTE descBytes[16];
		DWORD descRead = 0;
		if (!ReadFile(file, descBytes, sizeof(descBytes), &descRead, nullptr) || descRead != sizeof(descBytes)) {
			CloseHandle(file);
			return false;
		}
		chunks[i].fileOffset = *(UINT32*)(descBytes + 0);
		chunks[i].compressedSize = *(UINT32*)(descBytes + 8);
		chunks[i].uncompressedSize = *(UINT32*)(descBytes + 12);
	}
	CloseHandle(file);

	// Normalize format mappings for DirectStorage
	if ((UINT)format == 72)
		format = DXGI_FORMAT_BC1_UNORM_SRGB;
	else if ((UINT)format == 78)
		format = DXGI_FORMAT_BC7_UNORM;

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

	HANDLE file = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());

	char magic[4] = {};
	DWORD bytesRead = 0;
	ReadFile(file, magic, sizeof(magic), &bytesRead, nullptr);
	CloseHandle(file);

	if (bytesRead == 4 && memcmp(magic, "GDDS", 4) == 0) {
		GDDSInfo ginfo;
		if (!ParseGDDS(filename.c_str(), &ginfo))
			return E_NOTIMPL;
		return LoadGDDS(gameDevice, filename, bindFlags, miscFlags, ppTexture, ginfo);
	} else {
		DDSInfo dinfo;
		if (!ParseDDS(filename.c_str(), &dinfo))
			return E_NOTIMPL;
		return LoadDDS(gameDevice, filename, bindFlags, miscFlags, ppTexture, dinfo);
	}
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

	D3D12_RESOURCE_DESC bufDesc{};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = info.dataSize + info.headerSize;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	D3D12_HEAP_PROPERTIES hpUpload{};
	hpUpload.Type = D3D12_HEAP_TYPE_UPLOAD;

	ComPtr<ID3D12Resource> uploadBuffer;
	hr = m_d3d12Device->CreateCommittedResource(
		&hpUpload,
		D3D12_HEAP_FLAG_NONE,
		&bufDesc,
		D3D12_RESOURCE_STATE_GENERIC_READ,
		nullptr,
		IID_PPV_ARGS(&uploadBuffer));
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
	req.Destination.Buffer.Resource = uploadBuffer.Get();
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
		hr = uploadBuffer->Map(0, nullptr, &mapped);
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
		uploadBuffer->Unmap(0, nullptr);
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
	if (info.chunkCount >= 1) {
		HRESULT hr;
		if (info.chunkCount == 1)
			hr = LoadGDDS_GpuSingleChunk(gameDevice, filename, bindFlags, miscFlags, ppTexture, info);
		else
			hr = LoadGDDS_GpuMultiChunk(gameDevice, filename, bindFlags, miscFlags, ppTexture, info);

		if (SUCCEEDED(hr))
			return hr;
	}

	return LoadGDDS_CpuFallback(gameDevice, filename, bindFlags, miscFlags, ppTexture, info);
}

HRESULT DirectStorageManager::LoadGDDS_GpuSingleChunk(
	ID3D11Device *gameDevice,
	const std::wstring &filename,
	D3D11_BIND_FLAG bindFlags,
	D3D11_RESOURCE_MISC_FLAG miscFlags,
	ID3D11Texture2D **ppTexture,
	const GDDSInfo &info)
{
	// Decompress directly into shared D3D11/D3D12 texture with zero staging copies
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
	desc11.MiscFlags = (miscFlags | D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);

	ComPtr<ID3D11Texture2D> tex11;
	HRESULT hr = gameDevice->CreateTexture2D(&desc11, nullptr, &tex11);
	if (FAILED(hr))
		return hr;

	ComPtr<IDXGIResource1> dxgiRes;
	hr = tex11.As(&dxgiRes);
	if (FAILED(hr))
		return hr;

	HANDLE sharedHandle = nullptr;
	hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);
	if (FAILED(hr))
		return hr;

	ComPtr<ID3D12Resource> tex12;
	hr = m_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&tex12));
	CloseHandle(sharedHandle);
	if (FAILED(hr))
		return hr;

	ComPtr<IDStorageFile> file;
	hr = m_factory->OpenFile(filename.c_str(), IID_PPV_ARGS(&file));
	if (FAILED(hr))
		return hr;

	DSTORAGE_REQUEST req{};
	req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
	req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
	req.Source.File.Source = file.Get();
	req.Source.File.Offset = info.chunks[0].fileOffset;
	req.Source.File.Size = info.chunks[0].compressedSize;
	req.Destination.Texture.Resource = tex12.Get();
	req.Destination.Texture.SubresourceIndex = 0;
	req.Destination.Texture.Region.left = 0;
	req.Destination.Texture.Region.top = 0;
	req.Destination.Texture.Region.front = 0;
	req.Destination.Texture.Region.right = info.width;
	req.Destination.Texture.Region.bottom = info.height;
	req.Destination.Texture.Region.back = 1;
	req.UncompressedSize = info.chunks[0].uncompressedSize;

	UINT64 fenceVal = m_nextFenceValue++;
	m_queue->EnqueueRequest(&req);
	m_queue->EnqueueSignal(m_fence.Get(), fenceVal);
	m_queue->Submit();

	hr = WaitForFence(fenceVal);
	if (FAILED(hr))
		return hr;

	hr = m_d3d12Device->GetDeviceRemovedReason();
	if (FAILED(hr))
		return hr;

	LogDebugW(L"DirectStorage: Loaded %ls (GPU single-chunk)\n", filename.c_str());
	*ppTexture = tex11.Detach();
	return S_OK;
}

HRESULT DirectStorageManager::LoadGDDS_GpuMultiChunk(
	ID3D11Device *gameDevice,
	const std::wstring &filename,
	D3D11_BIND_FLAG bindFlags,
	D3D11_RESOURCE_MISC_FLAG miscFlags,
	ID3D11Texture2D **ppTexture,
	const GDDSInfo &info)
{
	// Multi-chunk GPU decompression via slice regions
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
	desc11.MiscFlags = (miscFlags | D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);

	ComPtr<ID3D11Texture2D> tex11;
	HRESULT hr = gameDevice->CreateTexture2D(&desc11, nullptr, &tex11);
	if (FAILED(hr))
		return hr;

	ComPtr<IDXGIResource1> dxgiRes;
	hr = tex11.As(&dxgiRes);
	if (FAILED(hr))
		return hr;

	HANDLE sharedHandle = nullptr;
	hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle);
	if (FAILED(hr))
		return hr;

	ComPtr<ID3D12Resource> tex12;
	hr = m_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(&tex12));
	CloseHandle(sharedHandle);
	if (FAILED(hr))
		return hr;

	ComPtr<IDStorageFile> file;
	hr = m_factory->OpenFile(filename.c_str(), IID_PPV_ARGS(&file));
	if (FAILED(hr))
		return hr;

	UINT sliceHeight = info.height / info.chunkCount;
	for (UINT i = 0; i < info.chunkCount; ++i) {
		DSTORAGE_REQUEST req{};
		req.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
		req.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		req.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
		req.Source.File.Source = file.Get();
		req.Source.File.Offset = info.chunks[i].fileOffset;
		req.Source.File.Size = info.chunks[i].compressedSize;
		req.Destination.Texture.Resource = tex12.Get();
		req.Destination.Texture.SubresourceIndex = 0;
		req.Destination.Texture.Region.left = 0;
		req.Destination.Texture.Region.top = i * sliceHeight;
		req.Destination.Texture.Region.front = 0;
		req.Destination.Texture.Region.right = info.width;
		req.Destination.Texture.Region.bottom = (i + 1) * sliceHeight;
		req.Destination.Texture.Region.back = 1;
		req.UncompressedSize = info.chunks[i].uncompressedSize;
		m_queue->EnqueueRequest(&req);
	}

	UINT64 fenceVal = m_nextFenceValue++;
	m_queue->EnqueueSignal(m_fence.Get(), fenceVal);
	m_queue->Submit();

	hr = WaitForFence(fenceVal);
	if (FAILED(hr))
		return hr;

	hr = m_d3d12Device->GetDeviceRemovedReason();
	if (FAILED(hr))
		return hr;

	LogDebugW(L"DirectStorage: Loaded %ls (GPU multi-chunk, %u chunks)\n", filename.c_str(), info.chunkCount);
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
	// CPU decompression fallback via GDeflate codec
	UINT64 totalSize = 0;
	for (const auto &chunk : info.chunks)
		totalSize += chunk.uncompressedSize;

	std::vector<BYTE> decompressedData(totalSize);
	HANDLE file = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return HRESULT_FROM_WIN32(GetLastError());

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
	UINT slicePitch = SlicePitch(info.format, info.width, info.height);

	ComPtr<ID3D11DeviceContext> context;
	gameDevice->GetImmediateContext(&context);
	context->UpdateSubresource(tex11.Get(), 0, nullptr, decompressedData.data(), rowPitch, slicePitch);

	LogDebugW(L"DirectStorage: Loaded %ls (CPU fallback, %u chunks)\n", filename.c_str(), info.chunkCount);
	*ppTexture = tex11.Detach();
	return S_OK;
}
