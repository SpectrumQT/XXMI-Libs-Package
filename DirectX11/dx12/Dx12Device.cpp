#include "Dx12Device.h"

#include <vector>

#include "util.h"

#include "DescriptorShadow.h"
#include "Dx12CommandList.h"
#include "Dx12CommandQueue.h"
#include "Dx12PipelineLibrary.h"
#include "PipelineRegistry.h"
#include "ResourceTracker.h"

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
	mRef(1),
	mTied(false)
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

// {4C0E8B21-9A57-4F3D-B6E2-1D8A7C35F904}
static const GUID device_destroyed_guid = { 0x4c0e8b21, 0x9a57, 0x4f3d, { 0xb6, 0xe2, 0x1d, 0x8a, 0x7c, 0x35, 0xf9, 0x04 } };

static void device_destroyed(void *context)
{
	Dx12Device *device = (Dx12Device*)context;

	Dx12LogInfo("Dx12Device %p destroyed\n", device);
	delete device;
}

// Like the queues (see Dx12CommandQueue::Tie), the proxy goes when the
// original does, not when the references taken through it are gone: swap
// chains answer GetDevice with the proxy, and overlays keep the pointer after
// letting go of it. RTSS hands it to D3D11On12CreateDevice after the game made
// its swap chain again.
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
	    && size == sizeof(proxy) && proxy && proxy->Supports(riid)
	    && (proxy->mTied ? InterlockedIncrement(&proxy->mRef) > 0 : add_ref_if_alive(&proxy->mRef))) {
		// The reference the caller was given on the original now
		// belongs to the reference just taken on the proxy.
		Dx12LogInfo("Returning existing Dx12Device %p for ID3D12Device %p\n", proxy, device);
		*ppDevice = proxy;
	} else {
		proxy = new Dx12Device(device);
		if (proxy->Supports(riid)) {
			device->SetPrivateData(Dx12Device::ProxyIID, sizeof(proxy), &proxy);
			proxy->mTied = Dx12NotifyDestruction(proxy->mOrig, device_destroyed_guid, device_destroyed, proxy);
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
	bool tied = mTied;
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref || tied) {
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

// wrap_child for a heap, which may have descriptors the game did not ask for.
// A heap that cannot be wrapped must not have any: the game would be told of
// them.
static Dx12DescriptorHeap* wrap_heap(Dx12Device *device, REFIID riid, void **ppv, UINT reserve)
{
	IUnknown *unknown = (IUnknown*)*ppv;
	ID3D12DescriptorHeap *orig = NULL;
	Dx12DescriptorHeap *proxy;

	if (FAILED(unknown->QueryInterface(IID_PPV_ARGS(&orig))))
		return NULL;

	proxy = new Dx12DescriptorHeap(device, orig, reserve);
	orig->Release();

	if (!proxy->Usable(riid)) {
		proxy->Abandon();
		return NULL;
	}

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
		if (proxy)
			proxy->Tie();
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
		if (proxy)
			proxy->Tie();
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
		if (proxy)
			proxy->mState.pipeline = pInitialState;
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
	D3D12_DESCRIPTOR_HEAP_DESC desc;
	UINT reserve = 0;
	HRESULT hr = E_FAIL;

	// See Dx12DescriptorHeap::mReserve. The game may have asked for all
	// the descriptors the hardware allows in one heap, and then it has to
	// do without.
	if (pDescriptorHeapDesc && ppvHeap
	    && pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
	    && (pDescriptorHeapDesc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)) {
		desc = *pDescriptorHeapDesc;
		desc.NumDescriptors += DX12_HEAP_RESERVE;
		hr = mOrig->CreateDescriptorHeap(&desc, riid, ppvHeap);
		if (SUCCEEDED(hr))
			reserve = DX12_HEAP_RESERVE;
		else
			Dx12LogInfo("*** No room for %u more descriptors in a heap of %u, mods cannot replace textures bound through it\n",
				DX12_HEAP_RESERVE, pDescriptorHeapDesc->NumDescriptors);
	}
	if (!reserve)
		hr = mOrig->CreateDescriptorHeap(pDescriptorHeapDesc, riid, ppvHeap);

	if (SUCCEEDED(hr) && ppvHeap && *ppvHeap) {
		void *orig = *ppvHeap;
		Dx12DescriptorHeap *proxy = wrap_heap(this, riid, ppvHeap, reserve);
		Dx12LogDebug("Dx12DescriptorHeap %p created to wrap ID3D12DescriptorHeap %p (type %d, %u descriptors, flags 0x%x)\n",
			proxy, orig, pDescriptorHeapDesc->Type, pDescriptorHeapDesc->NumDescriptors, pDescriptorHeapDesc->Flags);
	}

	return hr;
}

#ifdef _WIN64
// CopyDescriptors for the destination ranges that have a mirror, when not all
// of them do. Sources are the original device's handles.
static void mirror_descriptors(ID3D12Device *device, UINT dest_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dest_ranges,
		const UINT *dest_sizes, UINT src_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_ranges, const UINT *src_sizes,
		D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	UINT increment = device->GetDescriptorHandleIncrementSize(type);
	UINT dest_range, src_range = 0, src_offset = 0, i, size;
	D3D12_CPU_DESCRIPTOR_HANDLE mirror, src;
	bool has_mirror;

	for (dest_range = 0; dest_range < dest_range_count; dest_range++) {
		has_mirror = Dx12MirrorCpuHandle(dest_ranges[dest_range], &mirror);
		size = dest_sizes ? dest_sizes[dest_range] : 1;

		for (i = 0; i < size; i++) {
			while (src_range < src_range_count && src_offset >= (src_sizes ? src_sizes[src_range] : 1)) {
				src_range++;
				src_offset = 0;
			}
			if (src_range >= src_range_count)
				return;

			if (has_mirror) {
				src.ptr = src_ranges[src_range].ptr + (SIZE_T)src_offset * increment;
				device->CopyDescriptorsSimple(1, mirror, src, type);
				mirror.ptr += increment;
			}
			src_offset++;
		}
	}
}
#endif

void STDMETHODCALLTYPE Dx12Device::CopyDescriptors(UINT NumDestDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pDestDescriptorRangeStarts, const UINT *pDestDescriptorRangeSizes, UINT NumSrcDescriptorRanges, const D3D12_CPU_DESCRIPTOR_HANDLE *pSrcDescriptorRangeStarts, const UINT *pSrcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
#ifdef _WIN64
	// Engines call this per draw to fill their shader visible heap, so
	// the usual sizes are translated on the stack.
	D3D12_CPU_DESCRIPTOR_HANDLE stack[128], *handles = stack;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> large;
	UINT i, mirrored = 0;

	if (!pDestDescriptorRangeStarts || !pSrcDescriptorRangeStarts) {
		mOrig->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
			NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);
		return;
	}

	if ((UINT64)NumDestDescriptorRanges + NumSrcDescriptorRanges > ARRAYSIZE(stack)) {
		large.resize((size_t)NumDestDescriptorRanges + NumSrcDescriptorRanges);
		handles = large.data();
	}

	Dx12ShadowCopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
		NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes);

	for (i = 0; i < NumDestDescriptorRanges; i++)
		handles[i] = Dx12OrigCpuHandle(pDestDescriptorRangeStarts[i]);
	for (i = 0; i < NumSrcDescriptorRanges; i++)
		handles[NumDestDescriptorRanges + i] = Dx12OrigCpuHandle(pSrcDescriptorRangeStarts[i]);

	mOrig->CopyDescriptors(NumDestDescriptorRanges, handles, pDestDescriptorRangeSizes,
		NumSrcDescriptorRanges, handles + NumDestDescriptorRanges, pSrcDescriptorRangeSizes, DescriptorHeapsType);

	// Once more for the destinations that have a mirror, which usually is
	// all of them or none
	for (i = 0; i < NumDestDescriptorRanges; i++)
		mirrored += Dx12MirrorCpuHandle(pDestDescriptorRangeStarts[i], &handles[i]);
	if (mirrored == NumDestDescriptorRanges) {
		mOrig->CopyDescriptors(NumDestDescriptorRanges, handles, pDestDescriptorRangeSizes,
			NumSrcDescriptorRanges, handles + NumDestDescriptorRanges, pSrcDescriptorRangeSizes, DescriptorHeapsType);
	} else if (mirrored) {
		mirror_descriptors(mOrig, NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
			NumSrcDescriptorRanges, handles + NumDestDescriptorRanges, pSrcDescriptorRangeSizes, DescriptorHeapsType);
	}
#else
	mOrig->CopyDescriptors(NumDestDescriptorRanges, pDestDescriptorRangeStarts, pDestDescriptorRangeSizes,
		NumSrcDescriptorRanges, pSrcDescriptorRangeStarts, pSrcDescriptorRangeSizes, DescriptorHeapsType);
#endif
}

