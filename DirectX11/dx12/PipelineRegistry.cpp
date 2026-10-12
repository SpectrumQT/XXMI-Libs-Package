#include "PipelineRegistry.h"

#include <unordered_map>

#include "util.h"
#include "../globals.h"

#include "DxilMetadata.h"
#include "HashDump.h"
#include "ResourceTracker.h"

const char *const dx12_stage_names[DX12_STAGE_COUNT] = { "vs", "ps", "gs", "hs", "ds", "cs" };

static SRWLOCK shader_lock = SRWLOCK_INIT;
static std::unordered_map<UINT64, Dx12ShaderInfo*> shaders;
static std::vector<const Dx12ShaderInfo*> shader_list;

static SRWLOCK pipeline_lock = SRWLOCK_INIT;
static std::unordered_map<ID3D12PipelineState*, Dx12PipelineInfo*> pipelines;

volatile bool dx12_keep_shader_bytecode;
static volatile bool graphics_dxil;

static SRWLOCK signature_lock = SRWLOCK_INIT;
static std::unordered_map<ID3D12RootSignature*, Dx12RootSignatureInfo*> signatures;

// -----------------------------------------------------------------------------
// Shader and root signature blobs share the DXBC container format: a 32 byte
// header ending in the number of parts, the offset of each part, and parts
// made of a four character code, a size and that many bytes of data.
//
// Everything is read through bounds checked loads. These blobs come from the
// game and a root signature can be created from any memory it likes.

struct Blob {
	const BYTE *data;
	size_t size;
};

static bool read32(Blob blob, size_t offset, UINT32 *value)
{
	if (offset > blob.size || blob.size - offset < 4)
		return false;
	memcpy(value, blob.data + offset, 4);
	return true;
}

static bool find_part(Blob container, const char *fourcc, Blob *part)
{
	UINT32 count, offset, size, i;

	if (!container.data || container.size < 32 || memcmp(container.data, "DXBC", 4))
		return false;
	if (!read32(container, 28, &count) || count > 256)
		return false;

	for (i = 0; i < count; i++) {
		if (!read32(container, 32 + (size_t)i * 4, &offset) || !read32(container, (size_t)offset + 4, &size))
			return false;
		if (memcmp(container.data + offset, fourcc, 4))
			continue;
		if (container.size - offset - 8 < size)
			return false;
		part->data = container.data + offset + 8;
		part->size = size;
		return true;
	}

	return false;
}

static bool same_string(Blob blob, UINT32 a, UINT32 b)
{
	for (; a < blob.size && b < blob.size; a++, b++) {
		if (blob.data[a] != blob.data[b])
			return false;
		if (!blob.data[a])
			return true;
	}
	return false;
}

// The output signature: a count, 4 bytes that are not used, and an element for
// each output that starts with the offset of its semantic name. OSG1, which
// DXIL has, puts a stream number before that and is 8 bytes longer.
static UINT32 count_targets(Blob part, bool wide)
{
	static const char name[] = "SV_Target";
	UINT32 count, offset, i, targets = 0;
	size_t size = wide ? 32 : 24;

	if (!read32(part, 0, &count) || count > 64)
		return 0;

	for (i = 0; i < count; i++) {
		if (!read32(part, 8 + i * size + (wide ? 4 : 0), &offset))
			return 0;
		if (offset < part.size && part.size - offset >= sizeof(name) && !_stricmp((const char*)part.data + offset, name))
			targets++;
	}
	return targets;
}

// The elements of the signature parts in full: name, index, system value,
// component type, register, mask, read/write mask. The *SG1 form has a
// stream number in front and a minimum precision after.
bool Dx12ReadSignature(const void *bytecode, size_t size, bool output, std::vector<Dx12SignatureElement> *elements)
{
	Blob container = { (const BYTE*)bytecode, size }, part;
	UINT32 count, offset, value, i;
	size_t entry, base;
	bool wide;

	elements->clear();
	if (find_part(container, output ? "OSG1" : "ISG1", &part))
		wide = true;
	else if (find_part(container, output ? "OSGN" : "ISGN", &part))
		wide = false;
	else
		return false;
	entry = wide ? 32 : 24;

	if (!read32(part, 0, &count) || count > 64)
		return false;
	for (i = 0; i < count; i++) {
		Dx12SignatureElement element = {};

		base = 8 + i * entry + (wide ? 4 : 0);
		if (!read32(part, base, &offset) || offset >= part.size)
			return false;
		strncpy_s(element.name, (const char*)part.data + offset, min(sizeof(element.name) - 1, part.size - offset));
		if (!read32(part, base + 4, &element.index) || !read32(part, base + 8, &element.system_value)
		    || !read32(part, base + 16, &element.reg) || !read32(part, base + 20, &value))
			return false;
		element.mask = value & 0xff;
		elements->push_back(element);
	}
	return true;
}

