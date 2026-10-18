#pragma once

#include "CommandList.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <pcre2.h>

enum class ShaderRegexCache {
	NO_CACHE,
	NO_MATCH,
	MATCH,
	PATCH
};

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

	// The descriptor set a Vulkan shader has it in, -1 otherwise
	int32_t set = -1;
};

struct ShaderBindings
{
	std::array<ShaderConstantBuffer, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> constant_buffers{};
	std::array<ShaderResource, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> resources{};
	// The registers of the textures the code samples, in the order it first
	// does. Only known of a Vulkan shader.
	std::vector<uint8_t> sampled;
};

struct OriginalShaderInfo;

bool apply_shader_regex_groups(std::string *asm_text, const wchar_t *shader_type, std::string *shader_model, UINT64 hash, std::wstring *tagline, std::vector<uint32_t> *match_ids_out = NULL, bool link = true);
void save_shader_regex_cache_bin(UINT64 hash, const wchar_t *shader_type, vector<byte> *bytecode);
bool unlink_shader_regex_command_lists_and_filter_index(UINT64 shader_hash);
// For the shader of a D3D12 game, which there is no assembly text of for a
// pattern to match: links the groups that name any shader model of its type
// ("vs", "ps", ...) and either have no patterns or say with dx12_match_* keys
// what the shader has to declare, which it does. True if there was one.
struct Dx12ShaderInfo;
bool link_shader_regex_groups_dx12(const Dx12ShaderInfo *shader);
// For a shader there is only a list of declarations of, written the way the
// D3D11 disassembler would: links the groups of its type ("vs", "ps", ...)
// whose patterns all match that and replace nothing.
bool link_shader_regex_groups_declared(const char *shader_type, UINT64 hash, std::string *declarations);

// Works out and applies the ShaderRegex outcome for one shader registered in
// G->mReloadedShaders: links the command lists of the matched groups to its
// ShaderOverride and, when it is patched, creates the replacement shader on
// device unless the one it already has was patched under the current
// ShaderRegex sections. Returns true when a ShaderRegex replacement is in
// place afterwards. With shader_regex_background enabled the outcome may not
// be known yet, or the replacement not created yet: pending is set then and
// the caller has to call again later. Call with G->mCriticalSection held.
bool apply_shader_regex_to_shader(ID3D11Device *device, OriginalShaderInfo *orig_info, bool *pending);

// [Rendering] shader_regex_background, see the comment in ShaderRegex.cpp:
void set_shader_regex_background(int mode);

// Starts on a shader's ShaderRegex outcome in the background as soon as the
// game has created it, ahead of the first draw that needs it:
void prewarm_shader_regex(ID3D11Device *device, UINT64 hash, const wchar_t *shader_type,
		ID3DBlob *bytecode, ID3D11ClassLinkage *linkage);

// Keeps the background threads out of the ShaderRegex sections and the
// settings their analysis reads while a config reload replaces them:
class ShaderRegexConfigUpdate {
public:
	ShaderRegexConfigUpdate();
	~ShaderRegexConfigUpdate();
};

// And for other threads of their own reading the sections while it does not
class ShaderRegexConfigRead {
public:
	ShaderRegexConfigRead();
	~ShaderRegexConfigRead();
};

// Config reload: forgets the results of the previous ShaderRegex sections.
// Call with G->mCriticalSection held.
void drop_stale_shader_regex_results();

typedef std::set<std::string> ShaderRegexTemps;
typedef std::set<std::string> ShaderRegexModels;

class ShaderRegexPattern {
public:
	pcre2_code *regex;
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
	// With the temps already substituted in that
	bool patch_with(std::string *asm_text, const std::string &replace);
};

// $temp and ${temp} in a replacement
void substitute_shader_regex_temp(std::string &replacement, const std::string &temp, const std::string &with);

// These are sorted to make sure we get consistent results between runs
// in case the user does something that winds up depending on the order:
typedef std::map<std::wstring, ShaderRegexPattern> ShaderRegexPatterns;
typedef std::vector<std::string> ShaderRegexDeclarations;

// What the InsertDeclarations of a group with SpirvPattern subsections add to
// a SPIR-V shader, see vk/SpirvPatch.h
enum class SpirvDeclarationKind {
	TEXTURE2D,
	INI_PARAMS,
	SAMPLER,
};

struct SpirvDeclaration {
	SpirvDeclarationKind kind;
	unsigned reg;
};

// A line of InsertDeclarations, as a D3D11 shader would have it
bool parse_spirv_declaration(const std::string &line, int ini_params_reg, SpirvDeclaration *declaration);

class ShaderRegexGroup {
public:
	std::wstring ini_section;

	ShaderRegexPatterns patterns;

	// SpirvPattern* subsections, applied by name to the text of a Vulkan
	// game's SPIR-V shader. A group with any is for those alone, and its
	// Pattern subsections (which may not replace) only say which shaders,
	// by what they declare as a D3D11 one would. The other paths leave it
	// alone.
	ShaderRegexPatterns spirv_patterns;
	std::vector<SpirvDeclaration> spirv_declarations;

	ShaderRegexDeclarations declarations;
	ShaderRegexModels shader_models;
	ShaderRegexTemps temp_regs;
	float filter_index;

	// The dx12_match_* keys: registers of space 0 the shader of a D3D12
	// game has to declare (t0-t127, b0-b63, u0-u63), how many render
	// targets it writes and how many bytes its constant buffers have
	bool dx12_match;
	UINT64 dx12_srv[2], dx12_cbv, dx12_uav;
	FuzzyMatch dx12_targets;
	FuzzyMatch dx12_cb_bytes[16];

	CommandList command_list;
	CommandList post_command_list;
	std::shared_ptr<RunLinkedCommandList> link;
	std::shared_ptr<RunLinkedCommandList> post_link;

	bool spirv() const { return !spirv_patterns.empty(); }
	void apply_regex_patterns(std::string *asm_text, bool *match, bool *patch);
	void link_command_lists_and_filter_index(UINT64 shader_hash);
	bool dx12_matches(const Dx12ShaderInfo *shader) const;

	ShaderRegexGroup() :
		filter_index(FLT_MAX),
		dx12_match(false),
		dx12_srv(),
		dx12_cbv(0),
		dx12_uav(0)
	{}
};

// Sorted to make sure that we always apply the regex patterns in a consistent
// order, in case the user writes multiple patterns that depend on each other:
typedef std::map<std::wstring, ShaderRegexGroup> ShaderRegexGroups;
extern ShaderRegexGroups shader_regex_groups;
// Whether any of them is for D3D11 or D3D12 shaders: those with SpirvPattern
// subsections are not
extern bool shader_regex_d3d_groups;
extern std::vector<ShaderRegexGroup*> shader_regex_group_index;

// This hash is of all ShaderRegex sections and is used to determine if a
// cached shader is still valid and to avoid discarding regex patched shaders:
extern uint32_t shader_regex_hash;
