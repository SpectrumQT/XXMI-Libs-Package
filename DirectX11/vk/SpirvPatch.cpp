#include "SpirvPatch.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <unordered_map>

#include "SpirvTools.h"
#include "VkDrawPath.h"
#include "../globals.h"

namespace {

enum : uint32_t {
	OP_NAME = 5,
	OP_EXT_INST_IMPORT = 11,
	OP_MEMORY_MODEL = 14,
	OP_ENTRY_POINT = 15,
	OP_CAPABILITY = 17,
	OP_TYPE_VOID = 19,
	OP_TYPE_BOOL = 20,
	OP_TYPE_INT = 21,
	OP_TYPE_FLOAT = 22,
	OP_TYPE_VECTOR = 23,
	OP_TYPE_IMAGE = 25,
	OP_TYPE_SAMPLER = 26,
	OP_TYPE_SAMPLED_IMAGE = 27,
	OP_TYPE_POINTER = 32,
	OP_FUNCTION = 54,
	OP_FUNCTION_PARAMETER = 55,
	OP_VARIABLE = 59,
	OP_DECORATE = 71,
	OP_LINE = 8,
	OP_NO_LINE = 317,
	OP_LABEL = 248,
};

enum : uint32_t {
	DECORATION_BUILT_IN = 11,
	DECORATION_LOCATION = 30,
	DECORATION_DESCRIPTOR_SET = 34,
};

enum : uint32_t {
	CAPABILITY_IMAGE_QUERY = 50,
};

enum : uint32_t {
	EXECUTION_MODEL_VERTEX = 0,
	EXECUTION_MODEL_FRAGMENT = 4,
};

// What the DescriptorSet of a declared variable says until the patched code is
// given the number of the set: none a game would use
const uint32_t SET_PLACEHOLDER = 0x7a7a7a7a;

struct Instruction {
	size_t at;
	uint32_t opcode, words;
};

// Those of the module up to the end of the declarations, which is where an
// instruction that is no debug or annotation one and no type, constant or
// global variable comes
bool is_preamble(uint32_t opcode)
{
	switch (opcode) {
		case 2: case 3: case 4: case 5: case 6: case 7: // Source*, Name, MemberName, String
		case 10: case 11: case 14: case 15: case 16: case 17: // Extension, ExtInstImport, MemoryModel, EntryPoint, ExecutionMode, Capability
		case 330: case 331: // ModuleProcessed, ExecutionModeId
			return true;
	}
	return false;
}

bool is_annotation(uint32_t opcode)
{
	switch (opcode) {
		case 71: case 72: case 73: case 74: case 75: // Decorate, MemberDecorate, DecorationGroup, Group*Decorate
		case 332: case 5632: case 5633: // DecorateId, DecorateString, MemberDecorateString
			return true;
	}
	return false;
}

const char* storage_name(uint32_t storage)
{
	switch (storage) {
		case 0: return "UniformConstant";
		case 1: return "Input";
		case 2: return "Uniform";
		case 3: return "Output";
		case 4: return "Workgroup";
		case 6: return "Private";
		case 7: return "Function";
		case 9: return "PushConstant";
		case 12: return "StorageBuffer";
	}
	return NULL;
}

const char* built_in_name(uint32_t built_in)
{
	switch (built_in) {
		case 0: return "Position";
		case 1: return "PointSize";
		case 3: return "ClipDistance";
		case 4: return "CullDistance";
		case 7: return "PrimitiveId";
		case 9: return "Layer";
		case 10: return "ViewportIndex";
		case 15: return "FragCoord";
		case 16: return "PointCoord";
		case 17: return "FrontFacing";
		case 18: return "SampleId";
		case 19: return "SamplePosition";
		case 20: return "SampleMask";
		case 22: return "FragDepth";
		case 23: return "HelperInvocation";
		case 42: return "VertexIndex";
		case 43: return "InstanceIndex";
	}
	return NULL;
}

const char* image_dim_name(uint32_t dim)
{
	switch (dim) {
		case 0: return "1d";
		case 1: return "2d";
		case 2: return "3d";
		case 3: return "cube";
		case 5: return "buffer";
	}
	return NULL;
}

struct Module {
	const uint32_t *code;
	size_t words;
	std::vector<Instruction> instructions;
	// The canonical names of ids, without the leading %
	std::unordered_map<uint32_t, std::string> names;
	std::set<std::string> taken;
	// The entry point a pipeline uses, by its name and execution model; the
	// first one if no name is given
	const char *entry_name;
	uint32_t execution_model;
	uint32_t entry;
	bool image_query, glsl;

