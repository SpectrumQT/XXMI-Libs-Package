#include "Dx12CommandList.h"

#include <vector>

#include "BufferHash.h"
#include "Dx12Device.h"
#include "HashDump.h"
#include "ResourceTracker.h"
#include "TextureHash.h"

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
	Dx12ResetCmdState(&mState, NULL);
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
	Dx12ResetCmdState(&mState, pInitialState);
	mTransitions.clear();

	return mOrig->Reset(pAllocator, pInitialState);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::ClearState(ID3D12PipelineState *pPipelineState)
{
	Dx12ResetCmdState(&mState, pPipelineState);
	mOrig->ClearState(pPipelineState);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
	if (dx12_dump_draws)
		Dx12DumpDraw(this, "draw", VertexCountPerInstance, InstanceCount, StartVertexLocation, 0);
	mDraws++;
	mOrig->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
	if (dx12_dump_draws)
		Dx12DumpDraw(this, "indexed", IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation);
	mDraws++;
	mOrig->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
	if (dx12_dump_draws)
		Dx12DumpDraw(this, "dispatch", ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ, 0);
	mDispatches++;
	mOrig->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

// What an indirect call executes depends on its command signature, which is
// not tracked yet. Counted with the draws.
void STDMETHODCALLTYPE Dx12GraphicsCommandList::ExecuteIndirect(ID3D12CommandSignature *pCommandSignature, UINT MaxCommandCount, ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset, ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset)
{
	if (dx12_dump_draws)
		Dx12DumpDraw(this, "indirect", MaxCommandCount, 0, 0, 0);
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

	mState.view_heap = mState.sampler_heap = NULL;

	if (!ppDescriptorHeaps) {
		mOrig->SetDescriptorHeaps(NumDescriptorHeaps, ppDescriptorHeaps);
		return;
	}

	if (NumDescriptorHeaps > ARRAYSIZE(stack)) {
		large.resize(NumDescriptorHeaps);
		heaps = large.data();
	}

	for (i = 0; i < NumDescriptorHeaps; i++) {
		heaps[i] = Dx12Unwrap<Dx12DescriptorHeap>(ppDescriptorHeaps[i]);
		if (heaps[i] == ppDescriptorHeaps[i])
			continue;

		Dx12DescriptorHeap *proxy = static_cast<Dx12DescriptorHeap*>(ppDescriptorHeaps[i]);
		if (proxy->mType == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			mState.sampler_heap = proxy;
		else
			mState.view_heap = proxy;
	}

	mOrig->SetDescriptorHeaps(NumDescriptorHeaps, heaps);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::OMSetRenderTargets(UINT NumRenderTargetDescriptors, const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors, BOOL RTsSingleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor)
{
	D3D12_CPU_DESCRIPTOR_HANDLE targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT], depth_stencil;
	UINT i, count;

	mState.render_target_count = 0;
	if (pRenderTargetDescriptors && NumRenderTargetDescriptors <= ARRAYSIZE(targets)) {
		mState.render_target_count = NumRenderTargetDescriptors;
		for (i = 0; i < NumRenderTargetDescriptors; i++) {
			mState.render_targets[i] = RTsSingleHandleToDescriptorRange
				? Dx12AdvanceCpuHandle(pRenderTargetDescriptors[0], i) : pRenderTargetDescriptors[i];
		}
	}
	mState.has_depth_stencil = !!pDepthStencilDescriptor;
	if (pDepthStencilDescriptor)
		mState.depth_stencil = *pDepthStencilDescriptor;

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

	mState.render_target_count = 0;
	mState.has_depth_stencil = !!pDepthStencil;
	if (pDepthStencil)
		mState.depth_stencil = pDepthStencil->cpuDescriptor;

	if (pRenderTargets && NumRenderTargets <= ARRAYSIZE(targets)) {
		mState.render_target_count = NumRenderTargets;
		for (i = 0; i < NumRenderTargets; i++) {
			mState.render_targets[i] = pRenderTargets[i].cpuDescriptor;
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

// -----------------------------------------------------------------------------
// State the game sets, see CmdState.h

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetPipelineState(ID3D12PipelineState *pPipelineState)
{
	mState.pipeline = pPipelineState;
	mOrig->SetPipelineState(pPipelineState);
}

// A raytracing state object replaces the pipeline for DispatchRays
void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetPipelineState1(ID3D12StateObject *pStateObject)
{
	mState.pipeline = NULL;
	mOrig->SetPipelineState1(pStateObject);
}

// Setting the signature that is already set keeps the arguments, any other
// one invalidates them all.
static inline void set_root_signature(Dx12RootArguments *arguments, ID3D12RootSignature *signature)
{
	if (arguments->signature == signature)
		return;
	arguments->signature = signature;
	arguments->set = 0;
}

static inline void set_root_argument(Dx12RootArguments *arguments, UINT index, UINT64 value)
{
	if (index >= DX12_MAX_ROOT_ARGUMENTS)
		return;
	arguments->values[index] = value;
	arguments->set |= 1ull << index;
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetGraphicsRootSignature(ID3D12RootSignature *pRootSignature)
{
	set_root_signature(&mState.graphics, pRootSignature);
	mOrig->SetGraphicsRootSignature(pRootSignature);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetGraphicsRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
	set_root_argument(&mState.graphics, RootParameterIndex, BaseDescriptor.ptr);
	mOrig->SetGraphicsRootDescriptorTable(RootParameterIndex, BaseDescriptor);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetGraphicsRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	set_root_argument(&mState.graphics, RootParameterIndex, BufferLocation);
	mOrig->SetGraphicsRootConstantBufferView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetGraphicsRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	set_root_argument(&mState.graphics, RootParameterIndex, BufferLocation);
	mOrig->SetGraphicsRootShaderResourceView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetGraphicsRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	set_root_argument(&mState.graphics, RootParameterIndex, BufferLocation);
	mOrig->SetGraphicsRootUnorderedAccessView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetComputeRootSignature(ID3D12RootSignature *pRootSignature)
{
	set_root_signature(&mState.compute, pRootSignature);
	mOrig->SetComputeRootSignature(pRootSignature);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetComputeRootDescriptorTable(UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
	set_root_argument(&mState.compute, RootParameterIndex, BaseDescriptor.ptr);
	mOrig->SetComputeRootDescriptorTable(RootParameterIndex, BaseDescriptor);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetComputeRootConstantBufferView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	set_root_argument(&mState.compute, RootParameterIndex, BufferLocation);
	mOrig->SetComputeRootConstantBufferView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetComputeRootShaderResourceView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	set_root_argument(&mState.compute, RootParameterIndex, BufferLocation);
	mOrig->SetComputeRootShaderResourceView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::SetComputeRootUnorderedAccessView(UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS BufferLocation)
{
	set_root_argument(&mState.compute, RootParameterIndex, BufferLocation);
	mOrig->SetComputeRootUnorderedAccessView(RootParameterIndex, BufferLocation);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::IASetVertexBuffers(UINT StartSlot, UINT NumViews, const D3D12_VERTEX_BUFFER_VIEW *pViews)
{
	UINT i;

	for (i = 0; i < NumViews && StartSlot + i < ARRAYSIZE(mState.vertex_buffers); i++) {
		if (pViews)
			mState.vertex_buffers[StartSlot + i] = pViews[i];
		else
			mState.vertex_buffers[StartSlot + i] = {};
	}

	mOrig->IASetVertexBuffers(StartSlot, NumViews, pViews);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *pView)
{
	if (pView)
		mState.index_buffer = *pView;
	else
		mState.index_buffer = {};

	mOrig->IASetIndexBuffer(pView);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY PrimitiveTopology)
{
	mState.topology = PrimitiveTopology;
	mOrig->IASetPrimitiveTopology(PrimitiveTopology);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::RSSetViewports(UINT NumViewports, const D3D12_VIEWPORT *pViewports)
{
	if (pViewports && NumViewports <= ARRAYSIZE(mState.viewports)) {
		mState.viewport_count = NumViewports;
		memcpy(mState.viewports, pViewports, NumViewports * sizeof(D3D12_VIEWPORT));
	}

	mOrig->RSSetViewports(NumViewports, pViewports);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::RSSetScissorRects(UINT NumRects, const D3D12_RECT *pRects)
{
	if (pRects && NumRects <= ARRAYSIZE(mState.scissors)) {
		mState.scissor_count = NumRects;
		memcpy(mState.scissors, pRects, NumRects * sizeof(D3D12_RECT));
	}

	mOrig->RSSetScissorRects(NumRects, pRects);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::OMSetBlendFactor(const FLOAT BlendFactor[ 4 ])
{
	UINT i;

	// No factor stands for all ones
	for (i = 0; i < 4; i++)
		mState.blend_factor[i] = BlendFactor ? BlendFactor[i] : 1.0f;

	mOrig->OMSetBlendFactor(BlendFactor);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::OMSetStencilRef(UINT StencilRef)
{
	mState.stencil_ref = StencilRef;
	mOrig->OMSetStencilRef(StencilRef);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::OMSetFrontAndBackStencilRef(UINT FrontStencilRef, UINT BackStencilRef)
{
	mState.stencil_ref = FrontStencilRef;
	mOrig->OMSetFrontAndBackStencilRef(FrontStencilRef, BackStencilRef);
}

// -----------------------------------------------------------------------------
// Resource states and contents

void STDMETHODCALLTYPE Dx12GraphicsCommandList::ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *pBarriers)
{
	UINT i;

	for (i = 0; pBarriers && i < NumBarriers; i++) {
		// The second half of a split barrier is the one with the state
		// the resource ends up in
		if (pBarriers[i].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
		    || (pBarriers[i].Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY))
			continue;

		mTransitions.push_back({ pBarriers[i].Transition.pResource, pBarriers[i].Transition.Subresource,
			(LONG)pBarriers[i].Transition.StateAfter });
	}

	mOrig->ResourceBarrier(NumBarriers, pBarriers);
}

// Enhanced barriers work with layouts, which are not followed
void STDMETHODCALLTYPE Dx12GraphicsCommandList::Barrier(UINT32 NumBarrierGroups, const D3D12_BARRIER_GROUP *pBarrierGroups)
{
	UINT32 i, j;

	for (i = 0; pBarrierGroups && i < NumBarrierGroups; i++) {
		const D3D12_BARRIER_GROUP *group = &pBarrierGroups[i];

		for (j = 0; j < group->NumBarriers; j++) {
			if (group->Type == D3D12_BARRIER_TYPE_TEXTURE) {
				mTransitions.push_back({ group->pTextureBarriers[j].pResource,
					D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, DX12_STATE_UNKNOWN });
			} else if (group->Type == D3D12_BARRIER_TYPE_BUFFER) {
				mTransitions.push_back({ group->pBufferBarriers[j].pResource,
					D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, DX12_STATE_UNKNOWN });
			}
		}
	}

	mOrig->Barrier(NumBarrierGroups, pBarrierGroups);
}

// The copies below are where the contents of textures and buffers come from,
// so this is where they are hashed: see TextureHash.h and BufferHash.h.
void STDMETHODCALLTYPE Dx12GraphicsCommandList::CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, UINT DstX, UINT DstY, UINT DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc, const D3D12_BOX *pSrcBox)
{
	Dx12CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
	mOrig->CopyTextureRegion(pDst, DstX, DstY, DstZ, pSrc, pSrcBox);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::CopyResource(ID3D12Resource *pDstResource, ID3D12Resource *pSrcResource)
{
	Dx12CopyResource(pDstResource, pSrcResource);
	mOrig->CopyResource(pDstResource, pSrcResource);
}

void STDMETHODCALLTYPE Dx12GraphicsCommandList::CopyBufferRegion(ID3D12Resource *pDstBuffer, UINT64 DstOffset, ID3D12Resource *pSrcBuffer, UINT64 SrcOffset, UINT64 NumBytes)
{
	Dx12CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
	mOrig->CopyBufferRegion(pDstBuffer, DstOffset, pSrcBuffer, SrcOffset, NumBytes);
}

#include "generated/Dx12GraphicsCommandList.fwd.inl"
