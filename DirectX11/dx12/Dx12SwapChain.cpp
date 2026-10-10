#include "Dx12SwapChain.h"

#include <vector>

#include "Dx12CommandQueue.h"
#include "Dx12Device.h"

// {6216F1D1-6BC6-4B1C-8CB5-7D5F8A18C381}
const GUID Dx12SwapChain::ProxyIID = { 0x6216f1d1, 0x6bc6, 0x4b1c, { 0x8c, 0xb5, 0x7d, 0x5f, 0x8a, 0x18, 0xc3, 0x81 } };
void *Dx12SwapChain::sVtbl;

static const IID *const swap_chain_iids[] = {
	&__uuidof(IDXGISwapChain), &__uuidof(IDXGISwapChain1), &__uuidof(IDXGISwapChain2),
	&__uuidof(IDXGISwapChain3), &__uuidof(IDXGISwapChain4),
};

Dx12SwapChain::Dx12SwapChain(Dx12CommandQueue *queue, IDXGISwapChain *orig) :
	mRef(1),
	mQueue(queue)
{
	mVersion = Dx12QueryNewest(orig, swap_chain_iids, ARRAYSIZE(swap_chain_iids), (void**)&mOrig);
	mQueue->AddRef();
	sVtbl = *(void**)this;
}

bool Dx12SwapChain::Supports(REFIID riid)
{
	int version;

	if (IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(IDXGIObject))
	    || IsEqualIID(riid, __uuidof(IDXGIDeviceSubObject)))
		return true;

	version = Dx12FindIID(swap_chain_iids, ARRAYSIZE(swap_chain_iids), riid);
	return version >= 0 && version <= mVersion;
}

void Dx12WrapSwapChain(Dx12CommandQueue *queue, IDXGISwapChain **ppSwapChain)
{
	IDXGISwapChain *orig = *ppSwapChain;
	Dx12SwapChain *proxy = new Dx12SwapChain(queue, orig);

	// The proxy took its own reference with the newest interface.
	orig->Release();
	*ppSwapChain = proxy;

	Dx12LogInfo("Dx12SwapChain %p created to wrap IDXGISwapChain%d %p presenting on Dx12CommandQueue %p\n",
		proxy, proxy->mVersion, proxy->mOrig, queue);
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::QueryInterface(REFIID riid, void **ppvObject)
{
	HRESULT hr;

	if (!ppvObject)
		return E_POINTER;

	if (Supports(riid)) {
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	hr = mOrig->QueryInterface(riid, ppvObject);
	Dx12LogForwardedQuery("Dx12SwapChain", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12SwapChain::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12SwapChain::Release()
{
	// Read first: once our reference is gone, so may be this object.
	IDXGISwapChain4 *orig = mOrig;
	Dx12CommandQueue *queue = mQueue;
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref) {
		orig->Release();
		return ref;
	}

	Dx12LogInfo("Dx12SwapChain %p destroyed\n", this);
	delete this;
	orig->Release();
	queue->Release();
	return 0;
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::GetDevice(REFIID riid, void **ppDevice)
{
	// For a D3D12 swap chain DXGI answers this with the device of the
	// queue, not with the queue it was created on. Overlays rely on it.
	HRESULT hr = mOrig->GetDevice(riid, ppDevice);


	if (SUCCEEDED(hr) && ppDevice && *ppDevice && mQueue->mDevice->Supports(riid)) {
		((IUnknown*)*ppDevice)->Release();
		mQueue->mDevice->AddRef();
		*ppDevice = mQueue->mDevice;
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	if (!(Flags & DXGI_PRESENT_TEST))
		Dx12FramePresented();

	return mOrig->Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::Present1(UINT SyncInterval, UINT PresentFlags, const DXGI_PRESENT_PARAMETERS *pPresentParameters)
{
	if (!(PresentFlags & DXGI_PRESENT_TEST))
		Dx12FramePresented();

	return mOrig->Present1(SyncInterval, PresentFlags, pPresentParameters);
}

HRESULT STDMETHODCALLTYPE Dx12SwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, const UINT *pCreationNodeMask, IUnknown *const *ppPresentQueue)
{
	std::vector<Dx12CommandQueue*> proxies;
	std::vector<IUnknown*> queues;
	HRESULT hr;
	UINT i;

	if (!ppPresentQueue)
		return mOrig->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);

	// DXGI needs the original queues, like when creating a swap chain
	for (i = 0; i < BufferCount; i++) {
		Dx12CommandQueue *proxy = Dx12QueryProxy<Dx12CommandQueue>(ppPresentQueue[i]);

		queues.push_back(proxy ? proxy->mOrig : ppPresentQueue[i]);
		if (proxy)
			proxies.push_back(proxy);
	}

	hr = mOrig->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, queues.data());

	for (Dx12CommandQueue *proxy : proxies)
		proxy->Release();

	return hr;
}

#include "generated/Dx12SwapChain.fwd.inl"
