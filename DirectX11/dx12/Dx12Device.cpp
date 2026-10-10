#include "Dx12Device.h"

#include <vector>

#include "util.h"

#include "Dx12CommandList.h"
#include "Dx12CommandQueue.h"
#include "Dx12DescriptorHeap.h"

// {B7EFF590-BAC0-4B25-A617-9374404C9211}
const GUID Dx12Device::ProxyIID = { 0xb7eff590, 0xbac0, 0x4b25, { 0xa6, 0x17, 0x93, 0x74, 0x40, 0x4c, 0x92, 0x11 } };
void *Dx12Device::sVtbl;

static const IID *const device_iids[] = {
	&__uuidof(ID3D12Device), &__uuidof(ID3D12Device1), &__uuidof(ID3D12Device2),
	&__uuidof(ID3D12Device3), &__uuidof(ID3D12Device4), &__uuidof(ID3D12Device5),
	&__uuidof(ID3D12Device6), &__uuidof(ID3D12Device7), &__uuidof(ID3D12Device8),
	&__uuidof(ID3D12Device9), &__uuidof(ID3D12Device10), &__uuidof(ID3D12Device11),
	&__uuidof(ID3D12Device12), &__uuidof(ID3D12Device13), &__uuidof(ID3D12Device14),
};

// Serialises looking up the proxy of a device against that proxy going away
static SRWLOCK device_lock = SRWLOCK_INIT;

Dx12Device::Dx12Device(ID3D12Device *orig) :
	mRef(1)
{
	mVersion = Dx12QueryNewest(orig, device_iids, ARRAYSIZE(device_iids), (void**)&mOrig);
	sVtbl = *(void**)this;
}

bool Dx12Device::Supports(REFIID riid)
{
	int version;

	if (IsEqualIID(riid, ProxyIID) || IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(ID3D12Object)))
		return true;

	version = Dx12FindIID(device_iids, ARRAYSIZE(device_iids), riid);
	return version >= 0 && version <= mVersion;
}

static bool add_ref_if_alive(volatile LONG *ref)
{
	LONG old;

	do {
		old = *ref;
		if (!old)
			return false;
	} while (InterlockedCompareExchange(ref, old + 1, old) != old);

	return true;
}

void Dx12WrapDevice(REFIID riid, void **ppDevice)
{
	IUnknown *unknown = (IUnknown*)*ppDevice;
	ID3D12Device *device = NULL;
	Dx12Device *proxy = NULL;
	UINT size = sizeof(proxy);

	if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&device))))
		return;

	AcquireSRWLockExclusive(&device_lock);

	// A proxy found here may be on its way out in Release, in which case
	// it is replaced.
	if (SUCCEEDED(device->GetPrivateData(Dx12Device::ProxyIID, &size, &proxy))
	    && size == sizeof(proxy) && proxy && proxy->Supports(riid) && add_ref_if_alive(&proxy->mRef)) {
		// The reference the caller was given on the original now
		// belongs to the reference just taken on the proxy.
		Dx12LogInfo("Returning existing Dx12Device %p for ID3D12Device %p\n", proxy, device);
		*ppDevice = proxy;
	} else {
		proxy = new Dx12Device(device);
		if (proxy->Supports(riid)) {
			device->SetPrivateData(Dx12Device::ProxyIID, sizeof(proxy), &proxy);
			// The proxy took its own reference with the newest
			// interface, this one is no longer needed.
			unknown->Release();
			Dx12LogInfo("Dx12Device %p created to wrap ID3D12Device%d %p\n", proxy, proxy->mVersion, proxy->mOrig);
			*ppDevice = proxy;
		} else {
			Dx12LogInfo("*** Not wrapping ID3D12Device %p requested with unknown interface %s\n",
				device, NameFromIID(riid).c_str());
			proxy->mOrig->Release();
			delete proxy;
		}
	}

	ReleaseSRWLockExclusive(&device_lock);

	device->Release();
}