static void mark_registers(UINT64 *mask, unsigned words, UINT32 first, UINT32 count)
{
	UINT32 reg;

	for (reg = first; count && reg < words * 64; reg++, count--)
		mask[reg / 64] |= 1ull << (reg % 64);
}

// PSV0, the pipeline state validation part of a DXIL container
// (DxilPipelineStateValidation.h in DirectXShaderCompiler):
//
//   u32 size of PSVRuntimeInfo, followed by that structure
//   u32 number of resources
//   u32 size of a resource record, if there are any resources
//   the records: u32 type, space, lower bound, upper bound, ...
static bool reflect_psv0(Blob psv, Dx12ShaderInfo *info)
{
	UINT32 info_size, count, record_size, type, space, lower, upper, i;
	size_t pos;

	if (!read32(psv, 0, &info_size))
		return false;
	pos = (size_t)info_size + 4;
	if (!read32(psv, pos, &count))
		return false;
	if (!count)
		return true;
	if (!read32(psv, pos + 4, &record_size) || record_size < 16 || count > 65536)
		return false;
	pos += 8;

	for (i = 0; i < count; i++, pos += record_size) {
		if (!read32(psv, pos, &type) || !read32(psv, pos + 4, &space)
		    || !read32(psv, pos + 8, &lower) || !read32(psv, pos + 12, &upper))
			return false;
		// PSVResourceType: 2 is a CBV, 3-5 SRVs, 6-9 UAVs
		if (upper < lower || type < 2)
			continue;
		if (space || type > 9) {
			info->unmasked = true;
			continue;
		}

		if (type == 2) {
			info->unmasked |= upper >= 64;
			mark_registers(&info->cbv, 1, lower, upper - lower + 1);
		} else if (type <= 5) {
			if (upper == UINT_MAX) {
				info->unbounded_srv = true;
			} else {
				info->unmasked |= upper >= 128;
				mark_registers(info->srv, 2, lower, upper - lower + 1);
			}
		} else {
			info->unmasked |= upper >= 64;
			mark_registers(&info->uav, 1, lower, upper == UINT_MAX ? 64 : upper - lower + 1);
		}
	}

	return true;
}

// RDEF, the resource definitions of a DXBC shader: the number of resource
// bindings at offset 8, the offset of the first at 12 and the shader model
// version at 16. A binding is name, type, return type, dimension, samples,
// bind point, bind count and flags, each a u32, plus space and id for shader
// model 5.1.
static bool reflect_rdef(Blob rdef, Dx12ShaderInfo *info)
{
	UINT32 count, offset, version, type, reg, reg_count, space, i;
	UINT32 cb_count, cb_offset, name, cb_name, cb_size, j;
	size_t pos, record_size;

	if (!read32(rdef, 8, &count) || !read32(rdef, 12, &offset) || !read32(rdef, 16, &version) || count > 65536)
		return false;
	if (!read32(rdef, 0, &cb_count) || !read32(rdef, 4, &cb_offset) || cb_count > 65536)
		return false;
	record_size = (version & 0xffff) == 0x0501 ? 40 : 32;

	for (i = 0, pos = offset; i < count; i++, pos += record_size) {
		if (!read32(rdef, pos + 4, &type) || !read32(rdef, pos + 20, &reg) || !read32(rdef, pos + 24, &reg_count))
			return false;
		space = 0;
		if (record_size == 40 && !read32(rdef, pos + 32, &space))
			return false;
		if (space) {
			info->unmasked |= type != D3D_SIT_SAMPLER;
			continue;
		}

		switch (type) {
			case D3D_SIT_CBUFFER:
				info->unmasked |= !reg_count || (UINT64)reg + reg_count > 64;
				mark_registers(&info->cbv, 1, reg, reg_count ? reg_count : 1);
				// The sizes are with the constant buffers, a list
				// of 24 byte records that starts with the name
				// and has the size at offset 12
				if (reg >= DX12_CB_SIZES || !read32(rdef, pos, &name))
					break;
				for (j = 0; j < cb_count; j++) {
					if (!read32(rdef, (size_t)cb_offset + j * 24, &cb_name)
					    || !read32(rdef, (size_t)cb_offset + j * 24 + 12, &cb_size))
						break;
					if (same_string(rdef, name, cb_name)) {
						info->cb_bytes[reg] = cb_size;
						break;
					}
				}
				break;
			case D3D_SIT_TBUFFER:
			case D3D_SIT_TEXTURE:
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
				// A bind count of 0 is an unbounded array
				if (!reg_count) {
					info->unbounded_srv = true;
				} else {
					info->unmasked |= (UINT64)reg + reg_count > 128;
					mark_registers(info->srv, 2, reg, reg_count);
				}
				break;
			case D3D_SIT_UAV_RWTYPED:
			case D3D_SIT_UAV_RWSTRUCTURED:
			case D3D_SIT_UAV_RWBYTEADDRESS:
			case D3D_SIT_UAV_APPEND_STRUCTURED:
			case D3D_SIT_UAV_CONSUME_STRUCTURED:
			case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
				info->unmasked |= !reg_count || (UINT64)reg + reg_count > 64;
				mark_registers(&info->uav, 1, reg, reg_count ? reg_count : 64);
				break;
			case D3D_SIT_SAMPLER:
				break;
			default:
				info->unmasked = true;
				break;
		}
	}

	return true;
}