	const uint32_t* operands(const Instruction &in) const { return code + in.at + 1; }

	void name(uint32_t id, const std::string &wanted)
	{
		if (names.count(id) || !taken.insert(wanted).second)
			return;
		names[id] = wanted;
	}

	const std::string* named(uint32_t id) const
	{
		auto i = names.find(id);
		return i == names.end() ? NULL : &i->second;
	}
};

bool parse(const uint32_t *code, size_t words, Module *module, std::string *why)
{
	size_t at;
	uint32_t count;

	if (words < 5 || code[0] != 0x07230203) {
		*why = "not SPIR-V";
		return false;
	}
	module->code = code;
	module->words = words;
	for (at = 5; at < words; at += count) {
		count = code[at] >> 16;
		if (!count || at + count > words) {
			*why = "an instruction runs past the end";
			return false;
		}
		module->instructions.push_back({ at, code[at] & 0xffff, count });
	}
	return true;
}

std::string literal_string(const uint32_t *words, size_t count)
{
	const char *chars = (const char*)words;

	return std::string(chars, strnlen(chars, count * 4));
}

// The names of the module's own ids the patterns may count on
void name_ids(Module *module)
{
	std::unordered_map<uint32_t, uint32_t> locations, built_ins;
	// The inputs and outputs of the entry point: another entry point of the
	// module may have its own at the same locations
	std::set<uint32_t> entry_interface;
	const std::string *base, *component;
	char text[64];

	module->entry = 0;
	module->image_query = module->glsl = false;
	for (const Instruction &in : module->instructions) {
		const uint32_t *op = module->operands(in);
		uint32_t n = in.words - 1;

		switch (in.opcode) {
			case OP_CAPABILITY:
				if (n >= 1 && op[0] == CAPABILITY_IMAGE_QUERY)
					module->image_query = true;
				break;
			case OP_EXT_INST_IMPORT:
				if (n >= 2 && literal_string(op + 1, n - 1) == "GLSL.std.450") {
					module->name(op[0], "zz_glsl");
					module->glsl = true;
				}
				break;
			case OP_ENTRY_POINT:
				if (n >= 2 && !module->entry && (!module->entry_name
				    || (op[0] == module->execution_model && literal_string(op + 2, n - 2) == module->entry_name))) {
					module->entry = op[1];
					module->name(op[1], "zz_main");
					for (uint32_t i = 2 + (uint32_t)literal_string(op + 2, n - 2).size() / 4 + 1; i < n; i++)
						entry_interface.insert(op[i]);
				}
				break;
			case OP_DECORATE:
				if (n >= 3 && op[1] == DECORATION_LOCATION)
					locations[op[0]] = op[2];
				else if (n >= 3 && op[1] == DECORATION_BUILT_IN)
					built_ins[op[0]] = op[2];
				break;
			case OP_TYPE_VOID:
				module->name(op[0], "zz_void");
				break;
			case OP_TYPE_BOOL:
				module->name(op[0], "zz_bool");
				break;
			case OP_TYPE_INT:
				if (n >= 3) {
					_snprintf_s(text, _TRUNCATE, op[2] ? "zz_int%u" : "zz_uint%u", op[1]);
					if (op[1] == 32)
						text[strlen(text) - 2] = 0;
					module->name(op[0], text);
				}
				break;
			case OP_TYPE_FLOAT:
				if (n >= 2) {
					_snprintf_s(text, _TRUNCATE, "zz_float%u", op[1]);
					if (op[1] == 32)
						text[strlen(text) - 2] = 0;
					module->name(op[0], text);
				}
				break;
			case OP_TYPE_VECTOR:
				if (n >= 3 && (component = module->named(op[1])) && op[2] >= 2 && op[2] <= 4) {
					_snprintf_s(text, _TRUNCATE, "zz_v%u%s", op[2], component->c_str() + 3);
					module->name(op[0], text);
				}
				break;
			case OP_TYPE_IMAGE:
				// Sampled, not multisampled, of any format and either no
				// depth image or not saying
				if (n >= 8 && (component = module->named(op[1])) && image_dim_name(op[2])
				    && op[3] != 1 && !op[5] && op[6] == 1 && !op[7]) {
					_snprintf_s(text, _TRUNCATE, "zz_image%s%s_%s", image_dim_name(op[2]), op[4] ? "array" : "",
						component->c_str() + 3);
					module->name(op[0], text);
				}
				break;
			case OP_TYPE_SAMPLER:
				module->name(op[0], "zz_sampler");
				break;
			case OP_TYPE_SAMPLED_IMAGE:
				if (n >= 2 && (base = module->named(op[1])))
					module->name(op[0], "zz_sampled_" + base->substr(3));
				break;
			case OP_TYPE_POINTER:
				if (n >= 3 && storage_name(op[1]) && (base = module->named(op[2])))
					module->name(op[0], std::string("zz_ptr_") + storage_name(op[1]) + "_" + base->substr(3));
				break;
			case OP_VARIABLE:
				if (n >= 3 && (op[2] == 1 || op[2] == 3) && entry_interface.count(op[1])) {
					const char *way = op[2] == 1 ? "in" : "out";
					auto location = locations.find(op[1]);
					auto built_in = built_ins.find(op[1]);

					if (location != locations.end()) {
						_snprintf_s(text, _TRUNCATE, "zz_%s_loc%u", way, location->second);
						module->name(op[1], text);
					} else if (built_in != built_ins.end() && built_in_name(built_in->second)) {
						_snprintf_s(text, _TRUNCATE, "zz_%s_%s", way, built_in_name(built_in->second));
						module->name(op[1], text);
					}
				}
				break;
		}
	}
}

// %123 written as the name the id has, outside of string literals
std::string rename_ids(const std::string &line, const Module &module)
{
	std::string out;
	bool quoted = false;
	size_t i, end;

	out.reserve(line.size() + 16);
	for (i = 0; i < line.size(); i++) {
		char c = line[i];

		if (c == '"' && (i == 0 || line[i - 1] != '\\'))
			quoted = !quoted;
		if (quoted || c != '%' || i + 1 >= line.size() || !isdigit((unsigned char)line[i + 1])) {
			out += c;
			continue;
		}
		for (end = i + 1; end < line.size() && isdigit((unsigned char)line[end]); end++)
			;
		const std::string *name = module.named((uint32_t)strtoul(line.c_str() + i + 1, NULL, 10));
		if (name && (end == line.size() || !isalnum((unsigned char)line[end]) && line[end] != '_')) {
			out += '%';
			out += *name;
		} else {
			out.append(line, i, end - i);
		}
		i = end - 1;
	}
	return out;
}

struct Wanted {
	const char *name;
	std::string definition;
};

// The types the patterns can count on, those an earlier one is made of first
std::vector<Wanted> wanted_types(const std::vector<SpirvDeclaration> &declarations)
{
	static const char *const scalars[] = { "bool", "int", "uint", "float" };
	std::vector<Wanted> types = {
		{ "zz_void", "OpTypeVoid" },
		{ "zz_bool", "OpTypeBool" },
		{ "zz_int", "OpTypeInt 32 1" },
		{ "zz_uint", "OpTypeInt 32 0" },
		{ "zz_float", "OpTypeFloat 32" },
	};
	static std::vector<std::string> vector_names;
	bool textures = false, samplers = false, ini_params = false;
	int n, i;

	if (vector_names.empty()) {
		for (n = 2; n <= 4; n++) {
			for (i = 0; i < ARRAYSIZE(scalars); i++)
				vector_names.push_back("zz_v" + std::to_string(n) + scalars[i]);
		}
	}
	for (n = 2; n <= 4; n++) {
		for (i = 0; i < ARRAYSIZE(scalars); i++)
			types.push_back({ vector_names[(n - 2) * ARRAYSIZE(scalars) + i].c_str(),
				std::string("OpTypeVector %zz_") + scalars[i] + " " + std::to_string(n) });
	}

	for (const SpirvDeclaration &declaration : declarations) {
		textures |= declaration.kind == SpirvDeclarationKind::TEXTURE2D;
		samplers |= declaration.kind == SpirvDeclarationKind::SAMPLER;
		ini_params |= declaration.kind == SpirvDeclarationKind::INI_PARAMS;
	}
	if (textures) {
		types.push_back({ "zz_image2d_float", "OpTypeImage %zz_float 2D 0 0 0 1 Unknown" });
		types.push_back({ "zz_ptr_UniformConstant_image2d_float", "OpTypePointer UniformConstant %zz_image2d_float" });
	}
	if (samplers) {
		types.push_back({ "zz_sampler", "OpTypeSampler" });
		types.push_back({ "zz_ptr_UniformConstant_sampler", "OpTypePointer UniformConstant %zz_sampler" });
	}
	if (textures && samplers)
		types.push_back({ "zz_sampled_image2d_float", "OpTypeSampledImage %zz_image2d_float" });
	if (ini_params)
		types.push_back({ "zz_ptr_Uniform_v4float", "OpTypePointer Uniform %zz_v4float" });
	return types;
}

std::string variable_name(const SpirvDeclaration &declaration)
{
	switch (declaration.kind) {
		case SpirvDeclarationKind::INI_PARAMS:
			return "IniParams";
		case SpirvDeclarationKind::TEXTURE2D:
			return "t" + std::to_string(declaration.reg);
	}
	return "s" + std::to_string(declaration.reg);
}

// The text of the module with the names, the declarations and the places the
// patterns can count on
bool make_prelude(const Module &module, const std::vector<std::string> &lines,
		const std::vector<SpirvDeclaration> &declarations, unsigned ini_params, std::string *text, std::string *why)
{
	std::string annotations, globals, interface_ids;
	size_t first_global = SIZE_MAX, first_function = SIZE_MAX, memory_model = SIZE_MAX, last_capability = SIZE_MAX;
	size_t i, entry_function = SIZE_MAX;
	bool textures = false;

	for (i = 0; i < module.instructions.size(); i++) {
		uint32_t opcode = module.instructions[i].opcode;

		if (opcode == OP_CAPABILITY)
			last_capability = i;
		if (opcode == OP_MEMORY_MODEL)
			memory_model = i;
		if (first_global == SIZE_MAX && !is_preamble(opcode) && !is_annotation(opcode))
			first_global = i;
		if (opcode == OP_FUNCTION) {
			if (first_function == SIZE_MAX)
				first_function = i;
			if (module.operands(module.instructions[i])[1] == module.entry)
				entry_function = i;
		}
	}
	if (!module.entry || entry_function == SIZE_MAX || memory_model == SIZE_MAX || first_global > first_function) {
		*why = "no entry point function";
		if (!module.entry && module.entry_name)
			*why += std::string(" named ") + module.entry_name + " for the stage";
		return false;
	}

	for (const Wanted &type : wanted_types(declarations)) {
		if (!module.taken.count(type.name))
			globals += std::string("%") + type.name + " = " + type.definition + "\n";
	}
	for (const SpirvDeclaration &declaration : declarations) {
		std::string name = variable_name(declaration);
		std::string set = " DescriptorSet " + std::to_string(SET_PLACEHOLDER) + "\n";
		std::string binding = " Binding " + std::to_string(SpirvPatchBinding(declaration)) + "\n";

		annotations += "OpDecorate %" + name + set + "OpDecorate %" + name + binding;
		switch (declaration.kind) {
			case SpirvDeclarationKind::INI_PARAMS:
				annotations += "OpDecorate %zz_iniparams_array ArrayStride 16\n"
					"OpMemberDecorate %zz_iniparams_block 0 Offset 0\n"
					"OpDecorate %zz_iniparams_block Block\n";
				globals += "%zz_iniparams_count = OpConstant %zz_uint " + std::to_string(ini_params ? ini_params : 1) + "\n"
					"%zz_iniparams_array = OpTypeArray %zz_v4float %zz_iniparams_count\n"
					"%zz_iniparams_block = OpTypeStruct %zz_iniparams_array\n"
					"%zz_ptr_Uniform_iniparams_block = OpTypePointer Uniform %zz_iniparams_block\n"
					"%" + name + " = OpVariable %zz_ptr_Uniform_iniparams_block Uniform\n";
				break;
			case SpirvDeclarationKind::TEXTURE2D:
				globals += "%" + name + " = OpVariable %zz_ptr_UniformConstant_image2d_float UniformConstant\n";
				textures = true;
				break;
			case SpirvDeclarationKind::SAMPLER:
				globals += "%" + name + " = OpVariable %zz_ptr_UniformConstant_sampler UniformConstant\n";
				break;
		}
		interface_ids += " %" + name;
	}
	globals += "@zz_globals\n";

	text->clear();
	for (i = 0; i < module.instructions.size(); i++) {
		const std::string &line = lines[i];

		if (i == memory_model && !module.glsl)
			*text += "%zz_glsl = OpExtInstImport \"GLSL.std.450\"\n";
		if (i == first_global)
			*text += annotations;
		if (i == first_function)
			*text += globals;

		if (module.instructions[i].opcode == OP_ENTRY_POINT && module.operands(module.instructions[i])[1] == module.entry
		    && module.code[1] >= 0x00010400) {
			// From 1.4 the interface of an entry point has every global
			// variable it uses
			*text += line + interface_ids + "\n";
		} else {
			*text += line + "\n";
		}

		if (i == last_capability && textures && !module.image_query)
			*text += "OpCapability ImageQuery\n";

		if (i == entry_function) {
			size_t label, j;

			// A block of our own comes first, with the variables of the
			// function, which have to be in the first: what the shader
			// had as its first stays as it was, OpPhi that name it too
			for (label = i + 1; label < module.instructions.size() && module.instructions[label].opcode == OP_FUNCTION_PARAMETER; label++)
				*text += lines[label] + "\n";
			if (label >= module.instructions.size() || module.instructions[label].opcode != OP_LABEL) {
				*why = "the entry point function has no block";
				return false;
			}
			*text += "%zz_entry = OpLabel\n";
			for (j = label + 1; j < module.instructions.size(); j++) {
				uint32_t opcode = module.instructions[j].opcode;

				if (opcode != OP_VARIABLE && opcode != OP_LINE && opcode != OP_NO_LINE)
					break;
				*text += lines[j] + "\n";
			}
			*text += "@zz_entry\n";
			*text += "OpBranch %" + std::to_string(module.operands(module.instructions[label])[0]) + "\n";
			*text += lines[label] + "\n";
			i = j - 1;
		}
	}
	return true;
}

bool group_wants(const ShaderRegexGroup &group, const char *stage, const std::string *declarations)
{
	size_t length = strlen(stage);

	if (!group.spirv())
		return false;
	if (std::none_of(group.shader_models.begin(), group.shader_models.end(), [&](const std::string &model) {
			return !model.compare(0, length, stage) && model[length] == '_'; }))
		return false;
	if (declarations) {
		for (auto &pattern : group.patterns) {
			if (!const_cast<ShaderRegexPattern&>(pattern.second).matches(const_cast<std::string*>(declarations)))
				return false;
		}
	}
	return true;
}

// How the module passes the validator, if it does at all
bool layout_rules_of(const uint32_t *code, size_t words, SpirvLayoutRules *rules, std::string *why)
{
	std::string error, first;
	int i;

	for (i = 0; i < SPIRV_LAYOUT_COUNT; i++) {
		if (SpirvValidate(code, words, (SpirvLayoutRules)i, &error)) {
			*rules = (SpirvLayoutRules)i;
			return true;
		}
		if (first.empty())
			first = error;
	}
	*why = "the shader as it is does not pass the validator: " + first;
	return false;
}

struct Candidate {
	unsigned ordinal;
	const ShaderRegexGroup *group;
};

SpirvPatchOutcome patch(const uint32_t *code, size_t words, const char *stage, const char *entry,
		const std::vector<Candidate> &candidates, unsigned ini_params, bool keep_text, SpirvPatched *patched, std::string *why)
{
	std::vector<SpirvDeclaration> declarations;
	std::vector<std::string> lines;
	std::string raw, text, error, out;
	SpirvLayoutRules rules;
	Module module;
	size_t start, end;

	module.entry_name = entry;
	module.execution_model = !strcmp(stage, "vs") ? EXECUTION_MODEL_VERTEX : EXECUTION_MODEL_FRAGMENT;
	*patched = SpirvPatched();
	why->clear();
	for (const Candidate &candidate : candidates) {
		for (const SpirvDeclaration &declaration : candidate.group->spirv_declarations) {
			if (std::none_of(declarations.begin(), declarations.end(), [&](const SpirvDeclaration &other) {
					return other.kind == declaration.kind && other.reg == declaration.reg; }))
				declarations.push_back(declaration);
		}
	}
	std::sort(declarations.begin(), declarations.end(), [](const SpirvDeclaration &a, const SpirvDeclaration &b) {
		return SpirvPatchBinding(a) < SpirvPatchBinding(b); });

	if (!parse(code, words, &module, why))
		return SpirvPatchOutcome::FAILED;
	if (!SpirvDisassemble(code, words, false, &raw, &error)) {
		*why = "could not be disassembled: " + error;
		return SpirvPatchOutcome::FAILED;
	}
	for (start = 0; start < raw.size(); start = end + 1) {
		end = raw.find('\n', start);
		if (end == std::string::npos)
			end = raw.size();
		if (end > start)
			lines.push_back(raw.substr(start, end - start));
	}
	if (lines.size() != module.instructions.size()) {
		*why = "the text is not a line an instruction";
		return SpirvPatchOutcome::FAILED;
	}
	name_ids(&module);
	for (std::string &line : lines)
		line = rename_ids(line, module);
	if (!make_prelude(module, lines, declarations, ini_params, &text, why))
		return SpirvPatchOutcome::FAILED;
	if (keep_text)
		patched->before = text;

	for (const Candidate &candidate : candidates) {
		std::string saved = text, replace;
		bool applied = true;

		for (auto &entry : candidate.group->spirv_patterns) {
			ShaderRegexPattern &pattern = const_cast<ShaderRegexPattern&>(entry.second);

			if (pattern.do_replace) {
				replace = pattern.replace;
				for (const std::string &temp : candidate.group->temp_regs)
					substitute_shader_regex_temp(replace, temp, "%zz" + std::to_string(candidate.ordinal) + "_" + temp);
				applied = pattern.patch_with(&text, replace);
			} else {
				applied = pattern.matches(&text);
			}
			if (!applied)
				break;
		}
		if (applied)
			patched->groups.push_back(candidate.group);
		else
			text = saved;
	}
	// Patterns that matched without changing anything are still a match:
	// with nothing to do to the shader, there is nothing to tell apart
	if (patched->groups.empty() && !candidates.empty())
		return SpirvPatchOutcome::NO_MATCH;

	for (start = 0; start < text.size(); start = end + 1) {
		end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		size_t first = text.find_first_not_of(" \t", start);
		if (first < end && !text.compare(first, 4, "@zz_"))
			continue;
		out.append(text, start, end - start);
		out += '\n';
	}
	if (keep_text)
		patched->after = out;

	if (!layout_rules_of(code, words, &rules, why))
		return SpirvPatchOutcome::FAILED;
	if (!SpirvAssemble(out, code[1], true, &patched->code, &error)) {
		*why = "could not be assembled: " + error;
		return SpirvPatchOutcome::FAILED;
	}
	for (start = 5; start < patched->code.size(); start += patched->code[start] >> 16) {
		const uint32_t *word = &patched->code[start];

		if (!(word[0] >> 16))
			break;
		if ((word[0] & 0xffff) == OP_DECORATE && (word[0] >> 16) == 4 && word[2] == DECORATION_DESCRIPTOR_SET
		    && word[3] == SET_PLACEHOLDER) {
			patched->set_words.push_back(start + 3);
			patched->code[start + 3] = 0;
		}
	}
	if (!SpirvValidate(patched->code.data(), patched->code.size(), rules, &error)) {
		*why = "the patched shader does not pass the validator: " + error;
		return SpirvPatchOutcome::FAILED;
	}
	// The prelude declares what every candidate group declares, but only what
	// the groups that applied read is what the shader needs from a draw
	for (const SpirvDeclaration &declaration : declarations) {
		if (std::any_of(patched->groups.begin(), patched->groups.end(), [&](const ShaderRegexGroup *group) {
				return std::any_of(group->spirv_declarations.begin(), group->spirv_declarations.end(), [&](const SpirvDeclaration &other) {
					return other.kind == declaration.kind && other.reg == declaration.reg; }); }))
			patched->declarations.push_back(declaration);
	}
	return SpirvPatchOutcome::PATCHED;
}

}

