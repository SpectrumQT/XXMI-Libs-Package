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

class DirectStorageManager
{
public:
	DirectStorageManager();
	~DirectStorageManager();

	bool Initialize(ID3D11Device *gameDevice);
	bool IsReady() const { return m_ready; }

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
	struct ChunkInfo {
		UINT32 fileOffset = 0;
		UINT32 compressedSize = 0;
		UINT32 uncompressedSize = 0;
	};

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

	struct DDSInfo {
		D3D12_RESOURCE_DESC texDesc{};
		UINT headerSize = 0;
		UINT64 dataOffset = 0;
		UINT64 dataSize = 0;
		bool cube = false;
		bool valid = false;
	};

	HRESULT WaitForFence(UINT64 value);

	static UINT BitsPerPixel(DXGI_FORMAT fmt);
	static bool IsBlockCompressed(DXGI_FORMAT fmt);
	static UINT RowPitch(DXGI_FORMAT fmt, UINT width);
	static UINT SlicePitch(DXGI_FORMAT fmt, UINT width, UINT height);

	bool ParseDDS(const wchar_t *filename, DDSInfo *out);
	bool ParseGDDS(const wchar_t *filename, GDDSInfo *out);

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

	HRESULT LoadGDDS_GpuSingleChunk(
		ID3D11Device *gameDevice,
		const std::wstring &filename,
		D3D11_BIND_FLAG bindFlags,
		D3D11_RESOURCE_MISC_FLAG miscFlags,
		ID3D11Texture2D **ppTexture,
		const GDDSInfo &info);

	HRESULT LoadGDDS_GpuMultiChunk(
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

	HRESULT CreateWrappedAndShare(
		ID3D12Resource *tex12,
		const D3D11_RESOURCE_FLAGS &flags11,
		ID3D11Device *gameDevice,
		ID3D11Texture2D **ppOut);

	Microsoft::WRL::ComPtr<ID3D12Device> m_d3d12Device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_cmdQueue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
	Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
	HANDLE m_fenceEvent = NULL;
	UINT64 m_nextFenceValue = 1;

	Microsoft::WRL::ComPtr<ID3D11Device> m_d3d11On12Device;
	Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3d11On12Context;
	Microsoft::WRL::ComPtr<ID3D11On12Device> m_d3d11On12;

	Microsoft::WRL::ComPtr<IDStorageFactory> m_factory;
	Microsoft::WRL::ComPtr<IDStorageQueue> m_queue;
	Microsoft::WRL::ComPtr<IDStorageCompressionCodec> m_gdeflateCodec;

	ID3D11Device *m_gameDevice = nullptr;
	bool m_initialized = false;
	bool m_ready = false;
};
