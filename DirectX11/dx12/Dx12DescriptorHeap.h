#pragma once

#include <map>
#include <vector>

#include "Dx12Common.h"

class Dx12Device;

// To know which heap and slot a descriptor handle refers to without searching
// address ranges, a heap proxy does not give the game the heap's real CPU
// handles (ReShade's approach). The handles it returns are
//
//   bits 56-63  DX12_HEAP_HANDLE_TAG
//   bits 28-55  index of the heap in dx12_heap_cpu_base
//   bits  0-27  byte offset of the descriptor in the heap
//
// which still work with the pointer arithmetic applications do on handles.
// Every device and command list method taking a CPU handle translates it back
// with Dx12OrigCpuHandle. The tag occupies bits no user mode address has, so
// real handles, from a heap we did not wrap or from someone who got at the
// original device, pass through unchanged.
//
// The reverse does not hold: an encoded handle must not reach an original
// device or command list without going through a proxy.
#ifdef _WIN64

const unsigned DX12_HEAP_OFFSET_BITS = 28;
const unsigned DX12_HEAP_INDEX_BITS = 16;
const unsigned DX12_HEAP_TAG_SHIFT = 56;
const SIZE_T DX12_HEAP_HANDLE_TAG = 0xd1;

extern SIZE_T dx12_heap_cpu_base[1 << DX12_HEAP_INDEX_BITS];
// Where the heap's mMirror starts, or 0
extern SIZE_T dx12_heap_mirror_base[1 << DX12_HEAP_INDEX_BITS];
extern class Dx12DescriptorHeap *dx12_heap_proxies[1 << DX12_HEAP_INDEX_BITS];

static inline D3D12_CPU_DESCRIPTOR_HANDLE Dx12OrigCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	if ((handle.ptr >> DX12_HEAP_TAG_SHIFT) != DX12_HEAP_HANDLE_TAG)
		return handle;

	handle.ptr = dx12_heap_cpu_base[(handle.ptr >> DX12_HEAP_OFFSET_BITS) & ((1 << DX12_HEAP_INDEX_BITS) - 1)]
		+ (handle.ptr & (((SIZE_T)1 << DX12_HEAP_OFFSET_BITS) - 1));
	return handle;
}

// Whether what is written to the descriptor also has to be written to a
// mirror, and where
static inline bool Dx12MirrorCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CPU_DESCRIPTOR_HANDLE *mirror)
{
	SIZE_T base;

	if ((handle.ptr >> DX12_HEAP_TAG_SHIFT) != DX12_HEAP_HANDLE_TAG)
		return false;

	base = dx12_heap_mirror_base[(handle.ptr >> DX12_HEAP_OFFSET_BITS) & ((1 << DX12_HEAP_INDEX_BITS) - 1)];
	if (!base)
		return false;

	mirror->ptr = base + (handle.ptr & (((SIZE_T)1 << DX12_HEAP_OFFSET_BITS) - 1));
	return true;
}

#else

static inline bool Dx12MirrorCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, D3D12_CPU_DESCRIPTOR_HANDLE *mirror)
{
	return false;
}

// No room for the encoding in a 32 bit handle, heaps are not wrapped there.
static inline D3D12_CPU_DESCRIPTOR_HANDLE Dx12OrigCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	return handle;
}

#endif

struct Dx12DescriptorSlot;

// How many descriptors to add to the game's shader visible view heaps
const UINT DX12_HEAP_RESERVE = 1 << 17;

// What a Dx12DescriptorArena handed out at once. The lists that use the
// descriptors have it pinned (Dx12ShimPin), and the arena hands them out
// again once nothing but the arena refers to it: then the lists have been
// executed and the GPU is done with them.
class Dx12DescriptorSpan final : public IUnknown
{
public:
	volatile LONG mRef;
	UINT mStart, mCount;

	Dx12DescriptorSpan(UINT start, UINT count) : mRef(1), mStart(start), mCount(count) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;
};

// Descriptors for the game's lists, handed out in spans and taken back in
// whatever order the lists using them are done with them: a list the game
// keeps to execute again, or leaves in a pool, may hold on to its spans long
// after lists recorded later. All with the lock of Bridge.h held.
class Dx12DescriptorArena
{
public:
	UINT mSize;
	// Descriptors of the spans not taken back yet
	UINT mLive;
	std::vector<Dx12DescriptorSpan*> mSpans;
	// What is not handed out: the number of descriptors by the first
	std::map<UINT, UINT> mFree;
	// Where the span handed out last ends, from where the next is looked
	// for: what is free before it was too small for one before
	UINT mNext;
	// The smallest number of descriptors found not to be had in one piece,
	// which stays so until something is taken back
	UINT mTooMany;
	// The frame in which nothing asked for could be had even after taking
	// back, where going through the spans again for every draw is no use
	UINT mFullFrame;

	Dx12DescriptorArena(UINT size = 0);
	~Dx12DescriptorArena();

	// *start is the index of the first of count descriptors. *span is not
	// referenced: pin it to the list before anything else, or the arena
	// takes it back on a later call.
	bool Alloc(UINT count, UINT *start, Dx12DescriptorSpan **span);
	// Takes back the spans nothing but the arena refers to. Not before
	// there is no room left, which keeps it from being done for every span.
	void Reclaim();

private:
	bool Take(UINT count, UINT *start);
};

class Dx12DescriptorHeap final : public ID3D12DescriptorHeap
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	ID3D12DescriptorHeap *mOrig;
	Dx12Device *mDevice;
	volatile LONG mRef;

	// Index in dx12_heap_cpu_base, or -1 if the heap could not be given
	// encoded handles and so cannot be wrapped
	int mIndex;
	D3D12_CPU_DESCRIPTOR_HANDLE mEncodedStart;

	D3D12_DESCRIPTOR_HEAP_TYPE mType;
	UINT mCount;
	UINT mIncrement;
	// 0 unless the heap is shader visible
	UINT64 mGpuStart;
	// What each descriptor of the heap refers to, see DescriptorShadow.h.
	// NULL for sampler heaps.
	Dx12DescriptorSlot *mSlots;

	// Descriptors after the mCount the game asked for, that it does not
	// know about. A mod's texture gets to a draw of the game through a
	// copy of the table the game had set, with the mod's view in place of
	// the game's, and that copy has to be in the same heap: switching
	// heaps would cost the game all its other tables.
	//
	// Handed out by a Dx12DescriptorArena: what is in it stays until the
	// lists using it have been executed, and when that leaves none there
	// is no copy.
	//
	// Descriptors cannot be read back from a shader visible heap, so what
	// such a copy is made from is mMirror: a heap that is not shader
	// visible, to which the device proxy writes everything the game
	// writes to this one. NULL without a reserve.
	UINT mReserve;
	ID3D12DescriptorHeap *mMirror;
	Dx12DescriptorArena mArena;
	// With the lock of Bridge.h held. *span as for Dx12DescriptorArena::Alloc.
	bool AllocReserved(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu,
			Dx12DescriptorSpan **span);
	// The descriptor of mMirror for the one a GPU handle of this heap
	// refers to
	D3D12_CPU_DESCRIPTOR_HANDLE MirrorCpuHandle(UINT64 gpu);

	// reserve is how many of the descriptors orig has are not the game's
	Dx12DescriptorHeap(Dx12Device *device, ID3D12DescriptorHeap *orig, UINT reserve = 0);
	~Dx12DescriptorHeap();

	bool Usable(REFIID riid);
	void Abandon();

#include "generated/Dx12DescriptorHeap.decl.inl"
};
