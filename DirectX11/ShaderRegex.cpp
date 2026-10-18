#include "ShaderRegex.h"
#include "CommandList.h"
#include "globals.h" // For ShaderOverride FIXME: This should be in a separate header
#include "log.h"
#include "Overlay.h"
#include "dx12/PipelineRegistry.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <unordered_map>

ShaderRegexGroups shader_regex_groups;
bool shader_regex_d3d_groups;
std::vector<ShaderRegexGroup*> shader_regex_group_index;
uint32_t shader_regex_hash;

// The outcome of ShaderRegex analysis for one shader hash, kept in memory so
// that a config reload which leaves the ShaderRegex sections alone, or a
// shader the game creates again, is served without reading the cache files
// or disassembling the shader again. Only valid while regex_hash matches
// shader_regex_hash. Accessed with G->mCriticalSection held. A result taken
// from a background job has no patched_bytecode, see restore_patched_bytecode().
struct ShaderRegexResult {
	uint32_t regex_hash = 0;
	std::wstring shader_type;
	bool patched = false;
	std::vector<uint32_t> match_ids;
	std::vector<byte> patched_bytecode;
	std::wstring tagline;
};
static std::unordered_map<UINT64, ShaderRegexResult> shader_regex_results;

static std::mutex shader_regex_analysis_lock;

static void log_pcre2_error_nonl(int err, char *fmt, ...)
{
	PCRE2_UCHAR buf[120]; // doco says "120 code units is ample"
	va_list ap;

	pcre2_get_error_message(err, buf, sizeof(buf));

	va_start(ap, fmt);
	vLogInfo(fmt, ap);
	va_end(ap);

	LogInfo(": %s\n", buf);
}

static bool get_shader_model(std::string *asm_text, std::string *shader_model)
{
	size_t shader_model_pos;

	for (
		shader_model_pos = asm_text->find("\n");
		shader_model_pos != std::string::npos && (*asm_text)[shader_model_pos + 1] == '/';
		shader_model_pos = asm_text->find("\n", shader_model_pos + 1)
	) {}

	if (shader_model_pos == std::string::npos)
		return false;

	*shader_model = asm_text->substr(shader_model_pos + 1, asm_text->find("\n", shader_model_pos + 1) - shader_model_pos - 1);
	return true;
}

static bool find_dcl_end(std::string *asm_text, size_t *dcl_end_pos)
{
	// FIXME: Might be better to scan forwards

	*dcl_end_pos = asm_text->rfind("\ndcl_");
	*dcl_end_pos = asm_text->find("\n", *dcl_end_pos + 1);

	if (*dcl_end_pos == std::string::npos) {
		LogInfo("WARNING: Unable to locate end of shader declarations!\n");
		return false;
	}

	return true;
}

static bool insert_declarations(std::string *asm_text, ShaderRegexDeclarations *declarations)
{
	ShaderRegexDeclarations::iterator i;
	std::string insert_str;
	size_t dcl_end;
	bool patch = false;

	if (!find_dcl_end(asm_text, &dcl_end))
		return false;

	for (i = declarations->begin(); i != declarations->end(); i++) {
		insert_str = std::string("\n") + *i;

		if (asm_text->find(insert_str + std::string("\n")) != std::string::npos)
			continue;

		asm_text->insert(dcl_end, insert_str);
		dcl_end += insert_str.size();

		patch = true;
	}

	return patch;
}

static bool find_dcl_temps(std::string *asm_text, size_t *dcl_temps_pos)
{
	// Could use regex for this as well, but given we only need to find a
	// constant string it will be more efficient to just do this:
	*dcl_temps_pos = asm_text->find("\ndcl_temps ", 0);

	if (*dcl_temps_pos == std::string::npos)
		return false;

	return true;
}

static unsigned get_dcl_temps(std::string *asm_text)
{
	size_t dcl_temps;
	unsigned tmp_regs = 0;

	if (!find_dcl_temps(asm_text, &dcl_temps))
		return 0;

	tmp_regs = stoul(asm_text->substr(dcl_temps + 10, 4));
	LogInfo("Found dcl_temps %d\n", tmp_regs);

	return tmp_regs;
}

static bool update_dcl_temps(std::string *asm_text, size_t new_val)
{
	size_t dcl_temps, dcl_temps_end, dcl_end;
	std::string insert_str;

	if (find_dcl_temps(asm_text, &dcl_temps)) {
		dcl_temps += 11;
		dcl_temps_end = asm_text->find("\n", dcl_temps);
		LogInfo("Updating dcl_temps %Iu\n", new_val);
		asm_text->replace(dcl_temps, dcl_temps_end - dcl_temps, std::to_string(new_val));
		return true;
	}

	if (!find_dcl_end(asm_text, &dcl_end))
		return false;

	insert_str = std::string("\ndcl_temps ") + std::to_string(new_val);
	LogInfo("Inserting dcl_temps %Iu\n", new_val);
	asm_text->insert(dcl_end, insert_str);
	dcl_end += insert_str.size();

	return true;
}

ShaderRegexPattern::ShaderRegexPattern() :
	regex(NULL),
	do_replace(false)
{
}

ShaderRegexPattern::~ShaderRegexPattern()
{
	pcre2_code_free(regex);
}

bool ShaderRegexPattern::compile(std::string *pattern)
{
	uint32_t name_table_entry_size;
	uint32_t name_table_count;
	uint32_t i;
	PCRE2_SPTR name_table;
	PCRE2_SIZE err_off;
	int err;

	// CASELESS is for compatibility with d3dcompiler_46 & 47 without
	// having to always remember to account for the dcl_constantbuffer
	// differences:
	regex = pcre2_compile((PCRE2_SPTR)pattern->c_str(),
			pattern->length(), // or PCRE2_ZERO_TERMINATED
			PCRE2_CASELESS | PCRE2_MULTILINE,
			&err, &err_off, NULL);
	if (!regex) {
		log_pcre2_error_nonl(err, "  WARNING: PCRE2 regex compilation failed at offset %u", (unsigned)err_off);
		return false;
	}

	// TODO: Use callback to confirm that JIT does actually get used, as in
	// some cases pcre2 can fall back to using the slower interpreter
	pcre2_jit_compile(regex, 0);

	pcre2_pattern_info(regex, PCRE2_INFO_NAMECOUNT, &name_table_count);
	pcre2_pattern_info(regex, PCRE2_INFO_NAMEENTRYSIZE, &name_table_entry_size);
	pcre2_pattern_info(regex, PCRE2_INFO_NAMETABLE, &name_table);

	static_assert(PCRE2_CODE_UNIT_WIDTH == 8, "Need to fix name table parsing for non-8bit pcre2");
	for (i = 0; i < name_table_count; i++)
		named_capture_groups.insert(std::string((char*)(name_table + name_table_entry_size*i + 2)));

	return true;
}

bool ShaderRegexPattern::named_group_overlaps(ShaderRegexTemps &other_set)
{
	ShaderRegexTemps intersection;

	// C++ why you be so verbose?
	std::set_intersection(
				named_capture_groups.begin(),
				named_capture_groups.end(),
				other_set.begin(),
				other_set.end(),
				std::inserter(intersection, intersection.begin()));

	return intersection.size() != 0;
}

