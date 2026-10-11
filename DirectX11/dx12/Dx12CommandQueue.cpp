#include "Dx12CommandQueue.h"

#include <vector>

#include "Bridge.h"
#include "Dx12CommandList.h"
#include "Dx12Device.h"
#include "ResourceTracker.h"

// {1B67B403-353E-45FD-B468-760207CA7886}
const GUID Dx12CommandQueue::ProxyIID = { 0x1b67b403, 0x353e, 0x45fd, { 0xb4, 0x68, 0x76, 0x02, 0x07, 0xca, 0x78, 0x86 } };
void *Dx12CommandQueue::sVtbl;

Dx12CommandQueue::Dx12CommandQueue(Dx12Device *device, ID3D12CommandQueue *orig) :
	mOrig(orig),
	mDevice(device),
	mRef(1)
{
	mOrig->AddRef();
	mDevice->AddRef();
	sVtbl = *(void**)this;
}

bool Dx12CommandQueue::Usable(REFIID riid)
{
	return IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(ID3D12Object))
	    || IsEqualIID(riid, __uuidof(ID3D12DeviceChild))
	    || IsEqualIID(riid, __uuidof(ID3D12Pageable))
	    || IsEqualIID(riid, __uuidof(ID3D12CommandQueue));
}

void Dx12CommandQueue::Abandon()
{
	mOrig->Release();
	mDevice->Release();
	delete this;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::QueryInterface(REFIID riid, void **ppvObject)
{
	HRESULT hr;

	if (!ppvObject)
		return E_POINTER;

	if (Usable(riid)) {
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	hr = mOrig->QueryInterface(riid, ppvObject);
	Dx12LogForwardedQuery("Dx12CommandQueue", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12CommandQueue::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12CommandQueue::Release()
{
	// Read first: once our reference is gone, so may be this object.
	ID3D12CommandQueue *orig = mOrig;
	Dx12Device *device = mDevice;
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref) {
		orig->Release();
		return ref;
	}

	Dx12LogInfo("Dx12CommandQueue %p destroyed\n", this);
	delete this;
	orig->Release();
	device->Release();
	return 0;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::GetDevice(REFIID riid, void **ppvDevice)
{
	return mDevice->QueryInterface(riid, ppvDevice);
}

void STDMETHODCALLTYPE Dx12CommandQueue::ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists)
{
	ID3D12CommandList *stack[64], **lists = stack;
	std::vector<ID3D12CommandList*> large;
	std::vector<IUnknown*> pins;
	UINT i;

	if (!ppCommandLists) {
		mOrig->ExecuteCommandLists(NumCommandLists, ppCommandLists);
		return;
	}

	if (NumCommandLists > ARRAYSIZE(stack)) {
		large.resize(NumCommandLists);
		lists = large.data();
	}

	for (i = 0; i < NumCommandLists; i++) {
		lists[i] = Dx12Unwrap<Dx12GraphicsCommandList>(ppCommandLists[i]);
		if (lists[i] == ppCommandLists[i])
			continue;

		// From here on the states the list was recorded to leave its
		// resources in are the ones they have, as far as anything
		// executed later is concerned
		Dx12GraphicsCommandList *proxy = static_cast<Dx12GraphicsCommandList*>(ppCommandLists[i]);
		for (const Dx12Transition &transition : proxy->mTransitions)
			Dx12ApplyTransition(transition.resource, transition.subresource, transition.state);
		proxy->mTransitions.clear();
		// Together, so that one signal of the fence does for all of
		// them
		pins.insert(pins.end(), proxy->mPins.begin(), proxy->mPins.end());
		proxy->mPins.clear();
	}

	InterlockedIncrement64(&dx12_stats.execute_calls);
	InterlockedAdd64(&dx12_stats.lists_executed, NumCommandLists);

	mOrig->ExecuteCommandLists(NumCommandLists, lists);

	if (!pins.empty())
		Dx12ShimPinsExecuted(mOrig, &pins);
}

#include "generated/Dx12CommandQueue.fwd.inl"