HRESULT STDMETHODCALLTYPE Dx12Device::QueryInterface(REFIID riid, void **ppvObject)
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
	Dx12LogForwardedQuery("Dx12Device", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12Device::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12Device::Release()
{
	// Read first: once our reference is gone, so may be this object.
	ID3D12Device14 *orig = mOrig;
	Dx12Device *registered = NULL;
	UINT size = sizeof(registered);
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref) {
		orig->Release();
		return ref;
	}

	AcquireSRWLockExclusive(&device_lock);
	if (SUCCEEDED(orig->GetPrivateData(ProxyIID, &size, &registered)) && registered == this)
		orig->SetPrivateData(ProxyIID, 0, NULL);
	ReleaseSRWLockExclusive(&device_lock);

	Dx12LogInfo("Dx12Device %p destroyed\n", this);
	delete this;
	orig->Release();
	return 0;
}

// Wraps an object the original device just returned in *ppv. Objects that
// turn out not to be of the kind Proxy wraps, or that were requested with an
// interface it does not implement, are left as they are.
template <class Proxy, class Interface>
static Proxy* wrap_child(Dx12Device *device, REFIID riid, void **ppv)
{
	IUnknown *unknown = (IUnknown*)*ppv;
	Interface *orig = NULL;
	Proxy *proxy;

	if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&orig))))
		return NULL;

	proxy = new Proxy(device, orig);
	orig->Release();

	if (!proxy->Usable(riid)) {
		proxy->Abandon();
		return NULL;
	}

	// The proxy took its own reference with the newest interface.
	unknown->Release();
	*ppv = proxy;
	return proxy;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID riid, void **ppCommandQueue)
{
	HRESULT hr = mOrig->CreateCommandQueue(pDesc, riid, ppCommandQueue);

	if (SUCCEEDED(hr) && ppCommandQueue && *ppCommandQueue) {
		void *orig = *ppCommandQueue;
		Dx12CommandQueue *proxy = wrap_child<Dx12CommandQueue, ID3D12CommandQueue>(this, riid, ppCommandQueue);
		Dx12LogInfo("Dx12CommandQueue %p created to wrap ID3D12CommandQueue %p (type %d)\n", proxy, orig, pDesc->Type);
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommandQueue1(const D3D12_COMMAND_QUEUE_DESC *pDesc, REFIID CreatorID, REFIID riid, void **ppCommandQueue)
{
	HRESULT hr = mOrig->CreateCommandQueue1(pDesc, CreatorID, riid, ppCommandQueue);

	if (SUCCEEDED(hr) && ppCommandQueue && *ppCommandQueue) {
		void *orig = *ppCommandQueue;
		Dx12CommandQueue *proxy = wrap_child<Dx12CommandQueue, ID3D12CommandQueue>(this, riid, ppCommandQueue);
		Dx12LogInfo("Dx12CommandQueue %p created to wrap ID3D12CommandQueue %p (type %d)\n", proxy, orig, pDesc->Type);
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator *pCommandAllocator, ID3D12PipelineState *pInitialState, REFIID riid, void **ppCommandList)
{
	HRESULT hr = mOrig->CreateCommandList(nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);

	if (SUCCEEDED(hr) && ppCommandList && *ppCommandList) {
		void *orig = *ppCommandList;
		Dx12GraphicsCommandList *proxy = wrap_child<Dx12GraphicsCommandList, ID3D12GraphicsCommandList>(this, riid, ppCommandList);
		Dx12LogDebug("Dx12GraphicsCommandList %p created to wrap ID3D12CommandList %p (type %d)\n", proxy, orig, type);
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommandList1(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type, D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **ppCommandList)
{
	HRESULT hr = mOrig->CreateCommandList1(nodeMask, type, flags, riid, ppCommandList);

	if (SUCCEEDED(hr) && ppCommandList && *ppCommandList) {
		void *orig = *ppCommandList;
		Dx12GraphicsCommandList *proxy = wrap_child<Dx12GraphicsCommandList, ID3D12GraphicsCommandList>(this, riid, ppCommandList);
		Dx12LogDebug("Dx12GraphicsCommandList %p created to wrap ID3D12CommandList %p (type %d)\n", proxy, orig, type);
	}

	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *pDescriptorHeapDesc, REFIID riid, void **ppvHeap)
{
	HRESULT hr = mOrig->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);

	if (SUCCEEDED(hr) && ppvHeap && *ppvHeap) {
		void *orig = *ppvHeap;
		Dx12DescriptorHeap *proxy = wrap_child<Dx12DescriptorHeap, ID3D12DescriptorHeap>(this, riid, ppvHeap);
		Dx12LogDebug("Dx12DescriptorHeap %p created to wrap ID3D12DescriptorHeap %p (type %d, %u descriptors, flags 0x%x)\n",
			proxy, orig, pDescriptorHeapDesc->Type, pDescriptorHeapDesc->NumDescriptors, pDescriptorHeapDesc->Flags);
	}

	return hr;
}

void STDMETHODCALLTYPE Dx12Device::CopyDescriptors(UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pDestDescriptorRangeStarts, const UINT *pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pSrcDescriptorRangeStarts, const UINT *pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
#ifdef _WIN64
	// Engines call this per draw to fill their shader visible heap, so
	// the usual sizes are translated on the stack.
	D3D12_CPU_DESCRIPTOR_HANDLE stack[128], *handles = stack;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> large;
	UINT i;

	if (!pDestDescriptorRangeStarts || !pSrcDescriptorRangeStarts) {
		mOrig->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
			NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);
		return;
	}

	if ((UINT64)NumDestDescriptorRanges + NumSrcDescriptorRanges > ARRAYSIZE(stack)) {
		large.resize((size_t)NumDestDescriptorRanges + NumSrcDescriptorRanges);
		handles = large.data();
	}

	for (i = 0; i < NumDestDescriptorRanges; i++)
		handles[i] = Dx12OrigCpuHandle(pDestDescriptorRangeStarts[i]);
	for (i = 0; i < NumSrcDescriptorRanges; i++)
		handles[NumDestDescriptorRanges + i] = Dx12OrigCpuHandle(pSrcDescriptorRangeStarts[i]);

	mOrig->CopyDescriptors(NumDestDescriptorRanges, handles, pDestDescriptorRangeSizes,
		NumSrcDescriptorRanges, handles + NumDestDescriptorRanges, pSrcDescriptorRangeSizes, DescriptorHeapsType);
#else
	mOrig->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
		NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);
#endif
}

// Of the objects we wrap, descriptor heaps and command queues are pageable.

static ID3D12Pageable* unwrap_pageable(ID3D12Pageable *object)
{
	object = Dx12Unwrap<Dx12DescriptorHeap>(object);
	return Dx12Unwrap<Dx12CommandQueue>(object);
}

// Returns objects itself when it holds no proxies, which is the usual case.
static ID3D12Pageable *const * unwrap_pageables(UINT count, ID3D12Pageable *const *objects, std::vector<ID3D12Pageable*> *storage)
{
	UINT i;

	if (!objects)
		return objects;

	for (i = 0; i < count; i++) {
		if (unwrap_pageable(objects[i]) != objects[i])
			break;
	}
	if (i == count)
		return objects;

	storage->resize(count);
	for (i = 0; i < count; i++)
		(*storage)[i] = unwrap_pageable(objects[i]);
	return storage->data();
}

HRESULT STDMETHODCALLTYPE Dx12Device::MakeResident(UINT NumObjects, ID3D12Pageable *const *ppObjects)
{
	std::vector<ID3D12Pageable*> storage;

	return mOrig->MakeResident(NumObjects, unwrap_pageables(NumObjects, ppObjects, &storage));
}

HRESULT STDMETHODCALLTYPE Dx12Device::Evict(UINT NumObjects, ID3D12Pageable *const *ppObjects)
{
	std::vector<ID3D12Pageable*> storage;

	return mOrig->Evict(NumObjects, unwrap_pageables(NumObjects, ppObjects, &storage));
}

HRESULT STDMETHODCALLTYPE Dx12Device::SetResidencyPriority(UINT NumObjects, ID3D12Pageable *const *ppObjects, const D3D12_RESIDENCY_PRIORITY *pPriorities)
{
	std::vector<ID3D12Pageable*> storage;

	return mOrig->SetResidencyPriority(NumObjects, unwrap_pageables(NumObjects, ppObjects, &storage), pPriorities);
}

HRESULT STDMETHODCALLTYPE Dx12Device::EnqueueMakeResident(D3D12_RESIDENCY_FLAGS Flags, UINT NumObjects, ID3D12Pageable *const *ppObjects, ID3D12Fence *pFenceToSignal, UINT64 FenceValueToSignal)
{
	std::vector<ID3D12Pageable*> storage;

	return mOrig->EnqueueMakeResident(Flags, NumObjects, unwrap_pageables(NumObjects, ppObjects, &storage), pFenceToSignal, FenceValueToSignal);
}

#include "generated/Dx12Device.fwd.inl"
