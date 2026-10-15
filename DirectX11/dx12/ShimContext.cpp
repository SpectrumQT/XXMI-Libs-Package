#include "ShimContext.h"

#include "DrawPath.h"
#include "ResourceTracker.h"
#include "ShimDevice.h"

template <class Slot, class Object>
static void assign(Slot *&slot, Object *object)
{
	if (object)
		object->AddRef();
	if (slot)
		slot->Release();
	slot = object;
}

template <class Object>
static void get(Object *object, Object **out)
{
	if (object)
		object->AddRef();
	*out = object;
}

ShimContext::ShimContext() :
	mRef(1),
	mFrameState(),
	mScopeState(),
	mState(&mScopeState),
	mChanged(false)
{
	ClearState();
	mState = &mFrameState;
	ClearState();
}

HRESULT STDMETHODCALLTYPE ShimContext::QueryInterface(REFIID riid, void **ppvObject)
{
	static const IID *const iids[] = { &__uuidof(IUnknown), &__uuidof(ID3D11DeviceChild),
		&__uuidof(ID3D11DeviceContext), &__uuidof(ID3D11DeviceContext1) };
	HRESULT hr = ShimQueryInterface(this, iids, ARRAYSIZE(iids), riid, ppvObject);

	if (FAILED(hr))
		Dx12LogForwardedQuery("ShimContext", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE ShimContext::AddRef()
{
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE ShimContext::Release()
{
	return InterlockedDecrement(&mRef);
}

void STDMETHODCALLTYPE ShimContext::GetDevice(ID3D11Device **ppDevice)
{
	Dx12ShimGetDevice(ppDevice);
}

HRESULT STDMETHODCALLTYPE ShimContext::GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData)
{
	return mPrivate.Get(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE ShimContext::SetPrivateData(REFGUID guid, UINT DataSize, const void *pData)
{
	return mPrivate.Set(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE ShimContext::SetPrivateDataInterface(REFGUID guid, const IUnknown *pData)
{
	return mPrivate.SetInterface(guid, pData);
}

D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE ShimContext::GetType()
{
	return D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

UINT STDMETHODCALLTYPE ShimContext::GetContextFlags()
{
	return 0;
}

void STDMETHODCALLTYPE ShimContext::Flush()
{
}

void STDMETHODCALLTYPE ShimContext::ClearState()
{
	unsigned long bit;
	UINT i;

	for (ShimStageState &stage : mState->stages) {
		assign(stage.shader, (ID3D11DeviceChild*)NULL);
		stage.shader_unbound = false;
		while (_BitScanForward(&bit, stage.cb_mask)) {
			stage.cb_mask &= stage.cb_mask - 1;
			assign(stage.cbs[bit], (ID3D11Buffer*)NULL);
		}
		memset(stage.cb_first, 0, sizeof(stage.cb_first));
		memset(stage.cb_count, 0, sizeof(stage.cb_count));
		for (UINT &mask : stage.srv_mask) {
			i = (UINT)(&mask - stage.srv_mask) * 32;
			while (_BitScanForward(&bit, mask)) {
				mask &= mask - 1;
				assign(stage.srvs[i + bit], (ID3D11ShaderResourceView*)NULL);
			}
		}
		for (i = 0; mState->samplers_set && i < SHIM_SAMPLER_SLOTS; i++)
			assign(stage.samplers[i], (ID3D11SamplerState*)NULL);
	}
	for (i = 0; mState->cs_uavs_set && i < SHIM_UAV_SLOTS; i++)
		assign(mState->cs_uavs[i], (ID3D11UnorderedAccessView*)NULL);
	for (i = 0; mState->om_uavs_set && i < SHIM_UAV_SLOTS; i++)
		assign(mState->om_uavs[i], (ID3D11UnorderedAccessView*)NULL);
	for (i = 0; i < ARRAYSIZE(mState->rtvs); i++)
		assign(mState->rtvs[i], (ID3D11RenderTargetView*)NULL);
	assign(mState->dsv, (ID3D11DepthStencilView*)NULL);
	assign(mState->blend, (ID3D11BlendState*)NULL);
	assign(mState->depth_stencil, (ID3D11DepthStencilState*)NULL);
	assign(mState->rasterizer, (ID3D11RasterizerState*)NULL);
	assign(mState->input_layout, (ID3D11InputLayout*)NULL);
	for (i = 0; mState->vbs_set && i < ARRAYSIZE(mState->vbs); i++)
		assign(mState->vbs[i], (ID3D11Buffer*)NULL);
	assign(mState->ib, (ID3D11Buffer*)NULL);

	// The stages are as ResetState would leave them by now, and they are
	// most of the state: this runs after every draw a mod changed
	memset(&mState->cs_uavs, 0, sizeof(*mState) - offsetof(ShimState, cs_uavs));
	for (FLOAT &factor : mState->blend_factor)
		factor = 1.0f;
	mState->sample_mask = 0xffffffff;
	mChanged = true;
}

void ShimContext::ResetState()
{
	memset(mState, 0, sizeof(*mState));
	for (FLOAT &factor : mState->blend_factor)
		factor = 1.0f;
	mState->sample_mask = 0xffffffff;
	mChanged = true;
}

// -----------------------------------------------------------------------------
// Shader stages

// What GetShader gives for the shader of the game's that a draw scope runs
// with, which there is no object of ours for: the engine saves it to set it
// back, and that has to be told from setting none
static ID3D11DeviceChild* game_shader()
{
	static ID3D11DeviceChild *shader = new ShimShaderObject<ID3D11DeviceChild>();

	return shader;
}

void ShimContext::SetShader(Dx12ShaderStage stage, ID3D11DeviceChild *shader)
{
	mState->stages[stage].shader_unbound = !shader;
	if (shader == game_shader())
		shader = NULL;
	assign(mState->stages[stage].shader, shader);
	mChanged = true;
}

void ShimContext::GetShader(Dx12ShaderStage stage, ID3D11DeviceChild **shader, UINT *class_instances)
{
	ID3D11DeviceChild *bound = mState->stages[stage].shader;

	if (!bound && !mState->stages[stage].shader_unbound && Dx12InDrawScope() && Dx12ScopeHasShader(stage))
		bound = game_shader();
	if (shader)
		get(bound, shader);
	if (class_instances)
		*class_instances = 0;
}

void ShimContext::SetConstantBuffers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11Buffer *const *buffers,
		const UINT *first, const UINT *constants)
{
	ShimStageState *state = &mState->stages[stage];
	UINT i;

	for (i = 0; i < count && start + i < SHIM_CBV_SLOTS; i++) {
		assign(state->cbs[start + i], buffers ? buffers[i] : NULL);
		state->cb_first[start + i] = first ? first[i] : 0;
		state->cb_count[start + i] = constants ? constants[i] : 0;
		if (state->cbs[start + i])
			state->cb_mask |= 1u << (start + i);
		else
			state->cb_mask &= ~(1u << (start + i));
	}
	mChanged = true;
}

void ShimContext::GetConstantBuffers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11Buffer **buffers,
		UINT *first, UINT *constants)
{
	ShimStageState *state = &mState->stages[stage];
	ShimResourceDesc desc;
	ID3D11Buffer *buffer;
	UINT i, slot;

	for (i = 0; i < count; i++) {
		slot = start + i;
		buffer = slot < SHIM_CBV_SLOTS ? state->cbs[slot] : NULL;
		if (!buffer && slot < SHIM_CBV_SLOTS && Dx12InDrawScope())
			buffer = Dx12ScopeConstantBuffer(stage, slot);
		if (buffers)
			get(buffer, &buffers[i]);
		if (first)
			first[i] = slot < SHIM_CBV_SLOTS ? state->cb_first[slot] : 0;
		if (!constants)
			continue;

		constants[i] = slot < SHIM_CBV_SLOTS ? state->cb_count[slot] : 0;
		// D3D11 reports the size of the buffer when no range was set
		if (!constants[i] && buffer) {
			ShimGetResourceDesc(buffer, &desc);
			constants[i] = desc.byte_width / 16;
		}
	}
}

// D3D11 does not let a resource be read by a shader while it is being
// rendered to: whichever was bound first gives way. Mods rely on that to swap
// a texture between the two roles without unbinding it.
bool ShimContext::IsOutput(ShimResource *resource)
{
	ShimView *view;

	for (ID3D11RenderTargetView *rtv : mState->rtvs) {
		if (rtv && ShimViewOf(rtv)->resource == resource)
			return true;
	}
	view = ShimViewOf(mState->dsv);
	return view && !view->read_only && view->resource == resource;
}

void ShimContext::UnbindInput(ShimResource *resource)
{
	unsigned long bit;
	UINT word, mask, slot;

	for (ShimStageState &stage : mState->stages) {
		for (word = 0; word < ARRAYSIZE(stage.srv_mask); word++) {
			for (mask = stage.srv_mask[word]; _BitScanForward(&bit, mask); mask &= mask - 1) {
				slot = word * 32 + bit;
				if (ShimViewOf(stage.srvs[slot])->resource != resource)
					continue;
				assign(stage.srvs[slot], (ID3D11ShaderResourceView*)NULL);
				stage.srv_mask[word] &= ~(1u << bit);
			}
		}
	}
	mChanged = true;
}

void ShimContext::SetShaderResources(Dx12ShaderStage stage, UINT start, UINT count, ID3D11ShaderResourceView *const *views)
{
	ShimStageState *state = &mState->stages[stage];
	ID3D11ShaderResourceView *view;
	UINT i;

	for (i = 0; i < count && start + i < SHIM_SRV_SLOTS; i++) {
		view = views ? views[i] : NULL;
		if (view && IsOutput(ShimViewOf(view)->resource))
			view = NULL;
		assign(state->srvs[start + i], view);
		if (view)
			state->srv_mask[(start + i) / 32] |= 1u << ((start + i) & 31);
		else
			state->srv_mask[(start + i) / 32] &= ~(1u << ((start + i) & 31));
	}
	mChanged = true;
}

void ShimContext::GetShaderResources(Dx12ShaderStage stage, UINT start, UINT count, ID3D11ShaderResourceView **views)
{
	ID3D11ShaderResourceView *view;
	UINT i;

	for (i = 0; i < count; i++) {
		view = start + i < SHIM_SRV_SLOTS ? mState->stages[stage].srvs[start + i] : NULL;
		// What a mod has not bound over is still the game's
		if (!view && start + i < SHIM_SRV_SLOTS && Dx12InDrawScope())
			view = Dx12ScopeShaderResource(stage, start + i);
		get(view, &views[i]);
	}
}

void ShimContext::SetSamplers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11SamplerState *const *samplers)
{
	UINT i;

	for (i = 0; i < count && start + i < SHIM_SAMPLER_SLOTS; i++)
		assign(mState->stages[stage].samplers[start + i], samplers ? samplers[i] : NULL);
	mState->samplers_set = true;
	mChanged = true;
}

void ShimContext::GetSamplers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11SamplerState **samplers)
{
	UINT i;

	for (i = 0; i < count; i++)
		get(start + i < SHIM_SAMPLER_SLOTS ? mState->stages[stage].samplers[start + i] : NULL, &samplers[i]);
}

#define SHIM_STAGE_METHODS(XS, stage, Shader) \
void STDMETHODCALLTYPE ShimContext::XS##SetShader(Shader *pShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) \
{ \
	SetShader(stage, pShader); \
} \
void STDMETHODCALLTYPE ShimContext::XS##GetShader(Shader **ppShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances) \
{ \
	GetShader(stage, (ID3D11DeviceChild**)ppShader, pNumClassInstances); \
} \
void STDMETHODCALLTYPE ShimContext::XS##SetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) \
{ \
	SetConstantBuffers(stage, StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL); \
} \
void STDMETHODCALLTYPE ShimContext::XS##GetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) \
{ \
	GetConstantBuffers(stage, StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL); \
} \
void STDMETHODCALLTYPE ShimContext::XS##SetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant, const UINT *pNumConstants) \
{ \
	SetConstantBuffers(stage, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); \
} \
void STDMETHODCALLTYPE ShimContext::XS##GetConstantBuffers1(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants) \
{ \
	GetConstantBuffers(stage, StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants); \
} \
void STDMETHODCALLTYPE ShimContext::XS##SetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) \
{ \
	SetShaderResources(stage, StartSlot, NumViews, ppShaderResourceViews); \
} \
void STDMETHODCALLTYPE ShimContext::XS##GetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) \
{ \
	GetShaderResources(stage, StartSlot, NumViews, ppShaderResourceViews); \
} \
void STDMETHODCALLTYPE ShimContext::XS##SetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) \
{ \
	SetSamplers(stage, StartSlot, NumSamplers, ppSamplers); \
} \
void STDMETHODCALLTYPE ShimContext::XS##GetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) \
{ \
	GetSamplers(stage, StartSlot, NumSamplers, ppSamplers); \
}

SHIM_STAGE_METHODS(VS, DX12_STAGE_VS, ID3D11VertexShader)
SHIM_STAGE_METHODS(PS, DX12_STAGE_PS, ID3D11PixelShader)
SHIM_STAGE_METHODS(GS, DX12_STAGE_GS, ID3D11GeometryShader)
SHIM_STAGE_METHODS(HS, DX12_STAGE_HS, ID3D11HullShader)
SHIM_STAGE_METHODS(DS, DX12_STAGE_DS, ID3D11DomainShader)
SHIM_STAGE_METHODS(CS, DX12_STAGE_CS, ID3D11ComputeShader)

void STDMETHODCALLTYPE ShimContext::CSSetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts)
{
	UINT i;

	for (i = 0; i < NumUAVs && StartSlot + i < SHIM_UAV_SLOTS; i++)
		assign(mState->cs_uavs[StartSlot + i], ppUnorderedAccessViews ? ppUnorderedAccessViews[i] : NULL);
	mState->cs_uavs_set = true;
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView **ppUnorderedAccessViews)
{
	ID3D11UnorderedAccessView *view;
	UINT i;

	for (i = 0; i < NumUAVs; i++) {
		view = StartSlot + i < SHIM_UAV_SLOTS ? mState->cs_uavs[StartSlot + i] : NULL;
		if (!view && StartSlot + i < SHIM_UAV_SLOTS && Dx12InDrawScope())
			view = Dx12ScopeUnorderedAccess(StartSlot + i);
		get(view, &ppUnorderedAccessViews[i]);
	}
}

// -----------------------------------------------------------------------------
// Input assembler

void STDMETHODCALLTYPE ShimContext::IASetInputLayout(ID3D11InputLayout *pInputLayout)
{
	assign(mState->input_layout, pInputLayout);
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::IAGetInputLayout(ID3D11InputLayout **ppInputLayout)
{
	get(mState->input_layout, ppInputLayout);
}

void STDMETHODCALLTYPE ShimContext::IASetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppVertexBuffers, const UINT *pStrides, const UINT *pOffsets)
{
	UINT i;

	mState->vbs_set = true;
	for (i = 0; i < NumBuffers && StartSlot + i < ARRAYSIZE(mState->vbs); i++) {
		assign(mState->vbs[StartSlot + i], ppVertexBuffers ? ppVertexBuffers[i] : NULL);
		mState->vb_strides[StartSlot + i] = pStrides ? pStrides[i] : 0;
		mState->vb_offsets[StartSlot + i] = pOffsets ? pOffsets[i] : 0;
	}
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppVertexBuffers, UINT *pStrides, UINT *pOffsets)
{
	ID3D11Buffer *buffer;
	UINT i, stride, offset;
	bool valid;

	for (i = 0; i < NumBuffers; i++) {
		valid = StartSlot + i < ARRAYSIZE(mState->vbs);
		buffer = valid ? mState->vbs[StartSlot + i] : NULL;
		stride = valid ? mState->vb_strides[StartSlot + i] : 0;
		offset = valid ? mState->vb_offsets[StartSlot + i] : 0;
		if (!buffer && valid && Dx12InDrawScope()) {
			buffer = Dx12ScopeVertexBuffer(StartSlot + i, &stride);
			offset = 0;
		}

		if (ppVertexBuffers)
			get(buffer, &ppVertexBuffers[i]);
		if (pStrides)
			pStrides[i] = stride;
		if (pOffsets)
			pOffsets[i] = offset;
	}
}

void STDMETHODCALLTYPE ShimContext::IASetIndexBuffer(ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset)
{
	assign(mState->ib, pIndexBuffer);
	mState->ib_format = Format;
	mState->ib_offset = Offset;
	mState->ib_unbound = !pIndexBuffer;
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::IAGetIndexBuffer(ID3D11Buffer **pIndexBuffer, DXGI_FORMAT *Format, UINT *Offset)
{
	ID3D11Buffer *buffer = mState->ib;
	DXGI_FORMAT format = mState->ib_format;
	UINT offset = mState->ib_offset;

	if (!buffer && !mState->ib_unbound && Dx12InDrawScope()) {
		buffer = Dx12ScopeIndexBuffer(&format);
		offset = 0;
	}

	if (pIndexBuffer)
		get(buffer, pIndexBuffer);
	if (Format)
		*Format = format;
	if (Offset)
		*Offset = offset;
}

void STDMETHODCALLTYPE ShimContext::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology)
{
	mState->topology = Topology;
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY *pTopology)
{
	*pTopology = mState->topology;
}

// -----------------------------------------------------------------------------
// Output merger and rasterizer

void ShimContext::SetRenderTargets(UINT count, ID3D11RenderTargetView *const *views, ID3D11DepthStencilView *depth_stencil)
{
	ShimView *view;
	UINT i;

	mChanged = true;
	for (i = 0; i < ARRAYSIZE(mState->rtvs); i++) {
		assign(mState->rtvs[i], i < count && views ? views[i] : NULL);
		if (mState->rtvs[i])
			UnbindInput(ShimViewOf(mState->rtvs[i])->resource);
	}

	assign(mState->dsv, depth_stencil);
	view = ShimViewOf(depth_stencil);
	if (view && !view->read_only)
		UnbindInput(view->resource);
}

void STDMETHODCALLTYPE ShimContext::OMSetRenderTargets(UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView)
{
	UINT i;

	SetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
	for (i = 0; mState->om_uavs_set && i < SHIM_UAV_SLOTS; i++)
		assign(mState->om_uavs[i], (ID3D11UnorderedAccessView*)NULL);
	mState->om_uavs_set = false;
}

void STDMETHODCALLTYPE ShimContext::OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews, const UINT *pUAVInitialCounts)
{
	UINT i;

	if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL)
		SetRenderTargets(NumRTVs, ppRenderTargetViews, pDepthStencilView);
	if (NumUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
		return;
	mChanged = true;
	mState->om_uavs_set = true;

	for (i = 0; i < SHIM_UAV_SLOTS; i++) {
		assign(mState->om_uavs[i], i >= UAVStartSlot && i - UAVStartSlot < NumUAVs && ppUnorderedAccessViews
			? ppUnorderedAccessViews[i - UAVStartSlot] : NULL);
	}
}

void STDMETHODCALLTYPE ShimContext::OMGetRenderTargets(UINT NumViews, ID3D11RenderTargetView **ppRenderTargetViews, ID3D11DepthStencilView **ppDepthStencilView)
{
	UINT i;

	for (i = 0; i < NumViews && ppRenderTargetViews; i++)
		get(i < ARRAYSIZE(mState->rtvs) ? mState->rtvs[i] : NULL, &ppRenderTargetViews[i]);
	if (ppDepthStencilView)
		get(mState->dsv, ppDepthStencilView);
}

void STDMETHODCALLTYPE ShimContext::OMGetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs, ID3D11RenderTargetView **ppRenderTargetViews, ID3D11DepthStencilView **ppDepthStencilView, UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView **ppUnorderedAccessViews)
{
	UINT i;

	OMGetRenderTargets(NumRTVs, ppRenderTargetViews, ppDepthStencilView);
	for (i = 0; i < NumUAVs && ppUnorderedAccessViews; i++)
		get(UAVStartSlot + i < SHIM_UAV_SLOTS ? mState->om_uavs[UAVStartSlot + i] : NULL, &ppUnorderedAccessViews[i]);
}

