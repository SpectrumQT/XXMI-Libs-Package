#pragma once

#include "ShimObjects.h"

// Everything the engine has bound on the shim context. D3D11 holds a
// reference on whatever is bound, and so does this.
struct ShimStageState {
	ID3D11DeviceChild *shader;
	// A mod set no shader, as with gs = null, which is not the same as
	// shader being NULL for the game's being kept
	bool shader_unbound;
	ID3D11Buffer *cbs[SHIM_CBV_SLOTS];
	// In constants of 16 bytes. A count of 0 is the whole buffer.
	UINT cb_first[SHIM_CBV_SLOTS];
	UINT cb_count[SHIM_CBV_SLOTS];
	ID3D11ShaderResourceView *srvs[SHIM_SRV_SLOTS];
	ID3D11SamplerState *samplers[SHIM_SAMPLER_SLOTS];
	// A bit per slot of cbs and srvs that has something bound, for going
	// through the few that do
	UINT cb_mask;
	UINT srv_mask[(SHIM_SRV_SLOTS + 31) / 32];
};

struct ShimState {
	ShimStageState stages[DX12_STAGE_COUNT];
	ID3D11UnorderedAccessView *cs_uavs[SHIM_UAV_SLOTS];
	// Those of the output merger, by register
	ID3D11UnorderedAccessView *om_uavs[SHIM_UAV_SLOTS];

	ID3D11RenderTargetView *rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	ID3D11DepthStencilView *dsv;
	ID3D11BlendState *blend;
	FLOAT blend_factor[4];
	UINT sample_mask;
	ID3D11DepthStencilState *depth_stencil;
	UINT stencil_ref;
	ID3D11RasterizerState *rasterizer;

	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT viewport_count;
	D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT scissor_count;

	ID3D11InputLayout *input_layout;
	D3D11_PRIMITIVE_TOPOLOGY topology;
	ID3D11Buffer *vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT vb_strides[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	UINT vb_offsets[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	ID3D11Buffer *ib;
	DXGI_FORMAT ib_format;
	UINT ib_offset;
	// A mod set no index buffer, as with a file of it that is missing,
	// which is not the same as ib being NULL for the game's being kept
	bool ib_unbound;
};

bool Dx12ShimPipelineInit();

// Record everything a draw or dispatch with this state needs up to the call
// itself. They return false if it cannot be done, having said why in the log,
// and the call has to be dropped.
bool Dx12ShimPrepareDraw(const ShimState *state, bool indexed);
bool Dx12ShimPrepareDispatch(const ShimState *state);

// Drops the pipeline states made with a shader or input layout that is going
// away
void Dx12ShimForgetPipelines(UINT64 id);

D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimDefaultSampler();
