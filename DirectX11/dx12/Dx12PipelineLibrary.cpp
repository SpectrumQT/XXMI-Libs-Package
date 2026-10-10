#include "Dx12PipelineLibrary.h"

#include "Dx12Device.h"
#include "PipelineRegistry.h"

// {0C8E0F4B-6B0D-4E8B-9B56-7D2C5E1A93F4}
const GUID Dx12PipelineLibrary::ProxyIID = { 0xc8e0f4b, 0x6b0d, 0x4e8b, { 0x9b, 0x56, 0x7d, 0x2c, 0x5e, 0x1a, 0x93, 0xf4 } };
void *Dx12PipelineLibrary::sVtbl;

static const IID *const library_iids[] = {
	&__uuidof(ID3D12PipelineLibrary), &__uuidof(ID3D12PipelineLibrary1),
};

Dx12PipelineLibrary::Dx12PipelineLibrary(Dx12Device *device, ID3D12PipelineLibrary *orig) :
	mDevice(device),
	mRef(1)
{
	mVersion = Dx12QueryNewest(orig, library_iids, ARRAYSIZE(library_iids), (void**)&mOrig);
	mDevice->AddRef();
	sVtbl = *(void**)this;
}

bool Dx12PipelineLibrary::Usable(REFIID riid)
{
	int version;

	if (IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(ID3D12Object))
	    || IsEqualIID(riid, __uuidof(ID3D12DeviceChild)))
		return true;

	version = Dx12FindIID(library_iids, ARRAYSIZE(library_iids), riid);
	return version >= 0 && version <= mVersion;
}

void Dx12PipelineLibrary::Abandon()
{
	mOrig->Release();
	mDevice->Release();
	delete this;
}

HRESULT STDMETHODCALLTYPE Dx12PipelineLibrary::QueryInterface(REFIID riid, void **ppvObject)
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
	Dx12LogForwardedQuery("Dx12PipelineLibrary", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12PipelineLibrary::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12PipelineLibrary::Release()
{
	// Read first: once our reference is gone, so may be this object.
	ID3D12PipelineLibrary1 *orig = mOrig;
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

HRESULT STDMETHODCALLTYPE Dx12PipelineLibrary::GetDevice(REFIID riid, void **ppvDevice)
{
	return mDevice->QueryInterface(riid, ppvDevice);
}

HRESULT STDMETHODCALLTYPE Dx12PipelineLibrary::LoadGraphicsPipeline(LPCWSTR pName, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState)
{
	HRESULT hr = mOrig->LoadGraphicsPipeline(pName, pDesc, riid, ppPipelineState);

	if (SUCCEEDED(hr))
		Dx12RegisterPipeline(ppPipelineState, pDesc);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12PipelineLibrary::LoadComputePipeline(LPCWSTR pName, const D3D12_COMPUTE_PIPELINE_STATE_DESC *pDesc, REFIID riid, void **ppPipelineState)
{
	HRESULT hr = mOrig->LoadComputePipeline(pName, pDesc, riid, ppPipelineState);

	if (SUCCEEDED(hr))
		Dx12RegisterPipeline(ppPipelineState, pDesc);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12PipelineLibrary::LoadPipeline(LPCWSTR pName, const D3D12_PIPELINE_STATE_STREAM_DESC *pDesc, REFIID riid, void **ppPipelineState)
{
	HRESULT hr = mOrig->LoadPipeline(pName, pDesc, riid, ppPipelineState);

	if (SUCCEEDED(hr))
		Dx12RegisterPipeline(ppPipelineState, pDesc);
	return hr;
}

#include "generated/Dx12PipelineLibrary.fwd.inl"
