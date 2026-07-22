#pragma once

#include <memory>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <forward_list>
#include <d3d11_1.h>
#include <DirectXMath.h>
#include <util.h>

#include "DrawCallInfo.h"
#include "ResourceHash.h"
#include "HackerInputLayout.h"

// Used to prevent typos leading to infinite recursion (or at least overflowing
// the real stack) due to a section running itself or a circular reference. 64
// should be more than generous - I don't want it to be too low and stifle
// people's imagination, but I'd be very surprised if anyone ever has a
// legitimate need to exceed this:
#define MAX_COMMAND_LIST_RECURSION 64

// Forward declarations instead of #includes to resolve circular includes (we
// include Hacker*.h, which includes Globals.h, which includes us):
class HackerDevice;
class HackerContext;
enum class FrameAnalysisOptions;
class ResourceCopyTarget;

struct InputLayoutElementOverride {
	struct Match {
		std::string semantic_name;                // empty = wildcard
		UINT semantic_index = UINT32_MAX;         // UINT32_MAX = wildcard
		UINT input_slot = UINT32_MAX;             // UINT32_MAX = wildcard
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // UNKNOWN = wildcard
		UINT format_byte_size = UINT32_MAX;       // UINT32_MAX = wildcard
		UINT aligned_byte_offset = UINT32_MAX;    // UINT32_MAX = wildcard
	} match;
	struct Replace {
		std::string semantic_name;                // empty = don't change
		UINT semantic_index = UINT32_MAX;         // UINT32_MAX = don't change
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN; // DXGI_FORMAT_UNKNOWN = don't change
		UINT aligned_byte_offset = UINT32_MAX;   // UINT32_MAX = don't change
	} replace;
};

class CommandListState {
public:
	HackerDevice *mHackerDevice;
	HackerContext *mHackerContext;
	ID3D11Device1 *mOrigDevice1;
	ID3D11DeviceContext1 *mOrigContext1;

	// Used to avoid querying the render target dimensions twice in the
	// common case we are going to store both width & height in separate
	// ini params:
	float rt_width, rt_height;
	DrawCallInfo *call_info;
	bool post;
	bool aborted;

	bool scissor_valid;
	D3D11_RECT scissor_rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];

	// If set this resource is in some way related to the command list
	// invocation - a constant buffer we are analysing, a render target
	// being cleared, etc.
	ResourceCopyTarget *this_target;
	ID3D11Resource **resource;
	ID3D11View *view;
	std::vector<InputLayoutElementOverride*> input_layout_overrides;

	// TODO: Cursor info and resources would be better off being cached
	// somewhere that is updated at most once per frame rather than once
	// per command list execution, and we would ideally skip the resource
	// creation if the cursor is unchanged.
	CURSORINFO cursor_info;
	POINT cursor_window_coords;
	ICONINFO cursor_info_ex;
	ID3D11Texture2D *cursor_mask_tex;
	ID3D11Texture2D *cursor_color_tex;
	ID3D11ShaderResourceView *cursor_mask_view;
	ID3D11ShaderResourceView *cursor_color_view;
	RECT window_rect;

	int recursion;
	int extra_indent;
	LARGE_INTEGER profiling_time_recursive;

	// Anything that needs to be updated at the end of the command list:
	bool update_params;

	CommandListState();
	~CommandListState();
};

class CommandListCommand {
public:
	wstring ini_line;

	// For performance metrics:
	LARGE_INTEGER pre_time_spent;
	LARGE_INTEGER post_time_spent;
	unsigned pre_executions;
	unsigned post_executions;

	virtual ~CommandListCommand() {};

	virtual void run(CommandListState*) = 0;
	virtual bool optimise(HackerDevice *device) { return false; }
	virtual bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) { return false; }
};

enum class VariableFlags {
	NONE            = 0,
	GLOBAL          = 0x00000001,
	PERSIST         = 0x00000002,
	INVALID         = (signed)0xffffffff,
};
SENSIBLE_ENUM(VariableFlags);
static EnumName_t<const wchar_t *, VariableFlags> VariableFlagNames[] = {
	{L"global", VariableFlags::GLOBAL},
	{L"persist", VariableFlags::PERSIST},

	{NULL, VariableFlags::INVALID} // End of list marker
};

class CommandListVariable {
public:
	wstring name;
	// TODO: Additional types, such as hash
	float fval;
	VariableFlags flags;

	CommandListVariable(wstring name, float fval, VariableFlags flags) :
		name(name), fval(fval), flags(flags)
	{}

	void CopyStateFrom(const CommandListVariable& src)
	{
		fval = src.fval;
		flags = src.flags;
	}
};

typedef std::unordered_map<std::wstring, class CommandListVariable> CommandListVariables;
extern CommandListVariables command_list_globals;
extern std::vector<CommandListVariable*> persistent_variables;

// The scope object is used to declare local variables in a command list. The
// multiple levels are to isolate variables declared inside if blocks from
// being accessed in a parent or sibling scope, while allowing variables
// declared in a parent scope to be used in an if block. This scope object is
// only used during command list parsing - once allocated the commands and
// operands referencing these variables will point directly to the variable
// objects to avoid slow lookups at runtime, and the scope object will be
// cleared to save memory (with some refactoring we could potentially even
// remove it from the CommandList class altogether).
typedef std::forward_list<std::unordered_map<std::wstring, CommandListVariable*>> CommandListScope;

class CommandList {
public:
	// Using vector of pointers to allow mixed types, and shared_ptr to handle
	// destruction of each object:
	typedef std::vector<std::shared_ptr<CommandListCommand>> Commands;
	Commands commands;

	// For local/static variables. These are only used in the main pre
	// command list as the post command list and any sub command lists (if
	// blocks, etc) shares the same local variables and scope object as the
	// pre list. static_vars used to hold the variables in the pre command
	// list so they can be freed along with the command list, but at
	// runtime they are accessed directly by pointer. forward_list is used
	// because it doesn't invalidate pointers on insertion like vectors do.
	std::forward_list<CommandListVariable> static_vars;
	CommandListScope *scope;

	// For performance metrics:
	wstring ini_section;
	bool post;
	LARGE_INTEGER time_spent_inclusive;
	LARGE_INTEGER time_spent_exclusive;
	unsigned executions;

	void clear();

	CommandList() :
		post(false),
		scope(NULL)
	{}
};

extern std::vector<CommandList*> registered_command_lists;
extern std::unordered_set<CommandList*> command_lists_profiling;
extern std::unordered_set<CommandListCommand*> command_lists_cmd_profiling;

// Forward declaration to avoid circular reference since Override.h includes
// HackerDevice.h includes HackerContext.h includes CommandList.h
class PresetOverride;

class PresetCommand : public CommandListCommand {
public:
	PresetOverride *preset;
	bool exclude;

	PresetCommand() :
		preset(NULL),
		exclude(false)
	{}

	void run(CommandListState*) override;
};

class ExplicitCommandListSection
{
public:
	CommandList command_list;
	CommandList post_command_list;
};

typedef std::unordered_map<std::wstring, class ExplicitCommandListSection> ExplicitCommandListSections;
extern ExplicitCommandListSections explicitCommandListSections;

class RunExplicitCommandList : public CommandListCommand {
public:
	ExplicitCommandListSection *command_list_section;
	bool run_pre_and_post_together;

	RunExplicitCommandList() :
		command_list_section(NULL),
		run_pre_and_post_together(false)
	{}

	void run(CommandListState*) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};

// XXX: If this is ever used for a purpose other than ShaderRegex in the future
// make sure the unlink_command_lists_and_filter_index function won't break
// whatever it is you are using it for.
class RunLinkedCommandList : public CommandListCommand {
public:
	CommandList *link;

	RunLinkedCommandList(CommandList *link) :
		link(link)
	{}

	void run(CommandListState*) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};