bool ShaderRegexPattern::matches(std::string *asm_text)
{
	pcre2_match_data *match_data = NULL;
	bool match = false;
	int rc;

	// TODO: Assign per-thread JIT stack if the default 32K turns out to be
	// insufficient. Can probably store this in the context, as that is
	// supposed to be per-thread, or use thread local storage.

	match_data = pcre2_match_data_create_from_pattern(regex, NULL);

	// TODO: Consider using pcre2_jit_match - doco claims 10% faster, but
	// has less sanity checks. TODO: Use callback to confirm JIT was used
	rc = pcre2_match(regex, (PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0, 0, match_data, NULL);
	if (rc == PCRE2_ERROR_NOMATCH)
		goto out_free;
	if (rc < 0) {
		log_pcre2_error_nonl(rc, "  WARNING: regex match error");
		goto out_free;
	}

	match = true;

out_free:
	pcre2_match_data_free(match_data);
	return match;
}

static void replacement_search_and_replace(std::string &str, const std::string *search, const std::string *replace)
{
	size_t pos;

	for (pos = str.find(*search); pos != std::string::npos; pos = str.find(*search, pos + 1)) {
		if (pos > 0 && (str[pos-1] == '$' || str[pos-1] == '\\'))
			continue;

		str.replace(pos, search->length(), *replace);
	}
}

void substitute_shader_regex_temp(std::string &replacement, const std::string &temp, const std::string &with)
{
	std::string search_str;

	search_str = std::string("$") + temp;
	replacement_search_and_replace(replacement, &search_str, &with);

	search_str = std::string("${") + temp + std::string("}");
	replacement_search_and_replace(replacement, &search_str, &with);
}

static void substitute_temp_regs(std::string &replacement, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	ShaderRegexTemps::iterator i;
	unsigned tmp_reg = dcl_temps;

	for (i = temp_regs->begin(); i != temp_regs->end(); i++, tmp_reg++)
		substitute_shader_regex_temp(replacement, *i, std::string("r") + std::to_string(tmp_reg));
}

bool ShaderRegexPattern::patch(std::string *asm_text, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	std::string replace_copy;

	// We operate on a copy of the replace string so that future shaders
	// don't get our temporary register numbers:
	replace_copy = replace;
	substitute_temp_regs(replace_copy, temp_regs, dcl_temps);
	return patch_with(asm_text, replace_copy);
}

bool ShaderRegexPattern::patch_with(std::string *asm_text, const std::string &replace_copy)
{
	pcre2_match_data *match_data = NULL;
	PCRE2_SIZE est_size, output_size;
	PCRE2_UCHAR *buf = NULL;
	bool patch = false;
	uint32_t options;
	int rc;

	static_assert(PCRE2_CODE_UNIT_WIDTH == 8, "Need to fix output buffer allocation for non-8bit pcre2");

	// TODO: Allow named capture groups from other patterns in the same
	// regex group to be substituted in, and provide some simple arithmetic
	// operators to e.g. allow a constant buffer byte offset to be divided
	// by 16 to get the constant buffer index and vice versa

	// At a minimum we want \n to be translated in the replace string,
	// which needs extended substitution processing to be enabled:
	options = PCRE2_SUBSTITUTE_EXTENDED;

	match_data = pcre2_match_data_create_from_pattern(regex, NULL);

	output_size = est_size = asm_text->length() + replace_copy.length() + 1024;
	buf = new PCRE2_UCHAR[output_size];
	rc = pcre2_substitute(regex,
			(PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0,
			options | PCRE2_SUBSTITUTE_OVERFLOW_LENGTH,
			match_data, NULL,
			(PCRE2_SPTR)replace_copy.c_str(), replace_copy.length(),
			buf, &output_size);

	if (rc == PCRE2_ERROR_NOMEMORY) {
		LogInfo("  NOTICE: regex replace requires a %u byte buffer\n", (unsigned)output_size);
		LogInfo("  NOTICE: We underestimated by %u bytes and have to start over\n", (unsigned)(output_size - est_size));
		LogInfo("  NOTICE: What kind of crazy are you doing to get down this code path?\n");
		LogInfo("  NOTICE: You didn't inject a matrix inverse or two in assembly did you?\n");
		LogInfo("  NOTICE: Once more, with passion!\n");

		delete [] buf;
		buf = new PCRE2_UCHAR[output_size];

		rc = pcre2_substitute(regex,
				(PCRE2_SPTR)asm_text->c_str(), asm_text->length(), 0,
				options, // No PCRE2_SUBSTITUTE_OVERFLOW_LENGTH this time
				match_data, NULL,
				(PCRE2_SPTR)replace_copy.c_str(), replace_copy.length(),
				buf, &output_size);
	}

	if (rc == 0)
		goto out_free;
	if (rc < 0) {
		log_pcre2_error_nonl(rc, "  WARNING: regex replace error");
		goto out_free;
	}

	*asm_text = (char*)buf;
	patch = true;

out_free:
	pcre2_match_data_free(match_data);
	delete [] buf;

	return patch;
}

bool parse_spirv_declaration(const std::string &line, int ini_params_reg, SpirvDeclaration *declaration)
{
	unsigned reg;
	int len = 0;

	if (sscanf_s(line.c_str(), "dcl_resource_texture2d (float,float,float,float) t%u%n", &reg, &len) == 1
	    && (size_t)len == line.size() && reg < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
		*declaration = { SpirvDeclarationKind::TEXTURE2D, reg };
		return true;
	}
	len = 0;
	if (sscanf_s(line.c_str(), "dcl_resource_texture1d (float,float,float,float) t%u%n", &reg, &len) == 1
	    && (size_t)len == line.size() && (int)reg == ini_params_reg) {
		*declaration = { SpirvDeclarationKind::INI_PARAMS, reg };
		return true;
	}
	len = 0;
	if (sscanf_s(line.c_str(), "dcl_sampler s%u, mode_default%n", &reg, &len) == 1
	    && (size_t)len == line.size() && reg < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
		*declaration = { SpirvDeclarationKind::SAMPLER, reg };
		return true;
	}
	return false;
}

void ShaderRegexGroup::apply_regex_patterns(std::string *asm_text, bool *match, bool *patch)
{
	ShaderRegexPatterns::iterator i;
	ShaderRegexPattern *pattern;
	unsigned dcl_temps = 0;

	// Match defaults to true so that if there are no patterns we can still
	// apply the command list. Patch defaults to false because we don't
	// want to waste time re-assembling the shader if we didn't change it.
	*match = true;
	*patch = false;

	if (!temp_regs.empty())
		dcl_temps = get_dcl_temps(asm_text);

	for (i = patterns.begin(); i != patterns.end(); i++) {
		pattern = &i->second;

		if (pattern->do_replace)
			*match = *patch = pattern->patch(asm_text, &temp_regs, dcl_temps);
		else
			*match = pattern->matches(asm_text);

		if (!*match) {
			*patch = false;
			return;
		}
	}

	// Only update dcl_temps if we are patching:
	if (*patch && !temp_regs.empty())
		*patch = update_dcl_temps(asm_text, dcl_temps + temp_regs.size());

	// But we can update declarations even if we aren't doing a regex
	// replace in some cases, so long as the patterns all matched (e.g.
	// globally disable the driver stereo cb):
	if (!declarations.empty())
		*patch = insert_declarations(asm_text, &declarations) || *patch;
}

void ShaderRegexGroup::link_command_lists_and_filter_index(UINT64 shader_hash)
{
	ShaderOverride *shader_override = NULL;
	wstring ini_section, ini_line;
	CommandList::Commands::reverse_iterator i;

	// Only link the command lists if we have something in ours to link in,
	// because this will create ShaderOverride sections for shaders that
	// don't already have one, adding more work in the draw calls.

	if (command_list.noop() && post_command_list.noop() && filter_index == FLT_MAX)
		return;

	shader_override = &G->mShaderOverrideMap[shader_hash];
	G->shader_override_generation++;

	// Initialise the ShaderOverride's command lists if they aren't already:
	if (shader_override->command_list.ini_section.empty()) {
		ini_section = command_list.ini_section + L".Match";
		shader_override->command_list.ini_section = ini_section;
		shader_override->post_command_list.ini_section = ini_section;
		shader_override->post_command_list.post = true;
	}

	// Set the filter index for partner filtering:
	if (shader_override->filter_index == FLT_MAX)
		shader_override->filter_index = filter_index;

	// If we have previously linked a command list (on any matched shader)
	// we will reuse the link command here, after checking that this
	// matched shader has not already been linked. Avoids the command lists
	// growing endlessly and eventually killing performance.
	if (link) {
		for (i = shader_override->command_list.commands.rbegin();
		         i != shader_override->command_list.commands.rend(); i++) {
			if (*i == link)
				return;
		}
		shader_override->command_list.commands.push_back(link);
		if (post_link)
			shader_override->post_command_list.commands.push_back(post_link);
		return;
	} else if (post_link) {
		for (i = shader_override->post_command_list.commands.rbegin();
		         i != shader_override->post_command_list.commands.rend(); i++) {
			if (*i == post_link)
				return;
		}
		shader_override->post_command_list.commands.push_back(post_link);
		return;
	}

	// This is the first shader this pattern has matched. Create a new
	// RunLinkedCommandList command and link it up:
	ini_line = L"[" + command_list.ini_section + L".Match] run = linked command list";

	if (!command_list.noop())
		link = LinkCommandLists(&shader_override->command_list, &command_list, &ini_line);

	if (!post_command_list.noop())
		post_link = LinkCommandLists(&shader_override->post_command_list, &post_command_list, &ini_line);
}

bool ShaderRegexGroup::dx12_matches(const Dx12ShaderInfo *shader) const
{
	UINT i;

	// Nothing is known about a shader that could not be read
	if (!shader->reflected)
		return false;
	if ((shader->srv[0] & dx12_srv[0]) != dx12_srv[0] || (shader->srv[1] & dx12_srv[1]) != dx12_srv[1]
	    || (shader->cbv & dx12_cbv) != dx12_cbv || (shader->uav & dx12_uav) != dx12_uav)
		return false;
	if (!dx12_targets.matches_uint(shader->targets))
		return false;
	for (i = 0; i < ARRAYSIZE(dx12_cb_bytes) && i < DX12_CB_SIZES; i++) {
		if (!dx12_cb_bytes[i].matches_uint(shader->cb_bytes[i]))
			return false;
	}
	return true;
}

bool link_shader_regex_groups_dx12(const Dx12ShaderInfo *shader)
{
	const char *shader_type = dx12_stage_names[shader->stage];
	size_t length = strlen(shader_type);
	bool linked = false;

	for (auto &entry : shader_regex_groups) {
		ShaderRegexGroup *group = &entry.second;

		if (group->spirv() || (group->dx12_match ? !group->dx12_matches(shader) : !group->patterns.empty()))
			continue;
		if (group->command_list.noop() && group->post_command_list.noop() && group->filter_index == FLT_MAX)
			continue;

		for (const std::string &model : group->shader_models) {
			if (model.compare(0, length, shader_type) || model[length] != '_')
				continue;
			group->link_command_lists_and_filter_index(shader->hash);
			linked = true;
			break;
		}
	}

	return linked;
}

bool link_shader_regex_groups_declared(const char *shader_type, UINT64 hash, std::string *declarations)
{
	size_t length = strlen(shader_type);
	bool linked = false;

	for (auto &entry : shader_regex_groups) {
		ShaderRegexGroup *group = &entry.second;
		bool match = true;

		if (group->patterns.empty() || group->dx12_match || !group->declarations.empty() || group->spirv())
			continue;
		if (group->command_list.noop() && group->post_command_list.noop() && group->filter_index == FLT_MAX)
			continue;
		if (std::none_of(group->shader_models.begin(), group->shader_models.end(), [&](const std::string &model) {
			return !model.compare(0, length, shader_type) && model[length] == '_'; }))
			continue;

		for (auto &pattern : group->patterns) {
			// One that rewrites the shader is after its instructions
			if (pattern.second.do_replace || !pattern.second.matches(declarations)) {
				match = false;
				break;
			}
		}
		if (!match)
			continue;

		LogInfo("ShaderRegex: %s %016I64x matches [%S] by its declarations\n", shader_type, hash, group->ini_section.c_str());
		group->link_command_lists_and_filter_index(hash);
		linked = true;
	}

	return linked;
}

bool unlink_shader_regex_command_lists_and_filter_index(UINT64 shader_hash)
{
	ShaderOverride *shader_override = NULL;
	CommandList::Commands::iterator i, next;
	RunLinkedCommandList *link;
	bool ret = false;

	auto shader_override_i = G->mShaderOverrideMap.find(shader_hash);
	if (shader_override_i == G->mShaderOverrideMap.end())
		return false;

	shader_override = &shader_override_i->second;

	for (i = shader_override->command_list.commands.begin(), next = i;
	    i != shader_override->command_list.commands.end(); i = next) {
		next++;
		link = dynamic_cast<RunLinkedCommandList*>(i->get());
		if (link) {
			next = shader_override->command_list.commands.erase(i);
			ret = true;
		}
	}

	for (i = shader_override->post_command_list.commands.begin(), next = i;
	    i != shader_override->post_command_list.commands.end(); i = next) {
		next++;
		link = dynamic_cast<RunLinkedCommandList*>(i->get());
		if (link) {
			next = shader_override->post_command_list.commands.erase(i);
			ret = true;
		}
	}

	if (shader_override->filter_index != shader_override->backup_filter_index) {
		shader_override->filter_index = shader_override->backup_filter_index;
		ret = true;
	}

	return ret;
}

#define SHADER_REGEX_CACHE_VERSION 1
struct ShaderRegexCacheHeader {
	uint32_t version;
	uint32_t shader_regex_hash;
	uint32_t patched;
	uint32_t num_matches;
};

static ShaderRegexCache load_shader_regex_cache(UINT64 hash, const wchar_t *shader_type, ShaderRegexResult *result, bool link)
{
	ShaderRegexCache ret = ShaderRegexCache::NO_CACHE;
	HANDLE meta_f = INVALID_HANDLE_VALUE;
	HANDLE bin_f = INVALID_HANDLE_VALUE;
	ShaderRegexCacheHeader *header;
	ShaderRegexGroup *group;
	wchar_t path[MAX_PATH];
	uint32_t *match_ids;
	DWORD size, size2;
	byte *buf = NULL;
	size_t suffix;
	uint32_t i;

	*result = ShaderRegexResult();
	result->regex_hash = shader_regex_hash;
	result->shader_type = shader_type;
	result->tagline = L"//";

	suffix = swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_regex.", G->SHADER_CACHE_PATH, hash, shader_type);
	wcscpy_s(path+suffix, MAX_PATH-suffix, L"dat");
	meta_f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (meta_f == INVALID_HANDLE_VALUE)
		return ret;

	size = GetFileSize(meta_f, 0);
	if (size < sizeof(ShaderRegexCacheHeader))
		goto out;

	buf = new byte[size];

	if (!ReadFile(meta_f, buf, size, &size2, NULL) || size != size2)
		goto out;

	header = (ShaderRegexCacheHeader*)buf;
	match_ids = (uint32_t*)(buf + sizeof(ShaderRegexCacheHeader));

	if (header->version != SHADER_REGEX_CACHE_VERSION
	 || header->shader_regex_hash != shader_regex_hash)
		goto out;

	if (size != sizeof(ShaderRegexCacheHeader) + header->num_matches * sizeof(uint32_t))
		goto out;

	result->patched = !!header->patched;

	// num_matches may be 0, which means the ShaderRegex didn't match the
	// shader, but we cache it anyway to skip processing the shader again.
	// We don't really need any special handling for this case, since
	// returning MATCH will already skip that handling in the caller, but
	// we return a special value so the caller can log it appropriately.
	if (header->num_matches == 0) {
		ret = ShaderRegexCache::NO_MATCH;
		goto out;
	}

	for (i = 0; i < header->num_matches; i++) {
		// The ShaderRegex groups are sorted and since the cached hash
		// already matched the map should be identical to when the
		// cache was made, so we can use that to find the matching
		// groups without having to do an expensive lookup by name:
		if (match_ids[i] >= shader_regex_group_index.size())
			goto out;
		group = shader_regex_group_index[match_ids[i]];
		result->match_ids.push_back(match_ids[i]);

		LogInfo("ShaderRegexCache: %S %016I64x matches [%S]\n", shader_type, hash, group->ini_section.c_str());

		if (header->patched)
			result->tagline.append(std::wstring(L"[") + group->ini_section + std::wstring(L"]"));

		if (link)
			group->link_command_lists_and_filter_index(hash);
	}

	if (header->patched) {
		wcscpy_s(path+suffix, MAX_PATH-suffix, L"bin");
		bin_f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (bin_f == INVALID_HANDLE_VALUE)
			goto out;
		size = GetFileSize(bin_f, 0);
		result->patched_bytecode.resize(size);
		if (!size || !ReadFile(bin_f, result->patched_bytecode.data(), size, &size2, NULL) || size != size2)
			goto out;
		ret = ShaderRegexCache::PATCH;
	} else
		ret = ShaderRegexCache::MATCH;

out:
	if (buf)
		delete [] buf;
	if (bin_f != INVALID_HANDLE_VALUE)
		CloseHandle(bin_f);
	if (meta_f != INVALID_HANDLE_VALUE)
		CloseHandle(meta_f);
	return ret;
}

static void save_shader_regex_cache_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
		bool patched, std::string *asm_text, std::wstring *tagline)
{
	ShaderRegexCacheHeader header;
	wchar_t path[MAX_PATH];
	FILE *f = NULL;
	size_t suffix;

	if (!G->SHADER_CACHE_PATH[0] || (!G->CACHE_SHADERS && !G->EXPORT_FIXED))
		return;

	suffix = swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_regex.", G->SHADER_CACHE_PATH, hash, shader_type);

	if (G->CACHE_SHADERS) {
		// TODO: When we have a condition field in ShaderRegex: The evaluations
		// of *all* valid conditions (not just those matched) must qualify the
		// cache, either by encoding them in the filename or extending the
		// metadata format.

		// Make sure there isn't an old stale .bin file *before* writing the
		// new metadata to make sure it can't be loaded by mistake. If we can't
		// remove it (e.g. another thread is currently reading it or permission
		// issues) it's better not to update the cache at all:
		wcscpy_s(path+suffix, MAX_PATH-suffix, L"bin");
		if (!DeleteFile(path) && GetLastError() != ERROR_FILE_NOT_FOUND)
			return;

		wcscpy_s(path+suffix, MAX_PATH-suffix, L"dat");
		wfopen_ensuring_access(&f, path, L"wb");
		if (!f)
			return;

		header.version = SHADER_REGEX_CACHE_VERSION;
		header.shader_regex_hash = shader_regex_hash;
		header.patched = patched;
		header.num_matches = (uint32_t)match_ids->size();
		fwrite(&header, 1, sizeof(ShaderRegexCacheHeader), f);
		fwrite(match_ids->data(), sizeof(uint32_t), match_ids->size(), f);

		fclose(f);
	}

	if (G->EXPORT_FIXED) {
		wcscpy_s(path+suffix, MAX_PATH-suffix, L"txt");
		if (patched) {
			wfopen_ensuring_access(&f, path, L"wb");
			if (!f) {
				LogInfo("  Error storing ShaderRegex assembly to %S\n", path);
				return;
			}

			fprintf_s(f, "%S\n", tagline->c_str());
			fwrite(asm_text->c_str(), 1, asm_text->size(), f);

			fclose(f);
			LogInfo("  Storing ShaderRegex assembly to %S\n", path);
		} else {
			if (DeleteFile(path))
				LogInfo("  Removed stale ShaderRegex assembly file %S\n", path);
		}
	}
}

void save_shader_regex_cache_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode)
{
	wchar_t path[MAX_PATH];
	FILE *f = NULL;

	if (!G->CACHE_SHADERS || !G->SHADER_CACHE_PATH[0])
		return;

	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_regex.bin", G->SHADER_CACHE_PATH, hash, shader_type);

	wfopen_ensuring_access(&f, path, L"wb");
	if (!f)
		return;
	fwrite(bytecode->data(), 1, bytecode->size(), f);
	fclose(f);
}

