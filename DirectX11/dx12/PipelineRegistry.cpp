#include "PipelineRegistry.h"

#include <unordered_map>

#include "util.h"

#include "HashDump.h"
#include "ResourceTracker.h"

const char *const dx12_stage_names[DX12_STAGE_COUNT] = { "vs", "ps", "gs", "hs", "ds", "cs" };

static SRWLOCK shader_lock = SRWLOCK_INIT;
static std::unordered_map<UINT64, Dx12ShaderInfo*> shaders;

static SRWLOCK pipeline_lock = SRWLOCK_INIT;
static std::unordered_map<ID3D12PipelineState*, Dx12PipelineInfo*> pipelines;

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
		if (space || upper < lower)
			continue;

		// PSVResourceType: 2 is a CBV, 3-5 SRVs, 6-9 UAVs
		if (type == 2) {
			mark_registers(&info->cbv, 1, lower, upper - lower + 1);
		} else if (type >= 3 && type <= 5) {
			if (upper == UINT_MAX)
				info->unbounded_srv = true;
			else
				mark_registers(info->srv, 2, lower, upper - lower + 1);
		} else if (type >= 6 && type <= 9) {
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
	size_t pos, record_size;

	if (!read32(rdef, 8, &count) || !read32(rdef, 12, &offset) || !read32(rdef, 16, &version) || count > 65536)
		return false;
	record_size = (version & 0xffff) == 0x0501 ? 40 : 32;

	for (i = 0, pos = offset; i < count; i++, pos += record_size) {
		if (!read32(rdef, pos + 4, &type) || !read32(rdef, pos + 20, &reg) || !read32(rdef, pos + 24, &reg_count))
			return false;
		space = 0;
		if (record_size == 40 && !read32(rdef, pos + 32, &space))
			return false;
		if (space)
			continue;

		switch (type) {
			case D3D_SIT_CBUFFER:
				mark_registers(&info->cbv, 1, reg, reg_count ? reg_count : 1);
				break;
			case D3D_SIT_TBUFFER:
			case D3D_SIT_TEXTURE:
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
				// A bind count of 0 is an unbounded array
				if (!reg_count)
					info->unbounded_srv = true;
				else
					mark_registers(info->srv, 2, reg, reg_count);
				break;
			case D3D_SIT_UAV_RWTYPED:
			case D3D_SIT_UAV_RWSTRUCTURED:
			case D3D_SIT_UAV_RWBYTEADDRESS:
			case D3D_SIT_UAV_APPEND_STRUCTURED:
			case D3D_SIT_UAV_CONSUME_STRUCTURED:
			case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
				mark_registers(&info->uav, 1, reg, reg_count ? reg_count : 64);
				break;
		}
	}

	return true;
}

static const Dx12ShaderInfo* shader_info(Dx12ShaderStage stage, const D3D12_SHADER_BYTECODE *bytecode)
{
	Dx12ShaderInfo *info, *existing = NULL;
	Blob container, part;
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
	info->size = (UINT32)bytecode->BytecodeLength;

	container.data = (const BYTE*)bytecode->pShaderBytecode;
	container.size = bytecode->BytecodeLength;
	info->dxil = find_part(container, "DXIL", &part);
	if (find_part(container, "PSV0", &part))
		info->reflected = reflect_psv0(part, info);
	else if (find_part(container, "RDEF", &part))
		info->reflected = reflect_rdef(part, info);

	AcquireSRWLockExclusive(&shader_lock);
	auto inserted = shaders.emplace(hash, info);
	ReleaseSRWLockExclusive(&shader_lock);

	if (!inserted.second) {
		delete info;
		return inserted.first->second;
	}

	Dx12DumpShader(stage, info);
	return info;
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
	}
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

void Dx12RegisterPipeline(void **ppPipelineState, const D3D12_PIPELINE_STATE_STREAM_DESC *desc)
{
	Dx12PipelineInfo *info;
	const BYTE *stream;
	size_t pos = 0, size;

	if (!desc || !desc->pPipelineStateSubobjectStream)
		return;

	info = new Dx12PipelineInfo();
	stream = (const BYTE*)desc->pPipelineStateSubobjectStream;

#define SUBOBJECT(T) \
	size = sizeof(StreamSubobject<T>); \
	if (desc->SizeInBytes - pos < size) { \
		size = 0; \
		break; \
	}
#define SHADER(stage) \
	SUBOBJECT(D3D12_SHADER_BYTECODE) \
	info->shaders[stage] = shader_info(stage, &((const StreamSubobject<D3D12_SHADER_BYTECODE>*)(stream + pos))->value);

	while (desc->SizeInBytes - pos >= sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE)) {
		size = 0;
		switch (*(const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE*)(stream + pos)) {
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS: SHADER(DX12_STAGE_VS) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS: SHADER(DX12_STAGE_PS) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS: SHADER(DX12_STAGE_GS) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS: SHADER(DX12_STAGE_HS) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS: SHADER(DX12_STAGE_DS) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS: SHADER(DX12_STAGE_CS) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
				SUBOBJECT(D3D12_SHADER_BYTECODE)
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
				SUBOBJECT(D3D12_INPUT_LAYOUT_DESC)
				set_input_layout(info, &((const StreamSubobject<D3D12_INPUT_LAYOUT_DESC>*)(stream + pos))->value);
				break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE: SUBOBJECT(ID3D12RootSignature*) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: SUBOBJECT(D3D12_STREAM_OUTPUT_DESC) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND: SUBOBJECT(D3D12_BLEND_DESC) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: SUBOBJECT(UINT) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER: SUBOBJECT(D3D12_RASTERIZER_DESC) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER1: SUBOBJECT(D3D12_RASTERIZER_DESC1) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2: SUBOBJECT(D3D12_RASTERIZER_DESC2) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: SUBOBJECT(D3D12_DEPTH_STENCIL_DESC) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: SUBOBJECT(D3D12_DEPTH_STENCIL_DESC1) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2: SUBOBJECT(D3D12_DEPTH_STENCIL_DESC2) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: SUBOBJECT(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: SUBOBJECT(D3D12_PRIMITIVE_TOPOLOGY_TYPE) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: SUBOBJECT(D3D12_RT_FORMAT_ARRAY) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: SUBOBJECT(DXGI_FORMAT) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: SUBOBJECT(DXGI_SAMPLE_DESC) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK: SUBOBJECT(UINT) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO: SUBOBJECT(D3D12_CACHED_PIPELINE_STATE) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS: SUBOBJECT(D3D12_PIPELINE_STATE_FLAGS) break;
			case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: SUBOBJECT(D3D12_VIEW_INSTANCING_DESC) break;
		}

		// A subobject this code does not know the size of, or a
		// truncated stream. Shaders come first in the streams engines
		// build, so what was found so far is still worth keeping.
		if (!size)
			break;
		pos += size;
	}

#undef SHADER
#undef SUBOBJECT

	register_pipeline(ppPipelineState, info);
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
