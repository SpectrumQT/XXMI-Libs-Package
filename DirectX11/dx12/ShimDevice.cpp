#include "ShimDevice.h"

#include <d3dcompiler.h>

#include "Dxc.h"
#include "PipelineRegistry.h"
#include "ShimContext.h"
#include "ShimPipeline.h"
#include "util.h"

ShimDevice::ShimDevice() :
	mRef(1)
{
}

HRESULT STDMETHODCALLTYPE ShimDevice::QueryInterface(REFIID riid, void **ppvObject)
{
	static const IID *const iids[] = { &__uuidof(IUnknown), &__uuidof(ID3D11Device), &__uuidof(ID3D11Device1) };
	HRESULT hr = ShimQueryInterface(this, iids, ARRAYSIZE(iids), riid, ppvObject);

	if (FAILED(hr))
		Dx12LogForwardedQuery("ShimDevice", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE ShimDevice::AddRef()
{
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE ShimDevice::Release()
{
	return InterlockedDecrement(&mRef);
}

HRESULT STDMETHODCALLTYPE ShimDevice::GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData)
{
	return mPrivate.Get(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE ShimDevice::SetPrivateData(REFGUID guid, UINT DataSize, const void *pData)
{
	return mPrivate.Set(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE ShimDevice::SetPrivateDataInterface(REFGUID guid, const IUnknown *pData)
{
	return mPrivate.SetInterface(guid, pData);
}

D3D_FEATURE_LEVEL STDMETHODCALLTYPE ShimDevice::GetFeatureLevel()
{
	return D3D_FEATURE_LEVEL_11_1;
}

UINT STDMETHODCALLTYPE ShimDevice::GetCreationFlags()
{
	return 0;
}

HRESULT STDMETHODCALLTYPE ShimDevice::GetDeviceRemovedReason()
{
	return dx12_shim->device->GetDeviceRemovedReason();
}

void STDMETHODCALLTYPE ShimDevice::GetImmediateContext(ID3D11DeviceContext **ppImmediateContext)
{
	dx12_shim->shim_context->AddRef();
	*ppImmediateContext = dx12_shim->shim_context;
}

void STDMETHODCALLTYPE ShimDevice::GetImmediateContext1(ID3D11DeviceContext1 **ppImmediateContext)
{
	dx12_shim->shim_context->AddRef();
	*ppImmediateContext = dx12_shim->shim_context;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CheckFormatSupport(DXGI_FORMAT Format, UINT *pFormatSupport)
{
	D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { Format };
	HRESULT hr = dx12_shim->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support));

	// D3D12_FORMAT_SUPPORT1 has the values of D3D11_FORMAT_SUPPORT
	if (pFormatSupport)
		*pFormatSupport = SUCCEEDED(hr) ? support.Support1 : 0;
	return hr;
}

// -----------------------------------------------------------------------------
// Resources

bool ShimWriteBuffer(ShimResource *resource, UINT64 offset, const void *data, UINT64 size)
{
	ID3D12Resource *upload_buffer;
	UINT64 upload_offset;
	BYTE *upload;

	if (!resource->resource || !size)
		return false;

	upload = Dx12ShimUpload(size, 16, &upload_buffer, &upload_offset, NULL);
	if (!upload)
		return false;
	memcpy(upload, data, size);

	Dx12ShimTransition(resource, D3D12_RESOURCE_STATE_COPY_DEST);
	Dx12ShimList()->CopyBufferRegion(resource->resource, offset, upload_buffer, upload_offset, size);
	return true;
}

bool ShimWriteSubresource(ShimResource *resource, UINT subresource, const void *data, UINT row_pitch, UINT slice_pitch)
{
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	UINT64 row_size, total, upload_offset;
	UINT rows, y, z;
	BYTE *upload;

	if (!resource->resource || !data)
		return false;

	dx12_shim->device->GetCopyableFootprints(&resource->desc, subresource, 1, 0, &layout, &rows, &row_size, &total);
	if (total == UINT64_MAX)
		return false;

	upload = Dx12ShimUpload(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &src.pResource, &upload_offset, NULL);
	if (!upload)
		return false;

	if (!row_pitch)
		row_pitch = (UINT)row_size;
	if (!slice_pitch)
		slice_pitch = row_pitch * rows;
	for (z = 0; z < layout.Footprint.Depth; z++) {
		for (y = 0; y < rows; y++) {
			memcpy(upload + ((SIZE_T)z * rows + y) * layout.Footprint.RowPitch,
				(const BYTE*)data + (SIZE_T)z * slice_pitch + (SIZE_T)y * row_pitch, (size_t)row_size);
		}
	}

	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.SubresourceIndex = subresource;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = layout;
	src.PlacedFootprint.Offset = upload_offset;

	// The transition may give the resource another D3D12 resource
	Dx12ShimTransition(resource, D3D12_RESOURCE_STATE_COPY_DEST);
	dst.pResource = resource->resource;
	Dx12ShimList()->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);
	return true;
}

void ShimFlushWrite(ShimResource *resource)
{
	// First, as writing goes through Dx12ShimTransition itself
	resource->write_pending = false;
	// The rows of a shadow are as far apart as those of the upload
	ShimWriteSubresource(resource, 0, resource->shadow.data(), 0, 0);
}

static D3D12_RESOURCE_FLAGS resource_flags(UINT bind_flags)
{
	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;

	if (bind_flags & D3D11_BIND_RENDER_TARGET)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	if (bind_flags & D3D11_BIND_DEPTH_STENCIL)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
	if (bind_flags & D3D11_BIND_UNORDERED_ACCESS)
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	return flags;
}

static HRESULT create_resource(ShimResource *resource, const D3D12_RESOURCE_DESC *desc)
{
	D3D12_HEAP_PROPERTIES heap = {};
	HRESULT hr;

	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	hr = dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, desc,
			D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&resource->resource));
	if (FAILED(hr)) {
		Dx12LogInfo("*** Failed to create a resource for a mod: dimension %d, %llux%ux%u, %u mips, format %d, flags 0x%x = 0x%x\n",
			desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize, desc->MipLevels,
			desc->Format, desc->Flags, hr);
		return hr;
	}

	resource->desc = resource->resource->GetDesc();
	resource->state = D3D12_RESOURCE_STATE_COMMON;
	return S_OK;
}

// A resource written once for each of the draws of a frame that use it goes
// through as many D3D12 resources a frame, which the lists of the last frames
// still hold, so a few more than that are kept before any is let go
const size_t SPARES_KEPT = 16;

bool ShimRenameResource(ShimResource *resource)
{
	static bool failed;
	ID3D12Resource *old = resource->resource, *fresh = NULL;
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_BARRIER barrier = {};
	size_t i;

	// One the lists are done with comes before a new one
	for (i = 0; i < resource->spares.size(); i++) {
		resource->spares[i]->AddRef();
		if (resource->spares[i]->Release() != 1)
			continue;
		fresh = resource->spares[i];
		resource->spares.erase(resource->spares.begin() + i);
		break;
	}
	if (!fresh) {
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &resource->desc,
				D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&fresh)))) {
			if (!failed)
				Dx12LogInfo("*** Failed to create a resource for a mod to write while the game's lists use it. The lists see the write.\n");
			failed = true;
			return false;
		}
	}

	// The fresh resource is in the common state, from where the copy
	// promotes it. The old one is left in the common state as well, as
	// the lists that use it expect.
	Dx12ShimTransition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Dx12ShimList()->CopyResource(fresh, old);
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = old;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = resource->state;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	Dx12ShimList()->ResourceBarrier(1, &barrier);

	resource->resource = fresh;
	resource->state = D3D12_RESOURCE_STATE_COPY_DEST;
	resource->spares.push_back(old);
	if (resource->spares.size() > SPARES_KEPT) {
		Dx12ShimDeferRelease(resource->spares.front());
		resource->spares.front()->Release();
		resource->spares.erase(resource->spares.begin());
	}
	for (ShimView *view : resource->views)
		ShimRecreateView(view);
	InterlockedIncrement64(&dx12_stats.resources_renamed);
	return true;
}

