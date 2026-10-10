#include "ShimObjects.h"

#include "ShimDevice.h"
#include "ShimPipeline.h"

UINT64 ShimNextId()
{
	static UINT64 id;

	return ++id;
}

void Dx12ShimGetDevice(ID3D11Device **ppDevice)
{
	dx12_shim->shim_device->AddRef();
	*ppDevice = dx12_shim->shim_device;
}

HRESULT ShimQueryInterface(IUnknown *object, const IID *const *iids, size_t count, REFIID riid, void **ppvObject)
{
	size_t i;

	if (!ppvObject)
		return E_POINTER;

	for (i = 0; i < count; i++) {
		if (IsEqualIID(*iids[i], riid)) {
			object->AddRef();
			*ppvObject = object;
			return S_OK;
		}
	}

	*ppvObject = NULL;
	return E_NOINTERFACE;
}

// -----------------------------------------------------------------------------

ShimPrivateData::~ShimPrivateData()
{
	// An interface may reach back to us while it is being released
	while (!mEntries.empty()) {
		IUnknown *unknown = mEntries.back().unknown;

		mEntries.pop_back();
		if (unknown)
			unknown->Release();
	}
}

void ShimPrivateData::Remove(REFGUID guid)
{
	size_t i;

	for (i = 0; i < mEntries.size(); i++) {
		if (IsEqualGUID(mEntries[i].guid, guid)) {
			IUnknown *unknown = mEntries[i].unknown;

			mEntries.erase(mEntries.begin() + i);
			if (unknown)
				unknown->Release();
			return;
		}
	}
}

HRESULT ShimPrivateData::Get(REFGUID guid, UINT *pDataSize, void *pData)
{
	if (!pDataSize)
		return E_INVALIDARG;

	for (Entry &entry : mEntries) {
		UINT size;

		if (!IsEqualGUID(entry.guid, guid))
			continue;

		size = entry.unknown ? sizeof(IUnknown*) : (UINT)entry.data.size();
		if (!pData) {
			*pDataSize = size;
			return S_OK;
		}
		if (*pDataSize < size) {
			*pDataSize = size;
			return DXGI_ERROR_MORE_DATA;
		}

		*pDataSize = size;
		if (entry.unknown) {
			entry.unknown->AddRef();
			*(IUnknown**)pData = entry.unknown;
		} else {
			memcpy(pData, entry.data.data(), size);
		}
		return S_OK;
	}

	*pDataSize = 0;
	return DXGI_ERROR_NOT_FOUND;
}

HRESULT ShimPrivateData::Set(REFGUID guid, UINT DataSize, const void *pData)
{
	Remove(guid);
	if (!pData)
		return S_OK;

	mEntries.push_back({ guid, NULL, std::vector<BYTE>((const BYTE*)pData, (const BYTE*)pData + DataSize) });
	return S_OK;
}

HRESULT ShimPrivateData::SetInterface(REFGUID guid, const IUnknown *pData)
{
	IUnknown *unknown = const_cast<IUnknown*>(pData);

	Remove(guid);
	if (!unknown)
		return S_OK;

	unknown->AddRef();
	mEntries.push_back({ guid, unknown });
	return S_OK;
}

// -----------------------------------------------------------------------------

void ShimDestroyResource(ShimResource *resource)
{
	Dx12ShimForgetResource(resource);

	if (resource->resource && !resource->foreign) {
		Dx12ShimDeferRelease(resource->resource);
		resource->resource->Release();
	}
	if (resource->readback) {
		Dx12ShimDeferRelease(resource->readback);
		resource->readback->Release();
	}
}

void ShimDestroyView(ShimView *view)
{
	Dx12ShimFreeDescriptor(view->heap_type, view->handle);
	if (view->resource11)
		view->resource11->Release();
}

void ShimDestroyShader(ShimShader *shader)
{
	Dx12ShimForgetPipelines(shader->id);
}

ShimSamplerState::~ShimSamplerState()
{
	Dx12ShimFreeDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, mHandle);
}

ShimInputLayout::~ShimInputLayout()
{
	Dx12ShimForgetPipelines(mId);
}