// https://msdn.microsoft.com/en-us/library/windows/desktop/gg615083(v=vs.85).aspx
enum class D3DCompileFlags {
	DEBUG                              = (1 << 0),
	SKIP_VALIDATION                    = (1 << 1),
	SKIP_OPTIMIZATION                  = (1 << 2),
	PACK_MATRIX_ROW_MAJOR              = (1 << 3),
	PACK_MATRIX_COLUMN_MAJOR           = (1 << 4),
	PARTIAL_PRECISION                  = (1 << 5),
	FORCE_VS_SOFTWARE_NO_OPT           = (1 << 6),
	FORCE_PS_SOFTWARE_NO_OPT           = (1 << 7),
	NO_PRESHADER                       = (1 << 8),
	AVOID_FLOW_CONTROL                 = (1 << 9),
	PREFER_FLOW_CONTROL                = (1 << 10),
	ENABLE_STRICTNESS                  = (1 << 11),
	ENABLE_BACKWARDS_COMPATIBILITY     = (1 << 12),
	IEEE_STRICTNESS                    = (1 << 13),
	OPTIMIZATION_LEVEL0                = (1 << 14),
	OPTIMIZATION_LEVEL1                = 0,
	OPTIMIZATION_LEVEL2                = ((1 << 14) | (1 << 15)),
	OPTIMIZATION_LEVEL3                = (1 << 15),
	WARNINGS_ARE_ERRORS                = (1 << 18),
	RESOURCES_MAY_ALIAS                = (1 << 19),
	ENABLE_UNBOUNDED_DESCRIPTOR_TABLES = (1 << 20),
	ALL_RESOURCES_BOUND                = (1 << 21),
	INVALID                            = (signed)0xffffffff,
};
SENSIBLE_ENUM(D3DCompileFlags);
static EnumName_t<const wchar_t *, D3DCompileFlags> D3DCompileFlagNames[] = {
	{L"debug", D3DCompileFlags::DEBUG},
	{L"skip_validation", D3DCompileFlags::SKIP_VALIDATION},
	{L"skip_optimization", D3DCompileFlags::SKIP_OPTIMIZATION},
	{L"pack_matrix_row_major", D3DCompileFlags::PACK_MATRIX_ROW_MAJOR},
	{L"pack_matrix_column_major", D3DCompileFlags::PACK_MATRIX_COLUMN_MAJOR},
	{L"partial_precision", D3DCompileFlags::PARTIAL_PRECISION},
	{L"force_vs_software_no_opt", D3DCompileFlags::FORCE_VS_SOFTWARE_NO_OPT},
	{L"force_ps_software_no_opt", D3DCompileFlags::FORCE_PS_SOFTWARE_NO_OPT},
	{L"no_preshader", D3DCompileFlags::NO_PRESHADER},
	{L"avoid_flow_control", D3DCompileFlags::AVOID_FLOW_CONTROL},
	{L"prefer_flow_control", D3DCompileFlags::PREFER_FLOW_CONTROL},
	{L"enable_strictness", D3DCompileFlags::ENABLE_STRICTNESS},
	{L"enable_backwards_compatibility", D3DCompileFlags::ENABLE_BACKWARDS_COMPATIBILITY},
	{L"ieee_strictness", D3DCompileFlags::IEEE_STRICTNESS},
	{L"optimization_level0", D3DCompileFlags::OPTIMIZATION_LEVEL0},
	{L"optimization_level1", D3DCompileFlags::OPTIMIZATION_LEVEL1},
	{L"optimization_level2", D3DCompileFlags::OPTIMIZATION_LEVEL2},
	{L"optimization_level3", D3DCompileFlags::OPTIMIZATION_LEVEL3},
	{L"warnings_are_errors", D3DCompileFlags::WARNINGS_ARE_ERRORS},
	// d3dcompiler47 only, but they won't hurt and adding them now means we
	// can use them when we do migrate later:
	{L"resources_may_alias", D3DCompileFlags::RESOURCES_MAY_ALIAS},
	{L"enable_unbounded_descriptor_tables", D3DCompileFlags::ENABLE_UNBOUNDED_DESCRIPTOR_TABLES},
	{L"all_resources_bound", D3DCompileFlags::ALL_RESOURCES_BOUND},
	{NULL, D3DCompileFlags::INVALID} // End of list marker
};

class CustomShader
{
public:
	bool vs_override, hs_override, ds_override, gs_override, ps_override, cs_override;
	D3DCompileFlags compile_flags;
	ID3D11VertexShader *vs;
	ID3D11HullShader *hs;
	ID3D11DomainShader *ds;
	ID3D11GeometryShader *gs;
	ID3D11PixelShader *ps;
	ID3D11ComputeShader *cs;

	ID3DBlob *vs_bytecode, *hs_bytecode, *ds_bytecode;
	ID3DBlob *gs_bytecode, *ps_bytecode, *cs_bytecode;

	int blend_override;
	D3D11_BLEND_DESC blend_desc;
	D3D11_BLEND_DESC blend_mask;
	ID3D11BlendState *blend_state;
	FLOAT blend_factor[4], blend_factor_merge_mask[4];
	UINT blend_sample_mask, blend_sample_mask_merge_mask;

	int depth_stencil_override;
	D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;
	D3D11_DEPTH_STENCIL_DESC depth_stencil_mask;
	ID3D11DepthStencilState *depth_stencil_state;
	UINT stencil_ref, stencil_ref_mask;

	int rs_override;
	D3D11_RASTERIZER_DESC rs_desc;
	D3D11_RASTERIZER_DESC rs_mask;
	ID3D11RasterizerState *rs_state;

	int sampler_override;
	D3D11_SAMPLER_DESC sampler_desc;
	ID3D11SamplerState* sampler_state;

	D3D11_PRIMITIVE_TOPOLOGY topology;

	CommandList command_list;
	CommandList post_command_list;

	bool substantiated;

	int max_executions_per_frame;
	unsigned frame_no;
	int executions_this_frame;

	CustomShader();
	~CustomShader();

	bool compile(char type, wchar_t *filename, const wstring *wname, const wstring *mod_namespace);
	void substantiate(ID3D11Device *mOrigDevice);

	void merge_blend_states(ID3D11BlendState *state, FLOAT blend_factor[4], UINT sample_mask, ID3D11Device *mOrigDevice);
	void merge_depth_stencil_states(ID3D11DepthStencilState *state, UINT stencil_ref, ID3D11Device *mOrigDevice);
	void merge_rasterizer_states(ID3D11RasterizerState *state, ID3D11Device *mOrigDevice);
};

typedef std::unordered_map<std::wstring, class CustomShader> CustomShaders;
extern CustomShaders customShaders;

class RunCustomShaderCommand : public CommandListCommand {
public:
	CustomShader *custom_shader;

	RunCustomShaderCommand() :
		custom_shader(NULL)
	{}

	void run(CommandListState*) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};

class SkipCommand : public CommandListCommand {
public:
	wstring ini_section;

	SkipCommand(wstring section) :
		ini_section(section)
	{}

	void run(CommandListState*) override;
};

// Handling=abort aborts the current command list, and any command lists that
// called it. e.g. it can be used in conjunction with checktextureoverride = oD
// to abort command list execution if a specific depth target is in use.
class AbortCommand : public CommandListCommand {
public:
	wstring ini_section;

	AbortCommand(wstring section) :
		ini_section(section)
	{}

	void run(CommandListState*) override;
};