void STDMETHODCALLTYPE ShimContext::OMSetBlendState(ID3D11BlendState *pBlendState, const FLOAT BlendFactor[4], UINT SampleMask)
{
	UINT i;

	assign(mState->blend, pBlendState);
	for (i = 0; i < 4; i++)
		mState->blend_factor[i] = BlendFactor ? BlendFactor[i] : 1.0f;
	mState->sample_mask = SampleMask;
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::OMGetBlendState(ID3D11BlendState **ppBlendState, FLOAT BlendFactor[4], UINT *pSampleMask)
{
	if (ppBlendState)
		get(mState->blend, ppBlendState);
	if (BlendFactor)
		memcpy(BlendFactor, mState->blend_factor, sizeof(mState->blend_factor));
	if (pSampleMask)
		*pSampleMask = mState->sample_mask;
}

void STDMETHODCALLTYPE ShimContext::OMSetDepthStencilState(ID3D11DepthStencilState *pDepthStencilState, UINT StencilRef)
{
	assign(mState->depth_stencil, pDepthStencilState);
	mState->stencil_ref = StencilRef;
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::OMGetDepthStencilState(ID3D11DepthStencilState **ppDepthStencilState, UINT *pStencilRef)
{
	if (ppDepthStencilState)
		get(mState->depth_stencil, ppDepthStencilState);
	if (pStencilRef)
		*pStencilRef = mState->stencil_ref;
}

void STDMETHODCALLTYPE ShimContext::SOSetTargets(UINT NumBuffers, ID3D11Buffer *const *ppSOTargets, const UINT *pOffsets)
{
	static bool reported;
	UINT i;

	// The engine sets these back to nothing when it restores state
	for (i = 0; i < NumBuffers && ppSOTargets; i++) {
		if (ppSOTargets[i])
			Dx12ShimNotImplemented("ShimContext::SOSetTargets", &reported);
	}
}

void STDMETHODCALLTYPE ShimContext::SOGetTargets(UINT NumBuffers, ID3D11Buffer **ppSOTargets)
{
	UINT i;

	for (i = 0; i < NumBuffers; i++)
		get(!i && Dx12InDrawScope() ? Dx12ScopeStreamOutput() : NULL, &ppSOTargets[i]);
}

void STDMETHODCALLTYPE ShimContext::RSSetState(ID3D11RasterizerState *pRasterizerState)
{
	assign(mState->rasterizer, pRasterizerState);
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::RSGetState(ID3D11RasterizerState **ppRasterizerState)
{
	get(mState->rasterizer, ppRasterizerState);
}

void STDMETHODCALLTYPE ShimContext::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT *pViewports)
{
	mState->viewport_count = min(NumViewports, (UINT)ARRAYSIZE(mState->viewports));
	if (pViewports)
		memcpy(mState->viewports, pViewports, mState->viewport_count * sizeof(D3D11_VIEWPORT));
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::RSGetViewports(UINT *pNumViewports, D3D11_VIEWPORT *pViewports)
{
	if (pViewports) {
		memset(pViewports, 0, *pNumViewports * sizeof(D3D11_VIEWPORT));
		memcpy(pViewports, mState->viewports, min(*pNumViewports, mState->viewport_count) * sizeof(D3D11_VIEWPORT));
	}
	*pNumViewports = mState->viewport_count;
}

void STDMETHODCALLTYPE ShimContext::RSSetScissorRects(UINT NumRects, const D3D11_RECT *pRects)
{
	mState->scissor_count = min(NumRects, (UINT)ARRAYSIZE(mState->scissors));
	if (pRects)
		memcpy(mState->scissors, pRects, mState->scissor_count * sizeof(D3D11_RECT));
	mChanged = true;
}

void STDMETHODCALLTYPE ShimContext::RSGetScissorRects(UINT *pNumRects, D3D11_RECT *pRects)
{
	if (pRects) {
		memset(pRects, 0, *pNumRects * sizeof(D3D11_RECT));
		memcpy(pRects, mState->scissors, min(*pNumRects, mState->scissor_count) * sizeof(D3D11_RECT));
	}
	*pNumRects = mState->scissor_count;
}

// -----------------------------------------------------------------------------
// Draws and clears

void STDMETHODCALLTYPE ShimContext::Draw(UINT VertexCount, UINT StartVertexLocation)
{
	if (Dx12InDrawScope())
		Dx12ScopeDraw(false, VertexCount, 1, StartVertexLocation, 0, 0);
	else if (Dx12ShimPrepareDraw(mState, false))
		Dx12ShimList()->DrawInstanced(VertexCount, 1, StartVertexLocation, 0);
}

void STDMETHODCALLTYPE ShimContext::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
	if (Dx12InDrawScope())
		Dx12ScopeDraw(false, VertexCountPerInstance, InstanceCount, StartVertexLocation, 0, StartInstanceLocation);
	else if (Dx12ShimPrepareDraw(mState, false))
		Dx12ShimList()->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
}

void STDMETHODCALLTYPE ShimContext::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
	if (Dx12InDrawScope())
		Dx12ScopeDraw(true, IndexCount, 1, StartIndexLocation, BaseVertexLocation, 0);
	else if (Dx12ShimPrepareDraw(mState, true))
		Dx12ShimList()->DrawIndexedInstanced(IndexCount, 1, StartIndexLocation, BaseVertexLocation, 0);
}

void STDMETHODCALLTYPE ShimContext::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
	if (Dx12InDrawScope()) {
		Dx12ScopeDraw(true, IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation);
	} else if (Dx12ShimPrepareDraw(mState, true)) {
		Dx12ShimList()->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation,
			BaseVertexLocation, StartInstanceLocation);
	}
}