static const Dx12ShaderInfo* shader_info(Dx12ShaderStage stage, const D3D12_SHADER_BYTECODE *bytecode)
{
	Dx12ShaderInfo *info, *existing = NULL;
	std::vector<Dx12DxilCBuffer> cbuffers;
	Blob container, part;
	BYTE *copy = NULL;
	UINT64 hash;

	if (!bytecode->pShaderBytecode || !bytecode->BytecodeLength)
		return NULL;

	// The same hash HackerDevice gives a D3D11 shader with the default
	// shader_hash setting
	hash = fnv_64_buf(bytecode->pShaderBytecode, bytecode->BytecodeLength);

	AcquireSRWLockShared(&shader_lock);
	auto i = shaders.find(hash);
	if (i != shaders.end())
		existing = i->second;
	ReleaseSRWLockShared(&shader_lock);
	if (existing)
		return existing;

	info = new Dx12ShaderInfo();
	info->hash = hash;
	info->stage = stage;
	info->size = (UINT32)bytecode->BytecodeLength;

	container.data = (const BYTE*)bytecode->pShaderBytecode;
	container.size = bytecode->BytecodeLength;
	info->dxil = find_part(container, "DXIL", &part);
	if (info->dxil && Dx12ReadDxilCBuffers(part.data, part.size, &cbuffers)) {
		for (const Dx12DxilCBuffer &cbuffer : cbuffers) {
			if (!cbuffer.space && cbuffer.reg < DX12_CB_SIZES)
				info->cb_bytes[cbuffer.reg] = cbuffer.bytes;
		}
	}
	if (stage == DX12_STAGE_PS) {
		if (find_part(container, "OSG1", &part))
			info->targets = count_targets(part, true);
		else if (find_part(container, "OSGN", &part))
			info->targets = count_targets(part, false);
	}
	if (find_part(container, "PSV0", &part))
		info->reflected = reflect_psv0(part, info);
	else if (find_part(container, "RDEF", &part))
		info->reflected = reflect_rdef(part, info);

	if (G->hunting || dx12_keep_shader_bytecode) {
		copy = (BYTE*)malloc(info->size);
		if (copy)
			memcpy(copy, bytecode->pShaderBytecode, info->size);
		info->bytecode = copy;
	}

	AcquireSRWLockExclusive(&shader_lock);
	auto inserted = shaders.emplace(hash, info);
	if (inserted.second)
		shader_list.push_back(info);
	ReleaseSRWLockExclusive(&shader_lock);

	if (!inserted.second) {
		free(copy);
		delete info;
		return inserted.first->second;
	}
	if (info->dxil && stage != DX12_STAGE_CS)
		graphics_dxil = true;

	Dx12DumpShader(stage, info);
	return info;
}

bool Dx12BytecodeIsDxil(const void *bytecode, size_t size)
{
	Blob container = { (const BYTE*)bytecode, size }, part;

	return find_part(container, "DXIL", &part);
}

bool Dx12GameHasDxilShaders()
{
	return graphics_dxil;
}

// -----------------------------------------------------------------------------
// The RTS0 part of a serialized root signature (DxilRootSignature.h in
// DirectXShaderCompiler). All offsets are from the start of the part.
//
//   header:     version, parameter count, parameters offset, static sampler
//               count, static samplers offset, flags
//   parameter:  type, shader visibility, payload offset
//   payloads:   table       range count, ranges offset
//               constants   register, space, number of values
//               descriptor  register, space (and flags from version 1.1)
//   range:      type, descriptor count, base register, space, (flags from
//               version 1.1,) offset from the start of the table
//
// Version is 1 for root signature 1.0, 2 for 1.1 and 3 for 1.2.