bool ShimBufferToGpu(ShimResource *resource)
{
	D3D12_RESOURCE_DESC desc = {};

	if (!resource->cpu_only)
		return true;

	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = resource->shadow.size();
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (!desc.Width || FAILED(create_resource(resource, &desc)))
		return false;

	resource->cpu_only = false;
	return ShimWriteBuffer(resource, 0, resource->shadow.data(), resource->shadow.size());
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateBuffer(const D3D11_BUFFER_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Buffer **ppBuffer)
{
	const UINT address_binds = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER;
	D3D12_RESOURCE_DESC desc = {};
	ShimBuffer *buffer;
	ShimResource *r;
	HRESULT hr;

	if (!pDesc || !pDesc->ByteWidth)
		return E_INVALIDARG;
	if (!ppBuffer)
		return S_FALSE;
	*ppBuffer = NULL;

	buffer = new ShimBuffer();
	buffer->mDesc = *pDesc;
	// D3D11 reports no stride for a buffer that is not structured, and the
	// engine compares descriptions to tell whether a copy needs a new buffer
	if (!(pDesc->MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))
		buffer->mDesc.StructureByteStride = 0;
	r = &buffer->r;
	r->dimension = D3D11_RESOURCE_DIMENSION_BUFFER;

	if (pDesc->Usage == D3D11_USAGE_STAGING)
		r->cpu_only = true;
	else if (pDesc->BindFlags && !(pDesc->BindFlags & ~address_binds))
		r->cpu_only = pDesc->Usage == D3D11_USAGE_DYNAMIC || (pDesc->BindFlags & D3D11_BIND_CONSTANT_BUFFER);

	if (!r->cpu_only) {
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = ShimAlign(pDesc->ByteWidth, 256);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = resource_flags(pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS);

		hr = create_resource(r, &desc);
		if (FAILED(hr)) {
			buffer->Release();
			return hr;
		}
	}

	if (r->cpu_only || pDesc->Usage == D3D11_USAGE_DYNAMIC) {
		// Padded like the resource would be, so a constant buffer
		// view of the whole of it stays within what is uploaded
		r->shadow.resize((size_t)ShimAlign(pDesc->ByteWidth, 256));
		if (pInitialData && pInitialData->pSysMem)
			memcpy(r->shadow.data(), pInitialData->pSysMem, pDesc->ByteWidth);
	}
	if (!r->cpu_only && pInitialData && pInitialData->pSysMem)
		ShimWriteBuffer(r, 0, pInitialData->pSysMem, pDesc->ByteWidth);

	*ppBuffer = buffer;
	return S_OK;
}

static HRESULT create_texture(ShimResource *r, D3D12_RESOURCE_DESC *desc, D3D11_USAGE usage, UINT bind_flags,
		UINT cpu_access, const D3D11_SUBRESOURCE_DATA *initial_data, UINT *mips)
{
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
	UINT64 row_size;
	UINT i, rows, subresources;
	HRESULT hr;

	desc->Flags = resource_flags(bind_flags);
	hr = create_resource(r, desc);
	if (FAILED(hr))
		return hr;
	// 0 asks for the full chain in both APIs
	*mips = r->desc.MipLevels;

	if (usage == D3D11_USAGE_DYNAMIC || (cpu_access & D3D11_CPU_ACCESS_WRITE)) {
		dx12_shim->device->GetCopyableFootprints(&r->desc, 0, 1, 0, &layout, &rows, &row_size, NULL);
		r->shadow.resize((size_t)(row_size * rows * layout.Footprint.Depth));
	}

	if (!initial_data)
		return S_OK;

	subresources = r->desc.MipLevels;
	if (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
		subresources *= r->desc.DepthOrArraySize;
	for (i = 0; i < subresources; i++) {
		if (!ShimWriteSubresource(r, i, initial_data[i].pSysMem, initial_data[i].SysMemPitch,
				initial_data[i].SysMemSlicePitch))
			continue;
		if (!i && !r->shadow.empty()) {
			dx12_shim->device->GetCopyableFootprints(&r->desc, 0, 1, 0, &layout, &rows, &row_size, NULL);
			for (UINT y = 0; y < rows * layout.Footprint.Depth; y++) {
				memcpy(r->shadow.data() + y * row_size, (const BYTE*)initial_data[0].pSysMem
					+ (SIZE_T)y * (initial_data[0].SysMemPitch ? initial_data[0].SysMemPitch : row_size),
					(size_t)row_size);
			}
		}
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture1D **ppTexture1D)
{
	D3D12_RESOURCE_DESC desc = {};
	ShimTexture1D *texture;
	HRESULT hr;

	if (!pDesc)
		return E_INVALIDARG;
	if (!ppTexture1D)
		return S_FALSE;
	*ppTexture1D = NULL;

	texture = new ShimTexture1D();
	texture->mDesc = *pDesc;
	texture->r.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;

	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
	desc.Width = pDesc->Width;
	desc.Height = 1;
	desc.DepthOrArraySize = (UINT16)pDesc->ArraySize;
	desc.MipLevels = (UINT16)pDesc->MipLevels;
	desc.Format = pDesc->Format;
	desc.SampleDesc.Count = 1;

	hr = create_texture(&texture->r, &desc, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags,
			pInitialData, &texture->mDesc.MipLevels);
	if (FAILED(hr)) {
		texture->Release();
		return hr;
	}

	*ppTexture1D = texture;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateTexture2D(const D3D11_TEXTURE2D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture2D **ppTexture2D)
{
	D3D12_RESOURCE_DESC desc = {};
	ShimTexture2D *texture;
	HRESULT hr;

	if (!pDesc)
		return E_INVALIDARG;
	if (!ppTexture2D)
		return S_FALSE;
	*ppTexture2D = NULL;

	texture = new ShimTexture2D();
	texture->mDesc = *pDesc;
	texture->r.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;

	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = pDesc->Width;
	desc.Height = pDesc->Height;
	desc.DepthOrArraySize = (UINT16)pDesc->ArraySize;
	desc.MipLevels = (UINT16)pDesc->MipLevels;
	desc.Format = pDesc->Format;
	desc.SampleDesc = pDesc->SampleDesc;

	hr = create_texture(&texture->r, &desc, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags,
			pInitialData, &texture->mDesc.MipLevels);
	if (FAILED(hr)) {
		texture->Release();
		return hr;
	}

	*ppTexture2D = texture;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateTexture3D(const D3D11_TEXTURE3D_DESC *pDesc, const D3D11_SUBRESOURCE_DATA *pInitialData, ID3D11Texture3D **ppTexture3D)
{
	D3D12_RESOURCE_DESC desc = {};
	ShimTexture3D *texture;
	HRESULT hr;

	if (!pDesc)
		return E_INVALIDARG;
	if (!ppTexture3D)
		return S_FALSE;
	*ppTexture3D = NULL;

	texture = new ShimTexture3D();
	texture->mDesc = *pDesc;
	texture->r.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;

	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	desc.Width = pDesc->Width;
	desc.Height = pDesc->Height;
	desc.DepthOrArraySize = (UINT16)pDesc->Depth;
	desc.MipLevels = (UINT16)pDesc->MipLevels;
	desc.Format = pDesc->Format;
	desc.SampleDesc.Count = 1;

	hr = create_texture(&texture->r, &desc, pDesc->Usage, pDesc->BindFlags, pDesc->CPUAccessFlags,
			pInitialData, &texture->mDesc.MipLevels);
	if (FAILED(hr)) {
		texture->Release();
		return hr;
	}

	*ppTexture3D = texture;
	return S_OK;
}

// -----------------------------------------------------------------------------
// Views
//
// The dimension enums of the view descriptions have the same values in both
// APIs, but the structs they select do not have the same members.

template <class View>
static HRESULT begin_view(ID3D11Resource *resource11, D3D12_DESCRIPTOR_HEAP_TYPE heap_type, DXGI_FORMAT format, View **out)
{
	ShimResource *resource = ShimResourceOf(resource11);
	View *view;

	if (!resource || !resource->resource) {
		Dx12LogDebug("No view can be made of %p: it has no D3D12 resource\n", resource11);
		return E_INVALIDARG;
	}

	view = new View();
	view->v.heap_type = heap_type;
	view->v.handle = Dx12ShimAllocDescriptor(heap_type);
	if (!view->v.handle.ptr) {
		view->Release();
		return E_OUTOFMEMORY;
	}
	resource11->AddRef();
	view->v.resource11 = resource11;
	view->v.resource = resource;
	view->v.format = format;
	resource->views.push_back(&view->v);

	*out = view;
	return S_OK;
}

static UINT buffer_elements(const ShimResourceDesc *resource)
{
	return resource->byte_width / (resource->stride ? resource->stride : 4);
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateShaderResourceView(ID3D11Resource *pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc, ID3D11ShaderResourceView **ppSRView)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC desc = {};
	D3D12_SHADER_RESOURCE_VIEW_DESC desc12 = {};
	ShimShaderResourceView *view;
	ShimResourceDesc resource;
	HRESULT hr;

	if (!pResource)
		return E_INVALIDARG;
	if (!ppSRView)
		return S_FALSE;
	*ppSRView = NULL;

	ShimGetResourceDesc(pResource, &resource);
	if (pDesc) {
		desc = *pDesc;
	} else {
		desc.Format = resource.format;
		switch (ShimResourceOf(pResource)->dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			desc.Buffer.NumElements = buffer_elements(&resource);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			desc.ViewDimension = resource.array_size > 1 ? D3D11_SRV_DIMENSION_TEXTURE1DARRAY : D3D11_SRV_DIMENSION_TEXTURE1D;
			desc.Texture1DArray.MipLevels = (UINT)-1;
			desc.Texture1DArray.ArraySize = resource.array_size;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			if (resource.samples > 1) {
				desc.ViewDimension = resource.array_size > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
				desc.Texture2DMSArray.ArraySize = resource.array_size;
			} else if (resource.misc_flags & D3D11_RESOURCE_MISC_TEXTURECUBE) {
				desc.ViewDimension = resource.array_size > 6 ? D3D11_SRV_DIMENSION_TEXTURECUBEARRAY : D3D11_SRV_DIMENSION_TEXTURECUBE;
				desc.TextureCubeArray.MipLevels = (UINT)-1;
				desc.TextureCubeArray.NumCubes = resource.array_size / 6;
			} else {
				desc.ViewDimension = resource.array_size > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
				desc.Texture2DArray.MipLevels = (UINT)-1;
				desc.Texture2DArray.ArraySize = resource.array_size;
			}
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
			desc.Texture3D.MipLevels = (UINT)-1;
			break;
		}
	}

	desc12.Format = desc.Format;
	desc12.ViewDimension = (D3D12_SRV_DIMENSION)desc.ViewDimension;
	desc12.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	switch (desc.ViewDimension) {
	case D3D11_SRV_DIMENSION_BUFFER:
	case D3D11_SRV_DIMENSION_BUFFEREX:
		// Both members of the union start with the same two fields
		desc12.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		desc12.Buffer.FirstElement = desc.Buffer.FirstElement;
		desc12.Buffer.NumElements = desc.Buffer.NumElements;
		if (desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX && (desc.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW))
			desc12.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		else if (desc.Format == DXGI_FORMAT_UNKNOWN)
			desc12.Buffer.StructureByteStride = resource.stride;
		// D3D11 refuses such a view. D3D12 removes the device for it, which
		// may be one that is not ours to lose.
		if (desc.Format == DXGI_FORMAT_UNKNOWN && !desc12.Buffer.StructureByteStride)
			return E_INVALIDARG;
		break;
	case D3D11_SRV_DIMENSION_TEXTURE1D:
		desc12.Texture1D.MostDetailedMip = desc.Texture1D.MostDetailedMip;
		desc12.Texture1D.MipLevels = desc.Texture1D.MipLevels;
		break;
	case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
		desc12.Texture1DArray.MostDetailedMip = desc.Texture1DArray.MostDetailedMip;
		desc12.Texture1DArray.MipLevels = desc.Texture1DArray.MipLevels;
		desc12.Texture1DArray.FirstArraySlice = desc.Texture1DArray.FirstArraySlice;
		desc12.Texture1DArray.ArraySize = desc.Texture1DArray.ArraySize;
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2D:
		desc12.Texture2D.MostDetailedMip = desc.Texture2D.MostDetailedMip;
		desc12.Texture2D.MipLevels = desc.Texture2D.MipLevels;
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
		desc12.Texture2DArray.MostDetailedMip = desc.Texture2DArray.MostDetailedMip;
		desc12.Texture2DArray.MipLevels = desc.Texture2DArray.MipLevels;
		desc12.Texture2DArray.FirstArraySlice = desc.Texture2DArray.FirstArraySlice;
		desc12.Texture2DArray.ArraySize = desc.Texture2DArray.ArraySize;
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2DMS:
		break;
	case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
		desc12.Texture2DMSArray.FirstArraySlice = desc.Texture2DMSArray.FirstArraySlice;
		desc12.Texture2DMSArray.ArraySize = desc.Texture2DMSArray.ArraySize;
		break;
	case D3D11_SRV_DIMENSION_TEXTURE3D:
		desc12.Texture3D.MostDetailedMip = desc.Texture3D.MostDetailedMip;
		desc12.Texture3D.MipLevels = desc.Texture3D.MipLevels;
		break;
	case D3D11_SRV_DIMENSION_TEXTURECUBE:
		desc12.TextureCube.MostDetailedMip = desc.TextureCube.MostDetailedMip;
		desc12.TextureCube.MipLevels = desc.TextureCube.MipLevels;
		break;
	case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
		desc12.TextureCubeArray.MostDetailedMip = desc.TextureCubeArray.MostDetailedMip;
		desc12.TextureCubeArray.MipLevels = desc.TextureCubeArray.MipLevels;
		desc12.TextureCubeArray.First2DArrayFace = desc.TextureCubeArray.First2DArrayFace;
		desc12.TextureCubeArray.NumCubes = desc.TextureCubeArray.NumCubes;
		break;
	default:
		return E_INVALIDARG;
	}

	// The stencil half of a depth stencil texture is its second plane
	if (desc.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT || desc.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT) {
		if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
			desc12.Texture2D.PlaneSlice = 1;
		else if (desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
			desc12.Texture2DArray.PlaneSlice = 1;
	}

	hr = begin_view(pResource, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, desc.Format, &view);
	if (FAILED(hr))
		return hr;
	view->mDesc = desc;
	view->v.kind = ShimView::SRV;
	view->v.desc12.srv = desc12;
	ShimRecreateView(&view->v);

	*ppSRView = view;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateUnorderedAccessView(ID3D11Resource *pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc, ID3D11UnorderedAccessView **ppUAView)
{
	static bool counter_reported;
	D3D11_UNORDERED_ACCESS_VIEW_DESC desc = {};
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc12 = {};
	ShimUnorderedAccessView *view;
	ShimResourceDesc resource;
	HRESULT hr;

	if (!pResource)
		return E_INVALIDARG;
	if (!ppUAView)
		return S_FALSE;
	*ppUAView = NULL;

	ShimGetResourceDesc(pResource, &resource);
	if (pDesc) {
		desc = *pDesc;
	} else {
		desc.Format = resource.format;
		switch (ShimResourceOf(pResource)->dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			desc.Buffer.NumElements = buffer_elements(&resource);
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			desc.ViewDimension = resource.array_size > 1 ? D3D11_UAV_DIMENSION_TEXTURE1DARRAY : D3D11_UAV_DIMENSION_TEXTURE1D;
			desc.Texture1DArray.ArraySize = resource.array_size;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			desc.ViewDimension = resource.array_size > 1 ? D3D11_UAV_DIMENSION_TEXTURE2DARRAY : D3D11_UAV_DIMENSION_TEXTURE2D;
			desc.Texture2DArray.ArraySize = resource.array_size;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			desc.Texture3D.WSize = (UINT)-1;
			break;
		}
	}

	desc12.Format = desc.Format;
	desc12.ViewDimension = (D3D12_UAV_DIMENSION)desc.ViewDimension;
	switch (desc.ViewDimension) {
	case D3D11_UAV_DIMENSION_BUFFER:
		desc12.Buffer.FirstElement = desc.Buffer.FirstElement;
		desc12.Buffer.NumElements = desc.Buffer.NumElements;
		if (desc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW)
			desc12.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
		else if (desc.Format == DXGI_FORMAT_UNKNOWN)
			desc12.Buffer.StructureByteStride = resource.stride;
		// As for a shader resource view
		if (desc.Format == DXGI_FORMAT_UNKNOWN && !desc12.Buffer.StructureByteStride)
			return E_INVALIDARG;
		// D3D12 keeps the hidden counter of these in a resource the
		// application provides, which the shim does not do
		if (desc.Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))
			Dx12ShimNotImplemented("An append or counter buffer view: its counter", &counter_reported);
		break;
	case D3D11_UAV_DIMENSION_TEXTURE1D:
		desc12.Texture1D.MipSlice = desc.Texture1D.MipSlice;
		break;
	case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
		desc12.Texture1DArray.MipSlice = desc.Texture1DArray.MipSlice;
		desc12.Texture1DArray.FirstArraySlice = desc.Texture1DArray.FirstArraySlice;
		desc12.Texture1DArray.ArraySize = desc.Texture1DArray.ArraySize;
		break;
	case D3D11_UAV_DIMENSION_TEXTURE2D:
		desc12.Texture2D.MipSlice = desc.Texture2D.MipSlice;
		break;
	case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
		desc12.Texture2DArray.MipSlice = desc.Texture2DArray.MipSlice;
		desc12.Texture2DArray.FirstArraySlice = desc.Texture2DArray.FirstArraySlice;
		desc12.Texture2DArray.ArraySize = desc.Texture2DArray.ArraySize;
		break;
	case D3D11_UAV_DIMENSION_TEXTURE3D:
		desc12.Texture3D.MipSlice = desc.Texture3D.MipSlice;
		desc12.Texture3D.FirstWSlice = desc.Texture3D.FirstWSlice;
		desc12.Texture3D.WSize = desc.Texture3D.WSize;
		break;
	default:
		return E_INVALIDARG;
	}

	hr = begin_view(pResource, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, desc.Format, &view);
	if (FAILED(hr))
		return hr;
	view->mDesc = desc;
	view->v.kind = ShimView::UAV;
	view->v.desc12.uav = desc12;
	ShimRecreateView(&view->v);

	*ppUAView = view;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateRenderTargetView(ID3D11Resource *pResource, const D3D11_RENDER_TARGET_VIEW_DESC *pDesc, ID3D11RenderTargetView **ppRTView)
{
	D3D11_RENDER_TARGET_VIEW_DESC desc = {};
	D3D12_RENDER_TARGET_VIEW_DESC desc12 = {};
	ShimRenderTargetView *view;
	ShimResourceDesc resource;
	HRESULT hr;

	if (!pResource)
		return E_INVALIDARG;
	if (!ppRTView)
		return S_FALSE;
	*ppRTView = NULL;

	ShimGetResourceDesc(pResource, &resource);
	if (pDesc) {
		desc = *pDesc;
	} else {
		desc.Format = resource.format;
		switch (ShimResourceOf(pResource)->dimension) {
		case D3D11_RESOURCE_DIMENSION_BUFFER:
			return E_INVALIDARG;
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			desc.ViewDimension = resource.array_size > 1 ? D3D11_RTV_DIMENSION_TEXTURE1DARRAY : D3D11_RTV_DIMENSION_TEXTURE1D;
			desc.Texture1DArray.ArraySize = resource.array_size;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			if (resource.samples > 1) {
				desc.ViewDimension = resource.array_size > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS;
				desc.Texture2DMSArray.ArraySize = resource.array_size;
			} else {
				desc.ViewDimension = resource.array_size > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DARRAY : D3D11_RTV_DIMENSION_TEXTURE2D;
				desc.Texture2DArray.ArraySize = resource.array_size;
			}
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
			desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
			desc.Texture3D.WSize = (UINT)-1;
			break;
		}
	}

	desc12.Format = desc.Format;
	desc12.ViewDimension = (D3D12_RTV_DIMENSION)desc.ViewDimension;
	switch (desc.ViewDimension) {
	case D3D11_RTV_DIMENSION_BUFFER:
		desc12.Buffer.FirstElement = desc.Buffer.FirstElement;
		desc12.Buffer.NumElements = desc.Buffer.NumElements;
		break;
	case D3D11_RTV_DIMENSION_TEXTURE1D:
		desc12.Texture1D.MipSlice = desc.Texture1D.MipSlice;
		break;
	case D3D11_RTV_DIMENSION_TEXTURE1DARRAY:
		desc12.Texture1DArray.MipSlice = desc.Texture1DArray.MipSlice;
		desc12.Texture1DArray.FirstArraySlice = desc.Texture1DArray.FirstArraySlice;
		desc12.Texture1DArray.ArraySize = desc.Texture1DArray.ArraySize;
		break;
	case D3D11_RTV_DIMENSION_TEXTURE2D:
		desc12.Texture2D.MipSlice = desc.Texture2D.MipSlice;
		break;
	case D3D11_RTV_DIMENSION_TEXTURE2DARRAY:
		desc12.Texture2DArray.MipSlice = desc.Texture2DArray.MipSlice;
		desc12.Texture2DArray.FirstArraySlice = desc.Texture2DArray.FirstArraySlice;
		desc12.Texture2DArray.ArraySize = desc.Texture2DArray.ArraySize;
		break;
	case D3D11_RTV_DIMENSION_TEXTURE2DMS:
		break;
	case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY:
		desc12.Texture2DMSArray.FirstArraySlice = desc.Texture2DMSArray.FirstArraySlice;
		desc12.Texture2DMSArray.ArraySize = desc.Texture2DMSArray.ArraySize;
		break;
	case D3D11_RTV_DIMENSION_TEXTURE3D:
		desc12.Texture3D.MipSlice = desc.Texture3D.MipSlice;
		desc12.Texture3D.FirstWSlice = desc.Texture3D.FirstWSlice;
		desc12.Texture3D.WSize = desc.Texture3D.WSize;
		break;
	default:
		return E_INVALIDARG;
	}

	hr = begin_view(pResource, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, desc.Format, &view);
	if (FAILED(hr))
		return hr;
	view->mDesc = desc;
	view->v.kind = ShimView::RTV;
	view->v.desc12.rtv = desc12;
	ShimRecreateView(&view->v);

	*ppRTView = view;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateDepthStencilView(ID3D11Resource *pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc, ID3D11DepthStencilView **ppDepthStencilView)
{
	D3D11_DEPTH_STENCIL_VIEW_DESC desc = {};
	D3D12_DEPTH_STENCIL_VIEW_DESC desc12 = {};
	ShimDepthStencilView *view;
	ShimResourceDesc resource;
	HRESULT hr;

	if (!pResource)
		return E_INVALIDARG;
	if (!ppDepthStencilView)
		return S_FALSE;
	*ppDepthStencilView = NULL;

	ShimGetResourceDesc(pResource, &resource);
	if (pDesc) {
		desc = *pDesc;
	} else {
		desc.Format = resource.format;
		switch (ShimResourceOf(pResource)->dimension) {
		case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
			desc.ViewDimension = resource.array_size > 1 ? D3D11_DSV_DIMENSION_TEXTURE1DARRAY : D3D11_DSV_DIMENSION_TEXTURE1D;
			desc.Texture1DArray.ArraySize = resource.array_size;
			break;
		case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
			if (resource.samples > 1) {
				desc.ViewDimension = resource.array_size > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
				desc.Texture2DMSArray.ArraySize = resource.array_size;
			} else {
				desc.ViewDimension = resource.array_size > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DARRAY : D3D11_DSV_DIMENSION_TEXTURE2D;
				desc.Texture2DArray.ArraySize = resource.array_size;
			}
			break;
		default:
			return E_INVALIDARG;
		}
	}

	desc12.Format = desc.Format;
	desc12.ViewDimension = (D3D12_DSV_DIMENSION)desc.ViewDimension;
	// D3D12_DSV_FLAGS has the values of D3D11_DSV_FLAG
	desc12.Flags = (D3D12_DSV_FLAGS)desc.Flags;
	switch (desc.ViewDimension) {
	case D3D11_DSV_DIMENSION_TEXTURE1D:
		desc12.Texture1D.MipSlice = desc.Texture1D.MipSlice;
		break;
	case D3D11_DSV_DIMENSION_TEXTURE1DARRAY:
		desc12.Texture1DArray.MipSlice = desc.Texture1DArray.MipSlice;
		desc12.Texture1DArray.FirstArraySlice = desc.Texture1DArray.FirstArraySlice;
		desc12.Texture1DArray.ArraySize = desc.Texture1DArray.ArraySize;
		break;
	case D3D11_DSV_DIMENSION_TEXTURE2D:
		desc12.Texture2D.MipSlice = desc.Texture2D.MipSlice;
		break;
	case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
		desc12.Texture2DArray.MipSlice = desc.Texture2DArray.MipSlice;
		desc12.Texture2DArray.FirstArraySlice = desc.Texture2DArray.FirstArraySlice;
		desc12.Texture2DArray.ArraySize = desc.Texture2DArray.ArraySize;
		break;
	case D3D11_DSV_DIMENSION_TEXTURE2DMS:
		break;
	case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
		desc12.Texture2DMSArray.FirstArraySlice = desc.Texture2DMSArray.FirstArraySlice;
		desc12.Texture2DMSArray.ArraySize = desc.Texture2DMSArray.ArraySize;
		break;
	default:
		return E_INVALIDARG;
	}

	hr = begin_view(pResource, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, desc.Format, &view);
	if (FAILED(hr))
		return hr;
	view->mDesc = desc;
	view->v.read_only = (desc.Flags & D3D11_DSV_READ_ONLY_DEPTH) && (desc.Flags & D3D11_DSV_READ_ONLY_STENCIL);
	view->v.kind = ShimView::DSV;
	view->v.desc12.dsv = desc12;
	ShimRecreateView(&view->v);

	*ppDepthStencilView = view;
	return S_OK;
}

// -----------------------------------------------------------------------------
// Shaders

// A shader stripped of its reflection data, as the ones DirectXTK draws the
// overlay with are, still declares what it reads in its code. Shader model 4
// and 5.0 only, where a register is its own binding.
static void reflect_declarations(ShimShader *shader)
{
	static const BYTE dimensions[] = {
		0, D3D12_SRV_DIMENSION_BUFFER, D3D12_SRV_DIMENSION_TEXTURE1D, D3D12_SRV_DIMENSION_TEXTURE2D,
		D3D12_SRV_DIMENSION_TEXTURE2DMS, D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURECUBE,
		D3D12_SRV_DIMENSION_TEXTURE1DARRAY, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
		D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY, D3D12_SRV_DIMENSION_TEXTURECUBEARRAY,
	};
	const BYTE *data = shader->bytecode.data();
	size_t size = shader->bytecode.size();
	const UINT32 *code = NULL, *end, *operand;
	// Opcodes of the shader bytecode, from d3d11TokenizedProgramFormat.hpp
	enum {
		CUSTOMDATA = 53, DCL_RESOURCE = 88, DCL_CONSTANT_BUFFER = 89, DCL_SAMPLER = 90,
		DCL_UAV_TYPED = 156, DCL_UAV_RAW = 157, DCL_UAV_STRUCTURED = 158,
		DCL_RESOURCE_RAW = 161, DCL_RESOURCE_STRUCTURED = 162,
	};
	UINT32 chunks, offset, bytes = 0, token, opcode, length, reg, dimension, i;

	if (size < 32 || memcmp(data, "DXBC", 4))
		return;
	memcpy(&chunks, data + 28, 4);
	for (i = 0; i < chunks && 32 + (size_t)(i + 1) * 4 <= size; i++) {
		memcpy(&offset, data + 32 + i * 4, 4);
		if ((size_t)offset + 8 > size)
			return;
		memcpy(&bytes, data + offset + 4, 4);
		if ((!memcmp(data + offset, "SHDR", 4) || !memcmp(data + offset, "SHEX", 4))
		    && bytes >= 8 && (size_t)offset + 8 + bytes <= size) {
			code = (const UINT32*)(data + offset + 8);
			break;
		}
	}
	if (!code || (code[0] >> 4 & 0xf) > 5 || ((code[0] >> 4 & 0xf) == 5 && (code[0] & 0xf)))
		return;

	end = code + min(code[1], bytes / 4);
	for (code += 2; code < end; code += length) {
		token = code[0];
		opcode = token & 0x7ff;
		length = opcode == CUSTOMDATA ? (code + 1 < end ? code[1] : 0) : token >> 24 & 0x7f;
		if (!length || code + length > end)
			return;
		if (opcode != DCL_RESOURCE && opcode != DCL_CONSTANT_BUFFER && opcode != DCL_SAMPLER
		    && opcode != DCL_RESOURCE_RAW && opcode != DCL_RESOURCE_STRUCTURED
		    && opcode != DCL_UAV_TYPED && opcode != DCL_UAV_RAW && opcode != DCL_UAV_STRUCTURED)
			continue;

		// Past the extensions of the opcode and of the operand to
		// the first index of the operand, the register
		for (operand = code; operand + 1 < code + length && (*operand & 0x80000000); operand++);
		for (operand++; operand + 1 < code + length && (*operand & 0x80000000); operand++);
		if (operand + 1 >= code + length)
			continue;
		reg = operand[1];

		switch (opcode) {
		case DCL_CONSTANT_BUFFER:
			if (reg < SHIM_CBV_SLOTS) {
				shader->cbv_used |= 1u << reg;
				shader->cbv_count = max(shader->cbv_count, reg + 1);
			}
			break;
		case DCL_SAMPLER:
			if (reg < SHIM_SAMPLER_SLOTS) {
				shader->sampler_used |= 1u << reg;
				shader->sampler_count = max(shader->sampler_count, reg + 1);
			}
			break;
		case DCL_RESOURCE:
			dimension = token >> 11 & 0x1f;
			if (reg < SHIM_SRV_SLOTS && dimension && dimension < ARRAYSIZE(dimensions)) {
				shader->srv_used[reg / 64] |= 1ull << (reg % 64);
				shader->srv_count = max(shader->srv_count, reg + 1);
				shader->srv_dimension[reg] = dimensions[dimension];
			}
			break;
		case DCL_RESOURCE_RAW:
		case DCL_RESOURCE_STRUCTURED:
			if (reg < SHIM_SRV_SLOTS) {
				shader->srv_used[reg / 64] |= 1ull << (reg % 64);
				shader->srv_count = max(shader->srv_count, reg + 1);
				shader->srv_dimension[reg] = D3D12_SRV_DIMENSION_BUFFER;
			}
			break;
		case DCL_UAV_TYPED:
		case DCL_UAV_RAW:
		case DCL_UAV_STRUCTURED:
			if (reg >= SHIM_UAV_SLOTS)
				break;
			shader->uav_used |= 1ull << reg;
			shader->uav_count = max(shader->uav_count, reg + 1);
			shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_BUFFER;
			// Of a typed view only, in the values of DCL_RESOURCE
			switch (opcode == DCL_UAV_TYPED ? token >> 11 & 0x1f : 1) {
			case 2: shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE1D; break;
			case 3: shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE2D; break;
			case 5: shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE3D; break;
			case 7: shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE1DARRAY; break;
			case 8: shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE2DARRAY; break;
			}
			break;
		}
	}
}

// The reflection of DXBC and that of DXIL have the same methods on different
// interfaces. False if the reflection could not be read.
template <class Reflection, class ShaderDesc, class BindDesc, class ParameterDesc>
static bool reflect_bindings(Reflection *reflection, ShimShader *shader)
{
	BindDesc bind;
	ParameterDesc output;
	ShaderDesc desc;
	UINT i, reg;

	if (FAILED(reflection->GetDesc(&desc)))
		return false;
	shader->reflected = true;

	if (shader->stage == DX12_STAGE_PS) {
		for (i = 0; i < desc.OutputParameters; i++) {
			if (SUCCEEDED(reflection->GetOutputParameterDesc(i, &output)) && !_stricmp(output.SemanticName, "SV_Target"))
				shader->targets++;
		}
	}

	if (!desc.BoundResources && !shader->dxil)
		reflect_declarations(shader);

	for (i = 0; i < desc.BoundResources; i++) {
		if (FAILED(reflection->GetResourceBindingDesc(i, &bind)))
			continue;

		for (reg = bind.BindPoint; reg < bind.BindPoint + (bind.BindCount ? bind.BindCount : 1); reg++) {
			switch (bind.Type) {
			case D3D_SIT_CBUFFER:
				if (reg < SHIM_CBV_SLOTS) {
					shader->cbv_used |= 1u << reg;
					shader->cbv_count = max(shader->cbv_count, reg + 1);
				}
				break;
			case D3D_SIT_SAMPLER:
				if (reg < SHIM_SAMPLER_SLOTS) {
					shader->sampler_used |= 1u << reg;
					shader->sampler_count = max(shader->sampler_count, reg + 1);
				}
				break;
			case D3D_SIT_TBUFFER:
			case D3D_SIT_TEXTURE:
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
				if (reg < SHIM_SRV_SLOTS) {
					shader->srv_used[reg / 64] |= 1ull << (reg % 64);
					shader->srv_count = max(shader->srv_count, reg + 1);
					shader->srv_dimension[reg] = D3D12_SRV_DIMENSION_BUFFER;
					if (bind.Type == D3D_SIT_TEXTURE && bind.Dimension != D3D_SRV_DIMENSION_BUFFEREX)
						shader->srv_dimension[reg] = (BYTE)bind.Dimension;
				}
				break;
			case D3D_SIT_UAV_RWTYPED:
			case D3D_SIT_UAV_RWSTRUCTURED:
			case D3D_SIT_UAV_RWBYTEADDRESS:
			case D3D_SIT_UAV_APPEND_STRUCTURED:
			case D3D_SIT_UAV_CONSUME_STRUCTURED:
			case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
				if (reg < SHIM_UAV_SLOTS) {
					shader->uav_used |= 1ull << reg;
					shader->uav_count = max(shader->uav_count, reg + 1);
					shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_BUFFER;
					switch (bind.Type == D3D_SIT_UAV_RWTYPED ? bind.Dimension : D3D_SRV_DIMENSION_BUFFER) {
					case D3D_SRV_DIMENSION_TEXTURE1D:
						shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE1D;
						break;
					case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
						shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
						break;
					case D3D_SRV_DIMENSION_TEXTURE2D:
						shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE2D;
						break;
					case D3D_SRV_DIMENSION_TEXTURE2DARRAY:
						shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
						break;
					case D3D_SRV_DIMENSION_TEXTURE3D:
						shader->uav_dimension[reg] = D3D12_UAV_DIMENSION_TEXTURE3D;
						break;
					}
				}
				break;
			}
		}
	}

	return true;
}

static void reflect_shader(ShimShader *shader)
{
	ID3D11ShaderReflection *reflection11 = NULL;
	ID3D12ShaderReflection *reflection12 = NULL;
	bool read = false;

	shader->dxil = Dx12BytecodeIsDxil(shader->bytecode.data(), shader->bytecode.size());
	if (shader->dxil) {
		if (Dx12DxcReflect(shader->bytecode.data(), shader->bytecode.size(), &reflection12)) {
			read = reflect_bindings<ID3D12ShaderReflection, D3D12_SHADER_DESC, D3D12_SHADER_INPUT_BIND_DESC,
				D3D12_SIGNATURE_PARAMETER_DESC>(reflection12, shader);
			reflection12->Release();
		}
	} else if (SUCCEEDED(D3DReflect(shader->bytecode.data(), shader->bytecode.size(), IID_PPV_ARGS(&reflection11)))) {
		read = reflect_bindings<ID3D11ShaderReflection, D3D11_SHADER_DESC, D3D11_SHADER_INPUT_BIND_DESC,
			D3D11_SIGNATURE_PARAMETER_DESC>(reflection11, shader);
		reflection11->Release();
	}
	if (!read) {
		Dx12LogInfo("*** No reflection for a %s %s shader of a mod, it will be given no resources\n",
			shader->dxil ? "DXIL" : "DXBC", dx12_stage_names[shader->stage]);
	}
}

void ShimSetShaderDxil(ID3D11DeviceChild *shader, const void *bytecode, size_t length)
{
	ShimShader *dxbc = ShimShaderOf(shader), *dxil = new ShimShader();

	dxil->stage = dxbc->stage;
	dxil->hash = fnv_64_buf(bytecode, length);
	dxil->bytecode.assign((const BYTE*)bytecode, (const BYTE*)bytecode + length);
	reflect_shader(dxil);

	delete dxbc->dxil_form;
	dxbc->dxil_form = dxil;
}

template <class Interface>
static HRESULT create_shader(Dx12ShaderStage stage, const void *bytecode, SIZE_T length, Interface **out)
{
	ShimShaderObject<Interface> *shader;

	if (!bytecode || !length)
		return E_INVALIDARG;
	if (!out)
		return S_FALSE;

	shader = new ShimShaderObject<Interface>();
	shader->s.stage = stage;
	shader->s.id = ShimNextId();
	shader->s.hash = fnv_64_buf(bytecode, length);
	shader->s.bytecode.assign((const BYTE*)bytecode, (const BYTE*)bytecode + length);
	reflect_shader(&shader->s);

	*out = shader;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateVertexShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11VertexShader **ppVertexShader)
{
	return create_shader(DX12_STAGE_VS, pShaderBytecode, BytecodeLength, ppVertexShader);
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateGeometryShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11GeometryShader **ppGeometryShader)
{
	return create_shader(DX12_STAGE_GS, pShaderBytecode, BytecodeLength, ppGeometryShader);
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreatePixelShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11PixelShader **ppPixelShader)
{
	return create_shader(DX12_STAGE_PS, pShaderBytecode, BytecodeLength, ppPixelShader);
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateHullShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11HullShader **ppHullShader)
{
	return create_shader(DX12_STAGE_HS, pShaderBytecode, BytecodeLength, ppHullShader);
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateDomainShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11DomainShader **ppDomainShader)
{
	return create_shader(DX12_STAGE_DS, pShaderBytecode, BytecodeLength, ppDomainShader);
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateComputeShader(const void *pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage *pClassLinkage, ID3D11ComputeShader **ppComputeShader)
{
	return create_shader(DX12_STAGE_CS, pShaderBytecode, BytecodeLength, ppComputeShader);
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC *pInputElementDescs, UINT NumElements, const void *pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout **ppInputLayout)
{
	ShimInputLayout *layout;
	UINT i;

	if (!pInputElementDescs && NumElements)
		return E_INVALIDARG;
	if (!ppInputLayout)
		return S_FALSE;

	layout = new ShimInputLayout();
	layout->mId = ShimNextId();
	layout->mSemantics.reserve(NumElements);
	for (i = 0; i < NumElements; i++) {
		const D3D11_INPUT_ELEMENT_DESC &element = pInputElementDescs[i];

		layout->mSemantics.push_back(element.SemanticName);
		// D3D12_INPUT_CLASSIFICATION has the values of
		// D3D11_INPUT_CLASSIFICATION
		layout->mElements.push_back({ layout->mSemantics.back().c_str(), element.SemanticIndex, element.Format,
			element.InputSlot, element.AlignedByteOffset, (D3D12_INPUT_CLASSIFICATION)element.InputSlotClass,
			element.InstanceDataStepRate });
	}

	*ppInputLayout = layout;
	return S_OK;
}

// -----------------------------------------------------------------------------
// State objects

HRESULT STDMETHODCALLTYPE ShimDevice::CreateBlendState(const D3D11_BLEND_DESC *pBlendStateDesc, ID3D11BlendState **ppBlendState)
{
	ShimBlendState *state;

	if (!pBlendStateDesc)
		return E_INVALIDARG;
	if (!ppBlendState)
		return S_FALSE;

	state = new ShimBlendState();
	state->mDesc = *pBlendStateDesc;
	*ppBlendState = state;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC *pDepthStencilDesc, ID3D11DepthStencilState **ppDepthStencilState)
{
	ShimDepthStencilState *state;

	if (!pDepthStencilDesc)
		return E_INVALIDARG;
	if (!ppDepthStencilState)
		return S_FALSE;

	state = new ShimDepthStencilState();
	state->mDesc = *pDepthStencilDesc;
	*ppDepthStencilState = state;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateRasterizerState(const D3D11_RASTERIZER_DESC *pRasterizerDesc, ID3D11RasterizerState **ppRasterizerState)
{
	ShimRasterizerState *state;

	if (!pRasterizerDesc)
		return E_INVALIDARG;
	if (!ppRasterizerState)
		return S_FALSE;

	state = new ShimRasterizerState();
	state->mDesc = *pRasterizerDesc;
	*ppRasterizerState = state;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimDevice::CreateSamplerState(const D3D11_SAMPLER_DESC *pSamplerDesc, ID3D11SamplerState **ppSamplerState)
{
	ShimSamplerState *state;

	if (!pSamplerDesc)
		return E_INVALIDARG;
	if (!ppSamplerState)
		return S_FALSE;

	state = new ShimSamplerState();
	state->mDesc = *pSamplerDesc;
	state->mHandle = Dx12ShimAllocDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
	if (!state->mHandle.ptr) {
		state->Release();
		return E_OUTOFMEMORY;
	}
	// The two sampler descriptions are the same struct
	dx12_shim->device->CreateSampler((const D3D12_SAMPLER_DESC*)pSamplerDesc, state->mHandle);

	*ppSamplerState = state;
	return S_OK;
}

#include "generated/ShimDevice.stub.inl"