void STDMETHODCALLTYPE Dx12Device::CopyDescriptorsSimple(UINT NumDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptorRangeStart, D3D12_CPU_DESCRIPTOR_HANDLE SrcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapsType)
{
	D3D12_CPU_DESCRIPTOR_HANDLE mirror;

	Dx12ShadowCopyDescriptorsSimple(NumDescriptors, DestDescriptorRangeStart, SrcDescriptorRangeStart);
	mOrig->CopyDescriptorsSimple(NumDescriptors, Dx12OrigCpuHandle(DestDescriptorRangeStart), Dx12OrigCpuHandle(SrcDescriptorRangeStart), DescriptorHeapsType);
	if (Dx12MirrorCpuHandle(DestDescriptorRangeStart, &mirror))
		mOrig->CopyDescriptorsSimple(NumDescriptors, mirror, Dx12OrigCpuHandle(SrcDescriptorRangeStart), DescriptorHeapsType);
}

void STDMETHODCALLTYPE Dx12Device::CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	D3D12_CPU_DESCRIPTOR_HANDLE mirror;

	Dx12ShadowConstantBufferView(pDesc, DestDescriptor);
	mOrig->CreateConstantBufferView(pDesc, Dx12OrigCpuHandle(DestDescriptor));
	if (Dx12MirrorCpuHandle(DestDescriptor, &mirror))
		mOrig->CreateConstantBufferView(pDesc, mirror);
}

