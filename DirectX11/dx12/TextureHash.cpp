#include "TextureHash.h"

#include "util.h"
#include "../globals.h"
#include "../ResourceHash.h"

#include "HashDump.h"

// For GetSurfaceInfo, which is what ResourceHash.cpp sizes a texture with
#include "DirectXTK/Src/pch.h"
#include "DirectXTK/Src/PlatformHelpers.h"
#include "DirectXTK/Src/LoaderHelpers.h"

Dx12TextureStats dx12_texture_stats;

const char *const dx12_format_rule_names[DX12_FORMAT_RULE_COUNT] = { "as is", "BC typeless", "Unreal typeless" };

static DXGI_FORMAT apply_format_rule(DXGI_FORMAT format, Dx12FormatRule rule)
{
	bool unreal = rule == DX12_FORMAT_UNREAL;

	if (rule == DX12_FORMAT_AS_IS)
		return format;

	switch (format) {
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
			return DXGI_FORMAT_BC1_TYPELESS;
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
			return DXGI_FORMAT_BC2_TYPELESS;
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
			return DXGI_FORMAT_BC3_TYPELESS;
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return DXGI_FORMAT_BC7_TYPELESS;
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
			return unreal ? format : DXGI_FORMAT_BC4_TYPELESS;
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
			return unreal ? format : DXGI_FORMAT_BC5_TYPELESS;
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
			return unreal ? format : DXGI_FORMAT_BC6H_TYPELESS;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return unreal ? DXGI_FORMAT_R8G8B8A8_TYPELESS : format;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return unreal ? DXGI_FORMAT_B8G8R8A8_TYPELESS : format;
	}

	return format;
}