enum class CustomResourceType {
	INVALID,
	BUFFER,
	STRUCTURED_BUFFER,
	RAW_BUFFER,
	TEXTURE1D,
	TEXTURE2D,
	TEXTURE3D,
	CUBE,
};
static EnumName_t<const wchar_t *, CustomResourceType> CustomResourceTypeNames[] = {
	// Use the same names as HLSL here since they are what shaderhackers
	// will see in the shaders, even if some of these have no distinction
	// from our point of view, or are just a misc flag:
	{L"Buffer", CustomResourceType::BUFFER},
	{L"StructuredBuffer", CustomResourceType::STRUCTURED_BUFFER},
	{L"AppendStructuredBuffer", CustomResourceType::STRUCTURED_BUFFER},
	{L"ConsumeStructuredBuffer", CustomResourceType::STRUCTURED_BUFFER},
	{L"ByteAddressBuffer", CustomResourceType::RAW_BUFFER},
	{L"Texture1D", CustomResourceType::TEXTURE1D},
	{L"Texture2D", CustomResourceType::TEXTURE2D},
	{L"Texture3D", CustomResourceType::TEXTURE3D},
	{L"TextureCube", CustomResourceType::CUBE},
	// RW variants are identical to the above (it's the usage that counts):
	{L"RWBuffer", CustomResourceType::BUFFER},
	{L"RWStructuredBuffer", CustomResourceType::STRUCTURED_BUFFER},
	{L"RWByteAddressBuffer", CustomResourceType::RAW_BUFFER},
	{L"RWTexture1D", CustomResourceType::TEXTURE1D},
	{L"RWTexture2D", CustomResourceType::TEXTURE2D},
	{L"RWTexture3D", CustomResourceType::TEXTURE3D},

	{NULL, CustomResourceType::INVALID} // End of list marker
};

// The bind flags are usually set automatically, but there are cases where
// these can be used to influence driver heuristics (e.g. a buffer that
// includes a render target or UAV bind flag may be stereoised), so we allow
// them to be set manually as well. If specified these will *replace* the bind
// flags 3DMigoto sets automatically - if you use these, you presumably know
// what you are doing. This enumeration is essentially the same as
// D3D11_BIND_FLAG, but this allows us to use parse_enum_option_string.
enum class CustomResourceBindFlags {
	INVALID         = 0x00000000,
	VERTEX_BUFFER   = 0x00000001,
	INDEX_BUFFER    = 0x00000002,
	CONSTANT_BUFFER = 0x00000004,
	SHADER_RESOURCE = 0x00000008,
	STREAM_OUTPUT   = 0x00000010,
	RENDER_TARGET   = 0x00000020,
	DEPTH_STENCIL   = 0x00000040,
	UNORDERED_ACCESS= 0x00000080,
	DECODER         = 0x00000200,
	VIDEO_ENCODER   = 0x00000400,
};
SENSIBLE_ENUM(CustomResourceBindFlags);
static EnumName_t<const wchar_t *, CustomResourceBindFlags> CustomResourceBindFlagNames[] = {
	{L"vertex_buffer", CustomResourceBindFlags::VERTEX_BUFFER},
	{L"index_buffer", CustomResourceBindFlags::INDEX_BUFFER},
	{L"constant_buffer", CustomResourceBindFlags::CONSTANT_BUFFER},
	{L"shader_resource", CustomResourceBindFlags::SHADER_RESOURCE},
	{L"stream_output", CustomResourceBindFlags::STREAM_OUTPUT},
	{L"render_target", CustomResourceBindFlags::RENDER_TARGET},
	{L"depth_stencil", CustomResourceBindFlags::DEPTH_STENCIL},
	{L"unordered_access", CustomResourceBindFlags::UNORDERED_ACCESS},
	{L"decoder", CustomResourceBindFlags::DECODER},
	{L"video_encoder", CustomResourceBindFlags::VIDEO_ENCODER},
	{NULL, CustomResourceBindFlags::INVALID} // End of list marker
};

// The ResourcePool holds a pool of cached resources for when a single copy
// operation or a custom resource may be copied to from multiple distinct
// incompatible resources (e.g. they may have differing sizes). This saves us
// from having to destroy the old cache and create a new one any time the game
// switches.
//
// The hash we are using is crc32c for the moment, which I think should (though
// I have not verified) produce distinct hashes for all distinct permutations
// of resource types and descriptions. We don't explicitly introduce any
// variations for different resource types, instead relying on the fact that
// the description size of each resource type is unique - and it would be
// highly unusual (though not forbidden) to mix different resource types in a
// single pool anyway.
typedef unordered_map<uint32_t, pair<ID3D11Resource*, ID3D11Device*>> ResourcePoolCache;
class ResourcePool
{
public:
	ResourcePoolCache cache;

	~ResourcePool();

	void emplace(uint32_t hash, ID3D11Resource *resource, ID3D11Device *device);
};

class CustomResourcePool;

class CustomResource
{
public:
	wstring name;

	ID3D11Resource *resource;
	std::unique_ptr<ResourceHandleInfo> handle_info;
	ResourcePool resource_pool;
	ID3D11Device *device;
	ID3D11View *view;
	bool is_null;

	D3D11_BIND_FLAG bind_flags;
	D3D11_RESOURCE_MISC_FLAG misc_flags;

	UINT stride;
	UINT offset;
	UINT buf_size;
	DXGI_FORMAT format;

	UINT source_stride;

	int max_copies_per_frame;
	unsigned frame_no;
	int copies_this_frame;

	CustomResourcePool* pool;
	int pool_index;

	wstring filename;
	bool substantiated;

	// Used to override description when copying or synthesise resources
	// from scratch:
	CustomResourceType override_type;
	CustomResourceBindFlags override_bind_flags;
	ResourceMiscFlags override_misc_flags;
	DXGI_FORMAT override_format;
	int override_width;
	int override_height;
	int override_depth;
	int override_mips;
	int override_array;
	int override_msaa;
	int override_msaa_quality;
	int override_byte_width;
	int override_stride;

	float width_multiply;
	float height_multiply;

	void *initial_data;
	size_t initial_data_size;

	CustomResource();
	~CustomResource();

	bool AddFlags(D3D11_BIND_FLAG extra_bind_flags, D3D11_RESOURCE_MISC_FLAG extra_misc_flags, bool recursive = true);
	void InitializeHandleInfo(void* data, size_t data_size);
	void SetHandleInfo(ID3D11Resource* source, size_t offset, size_t data_size);
	ResourceHandleInfo* GetHandleInfo();
	void Substantiate(ID3D11Device *mOrigDevice, D3D11_BIND_FLAG bind_flags, D3D11_RESOURCE_MISC_FLAG misc_flags);
	void OverrideBufferDesc(D3D11_BUFFER_DESC *desc);
	void OverrideTexDesc(D3D11_TEXTURE1D_DESC *desc);
	void OverrideTexDesc(D3D11_TEXTURE2D_DESC *desc);
	void OverrideTexDesc(D3D11_TEXTURE3D_DESC *desc);
	void OverrideOutOfBandInfo(DXGI_FORMAT *format, UINT *stride);
	void ResetRuntimeState();
	void CopyMetadataFrom(const CustomResource& src);
	void expire(ID3D11Device *mOrigDevice1, ID3D11DeviceContext *mOrigContext1);

private:
	void LoadFromFile(ID3D11Device *mOrigDevice);
	void LoadBufferFromFile(ID3D11Device *mOrigDevice);
	void SubstantiateBuffer(ID3D11Device *mOrigDevice, void **buf, DWORD size);
	void SubstantiateTexture1D(ID3D11Device *mOrigDevice);
	void SubstantiateTexture2D(ID3D11Device *mOrigDevice);
	void SubstantiateTexture3D(ID3D11Device *mOrigDevice);
};

typedef std::unordered_map<std::wstring, class CustomResource> CustomResources;
extern CustomResources customResources;

enum class PoolIndexType : uint8_t {
	INVALID          = 0b00000000,

	// Direct Index Types
	RING             = 0b00000001,
	STATIC           = 0b00000010,

	// Index Table Based Types
	FIFO             = 0b00000100,
	SPATIAL          = 0b00001000,

	INDEX_TABLE_MASK = 0b00001100,
};
SENSIBLE_ENUM(PoolIndexType);