void STDMETHODCALLTYPE Dx12Device::CreateShaderResourceView(ID3D12Resource *pResource, const D3D12_SHADER_RESOURCE_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	D3D12_CPU_DESCRIPTOR_HANDLE mirror;

	Dx12ShadowShaderResourceView(pResource, pDesc, DestDescriptor);
	mOrig->CreateShaderResourceView(pResource, pDesc, Dx12OrigCpuHandle(DestDescriptor));
	if (Dx12MirrorCpuHandle(DestDescriptor, &mirror))
		mOrig->CreateShaderResourceView(pResource, pDesc, mirror);
}

void STDMETHODCALLTYPE Dx12Device::CreateUnorderedAccessView(ID3D12Resource *pResource, ID3D12Resource *pCounterResource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	D3D12_CPU_DESCRIPTOR_HANDLE mirror;

	Dx12ShadowUnorderedAccessView(pResource, pDesc, DestDescriptor);
	mOrig->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, Dx12OrigCpuHandle(DestDescriptor));
	if (Dx12MirrorCpuHandle(DestDescriptor, &mirror))
		mOrig->CreateUnorderedAccessView(pResource, pCounterResource, pDesc, mirror);
}

void STDMETHODCALLTYPE Dx12Device::CreateRenderTargetView(ID3D12Resource *pResource, const D3D12_RENDER_TARGET_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	Dx12ShadowRenderTargetView(pResource, pDesc, DestDescriptor);
	mOrig->CreateRenderTargetView(pResource, pDesc, Dx12OrigCpuHandle(DestDescriptor));
}

