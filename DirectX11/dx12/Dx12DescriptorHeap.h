#pragma once

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

static inline D3D12_CPU_DESCRIPTOR_HANDLE Dx12OrigCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	if ((handle.ptr >> DX12_HEAP_TAG_SHIFT) != DX12_HEAP_HANDLE_TAG)
		return handle;

	handle.ptr = dx12_heap_cpu_base[(handle.ptr >> DX12_HEAP_OFFSET_BITS) & ((1 << DX12_HEAP_INDEX_BITS) - 1)]
		+ (handle.ptr & (((SIZE_T)1 << DX12_HEAP_OFFSET_BITS) - 1));
	return handle;
}

#else

// No room for the encoding in a 32 bit handle, heaps are not wrapped there.
static inline D3D12_CPU_DESCRIPTOR_HANDLE Dx12OrigCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	return handle;
}

#endif

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

	Dx12DescriptorHeap(Dx12Device *device, ID3D12DescriptorHeap *orig);
	~Dx12DescriptorHeap();

	bool Usable(REFIID riid);
	void Abandon();

#include "generated/Dx12DescriptorHeap.decl.inl"
};