bool get_shader_model_from_bytecode(const void* data, size_t size, std::string* out_model)
{
	if (!data || size < 32 || !out_model)
		return false;

	const uint8_t* buffer = static_cast<const uint8_t*>(data);

	// Validate DXBC header
	if (memcmp(buffer, "DXBC", 4) != 0)
		return false;

	const uint8_t* ptr = buffer + 4 + 16; // Skip FOURCC + hash

	// Read header fields
	if (ptr + 12 > buffer + size)
		return false;

	uint32_t one, totalSize, numChunks;
	memcpy(&one, ptr, 4); ptr += 4;
	memcpy(&totalSize, ptr, 4); ptr += 4;
	memcpy(&numChunks, ptr, 4); ptr += 4;

	if (numChunks == 0)
		return false;

	// Validate chunk table bounds
	if (ptr + numChunks * sizeof(uint32_t) > buffer + size)
		return false;

	const uint32_t* chunkOffsets = reinterpret_cast<const uint32_t*>(ptr);

	// Iterate chunks backwards (same as disassembler)
	for (int32_t i = (int32_t)numChunks - 1; i >= 0; --i)
	{
		uint32_t offset = chunkOffsets[i];

		if (offset + 12 > size)
			continue;

		const uint8_t* chunk = buffer + offset;

		// Look for shader code chunk
		if (memcmp(chunk, "SHEX", 4) != 0 && memcmp(chunk, "SHDR", 4) != 0)
			continue;

		// Version token is at +8
		uint32_t versionToken;
		memcpy(&versionToken, chunk + 8, 4);

		uint32_t type = (versionToken >> 16) & 0xFFFF;
		uint32_t major = (versionToken >> 4) & 0xF;
		uint32_t minor = (versionToken >> 0) & 0xF;

		const char* prefix = "xx";
		switch (type)
		{
			case 0: prefix = "ps"; break;
			case 1: prefix = "vs"; break;
			case 2: prefix = "gs"; break;
			case 3: prefix = "hs"; break;
			case 4: prefix = "ds"; break;
			case 5: prefix = "cs"; break;
		}

		char buf[16];
		snprintf(buf, sizeof(buf), "%s_%u_%u", prefix, major, minor);

		*out_model = buf;
		return true;
	}

	return false;
}

