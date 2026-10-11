#pragma once

#include <string>
#include <vector>

#include "Dx12Common.h"

// What a pipeline state or root signature was created from cannot be read
// back from the object, so it is recorded when the device proxy creates one:
// the shaders of a pipeline, identified by the same hash of their bytecode
// that HackerDevice uses for D3D11 shaders, and the layout of a root
// signature, which is what maps a shader register to a root argument.
//
// The records are keyed by the address of the object and go away with it. A
// pointer returned by the Find functions is good for as long as the object is
// alive, which the game guarantees for anything set on a command list that is
// still being recorded.

enum Dx12ShaderStage {
	DX12_STAGE_VS,
	DX12_STAGE_PS,
	DX12_STAGE_GS,
	DX12_STAGE_HS,
	DX12_STAGE_DS,
	DX12_STAGE_CS,
	DX12_STAGE_COUNT
};
extern const char *const dx12_stage_names[DX12_STAGE_COUNT];

const UINT DX12_CB_SIZES = 16;

// One per distinct bytecode, never freed
struct Dx12ShaderInfo {
	UINT64 hash;
	Dx12ShaderStage stage;
	UINT32 size;
	// DXIL (shader model 6) as opposed to the DXBC that D3D11 also takes
	bool dxil;
	// Whether the registers below could be read from the shader: PSV0
	// for DXIL, RDEF for DXBC
	bool reflected;
	// An SRV range without an upper bound. Any register may be read.
	bool unbounded_srv;
	// A view register the masks below have no bit for: of another space
	// than 0, or past what they reach
	bool unmasked;
	// Registers of space 0 the shader declares: t0-t127, b0-b63, u0-u63
	UINT64 srv[2];
	UINT64 cbv;
	UINT64 uav;
	// How many bytes the shader declares for the constant buffers b0-b15 of
	// space 0, which is all that tells where a buffer bound by address
	// ends. 0 where it has none or that could not be read.
	UINT32 cb_bytes[DX12_CB_SIZES];
	// How many render targets a pixel shader writes
	UINT32 targets;
	// A copy of the shader as the game gave it, size bytes. Only kept
	// with hunting on, for marking a shader to write it to ShaderFixes,
	// or with dx12_keep_shader_bytecode.
	const BYTE *bytecode;
};

struct Dx12RootRange {
	D3D12_DESCRIPTOR_RANGE_TYPE type;
	UINT base;
	// UINT_MAX if unbounded
	UINT count;
	UINT space;
	// In descriptors from the start of the table
	UINT offset;
};

struct Dx12RootParameter {
	D3D12_ROOT_PARAMETER_TYPE type;
	D3D12_SHADER_VISIBILITY visibility;
	// Root descriptors and constants
	UINT reg;
	UINT space;
	// Descriptor tables
	std::vector<Dx12RootRange> ranges;
};

struct Dx12RootSignatureInfo {
	std::vector<Dx12RootParameter> parameters;
	// For DrawPath: whether a register is one its shaders can be given
	// something for, with the register it was worked out for. 0 if it
	// has not been.
	mutable volatile LONG ini_params = 0;
};

struct Dx12InputElement {
	std::string semantic;
	UINT index;
	DXGI_FORMAT format;
	UINT slot;
	UINT offset;
	bool per_instance;
	UINT step_rate;
};

// What DrawPath has worked out about the draws made with a pipeline state,
// kept with it to be found without a lookup. Written with the lock of the
// draw path held and read without: generation is 0 while the rest is being
// written.
struct Dx12DrawFilter {
	volatile LONG generation;
	UINT override_generation;
	bool only_checks;
	bool ib;
	BYTE cb_count;
	UINT vbs;
	struct {
		BYTE stage, slot;
	} cbs[8];
	// Bits of the conditions DrawPath watches: the ones that have to be
	// false and the ones that have to be true for only_checks to hold
	UINT64 guards_zero, guards_nonzero;
};

struct Dx12PipelineInfo {
	const Dx12ShaderInfo *shaders[DX12_STAGE_COUNT];
	// Whether those are all the shaders it has. Not so with a mesh shader,
	// or where the description could not be read to its end.
	bool shaders_known = true;
	std::vector<Dx12InputElement> input_layout;
	// Bit per vertex buffer slot the input layout reads
	UINT vb_slots = 0;
	mutable Dx12DrawFilter filter = {};

	// What a graphics pipeline was created from, for creating it again
	// with another input layout: a pipeline state stream, or a
	// D3D12_GRAPHICS_PIPELINE_STATE_DESC if not creation_stream. What it
	// points at was the game's and is gone, but for the root signature,
	// which is referenced.
	std::vector<BYTE> creation;
	bool creation_stream = false;
	// Of a stream: its size, how much of it could be read, and the type
	// of the subobject that ended the reading before the end
	UINT stream_bytes = 0, stream_read = 0, stream_stopped_at = 0;
	ID3D12RootSignature *root_signature = NULL;
	// The pipelines created of it that way by the hash of their input
	// layout, NULL for one that could not be. Referenced. For DrawPath,
	// with its lock held.
	mutable std::vector<std::pair<UINT64, ID3D12PipelineState*>> variants;

	~Dx12PipelineInfo();
};

// Set by whatever needs the shaders of the game's pipelines kept as the game
// gave them (Dx12ShaderInfo::bytecode) without hunting being on. Goes for the
// shaders seen from then on.
extern volatile bool dx12_keep_shader_bytecode;

// For the methods creating these objects, on success. ppv is what the game
// asked for, which may be nothing at all.
void Dx12RegisterRootSignature(void **ppvRootSignature, const void *blob, SIZE_T size);
void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc);
void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc);
void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_PIPELINE_STATE_STREAM_DESC *desc);

const Dx12PipelineInfo* Dx12FindPipeline(ID3D12PipelineState *pipeline);
// The game's pipeline with another input layout, which the caller hashed. Not
// referenced: it lives as long as the game's does. NULL if it cannot be made,
// for one because the game's shaders were not kept.
ID3D12PipelineState* Dx12PipelineWithInputLayout(const Dx12PipelineInfo *info, ID3D12Device *device, UINT64 layout_hash,
		const D3D12_INPUT_ELEMENT_DESC *elements, UINT count);
const Dx12ShaderInfo* Dx12FindShader(UINT64 hash);
// Appends the shaders seen since the first from of them, in the order they
// were first seen in
void Dx12GetShaders(size_t from, std::vector<const Dx12ShaderInfo*> *found);
const Dx12RootSignatureInfo* Dx12FindRootSignature(ID3D12RootSignature *signature);

void Dx12GetPipelineCounts(size_t *pipelines, size_t *root_signatures, size_t *shaders);