bool ShimContext::DrawApart(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
	static const Dx12ShaderStage stages[] = { DX12_STAGE_VS, DX12_STAGE_PS };
	static const D3D11_VIEWPORT viewport = { 0, 0, 4096, 4096, 0, 1 };
	UINT cbs[ARRAYSIZE(stages)] = {}, i, slot;
	bool viewport_lent = !mState->viewport_count, prepared;

	// As for a dispatch: the shaders find the constant buffers of the game
	// wherever the mod bound none
	for (i = 0; i < ARRAYSIZE(stages); i++) {
		ShimStageState *stage = &mState->stages[stages[i]];
		const ShimShader *shader = ShimShaderOf(stage->shader);

		for (slot = 0; shader && slot < SHIM_CBV_SLOTS; slot++) {
			if ((shader->cbv_used & (1u << slot)) && !stage->cbs[slot]
			    && (stage->cbs[slot] = Dx12ScopeConstantBuffer(stages[i], slot)))
				cbs[i] |= 1u << slot;
		}
	}
	if (viewport_lent) {
		mState->viewports[0] = viewport;
		mState->viewport_count = 1;
	}
	prepared = Dx12ShimPrepareDraw(mState, indexed);
	if (viewport_lent)
		mState->viewport_count = 0;
	for (i = 0; i < ARRAYSIZE(stages); i++) {
		for (slot = 0; slot < SHIM_CBV_SLOTS; slot++) {
			if (cbs[i] & (1u << slot))
				mState->stages[stages[i]].cbs[slot] = NULL;
		}
	}
	if (!prepared)
		return false;

	if (indexed)
		Dx12ShimList()->DrawIndexedInstanced(count, instances, start, base_vertex, start_instance);
	else
		Dx12ShimList()->DrawInstanced(count, instances, start, start_instance);
	return true;
}