// Process groups that do not have patches to apply. Those can be handled without disassembly.
static void link_shader_regex_groups_without_patterns(const wchar_t* shader_type, std::string* shader_model, UINT64 hash, bool* decompilation_required, std::vector<uint32_t> *match_ids_out, bool link)
{
	ShaderRegexGroups::iterator i;
	vector<uint32_t> match_ids;
	vector<ShaderRegexGroup*> match_groups;
	uint32_t j;

	for (i = shader_regex_groups.begin(), j = 0; i != shader_regex_groups.end(); i++, j++) {
		ShaderRegexGroup* group = &i->second;

		// Skip group without matching shader model, and those for SPIR-V.
		if (!group->shader_models.count(*shader_model) || group->spirv()) {
			continue;
		}

		// Skip group with patterns. Those ones need txt to match.
		if (!group->patterns.empty()) {
			if (decompilation_required)
				*decompilation_required = true;
			continue;
		}

		match_ids.push_back(j);
		match_groups.push_back(group);

		LogInfo("ShaderRegex (no pattern): %S %016I64x matches [%S]\n", shader_type, hash, group->ini_section.c_str());
	}

	// If no ShaderRegEx requires decompilation, link CommandLists and update shader cache here instead of `apply_shader_regex_groups`. 
	if (decompilation_required && !*decompilation_required) {
		// Enable CommandList sections execution for this group.
		for (ShaderRegexGroup* group : match_groups) {
			if (link)
				group->link_command_lists_and_filter_index(hash);
		}
		// We save the cache metadata even if we didn't match anything. That
		// way we can skip checking for a match next time when we know there
		// won't be any. This only saves the metadata - the caller will use
		// save_shader_regex_cache_bin to save the assembled binary.
		save_shader_regex_cache_meta(hash, shader_type, &match_ids, false, nullptr, nullptr);
		if (match_ids_out)
			*match_ids_out = match_ids;
	}
}