static bool parse_root_signature(Blob rts, Dx12RootSignatureInfo *info)
{
	UINT32 version, count, offset, type, visibility, payload, range_count, range_offset, i, j;
	UINT32 next_offset;
	size_t pos, range_size;

	if (!read32(rts, 0, &version) || !read32(rts, 4, &count) || !read32(rts, 8, &offset) || count > 64)
		return false;
	range_size = version >= 2 ? 24 : 20;

	info->parameters.resize(count);
	for (i = 0, pos = offset; i < count; i++, pos += 12) {
		Dx12RootParameter *parameter = &info->parameters[i];

		if (!read32(rts, pos, &type) || !read32(rts, pos + 4, &visibility) || !read32(rts, pos + 8, &payload))
			return false;
		parameter->type = (D3D12_ROOT_PARAMETER_TYPE)type;
		parameter->visibility = (D3D12_SHADER_VISIBILITY)visibility;
		parameter->reg = parameter->space = 0;

		if (type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
			if (!read32(rts, payload, &parameter->reg) || !read32(rts, (size_t)payload + 4, &parameter->space))
				return false;
			continue;
		}

		if (!read32(rts, payload, &range_count) || !read32(rts, (size_t)payload + 4, &range_offset) || range_count > 4096)
			return false;

		parameter->ranges.resize(range_count);
		next_offset = 0;
		for (j = 0; j < range_count; j++) {
			Dx12RootRange *range = &parameter->ranges[j];
			size_t range_pos = range_offset + j * range_size;
			UINT32 range_type;

			if (!read32(rts, range_pos, &range_type) || !read32(rts, range_pos + 4, &range->count)
			    || !read32(rts, range_pos + 8, &range->base) || !read32(rts, range_pos + 12, &range->space)
			    || !read32(rts, range_pos + range_size - 4, &range->offset))
				return false;
			range->type = (D3D12_DESCRIPTOR_RANGE_TYPE)range_type;

			if (range->offset == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
				range->offset = next_offset;
			// Only the last range of a table may be unbounded
			next_offset = range->count == UINT_MAX ? UINT_MAX : range->offset + range->count;
		}
	}

	return true;
}

static void signature_destroyed(void *object)
{
	Dx12RootSignatureInfo *info = NULL;

	AcquireSRWLockExclusive(&signature_lock);
	auto i = signatures.find((ID3D12RootSignature*)object);
	if (i != signatures.end()) {
		info = i->second;
		signatures.erase(i);
	}
	ReleaseSRWLockExclusive(&signature_lock);

	delete info;
}

void Dx12RegisterRootSignature(void **ppvRootSignature, const void *blob, SIZE_T size)
{
	ID3D12RootSignature *signature = NULL;
	Dx12RootSignatureInfo *info;
	Blob container, rts;
	bool inserted;

	if (!ppvRootSignature || !*ppvRootSignature)
		return;
	if (FAILED(((IUnknown*)*ppvRootSignature)->QueryInterface(IID_PPV_ARGS(&signature))))
		return;

	// D3D12 returns the object it already has for a root signature it has
	// seen before
	AcquireSRWLockShared(&signature_lock);
	inserted = signatures.count(signature) != 0;
	ReleaseSRWLockShared(&signature_lock);
	if (inserted) {
		signature->Release();
		return;
	}

	info = new Dx12RootSignatureInfo();
	container.data = (const BYTE*)blob;
	container.size = size;
	if (!find_part(container, "RTS0", &rts) || !parse_root_signature(rts, info)) {
		Dx12LogInfo("*** Failed to parse the %Iu bytes ID3D12RootSignature %p was created from\n", size, signature);
		info->parameters.clear();
	}

	AcquireSRWLockExclusive(&signature_lock);
	inserted = signatures.emplace(signature, info).second;
	ReleaseSRWLockExclusive(&signature_lock);

	if (inserted)
		Dx12NotifyDestruction(signature, signature_destroyed);
	else
		delete info;

	signature->Release();
}

const Dx12RootSignatureInfo* Dx12FindRootSignature(ID3D12RootSignature *signature)
{
	const Dx12RootSignatureInfo *info = NULL;

	AcquireSRWLockShared(&signature_lock);
	auto i = signatures.find(signature);
	if (i != signatures.end())
		info = i->second;
	ReleaseSRWLockShared(&signature_lock);

	return info;
}

// -----------------------------------------------------------------------------

static void pipeline_destroyed(void *object)
{
	Dx12PipelineInfo *info = NULL;

	AcquireSRWLockExclusive(&pipeline_lock);
	auto i = pipelines.find((ID3D12PipelineState*)object);
	if (i != pipelines.end()) {
		info = i->second;
		pipelines.erase(i);
	}
	ReleaseSRWLockExclusive(&pipeline_lock);

	delete info;
}

// Takes ownership of info
static void register_pipeline(void **ppPipelineState, Dx12PipelineInfo *info)
{
	ID3D12PipelineState *pipeline = NULL;
	bool inserted;

	if (!ppPipelineState || !*ppPipelineState
	    || FAILED(((IUnknown*)*ppPipelineState)->QueryInterface(IID_PPV_ARGS(&pipeline)))) {
		delete info;
		return;
	}

	// Like root signatures, identical pipelines are one object
	AcquireSRWLockExclusive(&pipeline_lock);
	inserted = pipelines.emplace(pipeline, info).second;
	ReleaseSRWLockExclusive(&pipeline_lock);

	if (inserted)
		Dx12NotifyDestruction(pipeline, pipeline_destroyed);
	else
		delete info;

	pipeline->Release();
}

static void set_input_layout(Dx12PipelineInfo *info, const D3D12_INPUT_LAYOUT_DESC *layout)
{
	UINT i;

	if (!layout->pInputElementDescs)
		return;

	info->input_layout.resize(layout->NumElements);
	for (i = 0; i < layout->NumElements; i++) {
		const D3D12_INPUT_ELEMENT_DESC *element = &layout->pInputElementDescs[i];
		Dx12InputElement *out = &info->input_layout[i];

		out->semantic = element->SemanticName ? element->SemanticName : "";
		out->index = element->SemanticIndex;
		out->format = element->Format;
		out->slot = element->InputSlot;
		out->offset = element->AlignedByteOffset;
		out->per_instance = element->InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
		out->step_rate = element->InstanceDataStepRate;
		if (element->InputSlot < 32)
			info->vb_slots |= 1u << element->InputSlot;
	}
}

Dx12PipelineInfo::~Dx12PipelineInfo()
{
	for (auto &variant : variants) {
		if (variant.second)
			variant.second->Release();
	}
	if (root_signature)
		root_signature->Release();
}

static void keep_creation(Dx12PipelineInfo *info, const void *creation, size_t size, bool stream, ID3D12RootSignature *signature)
{
	info->creation.assign((const BYTE*)creation, (const BYTE*)creation + size);
	info->creation_stream = stream;
	info->root_signature = signature;
	if (signature)
		signature->AddRef();
}

void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
	Dx12PipelineInfo *info;

	if (!desc)
		return;

	info = new Dx12PipelineInfo();
	info->shaders[DX12_STAGE_VS] = shader_info(DX12_STAGE_VS, &desc->VS);
	info->shaders[DX12_STAGE_PS] = shader_info(DX12_STAGE_PS, &desc->PS);
	info->shaders[DX12_STAGE_GS] = shader_info(DX12_STAGE_GS, &desc->GS);
	info->shaders[DX12_STAGE_HS] = shader_info(DX12_STAGE_HS, &desc->HS);
	info->shaders[DX12_STAGE_DS] = shader_info(DX12_STAGE_DS, &desc->DS);
	set_input_layout(info, &desc->InputLayout);
	keep_creation(info, desc, sizeof(*desc), false, desc->pRootSignature);

	register_pipeline(ppPipelineState, info);
}

