#include "Spirv.h"

#include <vector>

enum {
	OpName = 5,
	OpEntryPoint = 15,
	OpTypeBool = 20,
	OpTypeInt = 21,
	OpTypeFloat = 22,
	OpTypeVector = 23,
	OpTypeMatrix = 24,
	OpTypeImage = 25,
	OpTypeSampler = 26,
	OpTypeSampledImage = 27,
	OpTypeArray = 28,
	OpTypeRuntimeArray = 29,
	OpTypeStruct = 30,
	OpTypePointer = 32,
	OpConstant = 43,
	OpVariable = 59,
	OpDecorate = 71,
	OpMemberDecorate = 72,
	OpTypeAccelerationStructureKHR = 5341,
};

enum {
	DecorationBufferBlock = 3,
	DecorationArrayStride = 6,
	DecorationMatrixStride = 7,
	DecorationNonWritable = 24,
	DecorationLocation = 30,
	DecorationBinding = 33,
	DecorationDescriptorSet = 34,
	DecorationOffset = 35,
};

enum {
	StorageUniformConstant = 0,
	StorageInput = 1,
	StorageUniform = 2,
	StoragePushConstant = 9,
	StorageStorageBuffer = 12,
};

const uint32_t SPIRV_MAGIC = 0x07230203;
const uint32_t NONE = 0xffffffff;
const uint32_t DIM_BUFFER = 5;

struct Id {
	// The instruction that defines the id, 0 if none of interest does
	uint32_t op;
	// Its operands after the result id. For a constant, its first word.
	const uint32_t *operands;
	uint32_t count;
	uint32_t value;

	std::string name;
	uint32_t set, binding, location;
	uint32_t array_stride;
	bool buffer_block, non_writable;
	// By member, for a struct
	std::vector<uint32_t> offsets, matrix_strides;
};

static std::string literal_string(const uint32_t *words, uint32_t count)
{
	const char *chars = (const char*)words;
	size_t length = strnlen(chars, count * sizeof(uint32_t));
	std::string text(chars, length);

	// These end up in a CSV where a space separates entries
	for (char &c : text) {
		if (c == ',' || c == ' ' || c == ':' || (unsigned char)c < 0x20)
			c = '_';
	}
	return text;
}

static uint32_t type_bytes(const std::vector<Id> &ids, uint32_t type, uint32_t matrix_stride, int depth)
{
	const Id *id;
	uint32_t bytes = 0, member, end, i;

	if (type >= ids.size() || depth > 16)
		return 0;
	id = &ids[type];

	switch (id->op) {
		case OpTypeBool:
			return 4;
		case OpTypeInt:
		case OpTypeFloat:
			return id->count >= 1 ? id->operands[0] / 8 : 0;
		case OpTypeVector:
			return id->count >= 2 ? id->operands[1] * type_bytes(ids, id->operands[0], 0, depth + 1) : 0;
		case OpTypeMatrix:
			if (id->count < 2)
				return 0;
			if (matrix_stride)
				return id->operands[1] * matrix_stride;
			return id->operands[1] * type_bytes(ids, id->operands[0], 0, depth + 1);
		case OpTypeArray:
			if (id->count < 2 || id->operands[1] >= ids.size())
				return 0;
			if (id->array_stride)
				return ids[id->operands[1]].value * id->array_stride;
			return ids[id->operands[1]].value * type_bytes(ids, id->operands[0], matrix_stride, depth + 1);
		case OpTypeStruct:
			for (i = 0; i < id->count; i++) {
				if (i >= id->offsets.size() || id->offsets[i] == NONE)
					continue;
				member = type_bytes(ids, id->operands[i],
					i < id->matrix_strides.size() ? id->matrix_strides[i] : 0, depth + 1);
				end = id->offsets[i] + member;
				if (end > bytes)
					bytes = end;
			}
			return bytes;
	}

	return 0;
}