bool apply_shader_regex_groups(std::string *asm_text, const wchar_t *shader_type, std::string *shader_model, UINT64 hash, std::wstring *tagline, std::vector<uint32_t> *match_ids_out, bool link)
{
	ShaderRegexGroups::iterator i;
	ShaderRegexGroup *group;
	bool patched = false;
	bool match, patch;
	vector<uint32_t> match_ids;
	uint32_t j;

	for (i = shader_regex_groups.begin(), j = 0; i != shader_regex_groups.end(); i++, j++) {
		group = &i->second;

		// Skip group without matching shader model, and those for SPIR-V.
		if (!group->shader_models.count(*shader_model) || group->spirv()) {
			continue;
		}

		// Match/patch only ShaderRegEx with Pattern.
		if (!group->patterns.empty()) {
			// Run patch.
			group->apply_regex_patterns(asm_text, &match, &patch);
			if (!match)
				continue;

			LogInfo("ShaderRegex: %s %016I64x matches [%S]\n", shader_model->c_str(), hash, group->ini_section.c_str());
			patched = patched || patch;

			// Append section to patch sequence.
			if (patch && tagline)
				tagline->append(std::wstring(L"[") + group->ini_section + std::wstring(L"]"));
		}

		match_ids.push_back(j);

		// Enable CommandList sections execution for this group.
		if (link)
			group->link_command_lists_and_filter_index(hash);
	}

	// We save the cache metadata even if we didn't match anything. That
	// way we can skip checking for a match next time when we know there
	// won't be any. This only saves the metadata - the caller will use
	// save_shader_regex_cache_bin to save the assembled binary.
	save_shader_regex_cache_meta(hash, shader_type, &match_ids, patched, asm_text, tagline);
	if (match_ids_out)
		*match_ids_out = match_ids;

	return patched;
}

static ShaderRegexResult* store_shader_regex_result(UINT64 hash, ShaderRegexResult *result)
{
	ShaderRegexResult *stored = &shader_regex_results[hash];
	*stored = std::move(*result);
	return stored;
}

// The analysis that used to live in HackerContext::DeferredShaderReplacement():
// disassemble, run the ShaderRegex groups over the assembly, reassemble when
// anything was patched. Only reads the ShaderRegex sections, and with link
// false leaves the command lists alone, so that the background threads can
// run it. Returns false when it failed:
static bool analyse_shader_regex_bytecode(UINT64 hash, const wchar_t *shader_type, const void *bytecode, size_t bytecode_size,
		std::string *shader_model, bool link, ShaderRegexResult *result)
{
	bool decompilation_required = false;
	bool patch_regex = false;
	string asm_text;
	vector<char> asm_vector;
	HRESULT hr;

	// One at a time, so that the background threads leave a core to the
	// game. A render thread that could not use them for a shader may be in
	// here as well:
	std::lock_guard<std::mutex> guard(shader_regex_analysis_lock);

	// Process ShaderRegex sections that don't require bytecode decompilation.
	link_shader_regex_groups_without_patterns(shader_type, shader_model, hash, &decompilation_required, &result->match_ids, link);

	// Skip disassemble entirely if there are no matching ShaderRegex with Patterns found.
	if (!decompilation_required) {
		LogInfo("%S %016I64x disassembly skipped: no matching ShaderRegex with Patterns found for %s.\n", shader_type, hash, shader_model->c_str());
		return true;
	}

	// Disassemble shader bytecode.
	asm_text = BinaryToAsmText(
		bytecode,
		bytecode_size,
		G->patch_cb_offsets,
		G->disassemble_undecipherable_custom_data);

	if (asm_text.empty())
		return false;

	// Apply patches from ShaderRegex with Patterns (and Templates).
	try {
		patch_regex = apply_shader_regex_groups(&asm_text, shader_type, shader_model, hash, &result->tagline, &result->match_ids, link);
	} catch (...) {
		LogInfo("    *** Exception while patching shader\n");
		return false;
	}

	if (!patch_regex) {
		LogInfo("Patch did not apply\n");
		return true;
	}

	// No longer logging this since we can output to ShaderFixes
	// via hunting if marking_actions = regex, or it could be
	// disassembled from the regex cache with cmd_Decompiler
	// LogInfo("Patched Shader:\n%s\n", asm_text.c_str());

	asm_vector.assign(asm_text.begin(), asm_text.end());

	try {
		vector<AssemblerParseError> parse_errors;
		hr = AssembleFluganWithSignatureParsing(&asm_vector, &result->patched_bytecode, &parse_errors);
		if (FAILED(hr)) {
			LogInfo("    *** Assembling patched shader failed\n");
			return false;
		}
		// Parse errors are currently being treated as non-fatal on
		// creation time replacement and ShaderRegex for backwards
		// compatibility (live shader reload is fatal).
		for (auto &parse_error : parse_errors)
			LogOverlayW(LOG_NOTICE, L"%016I64x-%ls %ls: %S\n",
					hash, shader_type, result->tagline.c_str(), parse_error.what());
	} catch (const exception &e) {
		LogOverlayW(LOG_WARNING, L"Error assembling ShaderRegex patched %016I64x-%ls\n%ls\n%S\n",
				hash, shader_type, result->tagline.c_str(), e.what());
		return false;
	}

	save_shader_regex_cache_bin(hash, shader_type, &result->patched_bytecode);
	result->patched = true;

	return true;
}

// Returns NULL when the analysis failed, in which case nothing is remembered
// and the shader will be analysed again after the next config reload:
static ShaderRegexResult* analyse_shader_regex(UINT64 hash, const wchar_t *shader_type, OriginalShaderInfo *orig_info, bool link, ShaderRegexResult *result)
{
	LogInfo("Performing deferred shader analysis on %S %016I64x...\n", shader_type, hash);

	// Detect shader model
	auto it = G->mShaderModelCache.find(hash);
	if (it != G->mShaderModelCache.end()) {
		orig_info->shaderModel = it->second.shaderModel;
		LogInfo("%S %016I64x shader model %s is loaded from cache.\n", shader_type, hash, orig_info->shaderModel.c_str());
	}
	else {
		if (orig_info->shaderModel == "bin") {
			// Get shader model from bytecode.
			if (!get_shader_model_from_bytecode(orig_info->byteCode->GetBufferPointer(), orig_info->byteCode->GetBufferSize(), &orig_info->shaderModel)) {
				LogInfo("%S %016I64x shader model detection from bytecode failed.\n", shader_type, hash);
				return NULL;
			}
			// Store shader model in cache.
			G->mShaderModelCache.emplace(hash, ShaderModelCacheEntry{ orig_info->shaderModel });
			LogInfo("%S %016I64x shader model %s detected from bytecode.\n", shader_type, hash, orig_info->shaderModel.c_str());
		}
	}

	if (!analyse_shader_regex_bytecode(hash, shader_type, orig_info->byteCode->GetBufferPointer(),
			orig_info->byteCode->GetBufferSize(), &orig_info->shaderModel, link, result))
		return NULL;

	return store_shader_regex_result(hash, result);
}