static EnumName_t<const wchar_t*, PoolIndexType> PoolIndexTypeNames[] = {
	{L"ring",    PoolIndexType::RING},
	{L"fifo",    PoolIndexType::FIFO},
	{L"static",  PoolIndexType::STATIC},
	{L"spatial", PoolIndexType::SPATIAL},
	{NULL, PoolIndexType::INVALID} // End of list marker
};

struct PoolElement
{
	enum class Type
	{
		None,
		Resource,
		Variable,
		Mixed,
	};

	Type type = Type::None;
	CustomResource* resource = nullptr;      // Lifetime managed by global resource registry `customResources`.
	CommandListVariable* variable = nullptr; // Lifetime managed by global variable registry `command_list_globals`.
};

struct PoolSlot
{
	uint32_t key = UINT32_MAX;
	unsigned last_update_frame = UINT32_MAX;
};

class CustomResourcePool
{
public:
	wstring name;

	PoolIndexType index_type = PoolIndexType::RING;
	bool lazy_initialization = true;
	bool element_type_switch_reset = true;
	unsigned keep_alive_frames = UINT32_MAX;
	bool reset_expired_elements = false;
	uint32_t spatial_radius = 0;

	CustomResource* resource_template = nullptr;
	std::unique_ptr<CommandListVariable> variable_template;

	void Initialize(size_t pool_size);
	bool PropagateFlags(D3D11_BIND_FLAG bind_flags, D3D11_RESOURCE_MISC_FLAG misc_flags);

	size_t GetElementIndex(float id, bool use_ring_index, bool is_assignment);
	CustomResource* GetResource(float id, bool template_lookup, bool use_ring_index, bool is_assignment);
	CommandListVariable* GetVariable(float id, bool template_lookup, bool use_ring_index, bool is_assignment);
	size_t GetPoolSize() const;

	void SetSourcePool(CustomResourcePool* source);
	const CustomResourcePool* ResolvePool() const;
	void CopyMetadataFrom(const CustomResourcePool& other);

	void ResetElements();
	void ResetPool(bool reset_elements = true);

private:
	void InitializeResource(size_t pool_index);
	void InitializeVariable(size_t pool_index);

	void AssignSlot(size_t slot, uint32_t key, bool is_assignment);
	void SwitchElementType(PoolElement& element, PoolElement::Type new_type);

	void ResetResource(CustomResource* custom_resource);
	void ResetVariable(CommandListVariable* variable);
	void ResetElement(size_t pool_index);

	void PostponeExpiration(PoolSlot& pool_slot, bool is_assignment);
	void ExpireElements();

	CustomResourcePool* source_pool = nullptr;

	size_t pool_size = 0;

	std::vector<PoolElement> elements;

	std::unique_ptr <FlatHashMap<uint32_t, size_t>> index_map; // O(1) lookup of key -> pool_index
	std::vector<PoolSlot> index_table; // O(1) lookup of pool_index -> key currently occupying slot

	size_t last_replacement_index = 0; // Ring pointer for FIFO eviction
	unsigned last_expiration_run = UINT32_MAX;
};

typedef std::unordered_map<std::wstring, CustomResourcePool> CustomResourcePools;
extern CustomResourcePools customResourcePools;

// Forward declaration since TextureOverride also contains a command list
struct TextureOverride;

enum class ResourceCopyTargetType : uint32_t {
	INVALID                = 0b00000000000000000000000000000000, // 0x00000000

	// D3D resources
	EMPTY                  = 0b00000000000000000000000000000001, // 0x00000001
	CONSTANT_BUFFER        = 0b00000000000000000000000000000010, // 0x00000002
	SHADER_RESOURCE        = 0b00000000000000000000000000000100, // 0x00000004
	// SAMPLER             = 0b00000000000000000000000000001000, // 0x00000008
	VERTEX_BUFFER          = 0b00000000000000000000000000010000, // 0x00000010
	INDEX_BUFFER           = 0b00000000000000000000000000100000, // 0x00000020
	STREAM_OUTPUT          = 0b00000000000000000000000001000000, // 0x00000040
	RENDER_TARGET          = 0b00000000000000000000000010000000, // 0x00000080
	DEPTH_STENCIL_TARGET   = 0b00000000000000000000000100000000, // 0x00000100
	UNORDERED_ACCESS_VIEW  = 0b00000000000000000000001000000000, // 0x00000200
	CUSTOM_RESOURCE        = 0b00000000000000000000010000000000, // 0x00000400

	D3D_RESOURCE_MASK      = 0b00000000000000000000011111111110, // 0x000007FE

	// Swap chains
	SWAP_CHAIN             = 0b00000000000000000000100000000000, // 0x00000800 - Meaning depends on whether or not upscaling has run yet this frame
	REAL_SWAP_CHAIN        = 0b00000000000000000001000000000000, // 0x00001000 - need this for upscaling used with "r_bb"
	FAKE_SWAP_CHAIN        = 0b00000000000000000010000000000000, // 0x00002000 - need this for upscaling used with "f_bb"
	
	SWAP_CHAIN_MASK        = 0b00000000000000000011100000000000, // 0x00003800

	// Special resources / pseudo-resources
	INI_PARAMS             = 0b00000000000000000100000000000000, // 0x00004000
	CURSOR_MASK            = 0b00000000000000001000000000000000, // 0x00008000
	CURSOR_COLOR           = 0b00000000000000010000000000000000, // 0x00010000
	THIS_RESOURCE          = 0b00000000000000100000000000000000, // 0x00020000
	POOL                   = 0b00000000000001000000000000000000, // 0x00040000
	CPU                    = 0b00000000000010000000000000000000, // 0x00080000
	VARIABLE               = 0b00000000000100000000000000000000, // 0x00100000

	SPECIAL_RESOURCE_MASK  = 0b00000000000111111100000000000000, // 0x001FC000

};
SENSIBLE_ENUM(ResourceCopyTargetType);
static EnumName_t<const wchar_t*, ResourceCopyTargetType> ResourceCopyTargetTypeNames[] = {
	{L"Empty", ResourceCopyTargetType::EMPTY},
	{L"ConstantBuffer", ResourceCopyTargetType::CONSTANT_BUFFER},
	{L"ShaderResource", ResourceCopyTargetType::SHADER_RESOURCE},
	{L"VertexBuffer", ResourceCopyTargetType::VERTEX_BUFFER},
	{L"IndexBuffer", ResourceCopyTargetType::INDEX_BUFFER},
	{L"StreamOutput", ResourceCopyTargetType::STREAM_OUTPUT},
	{L"RenderTarget", ResourceCopyTargetType::RENDER_TARGET},
	{L"DepthStencilTarget", ResourceCopyTargetType::DEPTH_STENCIL_TARGET},
	{L"UnorderedAccessView", ResourceCopyTargetType::UNORDERED_ACCESS_VIEW},
	{L"CustomResource", ResourceCopyTargetType::CUSTOM_RESOURCE},
	{L"IniParams", ResourceCopyTargetType::INI_PARAMS},
	{L"CursorMask", ResourceCopyTargetType::CURSOR_MASK},
	{L"CursorColor", ResourceCopyTargetType::CURSOR_COLOR},
	{L"ThisResource", ResourceCopyTargetType::THIS_RESOURCE},
	{L"Pool", ResourceCopyTargetType::POOL},
	{L"SwapChain", ResourceCopyTargetType::SWAP_CHAIN},
	{L"RealSwapChain", ResourceCopyTargetType::REAL_SWAP_CHAIN},
	{L"FakeSwapChain", ResourceCopyTargetType::FAKE_SWAP_CHAIN},
	{L"CPU", ResourceCopyTargetType::CPU},

	{NULL, ResourceCopyTargetType::INVALID} // End of list marker
};