void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc)
{
	Dx12PipelineInfo *info;

	if (!desc)
		return;

	info = new Dx12PipelineInfo();
	info->shaders[DX12_STAGE_CS] = shader_info(DX12_STAGE_CS, &desc->CS);

	register_pipeline(ppPipelineState, info);
}

// A pipeline state stream is a sequence of these, each aligned like a pointer
template <class T>
struct alignas(void*) StreamSubobject {
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
	T value;
};

// Where the value is in the subobjects that have a pointer in them, which are
// the only ones looked into
const size_t STREAM_VALUE = offsetof(StreamSubobject<void*>, value);

// 0 for a subobject this code does not know
static size_t subobject_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
#define SUBOBJECT(T) return sizeof(StreamSubobject<T>);
	switch (type) {
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS: SUBOBJECT(D3D12_SHADER_BYTECODE)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: SUBOBJECT(D3D12_INPUT_LAYOUT_DESC)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: SUBOBJECT(ID3D12RootSignature*)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: SUBOBJECT(D3D12_STREAM_OUTPUT_DESC)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: SUBOBJECT(D3D12_BLEND_DESC)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: SUBOBJECT(UINT)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: SUBOBJECT(D3D12_RASTERIZER_DESC)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: SUBOBJECT(D3D12_RASTERIZER_DESC1)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: SUBOBJECT(D3D12_RASTERIZER_DESC2)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: SUBOBJECT(D3D12_DEPTH_STENCIL_DESC)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: SUBOBJECT(D3D12_DEPTH_STENCIL_DESC1)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: SUBOBJECT(D3D12_DEPTH_STENCIL_DESC2)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: SUBOBJECT(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: SUBOBJECT(D3D12_PRIMITIVE_TOPOLOGY_TYPE)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: SUBOBJECT(D3D12_RT_FORMAT_ARRAY)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: SUBOBJECT(DXGI_FORMAT)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: SUBOBJECT(DXGI_SAMPLE_DESC)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: SUBOBJECT(UINT)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: SUBOBJECT(D3D12_CACHED_PIPELINE_STATE)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: SUBOBJECT(D3D12_PIPELINE_STATE_FLAGS)
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: SUBOBJECT(D3D12_VIEW_INSTANCING_DESC)
	}
#undef SUBOBJECT
	return 0;
}

