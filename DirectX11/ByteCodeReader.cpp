//#include "d3d11TokenizedProgramFormat.hpp"
//#include "globals.h"
//
//static D3D_SRV_DIMENSION dxbc_dimension_to_srv(D3D10_SB_RESOURCE_DIMENSION d)
//{
//	switch (d)
//	{
//	case D3D10_SB_RESOURCE_DIMENSION_BUFFER:            return D3D_SRV_DIMENSION_BUFFER;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D:         return D3D_SRV_DIMENSION_TEXTURE1D;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY:    return D3D_SRV_DIMENSION_TEXTURE1DARRAY;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D:         return D3D_SRV_DIMENSION_TEXTURE2D;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY:    return D3D_SRV_DIMENSION_TEXTURE2DARRAY;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMS:       return D3D_SRV_DIMENSION_TEXTURE2DMS;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DMSARRAY:  return D3D_SRV_DIMENSION_TEXTURE2DMSARRAY;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D:         return D3D_SRV_DIMENSION_TEXTURE3D;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBE:       return D3D_SRV_DIMENSION_TEXTURECUBE;
//	case D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY:  return D3D_SRV_DIMENSION_TEXTURECUBEARRAY;
//	case D3D11_SB_RESOURCE_DIMENSION_RAW_BUFFER:        return D3D_SRV_DIMENSION_BUFFEREX;
//	case D3D11_SB_RESOURCE_DIMENSION_STRUCTURED_BUFFER: return D3D_SRV_DIMENSION_BUFFER;
//	default:                                            return D3D_SRV_DIMENSION_UNKNOWN;
//	}
//}
//
//bool get_shader_bindings_from_bytecode(const void* data, size_t size, ShaderBindings* out)
//{
//	if (!data || !out || size < 32)
//		return false;
//
//	const uint8_t* p = static_cast<const uint8_t*>(data);
//	if (*(const uint32_t*)p != MAKEFOURCC('D', 'X', 'B', 'C'))
//		return false;
//
//	const uint32_t num_chunks = *(const uint32_t*)(p + 0x1C);
//	if (num_chunks > (size - 0x20) / sizeof(uint32_t))
//		return false;
//
//	const uint32_t* chunks = reinterpret_cast<const uint32_t*>(p + 0x20);
//
//	for (uint32_t i = 0; i < num_chunks; ++i)
//	{
//		const uint32_t offset = chunks[i];
//		if (offset > size || size - offset < 8)
//			continue;
//
//		const uint8_t* chunk = p + offset;
//		const uint32_t fourcc = *(const uint32_t*)(chunk + 0);
//		if (fourcc != MAKEFOURCC('S', 'H', 'D', 'R') &&
//			fourcc != MAKEFOURCC('S', 'H', 'E', 'X'))
//			continue;
//
//		const uint32_t chunk_size = *(const uint32_t*)(chunk + 4);
//		if (chunk_size < 8 || chunk_size > size - offset - 8)
//			return false;
//
//		const uint32_t version = *(const uint32_t*)(chunk + 8);
//		const uint32_t major = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(version);
//		const uint32_t minor = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(version);
//
//		if (major != 4 && !(major == 5 && minor == 0))
//			return false;
//
//		const uint32_t dword_count = *(const uint32_t*)(chunk + 12);
//		if (dword_count < 2 || dword_count > chunk_size / sizeof(uint32_t))
//			return false;
//
//		const uint32_t* tokens = reinterpret_cast<const uint32_t*>(chunk + 16);
//		uint32_t remaining = dword_count - 2;
//
//		while (remaining)
//		{
//			const uint32_t token = tokens[0];
//			const D3D10_SB_OPCODE_TYPE op = DECODE_D3D10_SB_OPCODE_TYPE(token);
//
//			// Custom-data block.
//			if (op == D3D10_SB_OPCODE_CUSTOMDATA)
//			{
//				if (remaining < 2)
//					return false;
//
//				const uint32_t length = tokens[1];
//				if (length < 2 || length > remaining)
//					return false;
//
//				tokens += length;
//				remaining -= length;
//				continue;
//			}
//
//			const uint32_t length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
//
//			if (!length || length > remaining)
//				return false;
//
//			const uint32_t* end = tokens + length;
//			const uint32_t* operand = tokens + 1;
//
//			switch (op)
//			{
//			case D3D10_SB_OPCODE_DCL_RESOURCE:
//			{
//				// dcl_resource_texture2d t0
//				//   slot = 0, type = TYPED, dimension = D3D_SRV_DIMENSION_TEXTURE2D
//
//				// opcode + operand + slot + return-type token
//				if (length >= 4 &&
//					DECODE_D3D10_SB_OPERAND_TYPE(operand[0]) == D3D10_SB_OPERAND_TYPE_RESOURCE &&
//					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand[0]) == D3D10_SB_OPERAND_INDEX_1D &&
//					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(operand[0], 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
//				{
//					const uint32_t slot = operand[1];
//					if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
//					{
//						auto& r = out->resources[slot];
//						r.type = ShaderResourceType::TYPED;
//						r.dimension = dxbc_dimension_to_srv(DECODE_D3D10_SB_RESOURCE_DIMENSION(token));
//						r.stride = 0;
//						//LogInfo("DCL SRV TYPED slot=%u\n", slot);
//					}
//				}
//				break;
//			}
//
//			case D3D11_SB_OPCODE_DCL_RESOURCE_RAW:
//			{
//				// dcl_resource_raw t0
//				//   slot = 1, type = RAW, stride = 0, dimension = D3D_SRV_DIMENSION_BUFFEREX
//
//				// opcode + operand + slot
//				if (length >= 3 &&
//					DECODE_D3D10_SB_OPERAND_TYPE(operand[0]) == D3D10_SB_OPERAND_TYPE_RESOURCE &&
//					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand[0]) == D3D10_SB_OPERAND_INDEX_1D &&
//					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(operand[0], 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
//				{
//					const uint32_t slot = operand[1];
//					if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
//					{
//						auto& r = out->resources[slot];
//						r.type = ShaderResourceType::RAW;
//						r.dimension = D3D_SRV_DIMENSION_BUFFEREX;
//						r.stride = 0;
//						//LogInfo("DCL SRV RAW slot=%u\n", slot);
//					}
//				}
//				break;
//			}
//
//			case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED:
//			{
//				// dcl_resource_structured t0, 16
//				//    slot = 2, stride = 16, type = STRUCTURED, dimension = D3D_SRV_DIMENSION_BUFFER
//
//				// opcode + operand + slot + stride
//				if (length >= 4 &&
//					DECODE_D3D10_SB_OPERAND_TYPE(operand[0]) == D3D10_SB_OPERAND_TYPE_RESOURCE &&
//					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand[0]) == D3D10_SB_OPERAND_INDEX_1D &&
//					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(operand[0], 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
//				{
//					const uint32_t slot = operand[1];
//					if (slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
//					{
//						auto& r = out->resources[slot];
//						r.type = ShaderResourceType::STRUCTURED;
//						r.dimension = D3D_SRV_DIMENSION_BUFFER;
//						r.stride = operand[2];
//						//LogInfo("DCL SRV STRUCTURED slot=%u, stride=%u\n", slot, operand[2]);
//					}
//				}
//				break;
//			}
//
//			case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER:
//			{
//				// dcl_constantbuffer cb0[4], immediateIndexed
//				//   slot = 0, size = 4, type = IMMEDIATE_INDEXED
//				// dcl_constantbuffer cb1[16], dynamicIndexed
//				//   slot = 1, size = 16, type = DYNAMIC_INDEXED
//
//				// opcode + cb operand + slot + size
//				if (length >= 4 &&
//					DECODE_D3D10_SB_OPERAND_TYPE(operand[0]) == D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER &&
//					DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(operand[0]) == D3D10_SB_OPERAND_INDEX_2D &&
//					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(operand[0], 0) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32 &&
//					DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(operand[0], 1) == D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
//				{
//					const uint32_t slot = operand[1];
//					if (slot < D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT)
//					{
//						auto& cb = out->constant_buffers[slot];
//						cb.size = operand[2];
//						cb.type = DECODE_D3D10_SB_CONSTANT_BUFFER_ACCESS_PATTERN(token) ==
//							D3D10_SB_CONSTANT_BUFFER_DYNAMIC_INDEXED
//							? ShaderConstantBufferType::DYNAMIC_INDEXED
//							: ShaderConstantBufferType::IMMEDIATE_INDEXED;
//						//LogInfo("DCL CB slot=%u, size=%u, dynamic=%d\n", slot, cb.size, cb.type == ShaderConstantBufferType::DYNAMIC_INDEXED);
//					}
//				}
//				break;
//			}
//
//			default:
//				break;
//			}
//
//			tokens = end;
//			remaining -= length;
//		}
//
//		return true;
//	}
//
//	return false;
//}