enum class ResourceCopyTargetEvaluationMode : uint16_t {
	INVALID                = 0b0000000000000000, // 0x0000
	// RESOURCE
	RESOURCE               = 0b0000000000000001, // 0x0001
	RESOURCE_IDENTITY      = 0b0000000000000010, // 0x0002
	RESOURCE_STRIDE        = 0b0000000000000100, // 0x0004
	RESOURCE_SOURCE_STRIDE = 0b0000000000001000, // 0x0008
	RESOURCE_SIZE          = 0b0000000000010000, // 0x0010
	RESOURCE_OFFSET        = 0b0000000000100000, // 0x0020
	RESOURCE_REGION_HASH   = 0b0000000001000000, // 0x0040
	RESOURCE_SPATIAL_HASH  = 0b0000000010000000, // 0x0080
	//                       0b0000000100000000, // 0x0100

	RESOURCE_MASK          = 0b0000000111111111,

	// POOL
	POOL_IDENTITY          = 0b0000001000000000, // 0x0200
	POOL_SIZE              = 0b0000010000000000, // 0x0400
	POOL_INDEX             = 0b0000100000000000, // 0x0800
	POOL_FULL_RANGE        = 0b0001000000000000, // 0x1000
	
	POOL_MASK              = 0b0001111000000000,

	// VARIABLE
	VARIABLE               = 0b0010000000000000, // 0x2000

	// LAYOUT
	LAYOUT_ELEMENT_FORMAT  = 0b0100000000000000, // 0x4000
	LAYOUT_ELEMENT_OFFSET  = 0b1000000000000000, // 0x8000
	
	LAYOUT_MASK            = 0b1100000000000000
};
SENSIBLE_ENUM(ResourceCopyTargetEvaluationMode);
static EnumName_t<const wchar_t*, ResourceCopyTargetEvaluationMode> ResourceCopyTargetEvaluationModeNames[] = {
	{L"Resource", ResourceCopyTargetEvaluationMode::RESOURCE},
	{L"ResourceIdentity", ResourceCopyTargetEvaluationMode::RESOURCE_IDENTITY},
	{L"ResourceStride", ResourceCopyTargetEvaluationMode::RESOURCE_STRIDE},
	{L"ResourceSourceStride", ResourceCopyTargetEvaluationMode::RESOURCE_SOURCE_STRIDE},
	{L"ResourceSize", ResourceCopyTargetEvaluationMode::RESOURCE_SIZE},
	{L"ResourceOffset", ResourceCopyTargetEvaluationMode::RESOURCE_OFFSET},
	{L"ResourceRegionHash", ResourceCopyTargetEvaluationMode::RESOURCE_REGION_HASH},
	{L"ResourceSpatialHash", ResourceCopyTargetEvaluationMode::RESOURCE_SPATIAL_HASH},

	{L"PoolIdentity", ResourceCopyTargetEvaluationMode::POOL_IDENTITY},
	{L"PoolSize", ResourceCopyTargetEvaluationMode::POOL_SIZE},
	{L"PoolIndex", ResourceCopyTargetEvaluationMode::POOL_INDEX},
	{L"PoolFullRange", ResourceCopyTargetEvaluationMode::POOL_FULL_RANGE},

	{L"Variable", ResourceCopyTargetEvaluationMode::VARIABLE},

	{L"LayoutElementFormat", ResourceCopyTargetEvaluationMode::LAYOUT_ELEMENT_FORMAT},
	{L"LayoutElementOffset", ResourceCopyTargetEvaluationMode::LAYOUT_ELEMENT_OFFSET},

	{NULL, ResourceCopyTargetEvaluationMode::INVALID} // End of list marker
};

class CommandListExpression;

struct MemberArg
{
	enum class Type {
		None = 0,
		Float,
		Signed,
		Unsigned,
		String,
	};

	Type type = Type::None;

	unique_ptr<CommandListExpression> expression;

	std::wstring constant_string;

	bool ParseAs(Type parse_type, const wstring* ini_namespace, CommandListScope* scope);

	float GetValue(CommandListState* state);
	const std::wstring& GetString() const;
};

enum class IniParserResult : uint8_t {
	TOKEN_NOT_FOUND = 0,
	TOKEN_FOUND = 1,
	SYNTAX_ERROR = 2,
};

class ResourceCopyTarget {
	static constexpr size_t MAX_MEMBER_ARGS_COUNT = 4;
public:
	ResourceCopyTargetType type = ResourceCopyTargetType::INVALID;
	ResourceCopyTargetEvaluationMode evaluation_mode = ResourceCopyTargetEvaluationMode::RESOURCE;
	wchar_t shader_type = L'\0';
	unsigned slot = 0;

	CustomResourcePool* custom_resource_pool = nullptr;
	std::unique_ptr<CommandListExpression> pool_dynamic_index_expression = nullptr;

	std::array<MemberArg, MAX_MEMBER_ARGS_COUNT> member_args{};

	bool forbid_view_cache = false;

	bool ParseTarget(const wchar_t *target, bool is_source, const wstring *ini_namespace, CommandListScope* scope);

	void SetCustomResource(CustomResource* resource);

	template<typename StaticT, typename Getter>
	StaticT* GetPoolObject(StaticT* static_object, CommandListState* state, bool is_assignment, Getter getter);

	CustomResource* GetCustomResource(CommandListState* state, bool is_assignment = false);
	CommandListVariable* GetPoolVariable(CommandListState* state, bool is_assignment = false);

	ID3D11Resource *GetResource(CommandListState *state,
			ID3D11View **view,
			UINT *stride,
			UINT *offset,
			DXGI_FORMAT *format,
			UINT *buf_size,
			ResourceCopyTarget *dst=NULL);
	void SetResource(CommandListState *state,
			ID3D11Resource *res,
			ID3D11View *view,
			UINT stride,
			UINT offset,
			DXGI_FORMAT format,
			UINT buf_size);

	void FindTextureOverrides(
			CommandListState *state,
			bool *resource_found,
			TextureOverrideMatches *matches);

	float GetResourceId(CommandListState* state);
	float GetPoolId();
	float GetResourceStride(CommandListState* state);
	float GetResourceSize(CommandListState* state);
	float GetResourceOffset(CommandListState* state);
	float GetResourceRegionHash(CommandListState* state);
	float GetResourceSpatialHash(CommandListState* state);

	D3D11_BIND_FLAG BindFlags(CommandListState *state, D3D11_RESOURCE_MISC_FLAG *misc_flags=NULL);

private:
	IniParserResult ParseTargetPrefix(const wchar_t*& target, size_t& length);
	IniParserResult GetNextArgument(const wchar_t*& arg_start, const wchar_t* args_end, std::wstring& text);
	bool ParseMemberArgument(const std::wstring& text, const std::wstring* ini_namespace, CommandListScope* scope, MemberArg& arg);
	IniParserResult ParseTargetMemberArguments(const wchar_t*& target, size_t& length, const wstring* ini_namespace, CommandListScope* scope, size_t& num_args);
	IniParserResult ParseTargetMember(const wchar_t*& target, size_t& length, wstring& temp_target, const wstring* ini_namespace, CommandListScope* scope);
	IniParserResult ParseTargetPipelineSlot(const wchar_t*& target, size_t length, bool is_source);
	IniParserResult ParseTargetCustomResource(const wchar_t*& target, size_t length, const wstring* ini_namespace, CommandListScope* scope);
	IniParserResult ParseTargetPool(const wchar_t*& target, size_t length, const wstring* ini_namespace, CommandListScope* scope, bool is_source);

	CustomResource* static_custom_resource = nullptr;  // Read access should go via GetCustomResource
	CommandListVariable* static_pool_variable = nullptr;
};

