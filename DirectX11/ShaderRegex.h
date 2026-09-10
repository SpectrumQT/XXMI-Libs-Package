#pragma once

#include "CommandList.h"

#include <map>
#include <set>
#include <string>
#include <vector>
#include <memory>
#include <atomic>

#include <pcre2.h>

enum class ShaderRegexCache {
	NO_CACHE,
	NO_MATCH,
	MATCH,
	PATCH
};

// State of the deferred ShaderRegex background processing for a single shader.
// Stored on OriginalShaderInfo so the render thread never blocks on the
// disassembly/regex/reassembly work, which now runs on a background thread.
enum class ShaderRegexJobState : uint8_t {
	UNPROCESSED = 0, // No analysis has been started yet.
	PROCESSING,      // A background job is in flight; keep using the original shader.
	PROCESSED,       // Analysis finalized (replacement bound, or none required).
	FAILED           // Analysis failed; no replacement until config reload.
};

// Forward declaration - full definition at the bottom of this header:
struct ShaderRegexJob;

bool get_shader_model_from_bytecode(const void* data, size_t size, std::string* out_model);

enum class ShaderConstantBufferType : uint8_t
{
	NONE              = 0,
	IMMEDIATE_INDEXED = 1,
	DYNAMIC_INDEXED   = 2
};

struct ShaderConstantBuffer
{
	ShaderConstantBufferType type = ShaderConstantBufferType::NONE;
	uint32_t size = 0;
};

enum class ShaderResourceType : uint8_t
{
	NONE       = 0,
	TYPED      = 1,
	STRUCTURED = 2,
	RAW        = 3
};

struct ShaderResource
{
	ShaderResourceType type = ShaderResourceType::NONE;

	// Always D3D_SRV_DIMENSION_BUFFEREX for ShaderResourceType::RAW.
	// Always D3D_SRV_DIMENSION_BUFFER for ShaderResourceType::STRUCTURED.
	D3D_SRV_DIMENSION dimension = D3D_SRV_DIMENSION_UNKNOWN;

	// Only meaningful for ShaderResourceType::STRUCTURED.
	uint32_t stride = 0;
};

struct ShaderBindings
{
	std::array<ShaderConstantBuffer, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constant_buffers{};
	std::array<ShaderResource, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
};

void link_shader_regex_groups_without_patterns(const wchar_t* shader_type, std::string* shader_model, UINT64 hash, bool* decompilation_required);
bool apply_shader_regex_groups(std::string *asm_text, const wchar_t *shader_type, std::string *shader_model, UINT64 hash, std::wstring *tagline);
ShaderRegexCache load_shader_regex_cache(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode, std::wstring *tagline);
void save_shader_regex_cache_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode);
void save_shader_regex_cache_meta(UINT64 hash, const wchar_t *shader_type, vector<uint32_t> *match_ids,
		bool patched, std::string *asm_text, std::wstring *tagline);
bool unlink_shader_regex_command_lists_and_filter_index(UINT64 shader_hash);

// Submit a shader to the background ShaderRegex worker. The caller keeps the
// job alive and must wait for job->done before calling finalize_shader_regex_job.
void submit_shader_regex_job(std::shared_ptr<ShaderRegexJob> job);

// Block until every queued and in-flight background job has finished. Must be
// called on the render thread (e.g. from config reload) before the worker's
// inputs (shader_regex_groups / shader_regex_group_index / shader_regex_hash,
// etc.) are torn down or rebuilt.
void wait_for_shader_regex_jobs();

// Called on the render thread (under the global lock) once job->done is true.
// Links command lists for every matched group and writes the shader cache.
// Returns true if a replacement shader should be created and bound.
bool finalize_shader_regex_job(ShaderRegexJob *job, UINT64 hash, const wchar_t *shader_type,
		std::vector<byte> *out_bytecode, std::wstring *out_tagline);

typedef std::set<std::string> ShaderRegexTemps;
typedef std::set<std::string> ShaderRegexModels;

class ShaderRegexPattern {
public:
	pcre2_code *regex;
	std::string pattern;
	std::string replace;

	bool do_replace;

	// These will be used later when we implement our own advanced
	// substitution to allow matches to be used between multiple patterns
	// in the one regex group, and to apply some (very) simple arithmetic
	// to convert byte offsets to constant buffer indexes and vice versa
	std::set<std::string> named_capture_groups;

	ShaderRegexPattern();
	~ShaderRegexPattern();

	bool compile(std::string *pattern);
	bool named_group_overlaps(ShaderRegexTemps &other_set);
	bool matches(std::string *asm_text);
	bool patch(std::string *asm_text, ShaderRegexTemps *temp_regs, unsigned dcl_temps);
};

// These are sorted to make sure we get consistent results between runs
// in case the user does something that winds up depending on the order:
typedef std::map<std::wstring, ShaderRegexPattern> ShaderRegexPatterns;
typedef std::vector<std::string> ShaderRegexDeclarations;

class ShaderRegexGroup {
public:
	std::wstring ini_section;

	ShaderRegexPatterns patterns;

	ShaderRegexDeclarations declarations;
	ShaderRegexModels shader_models;
	ShaderRegexTemps temp_regs;
	float filter_index;

	CommandList command_list;
	CommandList post_command_list;
	std::shared_ptr<RunLinkedCommandList> link;
	std::shared_ptr<RunLinkedCommandList> post_link;

	void apply_regex_patterns(std::string *asm_text, bool *match, bool *patch);
	void link_command_lists_and_filter_index(UINT64 shader_hash);

	ShaderRegexGroup() :
		filter_index(FLT_MAX)
	{}
};

// Sorted to make sure that we always apply the regex patterns in a consistent
// order, in case the user writes multiple patterns that depend on each other:
typedef std::map<std::wstring, ShaderRegexGroup> ShaderRegexGroups;
extern ShaderRegexGroups shader_regex_groups;
extern std::vector<ShaderRegexGroup*> shader_regex_group_index;

// This hash is of all ShaderRegex sections and is used to determine if a
// cached shader is still valid and to avoid discarding regex patched shaders:
extern uint32_t shader_regex_hash;

// A background job that disassembles, regex matches/patches and reassembles a
// single shader off the render thread. Owned by shared_ptr: created and
// consumed on the render thread, executed by the worker thread.
//
// The worker reads the live shader_regex_groups / shader_regex_group_index /
// shader_regex_hash / G globals directly - this is safe because config reload
// calls wait_for_shader_regex_jobs() before tearing those down.
struct ShaderRegexJob {
	// Input (render thread -> worker):
	UINT64 hash = 0;
	std::wstring shader_type;
	std::string shader_model;
	std::vector<byte> bytecode;               // copy of the original bytecode

	// Worker -> render thread result. done is the synchronisation point: the
	// worker writes all results before storing true (release), and the render
	// thread reads them only after observing true (acquire).
	std::atomic<bool> done{ false };
	bool failed = false;                      // disassembly or assembly failed
	bool patched = false;                     // at least one pattern produced a patch
	bool from_cache = false;                  // result came from the on-disk cache
	std::vector<uint32_t> match_ids;          // indices into shader_regex_group_index
	std::vector<byte> patched_bytecode;
	std::string patched_asm;                  // kept for the EXPORT_FIXED cache .txt
	std::vector<std::string> parse_error_messages;
	std::wstring tagline;
};
