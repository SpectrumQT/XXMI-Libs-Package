#include "DescriptorShadow.h"

#include "ResourceTracker.h"

static void write_slot(D3D12_CPU_DESCRIPTOR_HANDLE dest, UINT8 kind, ID3D12Resource *resource,
		DXGI_FORMAT format, UINT dimension, UINT64 offset, UINT size)
{
	Dx12DescriptorSlot *slot = Dx12SlotsFromCpuHandle(dest, 1);

	if (!slot)
		return;

	slot->resource = resource;
	slot->offset = offset;
	slot->size = size;
	slot->format = (UINT16)format;
	slot->kind = kind;
	slot->dimension = (UINT8)dimension;
}

void Dx12ShadowConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	if (desc)
		write_slot(dest, DX12_VIEW_CBV, NULL, DXGI_FORMAT_UNKNOWN, 0, desc->BufferLocation, desc->SizeInBytes);
	else
		write_slot(dest, DX12_VIEW_CBV, NULL, DXGI_FORMAT_UNKNOWN, 0, 0, 0);
}

void Dx12ShadowShaderResourceView(ID3D12Resource *resource, const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	UINT64 offset = 0;
	UINT size = 0;

	if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
		offset = desc->Buffer.FirstElement;
		size = desc->Buffer.NumElements;
	}

	if (resource && desc && (desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE
	    || desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY))
		Dx12MarkCubeTexture(resource);

	write_slot(dest, DX12_VIEW_SRV, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
		desc ? desc->ViewDimension : 0, offset, size);
}

void Dx12ShadowUnorderedAccessView(ID3D12Resource *resource, const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	UINT64 offset = 0;
	UINT size = 0;

	if (desc && desc->ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
		offset = desc->Buffer.FirstElement;
		size = desc->Buffer.NumElements;
	}

	write_slot(dest, DX12_VIEW_UAV, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
		desc ? desc->ViewDimension : 0, offset, size);
}

void Dx12ShadowRenderTargetView(ID3D12Resource *resource, const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	write_slot(dest, DX12_VIEW_RTV, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
		desc ? desc->ViewDimension : 0, 0, 0);
}

void Dx12ShadowDepthStencilView(ID3D12Resource *resource, const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE dest)
{
	write_slot(dest, DX12_VIEW_DSV, resource, desc ? desc->Format : DXGI_FORMAT_UNKNOWN,
		desc ? desc->ViewDimension : 0, 0, 0);
}

static void copy_slots(D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src, UINT count)
{
	Dx12DescriptorSlot *dest_slots = Dx12SlotsFromCpuHandle(dest, count);
	Dx12DescriptorSlot *src_slots;

	if (!dest_slots)
		return;

	// Copied from a heap we know nothing about
	src_slots = Dx12SlotsFromCpuHandle(src, count);
	if (!src_slots) {
		memset(dest_slots, 0, count * sizeof(Dx12DescriptorSlot));
		return;
	}

	memmove(dest_slots, src_slots, count * sizeof(Dx12DescriptorSlot));
}

void Dx12ShadowCopyDescriptorsSimple(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest, D3D12_CPU_DESCRIPTOR_HANDLE src)
{
	copy_slots(dest, src, count);
}

#ifdef _WIN64

// The source ranges taken together are copied to the destination ranges taken
// together, the ranges of the two sides need not line up.
void Dx12ShadowCopyDescriptors(UINT dest_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dest_ranges, const UINT *dest_sizes,
		UINT src_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_ranges, const UINT *src_sizes)
{
	UINT dest_range = 0, src_range = 0, dest_done = 0, src_done = 0;
	UINT dest_size, src_size, count;

	while (dest_range < dest_range_count && src_range < src_range_count) {
		dest_size = dest_sizes ? dest_sizes[dest_range] : 1;
		src_size = src_sizes ? src_sizes[src_range] : 1;

		count = dest_size - dest_done;
		if (count > src_size - src_done)
			count = src_size - src_done;

		if (count) {
			copy_slots(Dx12AdvanceCpuHandle(dest_ranges[dest_range], dest_done),
				Dx12AdvanceCpuHandle(src_ranges[src_range], src_done), count);
		}

		dest_done += count;
		src_done += count;
		if (dest_done >= dest_size) {
			dest_range++;
			dest_done = 0;
		}
		if (src_done >= src_size) {
			src_range++;
			src_done = 0;
		}
	}
}

#else

void Dx12ShadowCopyDescriptors(UINT dest_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *dest_ranges, const UINT *dest_sizes,
		UINT src_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE *src_ranges, const UINT *src_sizes)
{
}

#endif