enum class ResourceCopyOptions {
	INVALID         = 0,
	COPY            = 0x00000001,
	REFERENCE       = 0x00000002,
	UNLESS_NULL     = 0x00000004,
	RESOLVE_MSAA    = 0x00000008,
	MONO            = 0x00000020,
	COPY_DESC       = 0x00000080,
	SET_VIEWPORT    = 0x00000100,
	NO_VIEW_CACHE   = 0x00000200,
	RAW_VIEW        = 0x00000400,

	COPY_MASK       = 0x000000c9, // Anything that implies a copy
	COPY_TYPE_MASK  = 0x000000cb, // Anything that implies a copy or a reference
};
SENSIBLE_ENUM(ResourceCopyOptions);
static EnumName_t<wchar_t *, ResourceCopyOptions> ResourceCopyOptionNames[] = {
	{L"copy", ResourceCopyOptions::COPY},
	{L"ref", ResourceCopyOptions::REFERENCE},
	{L"reference", ResourceCopyOptions::REFERENCE},
	{L"copy_desc", ResourceCopyOptions::COPY_DESC},
	{L"copy_description", ResourceCopyOptions::COPY_DESC},
	{L"unless_null", ResourceCopyOptions::UNLESS_NULL},
	{L"mono", ResourceCopyOptions::MONO},
	{L"set_viewport", ResourceCopyOptions::SET_VIEWPORT},
	{L"no_view_cache", ResourceCopyOptions::NO_VIEW_CACHE},
	{L"raw", ResourceCopyOptions::RAW_VIEW},

	// This one currently depends on device support for resolving the
	// given texture format (D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE), and
	// currently has no fallback, so we can't rely on it - don't encourage
	// people to use it and don't document it. TODO: Implement a fallback
	// using a shader to resolve any unsupported formats.
	{L"resolve_msaa", ResourceCopyOptions::RESOLVE_MSAA},

	{NULL, ResourceCopyOptions::INVALID} // End of list marker
};
// TODO: Add support for more behaviour modifiers, here's a few ideas
// off the top of my head - I don't intend to implement all these
// unless we have a proven need for them or maybe if they are trivial
// and have real potential to be useful later. For now they are just
// food for thought:
//
// view_format= - override DXGI Format when creating a view
// if_dest_is_null - only perform the operation if the destination is not currently assigned
// if_dest_is_compatible - only perform the operation if the destination exists, and is compatible with the source
// if_dest_is_null_or_incompatible - only perform the operation if the destination is not currently assigned, or is incompatible
// copy_subresource_region=... - Use copy_subresource_region instead of copy_resource
// mip_map, array, etc. - create a view that exposes only part of the resource
// overwrite - instead of creating a new resource for a copy operation, overwrite the resource already assigned to the destination (if it exists and is compatible)


class ResourceCopyOperation : public CommandListCommand {
public:
	ResourceCopyTarget src;
	ResourceCopyTarget dst;
	ResourceCopyOptions options;

	ID3D11Resource *cached_resource;
	ResourcePool resource_pool;
	ID3D11View *cached_view;

	ResourceCopyOperation();
	~ResourceCopyOperation();

	void CopyResourceToResource(CommandListState* state, ID3D11Resource* src_resource, ID3D11View* src_view, UINT stride, UINT offset, DXGI_FORMAT format, UINT buf_src_size);
	void CopyResourceToPool(CommandListState* state, ID3D11Resource* src_resource, ID3D11View* src_view, UINT stride, UINT offset, DXGI_FORMAT format, UINT buf_src_size);

	void run(CommandListState*) override;
};

class PoolCopyOperation : public CommandListCommand {
public:
	ResourceCopyTarget src;
	ResourceCopyTarget dst;
	ResourceCopyOptions options;

	void CopyPoolToPool(CommandListState* state);

	void run(CommandListState*) override;
};

class LayoutElementOperation : public CommandListCommand {
public:
	ResourceCopyTarget dst;
	InputLayoutElementOverride override;

	LayoutElementOperation() {};
	~LayoutElementOperation() {};

	void run(CommandListState*) override;
};

class ResourceStagingOperation : public ResourceCopyOperation {
public:
	bool staging;

	ResourceStagingOperation();

	HRESULT map(CommandListState *state, D3D11_MAPPED_SUBRESOURCE *map);
	void unmap(CommandListState *state);
};

class CommandListToken {
public:
	wstring token;
	size_t token_pos;

	CommandListToken(size_t token_pos, wstring token=L"") :
		token_pos(token_pos), token(token)
	{}
	virtual ~CommandListToken() {}; // Because C++
};

// Expression nodes that are evaluatable - nodes start off as non-evaluatable
// tokens and are later transformed into evaluatable operators and operands
// that inherit from this class.
class CommandListEvaluatable {
public:
	virtual ~CommandListEvaluatable() {}; // Because C++

	virtual float evaluate(CommandListState *state, HackerDevice *device=NULL) = 0;
	virtual bool static_evaluate(float *ret, HackerDevice *device=NULL) = 0;
	virtual bool optimise(HackerDevice *device, std::shared_ptr<CommandListEvaluatable> *replacement) = 0;
};

// Indicates that this node can be used as an operand, checked when
// transforming operators to bind adjacent operands. Includes groups of syntax
// that as a whole could be an operand, such as everything between parenthesis,
// and even (transformed) operators. Essentially, everything that is or will
// eventually become evaluatable
class CommandListOperandBase {
public:
};

// Indicates this node requires finalisation - replaces syntax parsing trees
// with evaluatable operands and operators
class CommandListFinalisable {
public:
	virtual std::shared_ptr<CommandListEvaluatable> finalise() = 0;
};

// Signifies that a node contains other syntax trees that can be walked over.
// Keeping things simple by just returning a vector of contained syntax trees
// that the caller can iterate over - note that this vector will not iterate
// over all the tokens in those syntax trees, just the trees themselves.
// Alternatives are implementing our own iterator, or the visitor pattern.
class CommandListWalkable {
public:
	typedef std::vector<std::shared_ptr<CommandListWalkable>> Walk;
	virtual Walk walk() = 0;
};

class CommandListSyntaxTree :
	public CommandListToken,
	public CommandListOperandBase,
	public CommandListFinalisable,
	public CommandListWalkable {
public:
	typedef std::vector<std::shared_ptr<CommandListToken>> Tokens;
	Tokens tokens;

	CommandListSyntaxTree(size_t token_pos) :
		CommandListToken(token_pos)
	{}
	std::shared_ptr<CommandListEvaluatable> finalise() override;
	Walk walk() override;
};

// Placeholder for operator tokens from the tokenisation stage. These will all
// be transformed into proper operators later in the expression parsing, and
// there should be none left in the final tree.
class CommandListOperatorToken : public CommandListToken {
public:
	CommandListOperatorToken(size_t token_pos, wstring token=L"") :
		CommandListToken(token_pos, token)
	{}
};

// Base class for operators. Subclass this and provide a static pattern and
// concrete evaluate function to implement an operator, then use the factory
// template below to transform matching operator tokens into these.
class CommandListOperator :
	public CommandListOperatorToken,
	public CommandListEvaluatable,
	public CommandListFinalisable,
	public CommandListOperandBase,
	public CommandListWalkable {
public:
	std::shared_ptr<CommandListToken> lhs_tree;
	std::shared_ptr<CommandListToken> rhs_tree;
	std::shared_ptr<CommandListEvaluatable> lhs;
	std::shared_ptr<CommandListEvaluatable> rhs;

	CommandListOperator(
			std::shared_ptr<CommandListToken> lhs,
			CommandListOperatorToken &t,
			std::shared_ptr<CommandListToken> rhs
		) : CommandListOperatorToken(t), lhs_tree(lhs), rhs_tree(rhs)
	{}

	std::shared_ptr<CommandListEvaluatable> finalise() override;
	float evaluate(CommandListState *state, HackerDevice *device=NULL) override;
	bool static_evaluate(float *ret, HackerDevice *device=NULL) override;
	bool optimise(HackerDevice *device, std::shared_ptr<CommandListEvaluatable> *replacement) override;
	Walk walk() override;

	static const wchar_t* pattern() { return L"<IMPLEMENT ME>"; }
	virtual float evaluate(float lhs, float rhs) = 0;
};