uint32_t SpirvPatchBinding(const SpirvDeclaration &declaration)
{
	switch (declaration.kind) {
		case SpirvDeclarationKind::INI_PARAMS:
			return 0;
		case SpirvDeclarationKind::TEXTURE2D:
			return 1 + declaration.reg;
	}
	return 1 + D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT + declaration.reg;
}

SpirvPatchOutcome SpirvPatchShader(const uint32_t *code, size_t words, const char *stage, const char *entry,
		const std::string *declarations, unsigned ini_params, bool keep_text,
		SpirvPatched *patched, std::string *why)
{
	std::vector<Candidate> candidates;
	unsigned ordinal = 0;

	for (auto &entry : shader_regex_groups) {
		if (!entry.second.spirv())
			continue;
		if (group_wants(entry.second, stage, declarations))
			candidates.push_back({ ordinal, &entry.second });
		ordinal++;
	}
	if (candidates.empty()) {
		*patched = SpirvPatched();
		*why = "no group is for it";
		return SpirvPatchOutcome::NO_MATCH;
	}
	return patch(code, words, stage, entry, candidates, ini_params, keep_text, patched, why);
}

std::vector<uint32_t> SpirvPatchedCode(const SpirvPatched &patched, uint32_t set)
{
	std::vector<uint32_t> code = patched.code;

	for (size_t word : patched.set_words)
		code[word] = set;
	return code;
}

