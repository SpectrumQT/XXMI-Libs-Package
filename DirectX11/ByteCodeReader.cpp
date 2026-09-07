#include "d3d11TokenizedProgramFormat.hpp"
#include "globals.h"

static D3D_SRV_DIMENSION dxbc_dimension_to_srv(D3D10_SB_RESOURCE_DIMENSION d)
{
	switch (d)
	{
	case D3D10_SB_RESOURCE_DIMENSION_BUFFER:            return D3D_SRV_DIMENSION_BUFFER;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D:         return D3D_SRV_DIMENSION_TEXTURE1D;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY:    return D3D_SRV_DIMENSION_TEXTURE1DARRAY;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D:         return D3D_SRV_DIMENSION_TEXTURE2D;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY:    return D3D_SRV_DIMENSION_TEXTURE2DARRAY;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS:       return D3D_SRV_DIMENSION_TEXTURE2DMS;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:  return D3D_SRV_DIMENSION_TEXTURE2DMSARRAY;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D:         return D3D_SRV_DIMENSION_TEXTURE3D;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE:       return D3D_SRV_DIMENSION_TEXTURECUBE;
	case D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY:  return D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
	case D3D11_SB_RESOURCE_DIMENSION_RAW_BUFFER:        return D3D_SRV_DIMENSION_BUFFEREX;
	case D3D11_SB_RESOURCE_DIMENSION_STRUCTURED_BUFFER: return D3D_SRV_DIMENSION_BUFFER;
	default:                                            return D3D_SRV_DIMENSION_UNKNOWN;
	}
}

static uint32_t read_u32(const uint8_t* ptr)
{
	uint32_t value;
	std::memcpy(&value, ptr, sizeof(value));
	return value;
}