void ShimGetResourceDesc(ID3D11Resource *resource, ShimResourceDesc *desc)
{
	*desc = {};
	desc->mips = 1;
	desc->array_size = 1;
	desc->samples = 1;

	switch (ShimResourceOf(resource)->dimension) {
	case D3D11_RESOURCE_DIMENSION_BUFFER: {
		const D3D11_BUFFER_DESC &buffer = static_cast<ShimBuffer*>(resource)->mDesc;

		desc->usage = buffer.Usage;
		desc->bind_flags = buffer.BindFlags;
		desc->cpu_access = buffer.CPUAccessFlags;
		desc->misc_flags = buffer.MiscFlags;
		desc->byte_width = buffer.ByteWidth;
		desc->stride = buffer.StructureByteStride;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
		const D3D11_TEXTURE1D_DESC &tex = static_cast<ShimTexture1D*>(resource)->mDesc;

		desc->usage = tex.Usage;
		desc->bind_flags = tex.BindFlags;
		desc->cpu_access = tex.CPUAccessFlags;
		desc->misc_flags = tex.MiscFlags;
		desc->format = tex.Format;
		desc->mips = tex.MipLevels;
		desc->array_size = tex.ArraySize;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
		const D3D11_TEXTURE2D_DESC &tex = static_cast<ShimTexture2D*>(resource)->mDesc;

		desc->usage = tex.Usage;
		desc->bind_flags = tex.BindFlags;
		desc->cpu_access = tex.CPUAccessFlags;
		desc->misc_flags = tex.MiscFlags;
		desc->format = tex.Format;
		desc->mips = tex.MipLevels;
		desc->array_size = tex.ArraySize;
		desc->samples = tex.SampleDesc.Count;
		break;
	}
	case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
		const D3D11_TEXTURE3D_DESC &tex = static_cast<ShimTexture3D*>(resource)->mDesc;

		desc->usage = tex.Usage;
		desc->bind_flags = tex.BindFlags;
		desc->cpu_access = tex.CPUAccessFlags;
		desc->misc_flags = tex.MiscFlags;
		desc->format = tex.Format;
		desc->mips = tex.MipLevels;
		break;
	}
	}
}

ShimTexture2D* ShimWrapTexture2D(ID3D12Resource *resource, D3D12_RESOURCE_STATES state)
{
	ShimTexture2D *texture = new ShimTexture2D();
	D3D11_TEXTURE2D_DESC *desc = &texture->mDesc;

	texture->r.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
	texture->r.resource = resource;
	texture->r.desc = resource->GetDesc();
	texture->r.state = state;
	texture->r.foreign = true;

	*desc = {};
	desc->Width = (UINT)texture->r.desc.Width;
	desc->Height = texture->r.desc.Height;
	desc->MipLevels = texture->r.desc.MipLevels;
	desc->ArraySize = texture->r.desc.DepthOrArraySize;
	desc->Format = texture->r.desc.Format;
	desc->SampleDesc = texture->r.desc.SampleDesc;
	desc->Usage = D3D11_USAGE_DEFAULT;
	if (!(texture->r.desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		desc->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
	if (texture->r.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
		desc->BindFlags |= D3D11_BIND_RENDER_TARGET;
	if (texture->r.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
		desc->BindFlags |= D3D11_BIND_DEPTH_STENCIL;
	if (texture->r.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		desc->BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

	return texture;
}

UINT64 Dx12ShimBufferAddress(ID3D11Buffer *buffer, D3D12_RESOURCE_STATES state)
{
	ShimResource *resource = ShimResourceOf(buffer);
	BYTE *upload;

	if (!resource->cpu_only) {
		if (!resource->resource)
			return 0;
		Dx12ShimTransition(resource, state);
		return resource->resource->GetGPUVirtualAddress();
	}

	if (resource->upload_epoch == dx12_shim_epoch && resource->upload_version == resource->version)
		return resource->upload_va;

	// 256 is what a constant buffer view has to be aligned to
	upload = Dx12ShimUpload(resource->shadow.size(), 256, NULL, NULL, &resource->upload_va);
	if (!upload)
		return 0;
	memcpy(upload, resource->shadow.data(), resource->shadow.size());
	resource->upload_epoch = dx12_shim_epoch;
	resource->upload_version = resource->version;
	return resource->upload_va;
}
