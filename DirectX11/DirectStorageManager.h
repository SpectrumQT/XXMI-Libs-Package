#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <dstorage.h>
#include <string>
#include <vector>
#include <mutex>

/**
 * DirectStorageManager
 *
 * Manages Microsoft DirectStorage 1.3 runtime initialization, D3D11/D3D12 interop,
 * and high-performance GPU GDeflate decompression for .gdds texture streaming in XXMI.
 */
class DirectStorageManager
{
public:
	DirectStorageManager();
	~DirectStorageManager();

	bool Initialize(ID3D11Device *gameDevice);
	bool IsReady() const { return m_ready; }

	// Main entry point for loading custom textures from disk
	HRESULT LoadTextureFromFile(
		ID3D11Device *gameDevice,
		const std::wstring &filename,
		D3D11_BIND_FLAG bindFlags,
		D3D11_RESOURCE_MISC_FLAG miscFlags,
		ID3D11Texture2D **ppTexture);

	static bool IsEnabled();
	static void SetEnabled(bool enabled);
	static DirectStorageManager* GetInstance();
	static void SetInstance(DirectStorageManager* instance);
	static bool EnsureInitialized(ID3D11Device *dev);

private:
	// GDDS container chunk descriptor
	struct ChunkInfo {
		UINT32 fileOffset = 0;
		UINT32 compressedSize = 0;
		UINT32 uncompressedSize = 0;
	};

	// Parsed GDDS texture container metadata
	struct GDDSInfo {
		D3D12_RESOURCE_DESC texDesc{};
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT chunkCount = 0;
		std::vector<ChunkInfo> chunks;
		UINT headerSize = 0;
		bool valid = false;
	};

	// Parsed standard DDS container metadata
	struct DDSInfo {
		D3D12_RESOURCE_DESC texDesc{};
		UINT headerSize = 0;
		UINT64 dataOffset = 0;
		UINT64 dataSize = 0;
		bool cube = false;
		bool valid = false;
	};

	// Fence synchronization with hybrid spin-wait
	HRESULT WaitForFence(UINT64 value);

	// Format & pitch calculation utilities
	static UINT BitsPerPixel(DXGI_FORMAT fmt);
	static bool IsBlockCompressed(DXGI_FORMAT fmt);
	static UINT RowPitch(DXGI_FORMAT fmt, UINT width);
	static UINT SlicePitch(DXGI_FORMAT fmt, UINT width, UINT height);

	// Container parsers
	static bool ParseDDS(const wchar_t *filename, DDSInfo *out);
	static bool ParseGDDS(const wchar_t *filename, GDDSInfo *out);

	// Texture allocation and cross-API sharing helpers
	HRESULT CreateSharedTexture(
		ID3D11Device *gameDevice,
		const D3D11_TEXTURE2D_DESC &desc11,
		ID3D11Texture2D **ppTex11,
		ID3D12Resource **ppTex12);

	HRESULT CreateWrappedAndShare(
		ID3D12Resource *tex12,
		const D3D11_RESOURCE_FLAGS &flags11,
		ID3D11Device *gameDevice,
		ID3D11Texture2D **ppOut);

	// Loading pipelines
	HRESULT LoadDDS(
		ID3D11Device *gameDevice,
		const std::wstring &filename,
		D3D11_BIND_FLAG bindFlags,
		D3D11_RESOURCE_MISC_FLAG miscFlags,
		ID3D11Texture2D **ppTexture,
		const DDSInfo &info);

	HRESULT LoadGDDS(
		ID3D11Device *gameDevice,
		const std::wstring &filename,
		D3D11_BIND_FLAG bindFlags,
		D3D11_RESOURCE_MISC_FLAG miscFlags,
		ID3D11Texture2D **ppTexture,
		const GDDSInfo &info);

	HRESULT LoadGDDS_Gpu(
		ID3D11Device *gameDevice,
		const std::wstring &filename,
		D3D11_BIND_FLAG bindFlags,
		D3D11_RESOURCE_MISC_FLAG miscFlags,
		ID3D11Texture2D **ppTexture,
		const GDDSInfo &info);

	HRESULT LoadGDDS_CpuFallback(
		ID3D11Device *gameDevice,
		const std::wstring &filename,
		D3D11_BIND_FLAG bindFlags,
		D3D11_RESOURCE_MISC_FLAG miscFlags,
		ID3D11Texture2D **ppTexture,
		const GDDSInfo &info);

	// D3D12 device and command infrastructure
	Microsoft::WRL::ComPtr<ID3D12Device> m_d3d12Device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_cmdQueue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
	Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
	HANDLE m_fenceEvent = NULL;
	UINT64 m_nextFenceValue = 1;

	// D3D11On12 interop for legacy DDS texture wrapping
	Microsoft::WRL::ComPtr<ID3D11Device> m_d3d11On12Device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3d11On12Context;
	Microsoft::WRL::ComPtr<ID3D11On12Device> m_d3d11On12;

	// DirectStorage 1.3 factory, queue, and CPU GDeflate codec
	Microsoft::WRL::ComPtr<IDStorageFactory> m_factory;
	Microsoft::WRL::ComPtr<IDStorageQueue> m_queue;
	Microsoft::WRL::ComPtr<IDStorageCompressionCodec> m_gdeflateCodec;

	// Persistent upload staging buffer for GPU streaming
	Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
	UINT64 m_uploadBufferSize = 0;
	std::mutex m_loadMutex;

	HRESULT EnsureUploadBufferSize(UINT64 requiredSize);

	ID3D11Device *m_gameDevice = nullptr;
	bool m_initialized = false;
	bool m_ready = false;
};