static void describe_resource(const std::vector<Id> &ids, const Id *variable, uint32_t storage, SpirvInfo *info)
{
	static const char *const kinds[] = { "ubo", "ssbo", "tex", "samp", "texsamp", "img", "tbuf", "itbuf", "as" };
	const Id *pointer, *type = NULL, *image = NULL;
	SpirvBinding binding = {};
	uint32_t type_id, bytes = 0;
	const std::string *name = &variable->name;
	char text[64];
	int depth;

	if (variable->operands[0] >= ids.size())
		return;
	pointer = &ids[variable->operands[0]];
	if (pointer->op != OpTypePointer || pointer->count < 2)
		return;

	// An array of textures is declared like one texture
	type_id = pointer->operands[1];
	for (depth = 0; depth < 8; depth++) {
		if (type_id >= ids.size())
			return;
		type = &ids[type_id];
		if ((type->op != OpTypeArray && type->op != OpTypeRuntimeArray) || type->count < 1)
			break;
		type_id = type->operands[0];
	}
	if (!type)
		return;

	switch (type->op) {
		case OpTypeStruct:
			bytes = type_bytes(ids, type_id, 0, 0);
			if (storage == StoragePushConstant) {
				_snprintf_s(text, _TRUNCATE, "pc:%u ", bytes);
				info->resources += text;
				return;
			}
			binding.kind = (storage == StorageStorageBuffer || type->buffer_block) ? SPIRV_SSBO : SPIRV_UBO;
			binding.bytes = bytes;
			if (binding.kind == SPIRV_SSBO && type->count) {
				uint32_t last = type->operands[type->count - 1];

				if (last < ids.size() && (ids[last].op == OpTypeRuntimeArray || ids[last].op == OpTypeArray))
					binding.stride = ids[last].array_stride;
				binding.read_only = type->non_writable || variable->non_writable;
			}
			// Compilers name the block and leave its instance without
			if (name->empty())
				name = &type->name;
			break;
		case OpTypeSampledImage:
			binding.kind = SPIRV_TEXTURE_SAMPLER;
			if (type->count >= 1 && type->operands[0] < ids.size() && ids[type->operands[0]].op == OpTypeImage) {
				image = &ids[type->operands[0]];
				if (image->count >= 2 && image->operands[1] == DIM_BUFFER)
					binding.kind = SPIRV_TEXEL_BUFFER;
			}
			break;
		case OpTypeImage:
			if (type->count < 6)
				return;
			image = type;
			if (type->operands[1] == DIM_BUFFER)
				binding.kind = type->operands[5] == 2 ? SPIRV_STORAGE_TEXEL_BUFFER : SPIRV_TEXEL_BUFFER;
			else
				binding.kind = type->operands[5] == 2 ? SPIRV_STORAGE_IMAGE : SPIRV_TEXTURE;
			break;
		case OpTypeSampler:
			binding.kind = SPIRV_SAMPLER;
			break;
		case OpTypeAccelerationStructureKHR:
			binding.kind = SPIRV_ACCELERATION_STRUCTURE;
			break;
		default:
			return;
	}
	// Sampled type, Dim, Depth, Arrayed, ...
	if (image && image->count >= 4) {
		binding.dim = image->operands[1];
		binding.arrayed = !!image->operands[3];
	}

	if (variable->set == NONE || variable->binding == NONE)
		return;

	if (name->empty())
		info->unnamed++;
	else
		info->named++;

	binding.set = variable->set;
	binding.binding = variable->binding;
	info->bindings.push_back(binding);

	_snprintf_s(text, _TRUNCATE, "s%ub%u:%s", variable->set, variable->binding, kinds[binding.kind]);
	info->resources += text;
	if (type->op == OpTypeStruct) {
		_snprintf_s(text, _TRUNCATE, ":%u", bytes);
		info->resources += text;
	}
	if (!name->empty())
		info->resources += ":" + *name;
	info->resources += " ";
}