// For local-patches/spirv-regex: the same as the DLL does to a shader, to a
// file of one.
//
//   mode 0: disassembled with the ids as numbers and assembled again, which
//           has to give the same words. Returns 0 if it does, 1 if not.
//   mode 1: the rules for block layouts it passes the validator with
//           (SpirvLayoutRules), -1 for none.
//   mode 2: the group given patches it, or with no SpirvPattern only the
//           prelude: 0 patched, 1 no match, 2 failed. The prelude, what the
//           patterns made and the patched code go to <out>.before.spvasm,
//           <out>.after.spvasm and <out>.spv if out is given.
//
// The group: shader_model and temps as in its main section, the lines of its
// InsertDeclarations, and its Pattern and SpirvPattern subsections as records
// that end in \x1e, of fields that end in \x1f: S for SpirvPattern or P for
// Pattern, the pattern, and the replacement or nothing.
extern "C" __declspec(dllexport) int VkSpirvPatchFile(const wchar_t *in_path, int mode, const char *shader_model,
		const char *temps, const char *declarations, const char *patterns, int ini_params_reg, unsigned ini_params,
		const wchar_t *out_path, char *why, unsigned why_size)
{
	std::ifstream file(in_path, std::ios::binary);
	std::vector<uint32_t> code;
	std::string error, text, line;
	std::vector<std::string> fields;
	SpirvLayoutRules rules;
	ShaderRegexGroup group;
	SpirvPatched patched;
	SpirvPatchOutcome outcome;
	unsigned spirv_patterns = 0, d3d_patterns = 0;
	size_t start, end;
	int result;

	auto answer = [&](int value, const std::string &message) {
		strncpy_s(why, why_size, message.c_str(), _TRUNCATE);
		return value;
	};

	text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	if (text.size() < 20 || text.size() % 4)
		return answer(mode == 1 ? -1 : 2, "not SPIR-V");
	code.resize(text.size() / 4);
	memcpy(code.data(), text.data(), text.size());

	if (mode == 0) {
		std::vector<uint32_t> again;

		if (!SpirvDisassemble(code.data(), code.size(), false, &text, &error))
			return answer(2, "could not be disassembled: " + error);
		if (!SpirvAssemble(text, code[1], true, &again, &error))
			return answer(2, "could not be assembled: " + error);
		// The assembler writes a generator and an id bound of its own
		again[2] = code[2];
		again[3] = code[3];
		return answer(again == code ? 0 : 1, again == code ? "same" : "different words");
	}
	if (mode == 1) {
		if (!layout_rules_of(code.data(), code.size(), &rules, &error))
			return answer(-1, error);
		return answer((int)rules, "valid");
	}

	auto words_of = [](const char *list) {
		std::set<std::string> words;
		std::string word;

		for (const char *c = list ? list : ""; ; c++) {
			if (*c && *c != ' ') {
				word += *c;
				continue;
			}
			if (!word.empty())
				words.insert(word);
			word.clear();
			if (!*c)
				return words;
		}
	};
	group.shader_models = words_of(shader_model);
	group.temp_regs = words_of(temps);
	for (start = 0; declarations && declarations[start]; start = end + 1) {
		const char *newline = strchr(declarations + start, '\n');
		SpirvDeclaration declaration;

		end = newline ? newline - declarations : strlen(declarations);
		line.assign(declarations + start, end - start);
		if (!line.empty() && !parse_spirv_declaration(line, ini_params_reg, &declaration))
			return answer(2, "not a declaration for SPIR-V: " + line);
		if (!line.empty())
			group.spirv_declarations.push_back(declaration);
		if (!newline)
			break;
	}
	for (start = 0; patterns && patterns[start]; start = end + 1) {
		const char *record = strchr(patterns + start, '\x1e');
		std::string pattern;
		size_t field;

		if (!record)
			return answer(2, "a pattern record does not end");
		end = record - patterns;
		fields.clear();
		for (field = start; field < end;) {
			const char *stop = (const char*)memchr(patterns + field, '\x1f', end - field);
			size_t stop_at = stop ? stop - patterns : end;

			fields.push_back(std::string(patterns + field, stop_at - field));
			field = stop_at + 1;
		}
		while (fields.size() < 3)
			fields.push_back("");
		wchar_t name[32];
		swprintf_s(name, L"%c%04u", fields[0][0], spirv_patterns + d3d_patterns);
		ShaderRegexPattern *made = fields[0] == "S" ? &group.spirv_patterns[name] : &group.patterns[name];
		if (fields[0] == "S")
			spirv_patterns++;
		else
			d3d_patterns++;
		pattern = fields[1];
		if (!made->compile(&pattern))
			return answer(2, "the pattern does not compile: " + fields[1]);
		if (!fields[2].empty()) {
			made->replace = fields[2];
			made->do_replace = true;
		}
	}

	if (!spirv_patterns) {
		// Only the prelude: a group with a pattern that matches anything
		// and changes nothing
		std::string anything = "^";
		group.spirv_patterns[L"S"].compile(&anything);
	}
	std::string declared;
	const std::string *declared_for = NULL;
	if (d3d_patterns) {
		declared = VkShaderDeclarations(code.data(), code.size() * 4);
		declared_for = &declared;
	}
	std::string stage = group.shader_models.empty() ? "ps" : group.shader_models.begin()->substr(0, 2);
	if (!group_wants(group, stage.c_str(), declared_for))
		return answer(1, "the Pattern subsections do not match what it declares");
	outcome = patch(code.data(), code.size(), stage.c_str(), NULL, { { 0, &group } }, ini_params, true, &patched, &error);
	if (out_path && *out_path && !patched.before.empty()) {
		std::wstring out = out_path;
		std::ofstream(out + L".before.spvasm", std::ios::binary) << patched.before;
		if (!patched.after.empty())
			std::ofstream(out + L".after.spvasm", std::ios::binary) << patched.after;
		if (outcome == SpirvPatchOutcome::PATCHED) {
			std::vector<uint32_t> final_code = SpirvPatchedCode(patched, 7);
			std::ofstream(out + L".spv", std::ios::binary).write((const char*)final_code.data(), final_code.size() * 4);
		}
	}
	result = outcome == SpirvPatchOutcome::PATCHED ? 0 : outcome == SpirvPatchOutcome::NO_MATCH ? 1 : 2;
	return answer(result, outcome == SpirvPatchOutcome::PATCHED ? "patched" : error);
}