// ShaderRegex in the background
//
// Working out a shader's ShaderRegex outcome for the first time means
// disassembling, matching and reassembling it, and a patched shader then has
// to be compiled by the driver, which together stalls the first draw that uses
// it for 100ms or more. With shader_regex_background enabled those two steps
// run on our own threads instead, starting when the game creates the shader.
// A draw that comes before they are done either uses the shader as the game
// made it and checks again on the next draw, or waits, depending on the mode.
//
// The threads never take G->mCriticalSection, so the render thread may wait
// for them while holding it. They read the ShaderRegex sections under
// shader_regex_config_lock, which a config reload holds exclusively, and leave
// linking the command lists of the matched groups to the render thread.

// 0: off, 1: draw unpatched until ready, 2: wait at the draw
static int shader_regex_background_mode;

static SRWLOCK shader_regex_config_lock = SRWLOCK_INIT;

ShaderRegexConfigUpdate::ShaderRegexConfigUpdate()
{
	AcquireSRWLockExclusive(&shader_regex_config_lock);
}

ShaderRegexConfigUpdate::~ShaderRegexConfigUpdate()
{
	ReleaseSRWLockExclusive(&shader_regex_config_lock);
}

ShaderRegexConfigRead::ShaderRegexConfigRead()
{
	AcquireSRWLockShared(&shader_regex_config_lock);
}

ShaderRegexConfigRead::~ShaderRegexConfigRead()
{
	ReleaseSRWLockShared(&shader_regex_config_lock);
}

void set_shader_regex_background(int mode)
{
	shader_regex_background_mode = mode;
}

enum class ShaderRegexJobState {
	QUEUED,
	RUNNING,
	DONE,
	FAILED,
};

enum class ShaderRegexJobStatus {
	NONE,
	PENDING,
	READY,
	FAILED,
};

// One per shader hash. Stays around once done to hand its shader object to
// every shader of that hash, until the ShaderRegex sections change:
struct ShaderRegexJob {
	uint64_t id = 0;
	ShaderRegexJobState state = ShaderRegexJobState::QUEUED;
	bool urgent = false;
	std::wstring shader_type;
	ID3D11Device *device = NULL;
	// The game's bytecode, for the analysis:
	ID3DBlob *bytecode = NULL;
	// Set when the render thread already found there are no cache files:
	bool cache_checked = false;
	// Either passed in by the render thread, leaving only the patched
	// shader to create, or worked out by the job. The command lists of its
	// groups have not been linked. The patched bytecode is dropped once the
	// shader has been created from it:
	bool have_result = false;
	ShaderRegexResult result;
	ID3D11DeviceChild *shader = NULL;
};

struct ShaderRegexJobs {
	std::mutex lock;
	std::condition_variable wake;
	std::condition_variable done;
	std::unordered_map<UINT64, ShaderRegexJob> jobs;
	std::deque<UINT64> urgent;
	std::deque<UINT64> normal;
	uint64_t next_id = 1;
	bool threads_started = false;
	unsigned threads = 0;
};

static ShaderRegexJobs& shader_regex_jobs()
{
	// Never freed: the threads may still be running when the statics of
	// the DLL are destroyed at exit.
	static ShaderRegexJobs *jobs = new ShaderRegexJobs();
	return *jobs;
}

static ShaderRegexResult copy_shader_regex_result_without_bytecode(const ShaderRegexResult &result)
{
	ShaderRegexResult copy;

	copy.regex_hash = result.regex_hash;
	copy.shader_type = result.shader_type;
	copy.patched = result.patched;
	copy.match_ids = result.match_ids;
	copy.tagline = result.tagline;
	return copy;
}

static ID3D11DeviceChild* create_patched_shader(ID3D11Device *device, const wstring &type,
		ID3D11ClassLinkage *linkage, const std::vector<byte> &patched_bytecode)
{
	const void *bytecode = patched_bytecode.data();
	SIZE_T size = patched_bytecode.size();
	ID3D11DeviceChild *shader = NULL;
	HRESULT hr;

	if (type == L"vs")
		hr = device->CreateVertexShader(bytecode, size, linkage, (ID3D11VertexShader**)&shader);
	else if (type == L"ps")
		hr = device->CreatePixelShader(bytecode, size, linkage, (ID3D11PixelShader**)&shader);
	else if (type == L"cs")
		hr = device->CreateComputeShader(bytecode, size, linkage, (ID3D11ComputeShader**)&shader);
	else if (type == L"gs")
		hr = device->CreateGeometryShader(bytecode, size, linkage, (ID3D11GeometryShader**)&shader);
	else if (type == L"hs")
		hr = device->CreateHullShader(bytecode, size, linkage, (ID3D11HullShader**)&shader);
	else if (type == L"ds")
		hr = device->CreateDomainShader(bytecode, size, linkage, (ID3D11DomainShader**)&shader);
	else
		hr = E_INVALIDARG;

	if (FAILED(hr) && shader) {
		shader->Release();
		shader = NULL;
	}

	return shader;
}

// The outcome from the cache files, else by analysis, without linking any
// command lists. Call with shader_regex_config_lock or G->mCriticalSection held:
static bool prepare_shader_regex_result(UINT64 hash, const wchar_t *shader_type, ID3DBlob *bytecode,
		bool cache_checked, ShaderRegexResult *result)
{
	std::string shader_model;

	if (!cache_checked && load_shader_regex_cache(hash, shader_type, result, false) != ShaderRegexCache::NO_CACHE)
		return true;

	*result = ShaderRegexResult();
	result->regex_hash = shader_regex_hash;
	result->shader_type = shader_type;
	result->tagline = L"//";

	if (!bytecode || !get_shader_model_from_bytecode(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), &shader_model))
		return false;

	return analyse_shader_regex_bytecode(hash, shader_type, bytecode->GetBufferPointer(),
			bytecode->GetBufferSize(), &shader_model, false, result);
}

static void release_shader_regex_job(ShaderRegexJob *job)
{
	if (job->bytecode)
		job->bytecode->Release();
	if (job->shader)
		job->shader->Release();
	if (job->device)
		job->device->Release();
	job->bytecode = NULL;
	job->shader = NULL;
	job->device = NULL;
}

static ShaderRegexJob* next_shader_regex_job(ShaderRegexJobs &j, UINT64 *hash)
{
	for (std::deque<UINT64> *queue : {&j.urgent, &j.normal}) {
		while (!queue->empty()) {
			*hash = queue->front();
			queue->pop_front();
			// A job may be queued in both, or have been taken back
			// by the render thread:
			auto i = j.jobs.find(*hash);
			if (i != j.jobs.end() && i->second.state == ShaderRegexJobState::QUEUED)
				return &i->second;
		}
	}

	return NULL;
}

static DWORD WINAPI shader_regex_thread(void *param)
{
	ShaderRegexJobs &j = shader_regex_jobs();

	// Whatever a draw is not waiting on should not take CPU time from the game:
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

	for (;;) {
		ID3D11DeviceChild *shader = NULL;
		ID3DBlob *bytecode = NULL, *job_bytecode = NULL;
		ID3D11Device *device = NULL;
		ShaderRegexResult result;
		std::wstring shader_type;
		bool cache_checked, have_result;
		UINT64 hash = 0;
		uint64_t id;

		{
			std::unique_lock<std::mutex> lock(j.lock);
			ShaderRegexJob *job;

			while (!(job = next_shader_regex_job(j, &hash)))
				j.wake.wait(lock);

			job->state = ShaderRegexJobState::RUNNING;
			id = job->id;
			shader_type = job->shader_type;
			cache_checked = job->cache_checked;
			have_result = job->have_result;
			result = std::move(job->result);
			bytecode = job->bytecode;
			if (bytecode)
				bytecode->AddRef();
			device = job->device;
			device->AddRef();
		}

		if (!have_result) {
			AcquireSRWLockShared(&shader_regex_config_lock);
			have_result = prepare_shader_regex_result(hash, shader_type.c_str(), bytecode, cache_checked, &result);
			ReleaseSRWLockShared(&shader_regex_config_lock);
		}

		if (have_result && result.patched) {
			shader = create_patched_shader(device, shader_type, NULL, result.patched_bytecode);
			if (!shader)
				LogInfo("    *** Creating replacement shader for %S %016I64x failed\n", shader_type.c_str(), hash);
			std::vector<byte>().swap(result.patched_bytecode);
		}

		{
			std::lock_guard<std::mutex> lock(j.lock);

			auto i = j.jobs.find(hash);
			if (i != j.jobs.end() && i->second.id == id) {
				ShaderRegexJob *job = &i->second;

				job->result = std::move(result);
				job->have_result = have_result;
				job->shader = shader;
				shader = NULL;
				job_bytecode = job->bytecode;
				job->bytecode = NULL;
				job->state = have_result ? ShaderRegexJobState::DONE : ShaderRegexJobState::FAILED;
			}
		}
		j.done.notify_all();

		if (shader)
			shader->Release();
		if (job_bytecode)
			job_bytecode->Release();
		if (bytecode)
			bytecode->Release();
		device->Release();
	}

	return 0;
}

