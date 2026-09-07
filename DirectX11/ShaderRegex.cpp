#include "ShaderRegex.h"
#include "CommandList.h"
#include "globals.h" // For ShaderOverride FIXME: This should be in a separate header
#include "log.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>

ShaderRegexGroups shader_regex_groups;
std::vector<ShaderRegexGroup*> shader_regex_group_index;
uint32_t shader_regex_hash;

// Background ShaderRegex worker state. The worker processes one shader at a
// time off the render thread so a new shader never blocks the frame.
static std::mutex s_shader_regex_queue_mutex;
static std::condition_variable s_shader_regex_queue_cv;
static std::deque<std::shared_ptr<ShaderRegexJob>> s_shader_regex_queue;
static bool s_shader_regex_worker_running = false;

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
	this->pattern = *pattern;
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

static void replacement_search_and_replace(std::string &str, std::string *search, std::string *replace)
{
	size_t pos;

	for (pos = str.find(*search); pos != std::string::npos; pos = str.find(*search, pos + 1)) {
		if (pos > 0 && (str[pos-1] == '$' || str[pos-1] == '\\'))
			continue;

		str.replace(pos, search->length(), *replace);
	}
}

static void substitute_temp_regs(std::string &replacement, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	ShaderRegexTemps::iterator i;
	unsigned tmp_reg = dcl_temps;
	std::string search_str, repl_str;

	for (i = temp_regs->begin(); i != temp_regs->end(); i++, tmp_reg++) {
		repl_str = std::string("r") + std::to_string(tmp_reg);

		search_str = std::string("$") + *i;
		replacement_search_and_replace(replacement, &search_str, &repl_str);

		search_str = std::string("${") + *i + std::string("}");
		replacement_search_and_replace(replacement, &search_str, &repl_str);
	}
}