void STDMETHODCALLTYPE ShimContext::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
	// A compute shader of a mod runs on our list like any other, and so
	// before the game's draw. What it writes that an earlier draw of the
	// game uses goes to another resource (see Bridge.h), so that draw sees
	// what it did when it was recorded. The game's own cannot be run
	// again: its pipeline state would have to be on our list.
	if (Dx12InDrawScope() && !mState->stages[DX12_STAGE_CS].shader) {
		Dx12ScopeUnsupported("to dispatch without a compute shader of its own");
		return;
	}
	if (Dx12InDrawScope()) {
		// Under D3D11 the shader finds what the game has bound wherever
		// the mod bound nothing. Gone from the state before anything
		// else looks at it. The views are referenced until then: asking
		// for the next may have the one before made again.
		ShimStageState *stage = &mState->stages[DX12_STAGE_CS];
		const ShimShader *shader = ShimShaderOf(stage->shader);
		UINT64 uavs = 0;
		UINT cbs = 0, slot;
		bool prepared;

		for (slot = 0; slot < SHIM_CBV_SLOTS; slot++) {
			if ((shader->cbv_used & (1u << slot)) && !stage->cbs[slot] && (stage->cbs[slot] = Dx12ScopeConstantBuffer(DX12_STAGE_CS, slot)))
				cbs |= 1u << slot;
		}
		for (slot = 0; slot < SHIM_UAV_SLOTS; slot++) {
			if ((shader->uav_used & (1ull << slot)) && !mState->cs_uavs[slot] && (mState->cs_uavs[slot] = Dx12ScopeWrittenBuffer(slot))) {
				mState->cs_uavs[slot]->AddRef();
				uavs |= 1ull << slot;
			}
		}
		prepared = Dx12ShimPrepareDispatch(mState);
		for (slot = 0; slot < SHIM_CBV_SLOTS; slot++) {
			if (cbs & (1u << slot))
				stage->cbs[slot] = NULL;
		}
		for (slot = 0; slot < SHIM_UAV_SLOTS; slot++) {
			if (uavs & (1ull << slot)) {
				mState->cs_uavs[slot]->Release();
				mState->cs_uavs[slot] = NULL;
			}
		}
		if (!prepared) {
			Dx12ScopeDispatched(false);
			return;
		}
	} else if (!Dx12ShimPrepareDispatch(mState)) {
		return;
	}

	Dx12ShimList()->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
	if (Dx12InDrawScope())
		Dx12ScopeDispatched(true);
}