void STDMETHODCALLTYPE Dx12Device::CreateDepthStencilView(ID3D12Resource *pResource, const D3D12_DEPTH_STENCIL_VIEW_DESC *pDesc, D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor)
{
	Dx12ShadowDepthStencilView(pResource, pDesc, DestDescriptor);
	mOrig->CreateDepthStencilView(pResource, pDesc, Dx12OrigCpuHandle(DestDescriptor));
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommittedResource(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riidResource, void **ppvResource)
{
	HRESULT hr = mOrig->CreateCommittedResource(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, riidResource, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riidResource, ppvResource, InitialResourceState);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommittedResource1(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, REFIID riidResource, void **ppvResource)
{
	HRESULT hr = mOrig->CreateCommittedResource1(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riidResource, ppvResource, InitialResourceState);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommittedResource2(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialResourceState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, REFIID riidResource, void **ppvResource)
{
	HRESULT hr = mOrig->CreateCommittedResource2(pHeapProperties, HeapFlags, pDesc, InitialResourceState, pOptimizedClearValue, pProtectedSession, riidResource, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riidResource, ppvResource, InitialResourceState);
	return hr;
}

// Resources created with a layout are used with enhanced barriers, which the
// state tracking does not follow.
HRESULT STDMETHODCALLTYPE Dx12Device::CreateCommittedResource3(const D3D12_HEAP_PROPERTIES *pHeapProperties, D3D12_HEAP_FLAGS HeapFlags, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, UINT32 NumCastableFormats, const DXGI_FORMAT *pCastableFormats, REFIID riidResource, void **ppvResource)
{
	HRESULT hr = mOrig->CreateCommittedResource3(pHeapProperties, HeapFlags, pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riidResource, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riidResource, ppvResource, DX12_STATE_UNKNOWN);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreatePlacedResource(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource)
{
	HRESULT hr = mOrig->CreatePlacedResource(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riid, ppvResource, InitialState);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreatePlacedResource1(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource)
{
	HRESULT hr = mOrig->CreatePlacedResource1(pHeap, HeapOffset, pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riid, ppvResource, InitialState);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreatePlacedResource2(ID3D12Heap *pHeap, UINT64 HeapOffset, const D3D12_RESOURCE_DESC1 *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, UINT32 NumCastableFormats, const DXGI_FORMAT *pCastableFormats, REFIID riid, void **ppvResource)
{
	HRESULT hr = mOrig->CreatePlacedResource2(pHeap, HeapOffset, pDesc, InitialLayout, pOptimizedClearValue, NumCastableFormats, pCastableFormats, riid, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riid, ppvResource, DX12_STATE_UNKNOWN);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateReservedResource(const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, REFIID riid, void **ppvResource)
{
	HRESULT hr = mOrig->CreateReservedResource(pDesc, InitialState, pOptimizedClearValue, riid, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riid, ppvResource, InitialState);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateReservedResource1(const D3D12_RESOURCE_DESC *pDesc, D3D12_RESOURCE_STATES InitialState, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, REFIID riid, void **ppvResource)
{
	HRESULT hr = mOrig->CreateReservedResource1(pDesc, InitialState, pOptimizedClearValue, pProtectedSession, riid, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riid, ppvResource, InitialState);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateReservedResource2(const D3D12_RESOURCE_DESC *pDesc, D3D12_BARRIER_LAYOUT InitialLayout, const D3D12_CLEAR_VALUE *pOptimizedClearValue, ID3D12ProtectedResourceSession *pProtectedSession, UINT32 NumCastableFormats, const DXGI_FORMAT *pCastableFormats, REFIID riid, void **ppvResource)
{
	HRESULT hr = mOrig->CreateReservedResource2(pDesc, InitialLayout, pOptimizedClearValue, pProtectedSession, NumCastableFormats, pCastableFormats, riid, ppvResource);

	if (SUCCEEDED(hr))
		Dx12TrackResource(riid, ppvResource, DX12_STATE_UNKNOWN);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateRootSignature(UINT nodeMask, const void *pBlobWithRootSignature, SIZE_T blobLengthInBytes, REFIID riid, void **ppvRootSignature)
{
	HRESULT hr = mOrig->CreateRootSignature(nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);

	if (SUCCEEDED(hr))
		Dx12RegisterRootSignature(ppvRootSignature, pBlobWithRootSignature, blobLengthInBytes);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState)
{
	HRESULT hr = mOrig->CreateGraphicsPipelineState(pDesc, riid, ppPipelineState);

	if (SUCCEEDED(hr))
		Dx12RegisterPipeline(ppPipelineState, pDesc);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState)
{
	HRESULT hr = mOrig->CreateComputePipelineState(pDesc, riid, ppPipelineState);

	if (SUCCEEDED(hr))
		Dx12RegisterPipeline(ppPipelineState, pDesc);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, REFIID riid, void **ppPipelineState)
{
	HRESULT hr = mOrig->CreatePipelineState(pDesc, riid, ppPipelineState);

	if (SUCCEEDED(hr))
		Dx12RegisterPipeline(ppPipelineState, pDesc);
	return hr;
}

// Pipelines loaded from a library never pass through the Create methods
HRESULT STDMETHODCALLTYPE Dx12Device::CreatePipelineLibrary(const void *pLibraryBlob, SIZE_T BlobLength, REFIID riid, void **ppPipelineLibrary)
{
	HRESULT hr = mOrig->CreatePipelineLibrary(pLibraryBlob, BlobLength, riid, ppPipelineLibrary);

	if (SUCCEEDED(hr) && ppPipelineLibrary && *ppPipelineLibrary) {
		void *orig = *ppPipelineLibrary;
		Dx12PipelineLibrary *proxy = wrap_child<Dx12PipelineLibrary, ID3D12PipelineLibrary>(this, riid, ppPipelineLibrary);
		Dx12LogInfo("Dx12PipelineLibrary %p created to wrap ID3D12PipelineLibrary %p (%Iu bytes)\n", proxy, orig, BlobLength);
	}

	return hr;
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