// Abstract base factory class for defining operators. Statically instantiate
// the template below for each implemented operator.
class CommandListOperatorFactoryBase {
public:
	virtual const wchar_t* pattern() = 0;
	virtual std::shared_ptr<CommandListOperator> create(
			std::shared_ptr<CommandListToken> lhs,
			CommandListOperatorToken &t,
			std::shared_ptr<CommandListToken> rhs) = 0;
};

// Template factory class for defining operators. Statically instantiate this
// and pass it in a list to transform_operators to transform all matching
// operator tokens in the syntax tree into fully fledged operators
template <class T>
class CommandListOperatorFactory : public CommandListOperatorFactoryBase {
public:
	const wchar_t* pattern() override {
		return T::pattern();
	}

	std::shared_ptr<CommandListOperator> create(
			std::shared_ptr<CommandListToken> lhs,
			CommandListOperatorToken &t,
			std::shared_ptr<CommandListToken> rhs) override
	{
		return std::make_shared<T>(lhs, t, rhs);
	}
};

enum class ParamOverrideType {
	INVALID,
	VALUE,
	INI_PARAM,
	VARIABLE,
	RT_WIDTH,
	RT_HEIGHT,
	RES_WIDTH,
	RES_HEIGHT,
	WINDOW_WIDTH,
	WINDOW_HEIGHT,
	TEXTURE,	// Needs shader type and slot number specified in
	SHADER,		// [ShaderOverride]. [TextureOverride] sections can
			// specify filter_index=N to define the value passed in
			// here. Special values for no [TextureOverride]
			// section = 0.0, or [TextureOverride] with no
			// filter_index = 1.0
	VERTEX_COUNT,
	INDEX_COUNT,
	INSTANCE_COUNT,
	FIRST_VERTEX,
	FIRST_INDEX,
	FIRST_INSTANCE,
	THREAD_GROUP_COUNT_X,
	THREAD_GROUP_COUNT_Y,
	THREAD_GROUP_COUNT_Z,
	INDIRECT_OFFSET,
	DRAW_TYPE,
	CURSOR_VISIBLE,
	CURSOR_SCREEN_X, // Cursor in screen coordinates in pixels
	CURSOR_SCREEN_Y,
	CURSOR_WINDOW_X, // Cursor in window client area coordinates in pixels
	CURSOR_WINDOW_Y,
	CURSOR_X,        // Cursor position scaled so that client area is the range [0:1]
	CURSOR_Y,
	CURSOR_HOTSPOT_X,
	CURSOR_HOTSPOT_Y,
	TIME,
	SCISSOR_LEFT,   // May have an optional scissor rectangle index
	SCISSOR_TOP,    // specified, which is parsed in code, or not -
	SCISSOR_RIGHT,  // in which case it will match the keyword list
	SCISSOR_BOTTOM,
	HUNTING,
	FRAME_ANALYSIS,
	EFFECTIVE_DPI, // For calculating UI scaling factor on 4K+. Note not the same thing as raw DPI.
	SLI,
	STEREO_ACTIVE,
	STEREO_AVAILABLE,
};
static EnumName_t<const wchar_t *, ParamOverrideType> ParamOverrideTypeNames[] = {
	{L"rt_width", ParamOverrideType::RT_WIDTH},
	{L"rt_height", ParamOverrideType::RT_HEIGHT},
	{L"res_width", ParamOverrideType::RES_WIDTH},
	{L"res_height", ParamOverrideType::RES_HEIGHT},
	{L"window_width", ParamOverrideType::WINDOW_WIDTH},
	{L"window_height", ParamOverrideType::WINDOW_HEIGHT},
	{L"vertex_count", ParamOverrideType::VERTEX_COUNT},
	{L"index_count", ParamOverrideType::INDEX_COUNT},
	{L"instance_count", ParamOverrideType::INSTANCE_COUNT},
	{L"first_vertex", ParamOverrideType::FIRST_VERTEX},
	{L"first_index", ParamOverrideType::FIRST_INDEX},
	{L"first_instance", ParamOverrideType::FIRST_INSTANCE},
	{L"thread_group_count_x", ParamOverrideType::THREAD_GROUP_COUNT_X},
	{L"thread_group_count_y", ParamOverrideType::THREAD_GROUP_COUNT_Y},
	{L"thread_group_count_z", ParamOverrideType::THREAD_GROUP_COUNT_Z},
	{L"indirect_offset", ParamOverrideType::INDIRECT_OFFSET},
	{L"draw_type", ParamOverrideType::DRAW_TYPE},
	{L"cursor_showing", ParamOverrideType::CURSOR_VISIBLE},
	{L"cursor_screen_x", ParamOverrideType::CURSOR_SCREEN_X},
	{L"cursor_screen_y", ParamOverrideType::CURSOR_SCREEN_Y},
	{L"cursor_window_x", ParamOverrideType::CURSOR_WINDOW_X},
	{L"cursor_window_y", ParamOverrideType::CURSOR_WINDOW_Y},
	{L"cursor_x", ParamOverrideType::CURSOR_X},
	{L"cursor_y", ParamOverrideType::CURSOR_Y},
	{L"cursor_hotspot_x", ParamOverrideType::CURSOR_HOTSPOT_X},
	{L"cursor_hotspot_y", ParamOverrideType::CURSOR_HOTSPOT_Y},
	{L"time", ParamOverrideType::TIME},
	{L"scissor_left", ParamOverrideType::SCISSOR_LEFT},
	{L"scissor_top", ParamOverrideType::SCISSOR_TOP},
	{L"scissor_right", ParamOverrideType::SCISSOR_RIGHT},
	{L"scissor_bottom", ParamOverrideType::SCISSOR_BOTTOM},
	{L"hunting", ParamOverrideType::HUNTING},
	{L"frame_analysis", ParamOverrideType::FRAME_ANALYSIS},
	{L"effective_dpi", ParamOverrideType::EFFECTIVE_DPI},
	{L"sli", ParamOverrideType::SLI},
	{L"stereo_active", ParamOverrideType::STEREO_ACTIVE},
	{L"stereo_available", ParamOverrideType::STEREO_AVAILABLE},
	{NULL, ParamOverrideType::INVALID} // End of list marker
};
class CommandListOperand :
	public CommandListToken,
	public CommandListOperandBase,
	public CommandListEvaluatable {
	float process_texture_filter(CommandListState*);
	float process_shader_filter(CommandListState*);
public:
	// TODO: Break up into separate classes for each operand type
	ParamOverrideType type;
	float val;

	// For INI_PARAM type:
	float DirectX::XMFLOAT4::*param_component;
	int param_idx;

	// For VARIABLE type:
	float *var_ftarget;

	// For texture filters:
	ResourceCopyTarget texture_filter_target;
	wchar_t shader_filter_target;

	// For scissor rectangle:
	unsigned scissor;

	CommandListOperand(size_t pos, wstring token=L"") :
		CommandListToken(pos, token),
		type(ParamOverrideType::INVALID),
		val(FLT_MAX),
		param_component(NULL),
		param_idx(0),
		var_ftarget(NULL),
		scissor(0)
	{}

	bool parse(const wstring *operand, const wstring *ini_namespace, CommandListScope *scope);
	float evaluate(CommandListState *state, HackerDevice *device=NULL) override;
	bool static_evaluate(float *ret, HackerDevice *device=NULL) override;
	bool optimise(HackerDevice *device, std::shared_ptr<CommandListEvaluatable> *replacement) override;
};

class CommandListExpression {
public:
	std::shared_ptr<CommandListEvaluatable> evaluatable;