// Call with the lock of the jobs held, and no job for this hash present.
// result is consumed if passed.
static bool queue_shader_regex_job(ShaderRegexJobs &j, UINT64 hash, const wchar_t *shader_type, ID3D11Device *device,
		ID3DBlob *bytecode, bool cache_checked, ShaderRegexResult *result, bool urgent)
{
	if (!j.threads_started) {
		j.threads_started = true;
		// Two, so that a slow driver compile does not hold up everything
		// else. The analysis itself is serialised.
		for (int i = 0; i < 2; i++) {
			HANDLE thread = CreateThread(NULL, 0, shader_regex_thread, NULL, 0, NULL);
			if (thread) {
				CloseHandle(thread);
				j.threads++;
			}
		}
		if (!j.threads)
			LogInfo("  *** Unable to start the ShaderRegex background threads\n");
	}
	if (!j.threads)
		return false;

	ShaderRegexJob *job = &j.jobs[hash];
	job->id = j.next_id++;
	job->urgent = urgent;
	job->shader_type = shader_type;
	job->device = device;
	job->device->AddRef();
	job->bytecode = bytecode;
	if (job->bytecode)
		job->bytecode->AddRef();
	job->cache_checked = cache_checked;
	if (result) {
		job->have_result = true;
		job->result = std::move(*result);
	}

	(urgent ? j.urgent : j.normal).push_back(hash);
	j.wake.notify_one();
	return true;
}

// A draw needs this one, so it goes ahead of the shaders that were queued
// when the game created them. Returns true if the caller should come back for
// it later, false if it has waited for the job to finish.
static bool await_shader_regex_job(ShaderRegexJobs &j, std::unique_lock<std::mutex> &lock, UINT64 hash)
{
	auto i = j.jobs.find(hash);

	if (i != j.jobs.end() && i->second.state == ShaderRegexJobState::QUEUED && !i->second.urgent) {
		i->second.urgent = true;
		j.urgent.push_back(hash);
		j.wake.notify_one();
	}

	if (shader_regex_background_mode != 2)
		return true;

	j.done.wait(lock, [&] {
		auto i = j.jobs.find(hash);
		return i == j.jobs.end()
			|| i->second.state == ShaderRegexJobState::DONE
			|| i->second.state == ShaderRegexJobState::FAILED;
	});
	return false;
}

// Class linkage belongs to the one shader object it was passed with, hooked
// devices would see the shaders we create as the game's, and a device created
// single threaded must not be called from our threads at all:
static bool shader_regex_background_allowed(ID3D11Device *device, ID3D11ClassLinkage *linkage)
{
	return shader_regex_background_mode && !linkage
		&& !(G->enable_hooks & EnableHooks::DEVICE)
		&& !(device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED);
}

void prewarm_shader_regex(ID3D11Device *device, UINT64 hash, const wchar_t *shader_type,
		ID3DBlob *bytecode, ID3D11ClassLinkage *linkage)
{
	if (!shader_regex_d3d_groups || !bytecode || !shader_regex_background_allowed(device, linkage))
		return;

	ShaderRegexJobs &j = shader_regex_jobs();
	std::lock_guard<std::mutex> lock(j.lock);

	if (j.jobs.count(hash))
		return;

	queue_shader_regex_job(j, hash, shader_type, device, bytecode, false, NULL, false);
}

// Hands over what a job worked out for a shader the render thread has no
// result for yet, without the command lists linked:
static ShaderRegexJobStatus take_shader_regex_job_result(UINT64 hash, const wchar_t *shader_type, ShaderRegexResult *result)
{
	ShaderRegexJobs &j = shader_regex_jobs();
	std::unique_lock<std::mutex> lock(j.lock);
	uint32_t n;

	for (;;) {
		auto i = j.jobs.find(hash);
		if (i == j.jobs.end())
			return ShaderRegexJobStatus::NONE;
		ShaderRegexJob *job = &i->second;

		switch (job->state) {
		case ShaderRegexJobState::QUEUED:
			if (!job->cache_checked) {
				// Not started, and for most shaders the cache
				// files answer in less time than a frame of
				// drawing them unpatched would be noticed for:
				release_shader_regex_job(job);
				j.jobs.erase(i);
				return ShaderRegexJobStatus::NONE;
			}
			// Fall through
		case ShaderRegexJobState::RUNNING:
			if (await_shader_regex_job(j, lock, hash))
				return ShaderRegexJobStatus::PENDING;
			continue;
		case ShaderRegexJobState::DONE: {
			bool valid = job->have_result
				&& job->result.regex_hash == shader_regex_hash
				&& job->result.shader_type == shader_type;

			for (n = 0; valid && n < job->result.match_ids.size(); n++)
				valid = job->result.match_ids[n] < shader_regex_group_index.size();

			if (!valid) {
				release_shader_regex_job(job);
				j.jobs.erase(i);
				return ShaderRegexJobStatus::NONE;
			}

			*result = copy_shader_regex_result_without_bytecode(job->result);
			return ShaderRegexJobStatus::READY;
		}
		case ShaderRegexJobState::FAILED:
			if (job->result.regex_hash == shader_regex_hash)
				return ShaderRegexJobStatus::FAILED;
			release_shader_regex_job(job);
			j.jobs.erase(i);
			return ShaderRegexJobStatus::NONE;
		}
	}
}

// Hands over the patched shader of a job, queueing one to create it from
// result if there is none. NONE means the caller has to create it itself:
static ShaderRegexJobStatus take_shader_regex_job_shader(ID3D11Device *device, UINT64 hash, ShaderRegexResult *result,
		ID3D11DeviceChild **shader)
{
	ShaderRegexJobs &j = shader_regex_jobs();
	std::unique_lock<std::mutex> lock(j.lock);

	for (;;) {
		auto i = j.jobs.find(hash);
		if (i == j.jobs.end()) {
			if (result->patched_bytecode.empty())
				return ShaderRegexJobStatus::NONE;

			// The job gets the only copy:
			ShaderRegexResult job_result = copy_shader_regex_result_without_bytecode(*result);
			job_result.patched_bytecode = std::move(result->patched_bytecode);
			if (!queue_shader_regex_job(j, hash, result->shader_type.c_str(), device, NULL, true, &job_result, true)) {
				result->patched_bytecode = std::move(job_result.patched_bytecode);
				return ShaderRegexJobStatus::NONE;
			}

			if (await_shader_regex_job(j, lock, hash))
				return ShaderRegexJobStatus::PENDING;
			continue;
		}
		ShaderRegexJob *job = &i->second;

		switch (job->state) {
		case ShaderRegexJobState::QUEUED:
		case ShaderRegexJobState::RUNNING:
			if (await_shader_regex_job(j, lock, hash))
				return ShaderRegexJobStatus::PENDING;
			continue;
		case ShaderRegexJobState::DONE:
			if (job->device != device)
				return ShaderRegexJobStatus::NONE;
			if (!job->have_result || !job->result.patched || job->result.regex_hash != result->regex_hash) {
				release_shader_regex_job(job);
				j.jobs.erase(i);
				continue;
			}
			// The driver refused it, which it would do again:
			if (!job->shader)
				return ShaderRegexJobStatus::FAILED;
			*shader = job->shader;
			(*shader)->AddRef();
			return ShaderRegexJobStatus::READY;
		case ShaderRegexJobState::FAILED:
			release_shader_regex_job(job);
			j.jobs.erase(i);
			continue;
		}
	}
}

