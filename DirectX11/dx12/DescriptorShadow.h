#pragma once

#include "Dx12DescriptorHeap.h"

// D3D12 cannot be asked what a descriptor refers to, so every heap proxy keeps
// a record per descriptor of the view that was created in it or copied to it.
// With the heap and slot encoded in the CPU handles the game uses, keeping the
// records up to date takes no searching.
//
// Records are written by whichever thread the game writes the descriptor on
// and read without a lock, like the descriptors themselves: the game already
// has to keep a slot still while a draw uses it.

enum Dx12ViewKind : UINT8 {
	DX12_VIEW_NONE,
	DX12_VIEW_CBV,
	DX12_VIEW_SRV,
	DX12_VIEW_UAV,
	DX12_VIEW_RTV,
	DX12_VIEW_DSV,
};

struct Dx12DescriptorSlot {
	// A key for ResourceTracker.h, the resource may be gone. NULL for a
	// CBV, which is created from an address.
	ID3D12Resource *resource;
	// CBV: GPU virtual address. Buffer SRV and UAV: first element.
	UINT64 offset;
	// CBV: bytes. Buffer SRV and UAV: number of elements.
	UINT32 size;
	// DXGI_FORMAT of the view, 0 if it was created without a description
	UINT16 format;
	UINT8 kind;
	// D3D12_SRV_DIMENSION, D3D12_UAV_DIMENSION, ... depending on kind
	UINT8 dimension;
};

#ifdef _WIN64

// Returns the records of count descriptors starting at a handle the game got
// from a heap proxy, or NULL.
static inline Dx12DescriptorSlot* Dx12SlotsFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT count)
{
	Dx12DescriptorHeap *heap;
	UINT64 slot;

	if ((handle.ptr >> DX12_HEAP_TAG_SHIFT) != DX12_HEAP_HANDLE_TAG)
		return NULL;

	heap = dx12_heap_proxies[(handle.ptr >> DX12_HEAP_OFFSET_BITS) & ((1 << DX12_HEAP_INDEX_BITS) - 1)];
	if (!heap || !heap->mSlots)
		return NULL;

	slot = (handle.ptr & (((SIZE_T)1 << DX12_HEAP_OFFSET_BITS) - 1)) / heap->mIncrement;
	if (slot + count > heap->mCount)
		return NULL;

	return &heap->mSlots[slot];
}

// The handle a number of descriptors after one the game got from a heap proxy
static inline D3D12_CPU_DESCRIPTOR_HANDLE Dx12AdvanceCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT descriptors)
{
	Dx12DescriptorHeap *heap;

	if ((handle.ptr >> DX12_HEAP_TAG_SHIFT) != DX12_HEAP_HANDLE_TAG)
		return handle;

	heap = dx12_heap_proxies[(handle.ptr >> DX12_HEAP_OFFSET_BITS) & ((1 << DX12_HEAP_INDEX_BITS) - 1)];
	if (heap)
		handle.ptr += (SIZE_T)descriptors * heap->mIncrement;
	return handle;
}

#else

static inline Dx12DescriptorSlot* Dx12SlotsFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT count)
{
	return NULL;
}

static inline D3D12_CPU_DESCRIPTOR_HANDLE Dx12AdvanceCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT descriptors)
{
	return handle;
}

#endif

// Returns the record of the descriptor index slots after a GPU handle into a
// shader visible heap, or NULL.
static inline Dx12DescriptorSlot* Dx12SlotFromGpuHandle(Dx12DescriptorHeap *heap, UINT64 handle, UINT index)
{
	UINT64 slot;

	if (!heap || !heap->mSlots || !heap->mGpuStart || handle < heap->mGpuStart)
		return NULL;

	slot = (handle - heap->mGpuStart) / heap->mIncrement + index;
	if (slot >= heap->mCount)
		return NULL;

	return &heap->mSlots[slot];
}

void Dx12ShadowConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);
void Dx12ShadowShaderResourceView(ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);
void Dx12ShadowUnorderedAccessView(ID3D12Resource *resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);
void Dx12ShadowRenderTargetView(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);
void Dx12ShadowDepthStencilView(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);

// The handles are the ones the game passed, before Dx12OrigCpuHandle
void Dx12ShadowCopyDescriptors(UINT dest_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dest_ranges, const UINT *dest_sizes,
		UINT src_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_ranges, const UINT *src_sizes);
void Dx12ShadowCopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src);
