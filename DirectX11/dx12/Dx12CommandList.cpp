#include "Dx12CommandList.h"

#include <vector>

#include "Dx12DescriptorHeap.h"
#include "Dx12Device.h"

// {E46EEB85-36B9-497A-947E-E9BA3B8724B4}
const GUID Dx12GraphicsCommandList::ProxyIID = { 0xe46eeb85, 0x36b9, 0x497a, { 0x94, 0x7e, 0xe9, 0xba, 0x3b, 0x87, 0x24, 0xb4 } };
void *Dx12GraphicsCommandList::sVtbl;

static const IID *const list_iids[] = {
	&__uuidof(ID3D12GraphicsCommandList), &__uuidof(ID3D12GraphicsCommandList1),
	&__uuidof(ID3D12GraphicsCommandList2), &__uuidof(ID3D12GraphicsCommandList3),
	&__uuidof(ID3D12GraphicsCommandList4), &__uuidof(ID3D12GraphicsCommandList5),
	&__uuidof(ID3D12GraphicsCommandList6), &__uuidof(ID3D12GraphicsCommandList7),
	&__uuidof(ID3D12GraphicsCommandList8), &__uuidof(ID3D12GraphicsCommandList9),
	&__uuidof(ID3D12GraphicsCommandList10),
};

Dx12GraphicsCommandList::Dx12GraphicsCommandList(Dx12Device *device, ID3D12GraphicsCommandList *orig) :
	mDevice(device),
	mRef(1),
	mDraws(0),
	mDispatches(0)
{
	mVersion = Dx12QueryNewest(orig, list_iids, ARRAYSIZE(list_iids), (void**)&mOrig);
	mDevice->AddRef();
	sVtbl = *(void**)this;
}

bool Dx12GraphicsCommandList::Usable(REFIID riid)
{
	int version;

	if (IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(ID3D12Object))
	    || IsEqualIID(riid, __uuidof(ID3D12DeviceChild))
	    || IsEqualIID(riid, __uuidof(ID3D12CommandList)))
		return true;

	version = Dx12FindIID(list_iids, ARRAYSIZE(list_iids), riid);
	return version >= 0 && version <= mVersion;
}

void Dx12GraphicsCommandList::Abandon()
{
	mOrig->Release();
	mDevice->Release();
	delete this;
}

HRESULT STDMETHODCALLTYPE Dx12GraphicsCommandList::QueryInterface(REFIID riid, void **ppvObject)
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
	Dx12LogForwardedQuery("Dx12GraphicsCommandList", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12GraphicsCommandList::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12GraphicsCommandList::Release()
{
	// Read first: once our reference is gone, so may be this object.
	ID3D12GraphicsCommandList10 *orig = mOrig;
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

HRESULT STDMETHODCALLTYPE Dx12GraphicsCommandList::GetDevice(REFIID riid, void **ppvDevice)
{
	return mDevice->QueryInterface(riid, ppvDevice);
}

HRESULT STDMETHODCALLTYPE Dx12GraphicsCommandList::Close()
{
	if (mDraws)
		InterlockedAdd64(&dx12_stats.draws, mDraws);
	if (mDispatches)
		InterlockedAdd64(&dx12_stats.dispatches, mDispatches);
	InterlockedIncrement64(&dx12_stats.lists_closed);
	mDraws = mDispatches = 0;

	return mOrig->Close();
}

HRESULT STDMETHODCALLTYPE Dx12GraphicsCommandList::Reset(ID3D12CommandAllocator *pAllocator, ID3D12PipelineState *pInitialState)
{
	mDraws = mDispatches = 0;

	return mOrig->Reset(pAllocator, pInitialState);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
	mDraws++;
	mOrig->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
	mDraws++;
	mOrig->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
	mDispatches++;
	mOrig->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

// What an indirect call executes depends on its command signature, which is
// not tracked yet. Counted with the draws.
void STDMETHODCALLTYPE Dx12GraphicsCommandList::ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset)
{
	mDraws++;
	mOrig->ExecuteIndirect(pCommandSignature, MaxCommandCount, pArgumentBuffer, ArgumentBufferOffset, pCountBuffer, CountBufferOffset);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::DispatchMesh(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
	mDraws++;
	mOrig->DispatchMesh(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::DispatchRays(const D3D12_DISPATCH_RAYS_DESC *pDesc)
{
	mDispatches++;
	mOrig->DispatchRays(pDesc);
}

// The draws of a bundle were counted when the bundle was recorded.
void STDMETHODCALLTYPE Dx12GraphicsCommandList::ExecuteBundle(ID3D12GraphicsCommandList *pCommandList)
{
	mOrig->ExecuteBundle(Dx12Unwrap<Dx12GraphicsCommandList>(pCommandList));
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetDescriptorHeaps(UINT NumDescriptorHeaps, ID3D12DescriptorHeap *const *ppDescriptorHeaps)
{
	// D3D12 accepts one heap per shader visible type
	ID3D12DescriptorHeap *stack[4], **heaps = stack;
	std::vector<ID3D12DescriptorHeap*> large;
	UINT i;

	if (!ppDescriptorHeaps) {
		mOrig->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
		return;
	}

	if (NumDescriptorHeaps > ARRAYSIZE(stack)) {
		large.resize(NumDescriptorHeaps);
		heaps = large.data();
	}

	for (i = 0; i < NumDescriptorHeaps; i++)
		heaps[i] = Dx12Unwrap<Dx12DescriptorHeap>(ppDescriptorHeaps[i]);

	mOrig->SetDescriptorHeaps(NumDescriptorHeaps, heaps);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor)
{
	D3D12_CPU_DESCRIPTOR_HANDLE targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT], depth_stencil;
	UINT i, count;

	if (pRenderTargetDescriptors && NumRenderTargetDescriptors <= ARRAYSIZE(targets)) {
		// A single handle stands for a contiguous range of descriptors
		count = RTsSingleHandleToDescriptorRange && NumRenderTargetDescriptors ? 1 : NumRenderTargetDescriptors;
		for (i = 0; i < count; i++)
			targets[i] = Dx12OrigCpuHandle(pRenderTargetDescriptors[i]);
		pRenderTargetDescriptors = targets;
	}

	if (pDepthStencilDescriptor) {
		depth_stencil = Dx12OrigCpuHandle(*pDepthStencilDescriptor);
		pDepthStencilDescriptor = &depth_stencil;
	}

	mOrig->OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors, RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::BeginRenderPass(UINT NumRenderTargets, const D3D12_RENDER_PASS_RENDER_TARGET_DESC *pRenderTargets, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *pDepthStencil, D3D12_RENDER_PASS_FLAGS Flags)
{
	D3D12_RENDER_PASS_RENDER_TARGET_DESC targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
	D3D12_RENDER_PASS_DEPTH_STENCIL_DESC depth_stencil;
	UINT i;

	if (pRenderTargets && NumRenderTargets <= ARRAYSIZE(targets)) {
		for (i = 0; i < NumRenderTargets; i++) {
			targets[i] = pRenderTargets[i];
			targets[i].cpuDescriptor = Dx12OrigCpuHandle(targets[i].cpuDescriptor);
		}
		pRenderTargets = targets;
	}

	if (pDepthStencil) {
		depth_stencil = *pDepthStencil;
		depth_stencil.cpuDescriptor = Dx12OrigCpuHandle(depth_stencil.cpuDescriptor);
		pDepthStencil = &depth_stencil;
	}

	mOrig->BeginRenderPass(NumRenderTargets, pRenderTargets, pDepthStencil, Flags);
}

#include "generated/Dx12GraphicsCommandList.fwd.inl"