// DX12_STAGE_COUNT for a subobject that is no shader of those stages
static Dx12ShaderStage subobject_stage(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type)
{
	switch (type) {
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: return DX12_STAGE_VS;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: return DX12_STAGE_PS;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: return DX12_STAGE_GS;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: return DX12_STAGE_HS;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: return DX12_STAGE_DS;
		case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: return DX12_STAGE_CS;
	}
	return DX12_STAGE_COUNT;
}

void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_PIPELINE_STATE_STREAM_DESC *desc)
{
	D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
	ID3D12RootSignature *signature = NULL;
	Dx12PipelineInfo *info;
	Dx12ShaderStage stage;
	const BYTE *stream, *value;
	size_t pos = 0, size;

	if (!desc || !desc->pPipelineStateSubobjectStream)
		return;

	info = new Dx12PipelineInfo();
	stream = (const BYTE*)desc->pPipelineStateSubobjectStream;

	while (desc->SizeInBytes - pos >= sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
		type = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(stream + pos);
		size = subobject_size(type);
		// A subobject this code does not know the size of, or a
		// truncated stream. Shaders come first in the streams engines
		// build, so what was found so far is still worth keeping.
		if (!size || desc->SizeInBytes - pos < size)
			break;

		value = stream + pos + STREAM_VALUE;
		stage = subobject_stage(type);
		if (stage != DX12_STAGE_COUNT)
			info->shaders[stage] = shader_info(stage, (const D3D12_SHADER_BYTECODE*)value);
		else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS || type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS)
			// A stream may have the subobject and no shader in it
			info->shaders_known &= !((const D3D12_SHADER_BYTECODE*)value)->pShaderBytecode;
		else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT)
			set_input_layout(info, (const D3D12_INPUT_LAYOUT_DESC*)value);
		else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE)
			signature = *(ID3D12RootSignature *const*)value;
		pos += size;
	}
	info->stream_bytes = (UINT)desc->SizeInBytes;
	info->stream_read = (UINT)pos;
	if (desc->SizeInBytes - pos >= sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
		info->stream_stopped_at = *(const UINT*)(stream + pos);
		info->shaders_known = false;
	} else if (!info->shaders[DX12_STAGE_CS]) {
		keep_creation(info, stream, pos, true, signature);
	}

	register_pipeline(ppPipelineState, info);
}

// The copy of what a pipeline was created from has the game's pointer to a
// shader, which is gone. A shader of a mod goes in whether the game had one
// for the stage or not, and so does none where a mod wants none.
static bool revive_shader(D3D12_SHADER_BYTECODE *bytecode, const Dx12ShaderInfo *shader, bool mod, bool removed)
{
	if (removed) {
		*bytecode = {};
		return true;
	}
	if (!mod && (!bytecode->pShaderBytecode || !bytecode->BytecodeLength))
		return true;
	if (!shader || !shader->bytecode)
		return false;

	bytecode->pShaderBytecode = shader->bytecode;
	bytecode->BytecodeLength = shader->size;
	return true;
}

static const char* stage_list(UINT stages, char *text, size_t size)
{
	Dx12ShaderStage stage;

	text[0] = 0;
	for (stage = DX12_STAGE_VS; stage < DX12_STAGE_COUNT; stage = (Dx12ShaderStage)(stage + 1)) {
		if (stages & 1u << stage) {
			if (text[0])
				strcat_s(text, size, " ");
			strcat_s(text, size, dx12_stage_names[stage]);
		}
	}
	return text;
}

