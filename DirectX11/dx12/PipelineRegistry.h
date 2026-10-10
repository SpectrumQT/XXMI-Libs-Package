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
	// with hunting on, for marking a shader to write it to ShaderFixes.
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
};

struct Dx12InputElement {
	std::string semantic;
	UINT index;
	DXGI_FORMAT format;
	UINT slot;
	UINT offset;
	bool per_instance;
};

struct Dx12PipelineInfo {
	const Dx12ShaderInfo *shaders[DX12_STAGE_COUNT];
	std::vector<Dx12InputElement> input_layout;
};

// For the methods creating these objects, on success. ppv is what the game
// asked for, which may be nothing at all.
void Dx12RegisterRootSignature(void **ppvRootSignature, const void *blob, SIZE_T size);
void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc);
void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc);
void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_PIPELINE_STATE_STREAM_DESC *desc);

const Dx12PipelineInfo* Dx12FindPipeline(ID3D12PipelineState *pipeline);
const Dx12ShaderInfo* Dx12FindShader(UINT64 hash);
// Appends the shaders seen since the first from of them, in the order they
// were first seen in
void Dx12GetShaders(size_t from, std::vector<const Dx12ShaderInfo*> *found);
const Dx12RootSignatureInfo* Dx12FindRootSignature(ID3D12RootSignature *signature);

void Dx12GetPipelineCounts(size_t *pipelines, size_t *root_signatures, size_t *shaders);