uint32_t Dx12TextureHash(const Dx12ResourceInfo *info, Dx12FormatRule rule)
{
	D3D11_TEXTURE2D_DESC desc;

	desc.Width = (UINT)info->desc.Width;
	desc.Height = info->desc.Height;
	desc.MipLevels = info->desc.MipLevels;
	desc.ArraySize = info->desc.DepthOrArraySize;
	desc.Format = apply_format_rule(info->desc.Format, rule);
	desc.SampleDesc = info->desc.SampleDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = 0;
	if (!(info->desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
	if (info->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
		desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
	if (info->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
		desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
	if (info->desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = info->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

	return CalcTexture2DDescHash(info->has_data_hash ? info->data_hash : 0, &desc);
}

static bool is_mappable(ID3D12Resource *resource, UINT64 *size)
{
	D3D12_HEAP_PROPERTIES heap;
	Dx12ResourceInfo info;

	if (Dx12GetResourceInfo(resource, &info)) {
		*size = info.desc.Width;
		return info.cpu_accessible && info.heap_type != D3D12_HEAP_TYPE_READBACK;
	}

	// A buffer that was not created through the device proxy
	if (FAILED(resource->GetHeapProperties(&heap, NULL)))
		return false;
	*size = resource->GetDesc().Width;
	return heap.Type == D3D12_HEAP_TYPE_UPLOAD || heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD;
}

static void texture_uploaded(ID3D12Resource *texture, UINT subresource, UINT x, UINT y, UINT z,
		ID3D12Resource *buffer, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *footprint, const D3D12_BOX *box)
{
	size_t row_bytes = 0, rows = 0, limit, count, row;
	Dx12ResourceInfo info;
	UINT64 buffer_size;
	uint32_t hash = 0;
	BYTE *data = NULL;
	UINT width, height;

	if (!Dx12GetResourceInfo(texture, &info)) {
		InterlockedIncrement64(&dx12_texture_stats.untracked);
		return;
	}
	if (info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
		InterlockedIncrement64(&dx12_texture_stats.not_2d);
		return;
	}
	if (subresource) {
		InterlockedIncrement64(&dx12_texture_stats.other_subresource);
		return;
	}

	width = (UINT)info.desc.Width;
	height = info.desc.Height;
	if (x || y || z || footprint->Footprint.Width < width || footprint->Footprint.Height < height
	    || (box && (box->left || box->top || box->front || box->right < width || box->bottom < height))) {
		InterlockedIncrement64(&dx12_texture_stats.partial);
		return;
	}

	// Under D3D11 the first write is the initial data, which is always
	// hashed, and anything after it an update, which only is with
	// track_texture_updates.
	if (info.has_data_hash && G->track_texture_updates != 1) {
		InterlockedIncrement64(&dx12_texture_stats.update_ignored);
		return;
	}

	DirectX::LoaderHelpers::GetSurfaceInfo(width, height, info.desc.Format, NULL, &row_bytes, &rows);
	if (!row_bytes || !rows) {
		InterlockedIncrement64(&dx12_texture_stats.unknown_format);
		return;
	}
	if (row_bytes > footprint->Footprint.RowPitch)
		row_bytes = footprint->Footprint.RowPitch;

	if (!is_mappable(buffer, &buffer_size)) {
		InterlockedIncrement64(&dx12_texture_stats.not_mappable);
		return;
	}
	if (footprint->Offset > buffer_size
	    || (UINT64)footprint->Footprint.RowPitch * (rows - 1) + row_bytes > buffer_size - footprint->Offset) {
		InterlockedIncrement64(&dx12_texture_stats.out_of_bounds);
		return;
	}

	if (FAILED(buffer->Map(0, NULL, (void**)&data)) || !data) {
		InterlockedIncrement64(&dx12_texture_stats.not_mappable);
		return;
	}

	// texture_hash = 1 covers the whole top mip without row padding. The
	// default hash stops after as many bytes as the mip has texels, see
	// CalcTexture2DDataHash for the history of that.
	limit = G->texture_hash_version ? SIZE_MAX : (size_t)width * height * info.desc.DepthOrArraySize;
	data += footprint->Offset;
	for (row = 0; row < rows && limit; row++, data += footprint->Footprint.RowPitch) {
		count = row_bytes < limit ? row_bytes : limit;
		hash = Dx12Crc32cWriteCombined(hash, data, count);
		limit -= count;
		InterlockedAdd64(&dx12_texture_stats.hashed_bytes, count);
	}

	buffer->Unmap(0, NULL);

	InterlockedIncrement64(&dx12_texture_stats.hashed);
	Dx12SetTextureDataHash(texture, true, hash);
	Dx12DumpTexture(texture, "upload");
}

// What HackerContext does with track_texture_updates for CopyResource and
// for a CopySubresourceRegion that replaces the top mip
static void propagate_hash(ID3D12Resource *dst, ID3D12Resource *src)
{
	Dx12ResourceInfo dst_info, src_info;

	if (!Dx12GetResourceInfo(dst, &dst_info) || dst_info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
		return;
	if (!Dx12GetResourceInfo(src, &src_info) || src_info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
		return;
	if (src_info.has_data_hash == dst_info.has_data_hash && src_info.data_hash == dst_info.data_hash)
		return;

	InterlockedIncrement64(&dx12_texture_stats.copy_propagated);
	Dx12SetTextureDataHash(dst, src_info.has_data_hash, src_info.data_hash);
	Dx12DumpTexture(dst, "copy");
}

void Dx12CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
		const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box)
{
	if (!dst || !src || !dst->pResource || !src->pResource || dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX)
		return;

	if (src->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
		texture_uploaded(dst->pResource, dst->SubresourceIndex, x, y, z, src->pResource, &src->PlacedFootprint, box);
		return;
	}

	if (G->track_texture_updates != 1)
		return;
	if (dst->SubresourceIndex || x || y || z || box) {
		InterlockedIncrement64(&dx12_texture_stats.copy_partial);
		return;
	}

	propagate_hash(dst->pResource, src->pResource);
}

void Dx12CopyResource(ID3D12Resource *dst, ID3D12Resource *src)
{
	if (G->track_texture_updates != 1 || !dst || !src)
		return;

	propagate_hash(dst, src);
}