	bool parse(const wstring *expression, const wstring *ini_namespace, CommandListScope *scope);
	float evaluate(CommandListState *state, HackerDevice *device=NULL);
	bool static_evaluate(float *ret, HackerDevice *device=NULL);
	bool optimise(HackerDevice *device);
};

class AssignmentCommand : public CommandListCommand {
public:
	CommandListExpression expression;

	bool optimise(HackerDevice *device) override;
};

class ParamOverride : public AssignmentCommand {
public:
	int param_idx;
	float DirectX::XMFLOAT4::*param_component;

	ParamOverride() :
		param_idx(-1),
		param_component(NULL)
	{}

	void run(CommandListState*) override;
};

class VariableAssignment : public AssignmentCommand {
public:
	CommandListVariable *var;

	VariableAssignment() :
		var(NULL)
	{}

	void run(CommandListState*) override;
};

class PoolVariableOperation : public AssignmentCommand {
public:
	ResourceCopyTarget dst;

	void SetVariableValue(CommandListState* state, CommandListVariable* dst, float value);
	void SetAllPoolVariables(CommandListState* state, float value);

	bool optimise(HackerDevice* device) override;
	void run(CommandListState*) override;
};

class IfCommand : public CommandListCommand {
public:
	CommandListExpression expression;
	bool pre_finalised, post_finalised;
	bool has_nested_else_if;
	wstring section;

	// Commands cannot statically contain command lists, because the
	// command may be optimised out and the command list freed while we are
	// still in the middle of optimisations. These are dynamically
	// allocated, and also stored in a second data structure until
	// optimisations are complete
	std::shared_ptr<CommandList> true_commands_pre;
	std::shared_ptr<CommandList> true_commands_post;
	std::shared_ptr<CommandList> false_commands_pre;
	std::shared_ptr<CommandList> false_commands_post;

	IfCommand(const wchar_t *section);

	void run(CommandListState*) override;
	bool optimise(HackerDevice *device) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};

class ElseIfCommand : public IfCommand {
public:
	ElseIfCommand(const wchar_t *section) :
		IfCommand(section)
	{}
};

class CommandPlaceholder : public CommandListCommand {
public:
	void run(CommandListState*) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};
class ElsePlaceholder : public CommandPlaceholder {
};

class CheckTextureOverrideCommand : public CommandListCommand {
public:
	// For processing command lists in TextureOverride sections:
	ResourceCopyTarget target;
	bool run_pre_and_post_together;

	CheckTextureOverrideCommand() :
		run_pre_and_post_together(false)
	{}

	void run(CommandListState*) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};

enum class DrawCommandType {
	INVALID,
	DRAW,
	DRAW_AUTO,
	DRAW_INDEXED,
	DRAW_INDEXED_INSTANCED,
	DRAW_INDEXED_INSTANCED_INDIRECT,
	DRAW_INSTANCED,
	DRAW_INSTANCED_INDIRECT,
	DISPATCH,
	DISPATCH_INDIRECT,

	// 3DMigoto special draw commands:
	FROM_CALLER,
	AUTO_VERTEX_COUNT,
	AUTO_INDEX_COUNT,
	AUTO_INDEX_INSTANCE_COUNT,
};

class DrawCommand : public CommandListCommand {
public:
	wstring ini_section;
	DrawCommandType type;

	CommandListExpression args[5];
	ResourceCopyTarget indirect_buffer;

	DrawCommand::DrawCommand() :
		type(DrawCommandType::INVALID)
	{}

	void do_indirect_draw_call(CommandListState *state, char *name,
		void (__stdcall ID3D11DeviceContext::*IndirectDrawCall)(THIS_
		ID3D11Buffer *pBufferForArgs,
		UINT AlignedByteOffsetForArgs));
	inline void eval_args(int nargs, INT result[5], CommandListState *state);
	void run(CommandListState*) override;
};

class StoreCommand : public CommandListCommand {
public:
	CommandListVariable* var;
	ResourceCopyTarget src;
	ResourceCopyOptions options;
	int loc;

	wstring ini_section;

	StoreCommand() :
		var(NULL)
	{}

	void run(CommandListState*) override;
};

class ClearViewCommand : public CommandListCommand {
public:
	ResourceCopyTarget target;

	FLOAT dsv_depth;
	UINT8 dsv_stencil;

	// If neither "depth" or "stencil" are specified, both will be used:
	bool clear_depth;
	bool clear_stencil;

	// fval is used for RTV colours and UAVs when clearing them with
	// floating point values. uval is used for UAVs if nothing looked like
	// a float.
	FLOAT fval[4];
	UINT uval[4];
	bool clear_uav_uint;

	ClearViewCommand();

	ID3D11View* create_best_view(ID3D11Resource *resource,
		CommandListState *state, UINT stride,
		UINT offset, DXGI_FORMAT format, UINT buf_src_size);
	void clear_unknown_view(ID3D11View*, CommandListState *state);

	void run(CommandListState*) override;
};

class ResetPerFrameLimitsCommand : public CommandListCommand {
public:
	CustomShader *shader;
	CustomResource *resource;

	ResetPerFrameLimitsCommand() :
		shader(NULL),
		resource(NULL)
	{}

	void run(CommandListState*) override;
};

class FrameAnalysisChangeOptionsCommand : public CommandListCommand {
public:
	FrameAnalysisOptions analyse_options;

	FrameAnalysisChangeOptionsCommand(wstring *val);

	void run(CommandListState*) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};

class FrameAnalysisDumpCommand : public CommandListCommand {
public:
	ResourceCopyTarget target;
	wstring target_name;
	FrameAnalysisOptions analyse_options;

	void run(CommandListState*) override;
	bool noop(bool post, bool ignore_cto_pre, bool ignore_cto_post) override;
};

class UpscalingFlipBBCommand : public CommandListCommand {
public:
	wstring ini_section;

	UpscalingFlipBBCommand(wstring section);
	~UpscalingFlipBBCommand();

	void run(CommandListState*) override;
};

class Draw3DMigotoOverlayCommand : public CommandListCommand {
public:
	wstring ini_section;

	Draw3DMigotoOverlayCommand(const wchar_t *section) :
		ini_section(section)
	{}

	void run(CommandListState*) override;
};

void RunCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list, DrawCallInfo *call_info,
		bool post);
void RunResourceCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list, ID3D11Resource **resource,
		bool post);
void RunViewCommandList(HackerDevice *mHackerDevice,
		HackerContext *mHackerContext,
		CommandList *command_list, ID3D11View *view,
		bool post);

bool ParseRunExplicitCommandList(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list,
		CommandList *post_command_list,
		const wstring *ini_namespace);

bool ParseCommandListGeneralCommands(const wchar_t *section,
		const wchar_t *key, wstring *val,
		CommandList *explicit_command_list,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace);
bool ParseCommandListIniParamOverride(const wchar_t *section,
		const wchar_t *key, wstring *val, CommandList *command_list,
		const wstring *ini_namespace);
bool ParseCommandListVariableAssignment(const wchar_t *section,
		const wchar_t *key, wstring *val, const wstring *raw_line,
		CommandList *command_list, CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace);
bool ParseCommandListResourceCopyTargetDirective(const wchar_t *section,
		const wchar_t *key, wstring *val, CommandList *command_list,
		const wstring *ini_namespace);
bool ParseCommandListFlowControl(const wchar_t *section, const wstring *line,
		CommandList *pre_command_list, CommandList *post_command_list,
		const wstring *ini_namespace);
std::shared_ptr<RunLinkedCommandList>
		LinkCommandLists(CommandList *dst, CommandList *link, const wstring *ini_line);
void optimise_command_lists(HackerDevice *device);
bool parse_command_list_var_name(const wstring &name, const wstring *ini_namespace, CommandListVariable **target);
bool valid_variable_name(const wstring &name);
