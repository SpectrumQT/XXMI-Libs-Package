#include "Dx12DescriptorHeap.h"

#include <iterator>
#include <vector>

#include "DescriptorShadow.h"
#include "Dx12Device.h"

// {634844CE-5C5A-4D3A-8685-27FD24F26C9D}
const GUID Dx12DescriptorHeap::ProxyIID = { 0x634844ce, 0x5c5a, 0x4d3a, { 0x86, 0x85, 0x27, 0xfd, 0x24, 0xf2, 0x6c, 0x9d } };
void *Dx12DescriptorHeap::sVtbl;

#ifdef _WIN64

SIZE_T dx12_heap_cpu_base[1 << DX12_HEAP_INDEX_BITS];
SIZE_T dx12_heap_mirror_base[1 << DX12_HEAP_INDEX_BITS];
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
	dx12_heap_mirror_base[index] = 0;
	free_heap_indices.push_back(index);
	ReleaseSRWLockExclusive(&heap_index_lock);
}

#endif

HRESULT STDMETHODCALLTYPE Dx12DescriptorSpan::QueryInterface(REFIID riid, void **ppvObject)
{
	if (!ppvObject)
		return E_POINTER;
	if (!IsEqualIID(riid, __uuidof(IUnknown))) {
		*ppvObject = NULL;
		return E_NOINTERFACE;
	}
	AddRef();
	*ppvObject = this;
	return S_OK;
}

ULONG STDMETHODCALLTYPE Dx12DescriptorSpan::AddRef()
{
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12DescriptorSpan::Release()
{
	ULONG ref = InterlockedDecrement(&mRef);

	if (!ref)
		delete this;
	return ref;
}

Dx12DescriptorArena::Dx12DescriptorArena(UINT size) :
	mSize(size),
	mLive(0),
	mNext(0),
	mTooMany(UINT_MAX),
	mFullFrame(UINT_MAX)
{
	if (size)
		mFree[0] = size;
}

Dx12DescriptorArena::~Dx12DescriptorArena()
{
	for (Dx12DescriptorSpan *span : mSpans)
		span->Release();
}

void Dx12DescriptorArena::Reclaim()
{
	size_t kept = 0;

	for (Dx12DescriptorSpan *span : mSpans) {
		UINT start = span->mStart, count = span->mCount;

		// Only who has a reference makes another, and the one who had
		// ours for a list had the lock
		if (span->mRef != 1) {
			mSpans[kept++] = span;
			continue;
		}

		auto next = mFree.lower_bound(start);
		if (next != mFree.end() && next->first == start + count) {
			count += next->second;
			next = mFree.erase(next);
		}
		if (next != mFree.begin() && std::prev(next)->first + std::prev(next)->second == start)
			std::prev(next)->second += count;
		else
			mFree.emplace_hint(next, start, count);

		mLive -= span->mCount;
		mTooMany = UINT_MAX;
		span->Release();
	}
	mSpans.resize(kept);
}

bool Dx12DescriptorArena::Take(UINT count, UINT *start)
{
	UINT rest;
	int pass;

	if (count >= mTooMany)
		return false;

	auto from = mFree.lower_bound(mNext);
	for (pass = 0; pass < 2; pass++) {
		auto i = pass ? mFree.begin() : from, end = pass ? from : mFree.end();

		for (; i != end; ++i) {
			if (i->second < count)
				continue;
			*start = i->first;
			rest = i->second - count;
			mFree.erase(i);
			if (rest)
				mFree[*start + count] = rest;
			mNext = *start + count;
			return true;
		}
	}
	mTooMany = count;
	return false;
}

bool Dx12DescriptorArena::Alloc(UINT count, UINT *start, Dx12DescriptorSpan **span)
{
	if (!count || count > mSize)
		return false;

	if (!Take(count, start)) {
		if (mFullFrame == dx12_frame)
			return false;
		Reclaim();
		if (!Take(count, start)) {
			mFullFrame = dx12_frame;
			return false;
		}
	}

	*span = new Dx12DescriptorSpan(*start, count);
	mSpans.push_back(*span);
	mLive += count;
	return true;
}

Dx12DescriptorHeap::Dx12DescriptorHeap(Dx12Device *device, ID3D12DescriptorHeap *orig, UINT reserve) :
	mOrig(orig),
	mDevice(device),
	mRef(1),
	mIndex(-1),
	mGpuStart(0),
	mSlots(NULL),
	mReserve(reserve),
	mMirror(NULL),
	mArena(reserve)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc = orig->GetDesc();

	mOrig->AddRef();
	mDevice->AddRef();
	sVtbl = *(void**)this;
	mEncodedStart.ptr = 0;
	mType = desc.Type;
	mCount = desc.NumDescriptors - mReserve;
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

	// Without a reserve the copy is made in a heap of ours, and from the
	// mirror all the same
	if (mGpuStart && mType == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
		desc.NumDescriptors = mCount;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		if (SUCCEEDED(mDevice->mOrig->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&mMirror))))
			dx12_heap_mirror_base[mIndex] = mMirror->GetCPUDescriptorHandleForHeapStart().ptr;
		else
			mReserve = 0;
	}
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
	if (mMirror)
		mMirror->Release();
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

D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE Dx12DescriptorHeap::GetDesc()
{
	D3D12_DESCRIPTOR_HEAP_DESC desc = mOrig->GetDesc();

	desc.NumDescriptors = mCount;
	return desc;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12DescriptorHeap::MirrorCpuHandle(UINT64 gpu)
{
	D3D12_CPU_DESCRIPTOR_HANDLE handle = mMirror->GetCPUDescriptorHandleForHeapStart();

	handle.ptr += (SIZE_T)(gpu - mGpuStart);
	return handle;
}

bool Dx12DescriptorHeap::AllocReserved(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu,
		Dx12DescriptorSpan **span)
{
	UINT start;

	if (!mReserve || !mGpuStart || !mArena.Alloc(count, &start, span))
		return false;

	*cpu = mOrig->GetCPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)(mCount + start) * mIncrement;
	gpu->ptr = mGpuStart + (UINT64)(mCount + start) * mIncrement;
	return true;
}

#include "generated/Dx12DescriptorHeap.fwd.inl"
