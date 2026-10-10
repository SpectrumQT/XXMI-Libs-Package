#pragma once

#include "Dx12Common.h"

class Dx12CommandQueue;

class Dx12SwapChain final : public IDXGISwapChain4
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	IDXGISwapChain4 *mOrig;
	// Highest N of the IDXGISwapChainN the original implements
	int mVersion;
	volatile LONG mRef;

	// The queue the swap chain presents on, which is what DXGI calls its
	// device for D3D12
	Dx12CommandQueue *mQueue;

	Dx12SwapChain(Dx12CommandQueue *queue, IDXGISwapChain *orig);

	bool Supports(REFIID riid);

#include "generated/Dx12SwapChain.decl.inl"
};

// Replaces a swap chain DXGI just created on the original of queue with its
// proxy.
void Dx12WrapSwapChain(Dx12CommandQueue *queue, IDXGISwapChain **ppSwapChain);