static void link_shader_regex_result(UINT64 hash, ShaderRegexResult *result)
{
	for (uint32_t id : result->match_ids)
		shader_regex_group_index[id]->link_command_lists_and_filter_index(hash);
}

// The outcome for a shader: from memory, else from a background job, else from
// the cache files, else by analysis. Without background the command lists of
// the matched groups are linked to the shader's ShaderOverride on the way,
// which a config reload clears. With it that is left to the caller, and NULL
// with pending set means the analysis is still running in the background:
static ShaderRegexResult* resolve_shader_regex(ID3D11Device *device, OriginalShaderInfo *orig_info, bool background, bool *pending)
{
	const wchar_t *shader_type = orig_info->shaderType.c_str();
	UINT64 hash = orig_info->hash;
	ShaderRegexResult result;
	uint32_t i;

	for (;;) {
		auto stored = shader_regex_results.find(hash);
		if (stored != shader_regex_results.end()) {
			ShaderRegexResult *r = &stored->second;
			bool valid = r->regex_hash == shader_regex_hash && r->shader_type == shader_type;

			for (i = 0; valid && i < r->match_ids.size(); i++)
				valid = r->match_ids[i] < shader_regex_group_index.size();

			if (valid) {
				if (!background)
					link_shader_regex_result(hash, r);
				return r;
			}

			shader_regex_results.erase(stored);
		}

		if (background) {
			switch (take_shader_regex_job_result(hash, shader_type, &result)) {
			case ShaderRegexJobStatus::PENDING:
				*pending = true;
				return NULL;
			case ShaderRegexJobStatus::READY:
				LogInfo("%S %016I64x ShaderRegex outcome taken from the background\n", shader_type, hash);
				return store_shader_regex_result(hash, &result);
			case ShaderRegexJobStatus::FAILED:
				return NULL;
			case ShaderRegexJobStatus::NONE:
				break;
			}
		}

		switch (load_shader_regex_cache(hash, shader_type, &result, !background)) {
		case ShaderRegexCache::NO_MATCH:
			LogInfo("%S %016I64x has cached ShaderRegex miss\n", shader_type, hash);
			return store_shader_regex_result(hash, &result);
		case ShaderRegexCache::MATCH:
			LogInfo("Loaded %S %016I64x command list from ShaderRegex cache\n", shader_type, hash);
			return store_shader_regex_result(hash, &result);
		case ShaderRegexCache::PATCH:
			LogInfo("Loaded %S %016I64x bytecode from ShaderRegex cache\n", shader_type, hash);
			return store_shader_regex_result(hash, &result);
		case ShaderRegexCache::NO_CACHE:
			break;
		}

		if (!background)
			break;

		{
			ShaderRegexJobs &j = shader_regex_jobs();
			std::unique_lock<std::mutex> lock(j.lock);

			if (!j.jobs.count(hash) && !queue_shader_regex_job(j, hash, shader_type, device, orig_info->byteCode, true, NULL, true))
				break;
		}
		// Comes back as pending, or as its outcome after waiting for it:
	}

	// A partial cache read may have linked some groups and filled some of
	// the result already. Start the analysis from a clean result:
	result = ShaderRegexResult();
	result.regex_hash = shader_regex_hash;
	result.shader_type = shader_type;
	result.tagline = L"//";

	return analyse_shader_regex(hash, shader_type, orig_info, !background, &result);
}

// Results taken from a background job come without the patched bytecode:
static bool restore_patched_bytecode(OriginalShaderInfo *orig_info, ShaderRegexResult *result)
{
	ShaderRegexResult restored;

	if (!prepare_shader_regex_result(orig_info->hash, orig_info->shaderType.c_str(), orig_info->byteCode, false, &restored))
		return false;
	if (!restored.patched || restored.patched_bytecode.empty())
		return false;

	result->patched_bytecode = std::move(restored.patched_bytecode);
	return true;
}

static bool create_shader_regex_replacement(ID3D11Device *device, OriginalShaderInfo *orig_info, ShaderRegexResult *result,
		bool background, bool *pending)
{
	ID3D11DeviceChild *replacement = NULL;

	if (background) {
		switch (take_shader_regex_job_shader(device, orig_info->hash, result, &replacement)) {
		case ShaderRegexJobStatus::PENDING:
			*pending = true;
			return false;
		case ShaderRegexJobStatus::FAILED:
			return false;
		case ShaderRegexJobStatus::READY:
		case ShaderRegexJobStatus::NONE:
			break;
		}
	}

	if (!replacement) {
		if (result->patched_bytecode.empty() && !restore_patched_bytecode(orig_info, result))
			return false;
		replacement = create_patched_shader(device, orig_info->shaderType, orig_info->linkage, result->patched_bytecode);
	}

	CleanupShaderMaps(replacement);
	if (!replacement) {
		LogInfo("    *** Creating replacement shader failed\n");
		return false;
	}

	// Update replacement map so we don't have to repeat this process.
	// Not updating the bytecode in the replaced shader map - we do that
	// elsewhere, but I think that is a bug. Need to untangle that first.
	if (orig_info->replacement)
		orig_info->replacement->Release();
	orig_info->replacement = replacement;
	orig_info->replacement_from_regex = true;
	orig_info->replacement_regex_hash = result->regex_hash;
	orig_info->infoText = result->tagline;

	return true;
}

bool apply_shader_regex_to_shader(ID3D11Device *device, OriginalShaderInfo *orig_info, bool *pending)
{
	bool background = shader_regex_background_allowed(device, orig_info->linkage);
	ShaderRegexResult *result;
	bool replaced = false;

	*pending = false;

	result = resolve_shader_regex(device, orig_info, background, pending);
	if (!result)
		return false;

	if (result->patched) {
		replaced = orig_info->replacement && orig_info->replacement_from_regex
			&& orig_info->replacement_regex_hash == result->regex_hash;
		if (!replaced)
			replaced = create_shader_regex_replacement(device, orig_info, result, background, pending);
	}

	// The command lists were written for the patched shader, so they wait
	// for it, and stay out if the driver refused it:
	if (background && !*pending && (!result->patched || replaced))
		link_shader_regex_result(orig_info->hash, result);

	return replaced;
}

void drop_stale_shader_regex_results()
{
	for (auto i = shader_regex_results.begin(); i != shader_regex_results.end();) {
		if (i->second.regex_hash != shader_regex_hash)
			i = shader_regex_results.erase(i);
		else
			i++;
	}

	ShaderRegexJobs &j = shader_regex_jobs();
	std::lock_guard<std::mutex> lock(j.lock);

	for (auto i = j.jobs.begin(); i != j.jobs.end();) {
		ShaderRegexJob *job = &i->second;
		// Running jobs are found stale when their outcome is taken.
		// Queued ones without a result work with the new sections:
		bool stale = job->state != ShaderRegexJobState::RUNNING
			&& (job->have_result || job->state == ShaderRegexJobState::FAILED)
			&& job->result.regex_hash != shader_regex_hash;

		if (stale) {
			release_shader_regex_job(job);
			i = j.jobs.erase(i);
		} else
			i++;
	}
}
