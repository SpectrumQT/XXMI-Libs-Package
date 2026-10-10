#include "CmdState.h"

void Dx12ResetCmdState(Dx12CmdState *state, ID3D12PipelineState *pipeline)
{
	memset(state, 0, sizeof(*state));
	state->pipeline = pipeline;
	state->blend_factor[0] = state->blend_factor[1] = state->blend_factor[2] = state->blend_factor[3] = 1.0f;
}

static const D3D12_SHADER_VISIBILITY stage_visibility[DX12_STAGE_COUNT] = {
	D3D12_SHADER_VISIBILITY_VERTEX,
	D3D12_SHADER_VISIBILITY_PIXEL,
	D3D12_SHADER_VISIBILITY_GEOMETRY,
	D3D12_SHADER_VISIBILITY_HULL,
	D3D12_SHADER_VISIBILITY_DOMAIN,
	D3D12_SHADER_VISIBILITY_ALL,
};

bool Dx12LocateRegister(const Dx12CmdState *state, Dx12ShaderStage stage, D3D12_DESCRIPTOR_RANGE_TYPE type,
		UINT reg, Dx12RegisterLocation *location)
{
	const Dx12RootArguments *arguments = stage == DX12_STAGE_CS ? &state->compute : &state->graphics;
	const Dx12RootSignatureInfo *signature;
	D3D12_ROOT_PARAMETER_TYPE descriptor_type;
	size_t i;

	if (!arguments->signature)
		return false;
	signature = Dx12FindRootSignature(arguments->signature);
	if (!signature)
		return false;

	switch (type) {
		case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: descriptor_type = D3D12_ROOT_PARAMETER_TYPE_SRV; break;
		case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: descriptor_type = D3D12_ROOT_PARAMETER_TYPE_UAV; break;
		case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: descriptor_type = D3D12_ROOT_PARAMETER_TYPE_CBV; break;
		default: return false;
	}

	for (i = 0; i < signature->parameters.size() && i < DX12_MAX_ROOT_ARGUMENTS; i++) {
		const Dx12RootParameter *parameter = &signature->parameters[i];

		if (parameter->visibility != D3D12_SHADER_VISIBILITY_ALL && parameter->visibility != stage_visibility[stage])
			continue;

		if (parameter->type == descriptor_type) {
			if (parameter->space || parameter->reg != reg)
				continue;
			location->parameter = (UINT)i;
			location->table = false;
			location->offset = 0;
			return true;
		}

		if (parameter->type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
			continue;

		for (const Dx12RootRange &range : parameter->ranges) {
			if (range.type != type || range.space || reg < range.base || reg - range.base >= range.count)
				continue;
			location->parameter = (UINT)i;
			location->table = true;
			location->offset = range.offset + (reg - range.base);
			return true;
		}
	}

	return false;
}

bool Dx12ResolveRegister(const Dx12CmdState *state, Dx12ShaderStage stage, D3D12_DESCRIPTOR_RANGE_TYPE type,
		UINT reg, Dx12DescriptorSlot *view)
{
	const Dx12RootArguments *arguments = stage == DX12_STAGE_CS ? &state->compute : &state->graphics;
	const Dx12DescriptorSlot *slot;
	Dx12RegisterLocation location;

	if (!Dx12LocateRegister(state, stage, type, reg, &location))
		return false;
	if (!(arguments->set & (1ull << location.parameter)))
		return false;

	if (!location.table) {
		memset(view, 0, sizeof(*view));
		view->kind = type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? DX12_VIEW_SRV
			: type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV ? DX12_VIEW_UAV : DX12_VIEW_CBV;
		view->offset = arguments->values[location.parameter];
		return true;
	}

	slot = Dx12SlotFromGpuHandle(state->view_heap, arguments->values[location.parameter], location.offset);
	if (!slot || slot->kind == DX12_VIEW_NONE)
		return false;

	*view = *slot;
	return true;
}