bool get_shader_bindings_from_bytecode(const void* data, size_t size, ShaderBindings* out)
{
	if (!data || !out || size < 32)
		return false;

	const uint8_t* p = static_cast<const uint8_t*>(data);
	if (read_u32(p) != MAKEFOURCC('D', 'X', 'B', 'C'))
		return false;

	const uint32_t num_chunks = read_u32(p + 0x1C);
	if (num_chunks > (size - 0x20) / sizeof(uint32_t))
		return false;

	for (uint32_t i = 0; i < num_chunks; ++i)
	{
		const uint32_t offset = read_u32(p + 0x20 + i * sizeof(uint32_t));

		// A chunk table entry pointing outside the container makes the DXBC invalid.
		if (offset > size || size - offset < 8)
			return false;

		// Each DXBC chunk starts with a FourCC and payload size.
		const uint8_t* chunk = p + offset;
		const uint32_t fourcc = read_u32(chunk + 0);
		if (fourcc != MAKEFOURCC('S', 'H', 'D', 'R') &&
			fourcc != MAKEFOURCC('S', 'H', 'E', 'X'))
			continue;

		// The chunk payload must fit entirely within the DXBC container.
		const uint32_t chunk_size = read_u32(chunk + 4);
		if (chunk_size < 8 || chunk_size > size - offset - 8)
			return false;

		const uint32_t version = read_u32(chunk + 8);
		const uint32_t major = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(version);
		const uint32_t minor = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(version);

		// This parser handles SM4.x and SM5.0 tokenized shader bytecode.
		if (major != 4 && !(major == 5 && minor == 0))
			return false;

		const uint32_t dword_count = read_u32(chunk + 12);
		const size_t token_bytes = static_cast<size_t>(dword_count) * sizeof(uint32_t);
		if (dword_count < 2 || token_bytes > chunk_size)
			return false;

		// The first two DWORDs are the version and token count.
		// The remaining DWORDs contain tokenized instructions and custom-data blocks.
		const uint8_t* tokens = chunk + 16;
		uint32_t remaining = dword_count - 2;

		while (remaining)
		{
			const uint32_t token = read_u32(tokens);
			const D3D10_SB_OPCODE_TYPE op = DECODE_D3D10_SB_OPCODE_TYPE(token);

			// Custom-data block: its length is stored in the second DWORD.
			if (op == D3D10_SB_OPCODE_CUSTOMDATA)
			{
				if (remaining < 2)
					return false;

				const uint32_t length = read_u32(tokens + sizeof(uint32_t));
				if (length < 2 || length > remaining)
					return false;

				tokens += length * sizeof(uint32_t);
				remaining -= length;
				continue;
			}

			const uint32_t length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);

			if (!length || length > remaining)
				return false;
			
			const uint8_t* end = tokens + length * sizeof(uint32_t);

			switch (op)
			{
			case D3D10_SB_OPCODE_DCL_RESOURCE:
			{
				// dcl_resource_texture2d t0
				//   slot = 0, type = TYPED, dimension = D3D_SRV_DIMENSION_TEXTURE2D

				// DWORD 0: opcode
				// DWORD 1: resource operand
				// DWORD 2: slot
				// DWORD 3: return type
				if (length < 4)
					return false;

				const uint32_t resource = read_u32(tokens + 4);
				const uint32_t slot = read_u32(tokens + 8);

				if (DECODE_D3D10_SB_OPERAND_TYPE(resource) == D3D10_SB_OPERAND_TYPE_RESOURCE &&
					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(resource) == D3D10_SB_OPERAND_INDEX_1D &&
					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(resource, 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
				{
					if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
					{
						auto& r = out->resources[slot];
						r.type = ShaderResourceType::TYPED;
						r.dimension = dxbc_dimension_to_srv(DECODE_D3D10_SB_RESOURCE_DIMENSION(token));
						r.stride = 0;
						//LogInfo("DCL SRV TYPED slot=%u\n", slot);
					}
				}
				break;
			}

			case D3D11_SB_OPCODE_DCL_RESOURCE_RAW:
			{
				// dcl_resource_raw t0
				//   slot = 1, type = RAW, stride = 0, dimension = D3D_SRV_DIMENSION_BUFFEREX

				// DWORD 0: opcode
				// DWORD 1: resource operand
				// DWORD 2: slot
				if (length < 3)
					return false;

				const uint32_t resource = read_u32(tokens + 4);
				const uint32_t slot = read_u32(tokens + 8);

				if (DECODE_D3D10_SB_OPERAND_TYPE(resource) == D3D10_SB_OPERAND_TYPE_RESOURCE &&
					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(resource) == D3D10_SB_OPERAND_INDEX_1D &&
					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(resource, 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
				{
					if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
					{
						auto& r = out->resources[slot];
						r.type = ShaderResourceType::RAW;
						r.dimension = D3D_SRV_DIMENSION_BUFFEREX;
						r.stride = 0;
						//LogInfo("DCL SRV RAW slot=%u\n", slot);
					}
				}
				break;
			}

			case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED:
			{
				// dcl_resource_structured t0, 16
				//    slot = 2, stride = 16, type = STRUCTURED, dimension = D3D_SRV_DIMENSION_BUFFER

				// DWORD 0: opcode
				// DWORD 1: resource operand
				// DWORD 2: slot
				// DWORD 3: stride
				if (length < 4)
					return false;

				const uint32_t resource = read_u32(tokens + 4);
				const uint32_t slot = read_u32(tokens + 8);
				const uint32_t stride = read_u32(tokens + 12);

				if (DECODE_D3D10_SB_OPERAND_TYPE(resource) == D3D10_SB_OPERAND_TYPE_RESOURCE &&
					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(resource) == D3D10_SB_OPERAND_INDEX_1D &&
					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(resource, 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
				{
					if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
					{
						auto& r = out->resources[slot];
						r.type = ShaderResourceType::STRUCTURED;
						r.dimension = D3D_SRV_DIMENSION_BUFFER;
						r.stride = stride;
						//LogInfo("DCL SRV STRUCTURED slot=%u, stride=%u\n", slot, stride);
					}
				}
				break;
			}

			case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER:
			{
				// dcl_constantbuffer cb0[4], immediateIndexed
				//   slot = 0, size = 4, type = IMMEDIATE_INDEXED
				// dcl_constantbuffer cb1[16], dynamicIndexed
				//   slot = 1, size = 16, type = DYNAMIC_INDEXED

				// DWORD 0: opcode
				// DWORD 1: CB operand
				// DWORD 2: slot
				// DWORD 3: size
				if (length < 4)
					return false;

				const uint32_t cb = read_u32(tokens + 4);
				const uint32_t slot = read_u32(tokens + 8);
				const uint32_t cb_size = read_u32(tokens + 12);

				if (DECODE_D3D10_SB_OPERAND_TYPE(cb) == D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER &&
					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(cb) == D3D10_SB_OPERAND_INDEX_2D &&
					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(cb, 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 &&
					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(cb, 1) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
				{
					if (slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
					{
						auto& cb = out->constant_buffers[slot];
						cb.size = cb_size;
						cb.type = DECODE_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(token) ==
							D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED
							? ShaderConstantBufferType::DYNAMIC_INDEXED
							: ShaderConstantBufferType::IMMEDIATE_INDEXED;
						//LogInfo("DCL CB slot=%u, size=%u, dynamic=%d\n", slot, cb_size, cb.type == ShaderConstantBufferType::DYNAMIC_INDEXED);
					}
				}
				break;
			}

			default:
				break;
			}

			tokens = end;
			remaining -= length;
		}

		return true;
	}

	return false;
}
