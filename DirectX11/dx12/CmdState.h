#pragma once

#include "DescriptorShadow.h"
#include "PipelineRegistry.h"

// What the game has set on a command list since its last Reset, kept by the
// list proxy. D3D12 has no getters, so this is the only way to know what a
// draw uses and, later, to put back what we change.
//
// Recording has to stay cheap as it is paid on every bind call: the methods
// store what they were given and nothing is looked up until someone asks, see
// Dx12ResolveRegister.

const UINT DX12_MAX_ROOT_ARGUMENTS = 64;

struct Dx12RootArguments {
	ID3D12RootSignature *signature;
	// Bit per root parameter that has been set since the signature was
	UINT64 set;
	// GPU descriptor handle for a table, GPU virtual address for a root
	// descriptor. Root constants are not recorded.
	UINT64 values[DX12_MAX_ROOT_ARGUMENTS];
};

struct Dx12CmdState {
	ID3D12PipelineState *pipeline;
	Dx12RootArguments graphics;
	Dx12RootArguments compute;

	// Not referenced, the game keeps the heaps of a list alive
	Dx12DescriptorHeap *view_heap;
	Dx12DescriptorHeap *sampler_heap;
	// The originals of all the heaps the game set, for setting them again.
	// heap_count is 0 if there were more than fit.
	ID3D12DescriptorHeap *heaps[4];
	UINT heap_count;

	D3D12_VERTEX_BUFFER_VIEW vertex_buffers[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	D3D12_INDEX_BUFFER_VIEW index_buffer;
	D3D12_PRIMITIVE_TOPOLOGY topology;

	// As the game passed them, for Dx12SlotsFromCpuHandle
	D3D12_CPU_DESCRIPTOR_HANDLE render_targets[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
	D3D12_CPU_DESCRIPTOR_HANDLE depth_stencil;
	UINT render_target_count;
	bool has_depth_stencil;

	D3D12_VIEWPORT viewports[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	D3D12_RECT scissors[D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	UINT viewport_count;
	UINT scissor_count;
	FLOAT blend_factor[4];
	UINT stencil_ref;
};

void Dx12ResetCmdState(Dx12CmdState *state, ID3D12PipelineState *pipeline);

// Where a root signature has a stage read register reg of space 0 from: a
// root descriptor, or a descriptor table at offset descriptors from its start.
struct Dx12RegisterLocation {
	UINT parameter;
	bool table;
	UINT offset;
};
bool Dx12LocateRegister(const Dx12RootSignatureInfo *signature, Dx12ShaderStage stage, D3D12_DESCRIPTOR_RANGE_TYPE type,
		UINT reg, Dx12RegisterLocation *location);

// Finds what a draw (or a dispatch, for DX12_STAGE_CS) would read through
// register reg of space 0: a t, u or b register depending on type.
//
// Returns false if the root signature has no such register for the stage or
// nothing has been set for it. Otherwise *view describes the binding like a
// descriptor would; for a root descriptor that is just the address in offset.
bool Dx12ResolveRegister(const Dx12CmdState *state, Dx12ShaderStage stage, D3D12_DESCRIPTOR_RANGE_TYPE type,
		UINT reg, Dx12DescriptorSlot *view);
// For asking about several registers: signature is what Dx12FindRootSignature
// has for the one that is set for the stage
bool Dx12ResolveRegister(const Dx12CmdState *state, const Dx12RootSignatureInfo *signature, Dx12ShaderStage stage,
		D3D12_DESCRIPTOR_RANGE_TYPE type, UINT reg, Dx12DescriptorSlot *view);
