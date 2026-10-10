#include "DxilMetadata.h"

#include <map>
#include <string>

// DXIL is LLVM 3.7 bitcode. This reads as much of the bitstream as it takes
// to follow the named metadata node dx.resources to the sizes of the constant
// buffers:
//
//   !dx.resources = !{ !{ SRVs, UAVs, CBuffers, Samplers } }
//   CBuffers = !{ !{ i32 id, T* undef, !"name", i32 space, i32 register,
//                    i32 count, i32 bytes, tags }, ... }
//
// An i32 in metadata refers to a constant of the module by its number among
// the module's values, so the global values and constants have to be counted
// as well. Everything else is skipped over.
//
// The shader comes from the game: every read is bounds checked and anything
// unexpected makes the whole thing fail.

namespace {

const unsigned END_BLOCK = 0, ENTER_SUBBLOCK = 1, DEFINE_ABBREV = 2, UNABBREV_RECORD = 3;
const unsigned BLOCKINFO_BLOCK = 0, MODULE_BLOCK = 8, CONSTANTS_BLOCK = 11, METADATA_BLOCK = 15;
const unsigned BLOCKINFO_SETBID = 1;
const unsigned MODULE_GLOBALVAR = 7, MODULE_FUNCTION = 8, MODULE_ALIAS = 9;
const unsigned CST_SETTYPE = 1, CST_NULL = 2, CST_INTEGER = 4;
const unsigned METADATA_STRING = 1, METADATA_VALUE = 2, METADATA_NODE = 3, METADATA_NAME = 4, METADATA_DISTINCT_NODE = 5,
	METADATA_KIND = 6, METADATA_NAMED_NODE = 10;

const size_t MAX_RECORD = 1 << 20;
const unsigned MAX_DEPTH = 8;

enum Encoding { ENC_LITERAL, ENC_FIXED, ENC_VBR, ENC_ARRAY, ENC_CHAR6, ENC_BLOB };

struct AbbrevOp {
	Encoding encoding;
	UINT64 value;
};
typedef std::vector<AbbrevOp> Abbrev;

struct MetadataEntry {
	enum Kind { OTHER, STRING, VALUE, NODE } kind;
	std::string string;
	UINT64 value;
	std::vector<UINT64> operands;
};

struct Reader {
	const BYTE *data;
	UINT64 bits;
	UINT64 pos;
	bool failed;

	std::map<unsigned, std::vector<Abbrev>> block_info;

	// Whether a value of the module is an integer constant, and which
	std::vector<bool> value_known;
	std::vector<INT64> values;
	std::vector<MetadataEntry> metadata;
	std::map<std::string, std::vector<UINT64>> named;

	UINT64 fixed(unsigned width)
	{
		UINT64 value = 0;
		unsigned i;

		if (width > 64 || pos + width > bits) {
			failed = true;
			return 0;
		}
		for (i = 0; i < width; i++, pos++)
			value |= (UINT64)((data[pos >> 3] >> (pos & 7)) & 1) << i;
		return value;
	}

	UINT64 vbr(unsigned width)
	{
		UINT64 value = 0, piece, high = 1ull << (width - 1);
		unsigned shift = 0;

		do {
			piece = fixed(width);
			if (shift < 64)
				value |= (piece & (high - 1)) << shift;
			shift += width - 1;
		} while ((piece & high) && !failed && shift < 128);
		return value;
	}

	void align()
	{
		pos = (pos + 31) & ~31ull;
		if (pos > bits)
			failed = true;
	}

	bool define_abbrev(Abbrev *abbrev)
	{
		UINT64 count = vbr(5), i;
		AbbrevOp op;

		for (i = 0; i < count && !failed; i++) {
			if (fixed(1)) {
				op.encoding = ENC_LITERAL;
				op.value = vbr(8);
			} else {
				op.value = 0;
				switch (fixed(3)) {
				case 1: op.encoding = ENC_FIXED; op.value = vbr(5); break;
				case 2: op.encoding = ENC_VBR; op.value = vbr(5); break;
				case 3: op.encoding = ENC_ARRAY; break;
				case 4: op.encoding = ENC_CHAR6; break;
				case 5: op.encoding = ENC_BLOB; break;
				default: failed = true; break;
				}
				if (op.encoding == ENC_VBR && !op.value)
					failed = true;
			}
			abbrev->push_back(op);
		}
		return !failed;
	}

