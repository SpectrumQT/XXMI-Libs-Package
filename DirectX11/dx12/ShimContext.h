#pragma once

#include "ShimPipeline.h"

// The D3D11 immediate context the engine works through. Binding calls only
// record what was bound: D3D12 wants it all at once, as a pipeline state and
// tables of descriptors, so it is put together when something is drawn.
class ShimContext final : public ID3D11DeviceContext1
{
public:
	volatile LONG mRef;
	ShimPrivateData mPrivate;
	// What the frame's own command lists have bound, and what the mods
	// bind for a draw of the game: the engine starts on each of those with
	// a context that has nothing of the frame's on it. See DrawPath.
	ShimState mFrameState, mScopeState;
	ShimState *mState;
	// Set by whatever changes a binding, for the draw path to tell whether
	// a draw left mScopeState as it found it
	bool mChanged;

	// A draw of a mod in the draw of a game that is not D3D12's, with a
	// vertex and a pixel shader of its own: recorded on our list, where
	// none of the targets of the game are. False if it could not be.
	bool DrawApart(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance);

	ShimContext();

	// What ClearState leaves, without releasing what was bound
	void ResetState();

	void SetShader(Dx12ShaderStage stage, ID3D11DeviceChild *shader);
	void GetShader(Dx12ShaderStage stage, ID3D11DeviceChild **shader, UINT *class_instances);
	void SetConstantBuffers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11Buffer *const *buffers,
			const UINT *first, const UINT *constants);
	void GetConstantBuffers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11Buffer **buffers,
			UINT *first, UINT *constants);
	void SetShaderResources(Dx12ShaderStage stage, UINT start, UINT count, ID3D11ShaderResourceView *const *views);
	void GetShaderResources(Dx12ShaderStage stage, UINT start, UINT count, ID3D11ShaderResourceView **views);
	void SetSamplers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11SamplerState *const *samplers);
	void GetSamplers(Dx12ShaderStage stage, UINT start, UINT count, ID3D11SamplerState **samplers);

	bool IsOutput(ShimResource *resource);
	void UnbindInput(ShimResource *resource);
	void SetRenderTargets(UINT count, ID3D11RenderTargetView *const *views, ID3D11DepthStencilView *depth_stencil);

#include "generated/ShimContext.decl.inl"
};