ID3D12PipelineState* Dx12PipelineVariant(const Dx12PipelineInfo *info, ID3D12Device *device, const Dx12PipelineVariantDesc *variant,
		const char **why_failed)
{
	static UINT reported;
	std::vector<BYTE> creation;
	// The game's layout where the variant keeps it: the pointer in the copy
	// is the game's and gone
	std::vector<D3D12_INPUT_ELEMENT_DESC> game_elements;
	const D3D12_INPUT_ELEMENT_DESC *elements = variant->elements;
	UINT count = variant->element_count;
	ID3D12PipelineState *pipeline = NULL;
	ID3D12Device2 *device2 = NULL;
	const Dx12ShaderInfo *shaders[DX12_STAGE_COUNT];
	const char *failed = NULL;
	UINT64 key = variant->layout_hash;
	UINT mod_stages = 0, changed_stages, patched = 0;
	Dx12ShaderStage stage;
	char stages[32];
	size_t pos, size;
	HRESULT hr = S_OK;

	for (stage = DX12_STAGE_VS; stage < DX12_STAGE_COUNT; stage = (Dx12ShaderStage)(stage + 1)) {
		shaders[stage] = variant->shaders[stage] ? variant->shaders[stage] : info->shaders[stage];
		if (variant->shaders[stage])
			mod_stages |= 1u << stage;
		key = key * 0x9e3779b97f4a7c15ull ^ (variant->shaders[stage] ? variant->shaders[stage]->hash : 0);
	}
	key = key * 0x9e3779b97f4a7c15ull ^ variant->removed_stages;
	changed_stages = mod_stages | variant->removed_stages;

	for (const auto &made : info->variants) {
		if (made.first == key) {
			if (why_failed && !made.second)
				*why_failed = "it could not be made before";
			return made.second;
		}
	}

	if (!elements) {
		for (const Dx12InputElement &input : info->input_layout) {
			game_elements.push_back({ input.semantic.c_str(), input.index, input.format, input.slot, input.offset,
				input.per_instance ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
				input.step_rate });
		}
		elements = game_elements.data();
		count = (UINT)game_elements.size();
	}

	creation = info->creation;
	if (creation.empty() || !info->shaders_known) {
		failed = "what the game created it from is not known";
	} else if (!info->creation_stream) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc = (D3D12_GRAPHICS_PIPELINE_STATE_DESC*)creation.data();
		D3D12_SHADER_BYTECODE *const bytecodes[] = { &desc->VS, &desc->PS, &desc->GS, &desc->HS, &desc->DS };

		for (stage = DX12_STAGE_VS; stage <= DX12_STAGE_DS; stage = (Dx12ShaderStage)(stage + 1)) {
			if (!revive_shader(bytecodes[stage], shaders[stage], !!(mod_stages & 1u << stage), !!(variant->removed_stages & 1u << stage)))
				failed = "the game's shaders were not kept";
		}
		if (!failed && (desc->StreamOutput.NumEntries || desc->StreamOutput.NumStrides))
			failed = "it has a stream output";
		desc->StreamOutput = {};
		desc->InputLayout.pInputElementDescs = elements;
		desc->InputLayout.NumElements = count;
		desc->CachedPSO = {};
		if (!failed)
			hr = device->CreateGraphicsPipelineState(desc, IID_PPV_ARGS(&pipeline));
	} else {
		for (pos = 0; !failed && pos < creation.size(); pos += size) {
			D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = *(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(creation.data() + pos);
			BYTE *value = creation.data() + pos + STREAM_VALUE;

			stage = subobject_stage(type);
			size = subobject_size(type);
			if (stage != DX12_STAGE_COUNT) {
				if (!revive_shader((D3D12_SHADER_BYTECODE*)value, shaders[stage], !!(mod_stages & 1u << stage),
						!!(variant->removed_stages & 1u << stage)))
					failed = "the game's shaders were not kept";
				patched |= 1u << stage;
			} else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT) {
				((D3D12_INPUT_LAYOUT_DESC*)value)->pInputElementDescs = elements;
				((D3D12_INPUT_LAYOUT_DESC*)value)->NumElements = count;
			} else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO) {
				*(D3D12_CACHED_PIPELINE_STATE*)value = {};
			} else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT) {
				D3D12_STREAM_OUTPUT_DESC *output = (D3D12_STREAM_OUTPUT_DESC*)value;

				if (output->NumEntries || output->NumStrides)
					failed = "it has a stream output";
				*output = {};
			} else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING) {
				if (((D3D12_VIEW_INSTANCING_DESC*)value)->ViewInstanceCount)
					failed = "it has view instancing";
			}
		}
		// A subobject is not made up for a stage the game's stream has
		// none for: the stream would have to be put together again
		if (!failed && (mod_stages & ~patched))
			failed = "the game's pipeline state stream has no subobject for a shader of the mod's";
		if (!failed && FAILED(device->QueryInterface(IID_PPV_ARGS(&device2))))
			failed = "the device takes no pipeline state streams";
		if (!failed) {
			D3D12_PIPELINE_STATE_STREAM_DESC desc = { creation.size(), creation.data() };

			hr = device2->CreatePipelineState(&desc, IID_PPV_ARGS(&pipeline));
		}
		if (device2)
			device2->Release();
	}

	if (!pipeline && reported < 16) {
		reported++;
		if (!changed_stages && failed)
			Dx12DumpNote("*** A mod's input layout cannot be given to a pipeline of the game: %s. The game's layout is used."
				" (%zu bytes kept, %s, shaders %s, stream of %u bytes read to %u, stopped at subobject %u)\n", failed,
				info->creation.size(), info->creation_stream ? "a stream" : "a description",
				info->shaders_known ? "known" : "not all known", info->stream_bytes, info->stream_read, info->stream_stopped_at);
		else if (!changed_stages)
			Dx12DumpNote("*** Creating a pipeline of the game again with a mod's input layout failed: 0x%x. The game's layout is used.\n", hr);
		else if (failed)
			Dx12DumpNote("*** A mod's %s shader cannot be given to a pipeline of the game: %s. (%zu bytes kept, %s, shaders %s)\n",
				stage_list(changed_stages, stages, sizeof(stages)), failed, info->creation.size(),
				info->creation_stream ? "a stream" : "a description", info->shaders_known ? "known" : "not all known");
		else
			Dx12DumpNote("*** Creating a pipeline of the game again with a mod's %s shader failed: 0x%x. The game's vs is %s%s; "
				"its ps %016llx writes %u render targets%s\n",
				stage_list(changed_stages, stages, sizeof(stages)), hr,
				!info->shaders[DX12_STAGE_VS] ? "none" : info->shaders[DX12_STAGE_VS]->dxil ? "DXIL" : "DXBC",
				!info->shaders[DX12_STAGE_VS] ? "" : info->shaders[DX12_STAGE_VS]->dxil
					? ", which the DXBC of a mod may not be put together with" : "",
				info->shaders[DX12_STAGE_PS] ? info->shaders[DX12_STAGE_PS]->hash : 0,
				info->shaders[DX12_STAGE_PS] ? info->shaders[DX12_STAGE_PS]->targets : 0,
				variant->shaders[DX12_STAGE_PS] ? "" : ", and the mod keeps it");
		if (!failed && variant->shaders[DX12_STAGE_PS])
			Dx12DumpNote("    The mod's ps %016llx writes %u render targets\n",
				variant->shaders[DX12_STAGE_PS]->hash, variant->shaders[DX12_STAGE_PS]->targets);
	}
	if (why_failed && !pipeline)
		*why_failed = failed ? failed : "D3D12 refused it";

	info->variants.emplace_back(key, pipeline);
	return pipeline;
}