void SpirvReflect(const uint32_t *code, size_t words, SpirvInfo *info)
{
	static const uint32_t stages[] = {
		VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
		VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
		VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT,
	};
	std::vector<Id> ids;
	std::vector<uint32_t> variables;
	size_t pos;
	char text[32];

	*info = {};

	if (words < 5 || code[0] != SPIRV_MAGIC || code[3] > (1u << 22))
		return;
	info->version = code[1];
	info->generator = code[2];
	ids.resize(code[3]);
	for (Id &id : ids)
		id.set = id.binding = id.location = NONE;

	for (pos = 5; pos < words;) {
		const uint32_t *ins = &code[pos];
		uint32_t count = ins[0] >> 16, op = ins[0] & 0xffff;
		uint32_t result = NONE, first = 0;

		if (!count || pos + count > words)
			return;
		pos += count;

		switch (op) {
			case OpEntryPoint:
				if (count >= 4 && !info->stage && ins[1] < ARRAYSIZE(stages)) {
					info->stage = stages[ins[1]];
					info->entry = literal_string(&ins[3], count - 3);
				}
				break;
			case OpName:
				if (count >= 3 && ins[1] < ids.size())
					ids[ins[1]].name = literal_string(&ins[2], count - 2);
				break;
			case OpDecorate:
				if (count < 3 || ins[1] >= ids.size())
					break;
				if (ins[2] == DecorationBufferBlock)
					ids[ins[1]].buffer_block = true;
				if (ins[2] == DecorationNonWritable)
					ids[ins[1]].non_writable = true;
				if (count < 4)
					break;
				if (ins[2] == DecorationDescriptorSet)
					ids[ins[1]].set = ins[3];
				else if (ins[2] == DecorationBinding)
					ids[ins[1]].binding = ins[3];
				else if (ins[2] == DecorationLocation)
					ids[ins[1]].location = ins[3];
				else if (ins[2] == DecorationArrayStride)
					ids[ins[1]].array_stride = ins[3];
				break;
			case OpMemberDecorate:
				if (count >= 4 && ins[1] < ids.size() && ins[3] == DecorationNonWritable)
					ids[ins[1]].non_writable = true;
				if (count < 5 || ins[1] >= ids.size() || ins[2] >= 4096)
					break;
				if (ins[3] == DecorationOffset) {
					if (ids[ins[1]].offsets.size() <= ins[2])
						ids[ins[1]].offsets.resize(ins[2] + 1, NONE);
					ids[ins[1]].offsets[ins[2]] = ins[4];
				} else if (ins[3] == DecorationMatrixStride) {
					if (ids[ins[1]].matrix_strides.size() <= ins[2])
						ids[ins[1]].matrix_strides.resize(ins[2] + 1, 0);
					ids[ins[1]].matrix_strides[ins[2]] = ins[4];
				}
				break;
			case OpTypeBool:
			case OpTypeInt:
			case OpTypeFloat:
			case OpTypeVector:
			case OpTypeMatrix:
			case OpTypeImage:
			case OpTypeSampler:
			case OpTypeSampledImage:
			case OpTypeArray:
			case OpTypeRuntimeArray:
			case OpTypeStruct:
			case OpTypePointer:
			case OpTypeAccelerationStructureKHR:
				// Result id, then the operands
				result = count >= 2 ? ins[1] : NONE;
				first = 2;
				break;
			case OpConstant:
			case OpVariable:
				// Type, result id, then the operands. The type is kept
				// as the first of them.
				result = count >= 3 ? ins[2] : NONE;
				first = 1;
				break;
		}

		if (result == NONE || result >= ids.size())
			continue;

		ids[result].op = op;
		ids[result].operands = &ins[first];
		ids[result].count = count - first;
		if (op == OpVariable) {
			ids[result].operands = &ins[1];
			variables.push_back(result);
		} else if (op == OpConstant) {
			ids[result].value = count >= 4 ? ins[3] : 0;
		}
	}

	for (uint32_t variable : variables) {
		const Id *id = &ids[variable];
		// Type, result id, storage class
		uint32_t storage = id->count >= 3 ? id->operands[2] : NONE;

		switch (storage) {
			case StorageInput:
				if (id->location == NONE)
					break;
				_snprintf_s(text, _TRUNCATE, "%u:", id->location);
				info->inputs += text + id->name + " ";
				break;
			case StorageUniformConstant:
			case StorageUniform:
			case StorageStorageBuffer:
			case StoragePushConstant:
				describe_resource(ids, id, storage, info);
				break;
		}
	}

	info->valid = true;
}