	UINT64 scalar(const AbbrevOp &op)
	{
		static const char char6[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

		switch (op.encoding) {
		case ENC_LITERAL: return op.value;
		case ENC_FIXED: return fixed((unsigned)op.value);
		case ENC_VBR: return vbr((unsigned)op.value);
		case ENC_CHAR6: return (BYTE)char6[fixed(6)];
		default: failed = true; return 0;
		}
	}

	bool record(unsigned id, const std::vector<Abbrev> &abbrevs, std::vector<UINT64> *ops)
	{
		UINT64 count, i;
		size_t n;

		ops->clear();

		if (id == UNABBREV_RECORD) {
			ops->push_back(vbr(6));
			count = vbr(6);
			if (count > MAX_RECORD)
				failed = true;
			for (i = 0; i < count && !failed; i++)
				ops->push_back(vbr(6));
			return !failed;
		}

		if (id - 4 >= abbrevs.size()) {
			failed = true;
			return false;
		}
		const Abbrev &abbrev = abbrevs[id - 4];

		for (n = 0; n < abbrev.size() && !failed; n++) {
			switch (abbrev[n].encoding) {
			case ENC_ARRAY:
				// The encoding of the elements is the operand
				// after, which is the last one
				if (n + 2 != abbrev.size()) {
					failed = true;
					break;
				}
				count = vbr(6);
				if (count > MAX_RECORD)
					failed = true;
				for (i = 0; i < count && !failed; i++)
					ops->push_back(scalar(abbrev[n + 1]));
				n++;
				break;
			case ENC_BLOB:
				count = vbr(6);
				align();
				if (count > MAX_RECORD || pos + count * 8 > bits) {
					failed = true;
					break;
				}
				for (i = 0; i < count; i++)
					ops->push_back(data[(pos >> 3) + i]);
				pos += count * 8;
				align();
				break;
			default:
				ops->push_back(scalar(abbrev[n]));
				break;
			}
		}
		return !failed && !ops->empty();
	}

	void module_record(const std::vector<UINT64> &ops)
	{
		if (ops[0] == MODULE_GLOBALVAR || ops[0] == MODULE_FUNCTION || ops[0] == MODULE_ALIAS) {
			value_known.push_back(false);
			values.push_back(0);
		}
	}

	void constants_record(const std::vector<UINT64> &ops)
	{
		if (ops[0] == CST_SETTYPE)
			return;

		if (ops[0] == CST_INTEGER && ops.size() >= 2) {
			value_known.push_back(true);
			// The sign is in the lowest bit
			values.push_back((ops[1] & 1) ? -(INT64)(ops[1] >> 1) : (INT64)(ops[1] >> 1));
		} else if (ops[0] == CST_NULL) {
			// How a zero is written, of any type
			value_known.push_back(true);
			values.push_back(0);
		} else {
			value_known.push_back(false);
			values.push_back(0);
		}
	}

	std::string pending_name;

	void metadata_record(const std::vector<UINT64> &ops)
	{
		MetadataEntry entry;
		size_t i;

		switch (ops[0]) {
		case METADATA_NAME:
			pending_name.clear();
			for (i = 1; i < ops.size(); i++)
				pending_name.push_back((char)ops[i]);
			return;
		case METADATA_NAMED_NODE:
			named[pending_name].assign(ops.begin() + 1, ops.end());
			return;
		case METADATA_KIND:
			return;
		case METADATA_STRING:
			entry.kind = MetadataEntry::STRING;
			for (i = 1; i < ops.size(); i++)
				entry.string.push_back((char)ops[i]);
			break;
		case METADATA_VALUE:
			entry.kind = ops.size() >= 3 ? MetadataEntry::VALUE : MetadataEntry::OTHER;
			entry.value = ops.size() >= 3 ? ops[2] : 0;
			break;
		case METADATA_NODE:
		case METADATA_DISTINCT_NODE:
			entry.kind = MetadataEntry::NODE;
			entry.operands.assign(ops.begin() + 1, ops.end());
			break;
		default:
			// Debug information. It still has a number.
			entry.kind = MetadataEntry::OTHER;
			break;
		}
		metadata.push_back(entry);
	}

	// Reads the block whose header has just been read
	bool block(unsigned block_id, unsigned width, unsigned depth)
	{
		std::vector<Abbrev> abbrevs;
		std::vector<UINT64> ops;
		unsigned id, child, child_width, info_target = ~0u;
		UINT64 words;

		auto info = block_info.find(block_id);
		if (info != block_info.end())
			abbrevs = info->second;

		while (!failed) {
			if (pos >= bits)
				return depth == 0;

			id = (unsigned)fixed(width);
			switch (id) {
			case END_BLOCK:
				align();
				return !failed;
			case ENTER_SUBBLOCK:
				child = (unsigned)vbr(8);
				child_width = (unsigned)vbr(4);
				align();
				words = fixed(32);
				if (failed || !child_width || pos + words * 32 > bits) {
					failed = true;
					break;
				}
				if (depth < MAX_DEPTH && (child == BLOCKINFO_BLOCK
				    || (block_id == ~0u && child == MODULE_BLOCK)
				    || (block_id == MODULE_BLOCK && (child == CONSTANTS_BLOCK || child == METADATA_BLOCK)))) {
					if (!block(child, child_width, depth + 1))
						failed = true;
				} else {
					pos += words * 32;
				}
				break;
			case DEFINE_ABBREV: {
				Abbrev abbrev;
				if (!define_abbrev(&abbrev))
					break;
				if (block_id == BLOCKINFO_BLOCK) {
					if (info_target == ~0u)
						failed = true;
					else
						block_info[info_target].push_back(abbrev);
				} else {
					abbrevs.push_back(abbrev);
				}
				break;
			}
			default:
				if (!record(id, abbrevs, &ops))
					break;
				switch (block_id) {
				case BLOCKINFO_BLOCK:
					if (ops[0] == BLOCKINFO_SETBID && ops.size() >= 2)
						info_target = (unsigned)ops[1];
					break;
				case MODULE_BLOCK: module_record(ops); break;
				case CONSTANTS_BLOCK: constants_record(ops); break;
				case METADATA_BLOCK: metadata_record(ops); break;
				}
				break;
			}
		}
		return false;
	}

	// operand is as in a node: one more than the number of the entry, 0
	// for null
	const MetadataEntry* node(UINT64 operand)
	{
		if (!operand || operand > metadata.size() || metadata[operand - 1].kind != MetadataEntry::NODE)
			return NULL;
		return &metadata[operand - 1];
	}

	bool integer(UINT64 operand, UINT *value)
	{
		const MetadataEntry *entry;

		if (!operand || operand > metadata.size())
			return false;
		entry = &metadata[operand - 1];
		if (entry->kind != MetadataEntry::VALUE || entry->value >= values.size() || !value_known[entry->value])
			return false;
		*value = (UINT)values[entry->value];
		return true;
	}
};

static UINT32 load32(const BYTE *data)
{
	UINT32 value;

	memcpy(&value, data, sizeof(value));
	return value;
}

}

bool Dx12ReadDxilCBuffers(const BYTE *data, size_t size, std::vector<Dx12DxilCBuffer> *buffers)
{
	std::vector<Dx12DxilCBuffer> found;
	const MetadataEntry *resources, *list, *buffer;
	Dx12DxilCBuffer cbuffer;
	UINT64 offset, length;
	Reader reader = {};

	// Program header: version, size in words, "DXIL", version of DXIL,
	// then offset and size of the bitcode, the offset counting from "DXIL"
	if (size < 24 || memcmp(data + 8, "DXIL", 4))
		return false;
	offset = (UINT64)load32(data + 16) + 8;
	length = load32(data + 20);
	if (offset > size || length > size - offset || length < 4)
		return false;

	reader.data = data + offset;
	reader.bits = length * 8;
	if (reader.fixed(32) != 0xdec04342)
		return false;
	if (!reader.block(~0u, 2, 0))
		return false;

	auto named = reader.named.find("dx.resources");
	if (named == reader.named.end()) {
		// A shader without any resources
		buffers->clear();
		return true;
	}
	// A named node lists entries by their number
	if (named->second.empty() || !(resources = reader.node(named->second[0] + 1)) || resources->operands.size() < 3)
		return false;

	list = reader.node(resources->operands[2]);
	if (list) {
		for (UINT64 operand : list->operands) {
			buffer = reader.node(operand);
			if (!buffer || buffer->operands.size() < 7
			    || !reader.integer(buffer->operands[3], &cbuffer.space)
			    || !reader.integer(buffer->operands[4], &cbuffer.reg)
			    || !reader.integer(buffer->operands[5], &cbuffer.count)
			    || !reader.integer(buffer->operands[6], &cbuffer.bytes))
				return false;
			found.push_back(cbuffer);
		}
	}

	buffers->swap(found);
	return true;
}