void STDMETHODCALLTYPE ShimContext::ClearRenderTargetView(ID3D11RenderTargetView *pRenderTargetView, const FLOAT ColorRGBA[4])
{
	ShimView *view = ShimViewOf(pRenderTargetView);

	if (!view || !Dx12ShimTransition(view->resource, D3D12_RESOURCE_STATE_RENDER_TARGET))
		return;

	Dx12ShimList()->ClearRenderTargetView(view->handle, ColorRGBA, 0, NULL);
}

void STDMETHODCALLTYPE ShimContext::ClearDepthStencilView(ID3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
	ShimView *view = ShimViewOf(pDepthStencilView);

	if (!view || !Dx12ShimTransition(view->resource, D3D12_RESOURCE_STATE_DEPTH_WRITE))
		return;

	// D3D12_CLEAR_FLAGS has the values of D3D11_CLEAR_FLAG
	Dx12ShimList()->ClearDepthStencilView(view->handle, (D3D12_CLEAR_FLAGS)ClearFlags, Depth, Stencil, 0, NULL);
}

// Clearing through an unordered access view takes its descriptor both ways:
// as it is, and in a heap that is set on the list.
static bool prepare_uav_clear(ShimView *view, D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;

	if (!view || !view->resource->resource)
		return false;

	cpu = Dx12ShimAllocViews(1, gpu);
	if (!cpu.ptr)
		return false;
	// Before the descriptor is read: the transition may write it again
	if (!Dx12ShimTransition(view->resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
		return false;
	dx12_shim->device->CopyDescriptorsSimple(1, cpu, view->handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	Dx12ShimBindHeaps();
	return true;
}

void STDMETHODCALLTYPE ShimContext::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView *pUnorderedAccessView, const UINT Values[4])
{
	ShimView *view = ShimViewOf(pUnorderedAccessView);
	D3D12_UNORDERED_ACCESS_VIEW_DESC words = {};
	D3D12_CPU_DESCRIPTOR_HANDLE cpu, shown;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	UINT stride;

	if (!view || !view->resource->resource)
		return;

	if (view->resource->dimension != D3D11_RESOURCE_DIMENSION_BUFFER || view->format != DXGI_FORMAT_UNKNOWN) {
		if (prepare_uav_clear(view, &gpu))
			Dx12ShimList()->ClearUnorderedAccessViewUint(gpu, view->handle, view->resource->resource, Values, 0, NULL);
		return;
	}

	// D3D12 does not clear through the view of a structured buffer, where
	// D3D11 writes the first value to every 32 bits of it
	const D3D11_BUFFER_UAV &buffer = static_cast<ShimUnorderedAccessView*>(pUnorderedAccessView)->mDesc.Buffer;
	stride = static_cast<ShimBuffer*>(view->resource11)->mDesc.StructureByteStride;
	if ((buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) || !stride || stride % 4)
		return;
	words.Format = DXGI_FORMAT_R32_UINT;
	words.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	words.Buffer.FirstElement = (UINT64)buffer.FirstElement * stride / 4;
	words.Buffer.NumElements = buffer.NumElements * (stride / 4);

	cpu = Dx12ShimAllocDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	shown = Dx12ShimAllocViews(1, &gpu);
	if (cpu.ptr && shown.ptr && Dx12ShimTransition(view->resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
		dx12_shim->device->CreateUnorderedAccessView(view->resource->resource, NULL, &words, cpu);
		dx12_shim->device->CopyDescriptorsSimple(1, shown, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		Dx12ShimBindHeaps();
		Dx12ShimList()->ClearUnorderedAccessViewUint(gpu, cpu, view->resource->resource, Values, 0, NULL);
	}
	if (cpu.ptr)
		Dx12ShimFreeDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, cpu);
}

void STDMETHODCALLTYPE ShimContext::ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView *pUnorderedAccessView, const FLOAT Values[4])
{
	ShimView *view = ShimViewOf(pUnorderedAccessView);
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;

	if (prepare_uav_clear(view, &gpu))
		Dx12ShimList()->ClearUnorderedAccessViewFloat(gpu, view->handle, view->resource->resource, Values, 0, NULL);
}

// -----------------------------------------------------------------------------
// Resource contents

static bool is_buffer(ShimResource *resource)
{
	return resource->dimension == D3D11_RESOURCE_DIMENSION_BUFFER;
}

static UINT texture_shadow_pitch(ShimResource *resource, UINT *rows)
{
	UINT64 row_size;

	ShimFootprint(resource, NULL, rows, &row_size, NULL);
	return (UINT)row_size;
}

// What the GPU copied into a staging buffer becomes its contents
static HRESULT fetch_readback(ShimResource *resource, UINT byte_width, bool wait)
{
	D3D12_RANGE range = { 0, byte_width }, no_write = {};
	void *data;

	if (!resource->readback_pending)
		return S_OK;

	if (!Dx12ShimFenceReached(resource->readback_fence)) {
		if (!wait)
			return DXGI_ERROR_WAS_STILL_DRAWING;
		Dx12ShimSubmitAndWait();
	}

	resource->readback_pending = false;
	if (FAILED(resource->readback->Map(0, &range, &data)))
		return E_FAIL;
	memcpy(resource->shadow.data(), data, byte_width);
	resource->readback->Unmap(0, &no_write);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE ShimContext::Map(ID3D11Resource *pResource, UINT Subresource, D3D11_MAP MapType, UINT MapFlags, D3D11_MAPPED_SUBRESOURCE *pMappedResource)
{
	static bool reported;
	ShimResource *resource = ShimResourceOf(pResource);
	ShimResourceDesc desc;
	UINT rows;
	HRESULT hr;

	if (!resource || !pMappedResource)
		return E_INVALIDARG;
	*pMappedResource = {};

	if (resource->shadow.empty() || Subresource) {
		Dx12ShimNotImplemented("ShimContext::Map of a resource that is not a buffer or a dynamic texture", &reported);
		return E_NOTIMPL;
	}

	if (is_buffer(resource)) {
		ShimGetResourceDesc(pResource, &desc);
		hr = fetch_readback(resource, desc.byte_width, !(MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT));
		if (FAILED(hr))
			return hr;
		pMappedResource->RowPitch = desc.byte_width;
		pMappedResource->DepthPitch = desc.byte_width;
	} else {
		pMappedResource->RowPitch = texture_shadow_pitch(resource, &rows);
		pMappedResource->DepthPitch = pMappedResource->RowPitch * rows;
	}

	ShimReadShadow(resource);
	pMappedResource->pData = resource->shadow.data();
	resource->mapped_for_write = MapType != D3D11_MAP_READ;
	return S_OK;
}

void STDMETHODCALLTYPE ShimContext::Unmap(ID3D11Resource *pResource, UINT Subresource)
{
	ShimResource *resource = ShimResourceOf(pResource);
	ShimResourceDesc desc;

	if (!resource || !resource->mapped_for_write)
		return;
	resource->mapped_for_write = false;

	if (resource->cpu_only) {
		resource->version++;
	} else if (is_buffer(resource)) {
		ShimGetResourceDesc(pResource, &desc);
		ShimWriteBuffer(resource, 0, resource->shadow.data(), desc.byte_width);
		resource->shadow_version = resource->version;
	} else {
		resource->write_pending = true;
	}
}

void STDMETHODCALLTYPE ShimContext::UpdateSubresource(ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
	static bool reported;
	ShimResource *resource = ShimResourceOf(pDstResource);
	ShimResourceDesc desc;
	bool shadow_whole;
	UINT offset, size;

	if (!resource || !pSrcData)
		return;

	if (!is_buffer(resource)) {
		if (pDstBox)
			Dx12ShimNotImplemented("ShimContext::UpdateSubresource of part of a texture", &reported);
		else
			ShimWriteSubresource(resource, DstSubresource, pSrcData, SrcRowPitch, SrcDepthPitch);
		return;
	}

	ShimGetResourceDesc(pDstResource, &desc);
	offset = pDstBox ? pDstBox->left : 0;
	size = pDstBox ? pDstBox->right - pDstBox->left : desc.byte_width;
	if (offset >= desc.byte_width)
		return;
	size = min(size, desc.byte_width - offset);

	VkScopeFinishRefresh(resource);
	shadow_whole = !resource->shadow.empty() && resource->shadow_version == resource->version;
	if (!resource->shadow.empty())
		memcpy(resource->shadow.data() + offset, pSrcData, size);
	if (resource->cpu_only)
		resource->version++;
	else
		ShimWriteBuffer(resource, offset, pSrcData, size);
	if (shadow_whole)
		resource->shadow_version = resource->version;
}

void STDMETHODCALLTYPE ShimContext::UpdateSubresource1(ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags)
{
	UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

static bool create_readback(ShimResource *resource)
{
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_DESC desc = {};

	if (resource->readback)
		return true;

	heap.Type = D3D12_HEAP_TYPE_READBACK;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = resource->shadow.size();
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	return SUCCEEDED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&resource->readback)));
}

// A buffer of the game has no resource to its name, only the address the draw
// in the scope has it at: a constant buffer in upload memory (foreign_va),
// which holds what the draw reads, or a vertex or index buffer (game_va). The
// latter holds that too if nothing of the game writes it, and otherwise what
// it held at the end of the frame before: our list is executed first. A mod's
// compute shader that reads the vertices of a mesh the game deforms every
// frame, to deform its own along, so lags a frame behind; reading them where
// the game has them would take its dispatch on the game's list. The memory is
// the game's to hand out again once its list is done, so it is kept until
// ours is.
static ID3D12Resource* game_source(ShimResource *src, UINT64 *offset)
{
	UINT64 va = src->foreign_va ? src->foreign_va : src->game_va;
	ID3D12Resource *resource;
	Dx12ResourceInfo info;

	if (!src->foreign || src->resource || !va || !Dx12FindBuffer(va, &resource, offset))
		return NULL;
	// Never a state a copy can read
	if (!Dx12GetResourceInfo(resource, &info) || info.heap_type == D3D12_HEAP_TYPE_READBACK)
		return NULL;

	Dx12ShimDeferRelease(resource);
	return resource;
}

static void copy_buffer(ID3D11Resource *dst11, UINT64 dst_offset, ID3D11Resource *src11, UINT64 src_offset, UINT64 size)
{
	static bool from_staging_reported;
	ShimResource *dst = ShimResourceOf(dst11), *src = ShimResourceOf(src11);
	ShimResourceDesc dst_desc, src_desc;
	ID3D12Resource *game;
	UINT64 game_offset;
	bool src_in_shadow;

	// The vertices skinned for a mod are in no buffer a list of ours could
	// copy from, and are not there before the game's list is executed. A
	// mod copies them to draw them again, for which knowing whose they are
	// will do.
	dst->skin_va = src->foreign ? src->skin_va : 0;

	ShimGetResourceDesc(dst11, &dst_desc);
	ShimGetResourceDesc(src11, &src_desc);
	if (src_offset >= src_desc.byte_width || dst_offset >= dst_desc.byte_width)
		return;
	size = min(size, min(src_desc.byte_width - src_offset, dst_desc.byte_width - dst_offset));

	ShimUploadShadow(src);
	src_in_shadow = src->cpu_only && !src->readback_pending;
	ShimReadShadow(src, src_offset, size);
	if (src->cpu_only && !src_in_shadow) {
		// Possible once the GPU is done, but nothing asks for it
		Dx12ShimNotImplemented("Copying from a staging buffer the GPU has not filled yet", &from_staging_reported);
		return;
	}

	// A constant or dynamic buffer has its contents uploaded from the CPU
	// for every draw. One the GPU fills in is a resource from then on.
	if (dst->cpu_only && !src_in_shadow && dst_desc.usage != D3D11_USAGE_STAGING && !ShimBufferToGpu(dst))
		return;

	if (dst->cpu_only) {
		if (src_in_shadow) {
			VkScopeFinishRefresh(dst);
			memcpy(dst->shadow.data() + dst_offset, src->shadow.data() + src_offset, (size_t)size);
			dst->version++;
		} else if (src->resource && create_readback(dst) && Dx12ShimTransition(src, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
			Dx12ShimList()->CopyBufferRegion(dst->readback, dst_offset, src->resource, src_offset, size);
			dst->readback_fence = Dx12ShimPendingFence();
			dst->readback_pending = true;
		}
		return;
	}

	if (src_in_shadow) {
		ShimWriteBuffer(dst, dst_offset, src->shadow.data() + src_offset, size);
	} else if (dst->resource && (game = game_source(src, &game_offset))) {
		if (Dx12ShimTransition(dst, D3D12_RESOURCE_STATE_COPY_DEST))
			Dx12ShimList()->CopyBufferRegion(dst->resource, dst_offset, game, game_offset + src_offset, size);
	} else if (src->resource && dst->resource && src != dst && Dx12ShimTransition(src, D3D12_RESOURCE_STATE_COPY_SOURCE)
		   && Dx12ShimTransition(dst, D3D12_RESOURCE_STATE_COPY_DEST)) {
		Dx12ShimList()->CopyBufferRegion(dst->resource, dst_offset, src->resource, src_offset, size);
	}
}

void STDMETHODCALLTYPE ShimContext::CopySubresourceRegion(ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource, const D3D11_BOX *pSrcBox)
{
	ShimResource *dst = ShimResourceOf(pDstResource), *src = ShimResourceOf(pSrcResource);
	D3D12_TEXTURE_COPY_LOCATION dst_location = {}, src_location = {};

	if (!dst || !src || is_buffer(dst) != is_buffer(src))
		return;

	if (is_buffer(dst)) {
		if (pSrcBox)
			copy_buffer(pDstResource, DstX, pSrcResource, pSrcBox->left, pSrcBox->right - pSrcBox->left);
		else
			copy_buffer(pDstResource, DstX, pSrcResource, 0, UINT64_MAX);
		return;
	}

	if (dst == src || !Dx12ShimTransition(src, D3D12_RESOURCE_STATE_COPY_SOURCE)
	    || !Dx12ShimTransition(dst, D3D12_RESOURCE_STATE_COPY_DEST))
		return;

	dst_location.pResource = dst->resource;
	dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_location.SubresourceIndex = DstSubresource;
	src_location.pResource = src->resource;
	src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_location.SubresourceIndex = SrcSubresource;
	// D3D12_BOX is the same struct as D3D11_BOX
	Dx12ShimList()->CopyTextureRegion(&dst_location, DstX, DstY, DstZ, &src_location, (const D3D12_BOX*)pSrcBox);
}

void STDMETHODCALLTYPE ShimContext::CopySubresourceRegion1(ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource, UINT SrcSubresource, const D3D11_BOX *pSrcBox, UINT CopyFlags)
{
	CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox);
}

void STDMETHODCALLTYPE ShimContext::CopyResource(ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource)
{
	ShimResource *dst = ShimResourceOf(pDstResource), *src = ShimResourceOf(pSrcResource);

	if (!dst || !src || is_buffer(dst) != is_buffer(src))
		return;

	if (is_buffer(dst)) {
		copy_buffer(pDstResource, 0, pSrcResource, 0, UINT64_MAX);
		return;
	}

	if (dst == src || !Dx12ShimTransition(src, D3D12_RESOURCE_STATE_COPY_SOURCE)
	    || !Dx12ShimTransition(dst, D3D12_RESOURCE_STATE_COPY_DEST))
		return;

	Dx12ShimList()->CopyResource(dst->resource, src->resource);
}

#include "generated/ShimContext.stub.inl"