const Dx12ShaderInfo* Dx12FindShader(UINT64 hash)
{
	const Dx12ShaderInfo *info = NULL;

	AcquireSRWLockShared(&shader_lock);
	auto i = shaders.find(hash);
	if (i != shaders.end())
		info = i->second;
	ReleaseSRWLockShared(&shader_lock);

	return info;
}

void Dx12GetShaders(size_t from, std::vector<const Dx12ShaderInfo*> *found)
{
	AcquireSRWLockShared(&shader_lock);
	if (from < shader_list.size())
		found->insert(found->end(), shader_list.begin() + from, shader_list.end());
	ReleaseSRWLockShared(&shader_lock);
}

const Dx12PipelineInfo* Dx12FindPipeline(ID3D12PipelineState *pipeline)
{
	const Dx12PipelineInfo *info = NULL;

	AcquireSRWLockShared(&pipeline_lock);
	auto i = pipelines.find(pipeline);
	if (i != pipelines.end())
		info = i->second;
	ReleaseSRWLockShared(&pipeline_lock);

	return info;
}

void Dx12GetPipelineCounts(size_t *pipeline_count, size_t *signature_count, size_t *shader_count)
{
	AcquireSRWLockShared(&pipeline_lock);
	*pipeline_count = pipelines.size();
	ReleaseSRWLockShared(&pipeline_lock);

	AcquireSRWLockShared(&signature_lock);
	*signature_count = signatures.size();
	ReleaseSRWLockShared(&signature_lock);

	AcquireSRWLockShared(&shader_lock);
	*shader_count = shaders.size();
	ReleaseSRWLockShared(&shader_lock);
}
