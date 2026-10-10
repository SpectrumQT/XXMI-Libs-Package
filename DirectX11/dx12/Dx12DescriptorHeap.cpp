#include "Dx12DescriptorHeap.h"

#include <vector>

#include "DescriptorShadow.h"
#include "Dx12Device.h"

// {634844CE-5C5A-4D3A-8685-27FD24F26C9D}
const GUID Dx12DescriptorHeap::ProxyIID = { 0x634844ce, 0x5c5a, 0x4d3a, { 0x86, 0x85, 0x27, 0xfd, 0x24, 0xf2, 0x6c, 0x9d } };
void *Dx12DescriptorHeap::sVtbl;

#ifdef _WIN64

SIZE_T dx12_heap_cpu_base[1 << DX12_HEAP_INDEX_BITS];
Dx12DescriptorHeap *dx12_heap_proxies[1 << DX12_HEAP_INDEX_BITS];

static SRWLOCK heap_index_lock = SRWLOCK_INIT;
static std::vector<int> free_heap_indices;
static int next_heap_index;

static int alloc_heap_index(SIZE_T cpu_base)
{
	int index = -1;

	AcquireSRWLockExclusive(&heap_index_lock);
	if (!free_heap_indices.empty()) {
		index = free_heap_indices.back();
		free_heap_indices.pop_back();
	} else if (next_heap_index < (int)ARRAYSIZE(dx12_heap_cpu_base)) {
		index = next_heap_index++;
	}
	if (index >= 0)
		dx12_heap_cpu_base[index] = cpu_base;
	ReleaseSRWLockExclusive(&heap_index_lock);

	return index;
}

static void free_heap_index(int index)
{
	AcquireSRWLockExclusive(&heap_index_lock);
	dx12_heap_cpu_base[index] = 0;
	free_heap_indices.push_back(index);
	ReleaseSRWLockExclusive(&heap_index_lock);
}

#endif

Dx12DescriptorHeap::Dx12DescriptorHeap(Dx12Device *device, ID3D12DescriptorHeap *orig) :
	mOrig(orig),
	mDevice(device),
	mRef(1),
	mIndex(-1),
	mGpuStart(0),
	mSlots(NULL)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc = orig->GetDesc();

	mOrig->AddRef();
	mDevice->AddRef();
	sVtbl = *(void**)this;
	mEncodedStart.ptr = 0;
	mType = desc.Type;
	mCount = desc.NumDescriptors;
	mIncrement = mDevice->mOrig->GetDescriptorHandleIncrementSize(desc.Type);
	if (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
		mGpuStart = mOrig->GetGPUDescriptorHandleForHeapStart().ptr;

#ifdef _WIN64
	UINT64 size = (UINT64)mCount * mIncrement;

	// The largest shader visible heap D3D12 allows is well below this, but
	// there is no limit on the size of the others.
	if (size >= (1ull << DX12_HEAP_OFFSET_BITS)) {
		Dx12LogInfo("*** Not wrapping ID3D12DescriptorHeap %p, %llu bytes of descriptors do not fit our handles\n", mOrig, size);
		return;
	}

	mIndex = alloc_heap_index(mOrig->GetCPUDescriptorHandleForHeapStart().ptr);
	if (mIndex < 0) {
		Dx12LogInfo("*** Not wrapping ID3D12DescriptorHeap %p, out of heap indices\n", mOrig);
		return;
	}

	mEncodedStart.ptr = (DX12_HEAP_HANDLE_TAG << DX12_HEAP_TAG_SHIFT) | ((SIZE_T)mIndex << DX12_HEAP_OFFSET_BITS);

	if (mType != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
		mSlots = (Dx12DescriptorSlot*)calloc(mCount, sizeof(Dx12DescriptorSlot));
	dx12_heap_proxies[mIndex] = this;
#endif
}

Dx12DescriptorHeap::~Dx12DescriptorHeap()
{
#ifdef _WIN64
	if (mIndex >= 0) {
		dx12_heap_proxies[mIndex] = NULL;
		free_heap_index(mIndex);
	}
#endif
	free(mSlots);
}

bool Dx12DescriptorHeap::Usable(REFIID riid)
{
	if (mIndex < 0)
		return false;

	return IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(ID3D12Object))
	    || IsEqualIID(riid, __uuidof(ID3D12DeviceChild))
	    || IsEqualIID(riid, __uuidof(ID3D12Pageable))
	    || IsEqualIID(riid, __uuidof(ID3D12DescriptorHeap));
}

void Dx12DescriptorHeap::Abandon()
{
	mOrig->Release();
	mDevice->Release();
	delete this;
}

HRESULT STDMETHODCALLTYPE Dx12DescriptorHeap::QueryInterface(REFIID riid, void **ppvObject)
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
	Dx12LogForwardedQuery("Dx12DescriptorHeap", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12DescriptorHeap::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12DescriptorHeap::Release()
{
	// Read first: once our reference is gone, so may be this object.
	ID3D12DescriptorHeap *orig = mOrig;
	Dx12Device *device = mDevice;
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref) {
		orig->Release();
		return ref;
	}

	delete this;
	orig->Release();
	device->Release();
	return 0;
}

HRESULT STDMETHODCALLTYPE Dx12DescriptorHeap::GetDevice(REFIID riid, void **ppvDevice)
{
	return mDevice->QueryInterface(riid, ppvDevice);
}

D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE Dx12DescriptorHeap::GetCPUDescriptorHandleForHeapStart()
{
	return mEncodedStart;
}

#include "generated/Dx12DescriptorHeap.fwd.inl"