bool ShaderRegexPattern::patch(std::string *asm_text, ShaderRegexTemps *temp_regs, unsigned dcl_temps)
{
	pcre2_match_data *match_data = NULL;
	PCRE2_SIZE est_size, output_size;
	std::string replace_copy;
	PCRE2_UCHAR *buf = NULL;
	bool patch = false;
	uint32_t options;
	int rc;

	static_assert(PCRE2_CODE_UNIT_WIDTH == 8, "Need to fix output buffer allocation for non-8bit pcre2");

	// We operate on a copy of the replace string so that future shaders
	// don't get our temporary register numbers:
	replace_copy = replace;
	substitute_temp_regs(replace_copy, temp_regs, dcl_temps);

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

// Pure file-I/O read of the ShaderRegex cache for a shader. Fills in the
// cached match ids and, for patched caches, the patched bytecode. It does NOT
// touch the config reloadable shader_regex_groups nor link any command lists,
// so it is safe to call from the background worker. The cache directory and
// shader_regex_hash are snapshotted into the job so the worker never reads
// globals that the render thread can reload.
static ShaderRegexCache read_shader_regex_cache(const wchar_t *shader_cache_path, uint32_t shader_regex_hash_snapshot,
		UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids, vector<byte> *bytecode, bool *patched)
{
	ShaderRegexCache ret = ShaderRegexCache::NO_CACHE;
	HANDLE meta_f = INVALID_HANDLE_VALUE;
	HANDLE bin_f = INVALID_HANDLE_VALUE;
	ShaderRegexCacheHeader *header;
	wchar_t path[MAX_PATH];
	uint32_t *file_match_ids;
	DWORD size, size2;
	byte *buf = NULL;
	size_t suffix;
	uint32_t i;

	if (!shader_cache_path[0])
		return ret;

	suffix = swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%ls_regex.", shader_cache_path, hash, shader_type);
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
	file_match_ids = (uint32_t*)(buf + sizeof(ShaderRegexCacheHeader));

	if (header->version != SHADER_REGEX_CACHE_VERSION
	 || header->shader_regex_hash != shader_regex_hash_snapshot)
		goto out;

	if (size != sizeof(ShaderRegexCacheHeader) + header->num_matches * sizeof(uint32_t))
		goto out;

	if (patched)
		*patched = !!header->patched;

	// num_matches may be 0, which means the ShaderRegex didn't match the
	// shader, but we cache it anyway to skip processing the shader again.
	// We don't really need any special handling for this case, since
	// returning MATCH will already skip that handling in the caller, but
	// we return a special value so the caller can log it appropriately.
	if (header->num_matches == 0) {
		ret = ShaderRegexCache::NO_MATCH;
		goto out;
	}

	match_ids->resize(header->num_matches);
	for (i = 0; i < header->num_matches; i++)
		(*match_ids)[i] = file_match_ids[i];

	if (header->patched) {
		wcscpy_s(path+suffix, MAX_PATH-suffix, L"bin");
		bin_f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (bin_f == INVALID_HANDLE_VALUE)
			goto out;
		size = GetFileSize(bin_f, 0);
		bytecode->resize(size);
		if (!size || !ReadFile(bin_f, bytecode->data(), size, &size2, NULL) || size != size2)
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

ShaderRegexCache load_shader_regex_cache(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode, std::wstring *tagline)
{
	ShaderRegexCache ret;
	ShaderRegexGroup *group;
	vector<uint32_t> match_ids;
	bool patched = false;

	ret = read_shader_regex_cache(G->SHADER_CACHE_PATH, shader_regex_hash, hash, shader_type,
			&match_ids, bytecode, &patched);

	if (ret == ShaderRegexCache::NO_CACHE || ret == ShaderRegexCache::NO_MATCH)
		return ret;

	// The ShaderRegex groups are sorted and since the cached hash already
	// matched the map should be identical to when the cache was made, so we
	// can use that to find the matching groups without having to do an
	// expensive lookup by name:
	for (uint32_t match_id : match_ids) {
		if (match_id >= shader_regex_group_index.size())
			return ShaderRegexCache::NO_CACHE;
		group = shader_regex_group_index[match_id];

		LogInfo("ShaderRegexCache: %S %016I64x matches [%S]\n", shader_type, hash, group->ini_section.c_str());

		if (patched && tagline)
			tagline->append(std::wstring(L"[") + group->ini_section + std::wstring(L"]"));

		group->link_command_lists_and_filter_index(hash);
	}

	return ret;
}

void save_shader_regex_cache_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
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

// Build an immutable snapshot of every ShaderRegex group that matches the
// given shader model, so the expensive disassembly/regex/reassembly work can
// run on a background thread without touching the config reloadable
// shader_regex_groups. Sets *decompilation_required if any matching group has
// patterns (those are the only ones that require disassembly). May be NULL -
// the background worker determines this itself from the snapshot's patterns.
void build_shader_regex_group_snapshot(const std::string *shader_model,
		std::vector<ShaderRegexGroupSnapshot> *snapshot, bool *decompilation_required)
{
	uint32_t j = 0;

	if (decompilation_required)
		*decompilation_required = false;

	for (auto &pair : shader_regex_groups) {
		ShaderRegexGroup *group = &pair.second;
		uint32_t group_index = j++;

		// Skip group without matching shader model.
		if (!group->shader_models.count(*shader_model))
			continue;

		ShaderRegexGroupSnapshot group_snap;
		group_snap.group_index = group_index;
		group_snap.ini_section = group->ini_section;
		group_snap.temp_regs = group->temp_regs;
		group_snap.declarations = group->declarations;

		for (auto &pattern_pair : group->patterns) {
			ShaderRegexPatternSnapshot pattern_snap;
			pattern_snap.pattern = pattern_pair.second.pattern;
			pattern_snap.replace = pattern_pair.second.replace;
			pattern_snap.do_replace = pattern_pair.second.do_replace;
			group_snap.patterns.push_back(std::move(pattern_snap));
		}

		if (!group_snap.patterns.empty() && decompilation_required)
			*decompilation_required = true;

		LogDebug("ShaderRegex snapshot: %s matches [%S]\n", shader_model->c_str(), group->ini_section.c_str());

		snapshot->push_back(std::move(group_snap));
	}
}

bool apply_shader_regex_groups(std::string *asm_text, const wchar_t *shader_type, std::string *shader_model, UINT64 hash, std::wstring *tagline)
{
	ShaderRegexGroups::iterator i;
	ShaderRegexGroup *group;
	bool patched = false;
	bool match, patch;
	vector<uint32_t> match_ids;
	uint32_t j;

	for (i = shader_regex_groups.begin(), j = 0; i != shader_regex_groups.end(); i++, j++) {
		group = &i->second;

		// Skip group without matching shader model.
		if (!group->shader_models.count(*shader_model)) {
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
		group->link_command_lists_and_filter_index(hash);
	}

	// We save the cache metadata even if we didn't match anything. That
	// way we can skip checking for a match next time when we know there
	// won't be any. This only saves the metadata - the caller will use
	// save_shader_regex_cache_bin to save the assembled binary.
	save_shader_regex_cache_meta(hash, shader_type, &match_ids, patched, asm_text, tagline);

	return patched;
}

// -----------------------------------------------------------------------------
// Background ShaderRegex worker.
//
// Disassembling, regex matching and re-assembling a shader is expensive, so we
// run it on a background thread and keep using the original shader until the
// replacement is ready. This means a new shader never blocks the render thread
// - it is swapped to the patched shader on a later draw call instead.
//
// Thread safety:
//  - The worker only ever touches the immutable ShaderRegexGroupSnapshot data
//    captured at submit time, never the config reloadable shader_regex_groups
//    or any DirectX state.
//  - Linking the command lists (which the draw call code executes) and writing
//    the shader cache happen on the render thread in finalize_shader_regex_job.
//  - LogInfo is safe to call from multiple threads because the CRT serializes
//    access to a shared FILE* stream.
// -----------------------------------------------------------------------------

// Mirrors ShaderRegexGroup::apply_regex_patterns, but works purely on an
// immutable snapshot and recompiles the regexes locally on the worker thread.
static void apply_regex_patterns_to_snapshot(ShaderRegexGroupSnapshot *group_snap, std::string *asm_text,
		bool *match_out, bool *patch_out)
{
	unsigned dcl_temps = 0;

	// Match defaults to true so that if there are no patterns we can still
	// apply the command list. Patch defaults to false because we don't want to
	// waste time re-assembling the shader if we didn't change it.
	*match_out = true;
	*patch_out = false;

	if (!group_snap->temp_regs.empty())
		dcl_temps = get_dcl_temps(asm_text);

	for (auto &pattern_snap : group_snap->patterns) {
		ShaderRegexPattern pattern;

		// Compiled regexes are cheap to produce compared to the disassembly
		// and reassembly work they replace, and keep the worker independent
		// of the live (config reloadable) groups:
		if (!pattern.compile(&pattern_snap.pattern)) {
			*match_out = false;
			*patch_out = false;
			return;
		}
		pattern.do_replace = pattern_snap.do_replace;
		pattern.replace = pattern_snap.replace;

		if (pattern.do_replace)
			*match_out = *patch_out = pattern.patch(asm_text, &group_snap->temp_regs, dcl_temps);
		else
			*match_out = pattern.matches(asm_text);

		if (!*match_out) {
			*patch_out = false;
			return;
		}
	}

	// Only update dcl_temps if we are patching:
	if (*patch_out && !group_snap->temp_regs.empty())
		*patch_out = update_dcl_temps(asm_text, dcl_temps + group_snap->temp_regs.size());

	// But we can update declarations even if we aren't doing a regex replace in
	// some cases, so long as the patterns all matched:
	if (!group_snap->declarations.empty())
		*patch_out = insert_declarations(asm_text, &group_snap->declarations) || *patch_out;
}

// Execute one job on the worker thread. Only fills in the job result - it never
// touches the render thread's state.
static void run_shader_regex_job(ShaderRegexJob *job)
{
	// 1) Try the on-disk cache first so the render thread never does file I/O
	// when a shader is first used:
	std::vector<byte> cached_bytecode;
	std::vector<uint32_t> cached_match_ids;
	bool cached_patched = false;
	ShaderRegexCache cache = read_shader_regex_cache(job->shader_cache_path.c_str(), job->shader_regex_hash,
			job->hash, job->shader_type.c_str(), &cached_match_ids, &cached_bytecode, &cached_patched);

	if (cache == ShaderRegexCache::NO_MATCH) {
		// Cached miss - nothing to link and nothing to patch:
		job->from_cache = true;
		job->done.store(true);
		return;
	}

	if (cache == ShaderRegexCache::MATCH || cache == ShaderRegexCache::PATCH) {
		job->from_cache = true;
		job->match_ids = std::move(cached_match_ids);

		// The cached groups all matched this shader model, so they must be
		// present in our snapshot. Treat a cache that references unknown groups
		// as stale and re-analyse the shader instead:
		bool valid = true;
		for (uint32_t id : job->match_ids) {
			bool found = false;
			for (auto &group_snap : job->groups) {
				if (group_snap.group_index == id) {
					found = true;
					break;
				}
			}
			if (!found) {
				valid = false;
				break;
			}
		}
		if (!valid) {
			LogInfo("ShaderRegexCache: %S %016I64x stale cache, re-analysing\n", job->shader_type.c_str(), job->hash);
			job->from_cache = false;
			job->match_ids.clear();
		} else if (cache == ShaderRegexCache::PATCH) {
			job->patched = true;
			job->patched_bytecode = std::move(cached_bytecode);

			// Rebuild the tagline from the snapshot's section names - we can't
			// touch the live groups from the worker. Start with the same "//"
			// prefix as the non-cached path (L1020) so Overlay's FindInfoText
			// can uniformly skip the first two characters:
			job->tagline = L"//";
			for (uint32_t id : job->match_ids)
				for (auto &group_snap : job->groups)
					if (group_snap.group_index == id) {
						job->tagline.append(std::wstring(L"[") + group_snap.ini_section + std::wstring(L"]"));
						break;
					}
			job->done.store(true);
			return;
		} else {
			job->done.store(true);
			return;
		}
	}

	// 2) No usable cache - analyse the shader. If none of the matching groups
	// have patterns, no disassembly is needed - their command lists still get
	// linked, but we can skip the expensive decompile:
	bool any_patterns = false;
	for (auto &group_snap : job->groups) {
		if (!group_snap.patterns.empty()) {
			any_patterns = true;
			break;
		}
	}

	if (!any_patterns) {
		for (auto &group_snap : job->groups)
			job->match_ids.push_back(group_snap.group_index);
		job->done.store(true);
		return;
	}

	// Disassemble the shader bytecode to assembly text:
	std::string asm_text = BinaryToAsmText(job->bytecode.data(), job->bytecode.size(),
			job->patch_cb_offsets, job->disassemble_undecipherable_custom_data);
	if (asm_text.empty()) {
		LogInfo("  Background ShaderRegex disassembly failed for %S %016I64x\n", job->shader_type.c_str(), job->hash);
		job->failed = true;
		job->done.store(true);
		return;
	}

	// Apply every matching group. Groups without patterns always "match" so that
	// their command lists can be linked; groups with patterns must match (and may
	// patch) the assembly:
	std::wstring tagline(L"//");
	std::vector<uint32_t> match_ids;
	bool patched = false;

	for (auto &group_snap : job->groups) {
		if (group_snap.patterns.empty()) {
			match_ids.push_back(group_snap.group_index);
			continue;
		}

		bool match, patch;
		apply_regex_patterns_to_snapshot(&group_snap, &asm_text, &match, &patch);
		if (!match)
			continue;

		LogInfo("ShaderRegex: %s %016I64x matches [%S]\n",
				job->shader_model.c_str(), job->hash, group_snap.ini_section.c_str());
		patched = patched || patch;

		// Append section to patch sequence:
		if (patch)
			tagline.append(std::wstring(L"[") + group_snap.ini_section + std::wstring(L"]"));

		match_ids.push_back(group_snap.group_index);
	}

	job->match_ids = std::move(match_ids);
	job->tagline = std::move(tagline);

	if (patched) {
		// Reassemble the patched assembly back into bytecode on the worker:
		std::vector<char> asm_vector(asm_text.begin(), asm_text.end());
		try {
			std::vector<AssemblerParseError> parse_errors;
			HRESULT hr = AssembleFluganWithSignatureParsing(&asm_vector, &job->patched_bytecode, &parse_errors);
			if (FAILED(hr)) {
				LogInfo("    *** Background ShaderRegex assembling patched shader failed\n");
				job->failed = true;
				job->done.store(true);
				return;
			}
			// Parse errors are currently treated as non-fatal - remember them so
			// the render thread can log them to the OSD:
			for (auto &parse_error : parse_errors)
				job->parse_error_messages.push_back(parse_error.what());
			job->patched_asm = std::move(asm_text);
			job->patched = true;
		} catch (const std::exception &e) {
			LogInfo("    *** Background ShaderRegex assembling patched shader threw: %s\n", e.what());
			job->failed = true;
		}
	}

	job->done.store(true);
}

static void shader_regex_worker_main()
{
	// Run at a low priority so disassembling/reassembling a shader in the
	// background never competes with the render thread for CPU time. This is
	// what makes the delayed replacement effectively invisible even on low
	// core-count CPUs - the worker only runs when the game leaves CPU time
	// available, and lets the render thread have it back whenever it is needed:
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

	for (;;) {
		std::shared_ptr<ShaderRegexJob> job;

		{
			std::unique_lock<std::mutex> lock(s_shader_regex_queue_mutex);
			s_shader_regex_queue_cv.wait(lock, [] { return !s_shader_regex_queue.empty(); });
			job = std::move(s_shader_regex_queue.front());
			s_shader_regex_queue.pop_front();
		}

		run_shader_regex_job(job.get());
	}
}

void submit_shader_regex_job(std::shared_ptr<ShaderRegexJob> job)
{
	{
		std::lock_guard<std::mutex> lock(s_shader_regex_queue_mutex);
		s_shader_regex_queue.push_back(std::move(job));
	}
	s_shader_regex_queue_cv.notify_one();

	// Lazily start the worker thread on the first job:
	std::lock_guard<std::mutex> lock(s_shader_regex_queue_mutex);
	if (s_shader_regex_worker_running)
		return;
	s_shader_regex_worker_running = true;
	try {
		std::thread(shader_regex_worker_main).detach();
	} catch (const std::system_error &) {
		// Couldn't start the worker thread (e.g. out of resources). Fail every
		// queued job so no shader stays stuck in PROCESSING forever; they will
		// fall back to using the original shader until the next config reload:
		LogInfo("WARNING: ShaderRegex worker thread creation failed\n");
		s_shader_regex_worker_running = false;
		for (auto &queued : s_shader_regex_queue) {
			queued->failed = true;
			queued->done.store(true);
		}
		s_shader_regex_queue.clear();
	}
}

bool finalize_shader_regex_job(ShaderRegexJob *job, UINT64 hash, const wchar_t *shader_type,
		std::vector<byte> *out_bytecode, std::wstring *out_tagline)
{
	// Linking the command lists must happen on the render thread because the
	// ShaderOverride command lists are executed by the draw call code:
	for (uint32_t match_id : job->match_ids) {
		if (match_id >= shader_regex_group_index.size()) {
			LogInfo("%S %016I64x ShaderRegex finalize failed: match id out of range\n", shader_type, hash);
			job->failed = true;
			return false;
		}
		shader_regex_group_index[match_id]->link_command_lists_and_filter_index(hash);
	}

	if (job->failed)
		return false;

	if (job->patched) {
		if (!job->from_cache) {
			// Only write the cache when we analysed the shader ourselves - a
			// cache hit already has its cache on disk:
			save_shader_regex_cache_meta(hash, shader_type, &job->match_ids, true, &job->patched_asm, &job->tagline);
			save_shader_regex_cache_bin(hash, shader_type, &job->patched_bytecode);
		}
		*out_bytecode = job->patched_bytecode;
		*out_tagline = job->tagline;
		return true;
	}

	if (!job->from_cache) {
		// Matched but nothing was patched - cache the metadata anyway so the
		// command list matching isn't repeated next time:
		save_shader_regex_cache_meta(hash, shader_type, &job->match_ids, false, nullptr, nullptr);
	}
	return false;
}