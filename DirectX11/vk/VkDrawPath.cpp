#include "VkDrawPath.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "VkDump.h"
#include "VkScope.h"

#include "../dx12/Bridge.h"
#include "../dx12/DrawPath.h"
#include "../dx12/ShimObjects.h"
#include "util.h"

#include "../globals.h"
#include "../HackerContext.h"
#include "../ResourceHash.h"
#include "../ShaderRegex.h"

bool vk_mods_active;

// Whether any draw could be of interest at all. Read without a lock by the
// threads recording, like wanted_generation, which changes when what was
// worked out for a pipeline no longer holds.
static volatile bool draw_path_active;
static volatile LONG wanted_generation = 1;

// Why a draw was stopped for, see draw_is_boring
enum SlowReason {
	SLOW_SETTINGS,
	SLOW_LISTS,
	SLOW_INDEX_COUNT,
	SLOW_REASONS
};

// Since the last report, with the lock of dx12/Bridge.h held
static struct {
	unsigned draws, skipped, mod_draws;
	unsigned slow[SLOW_REASONS];
	unsigned regions_hashed, regions_checked, regions_unmapped;
	UINT64 wait_ticks, engine_ticks;
} stats;

static UINT64 ticks()
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

// -----------------------------------------------------------------------------
// Shaders and pipelines

static int stage_of(VkShaderStageFlagBits stage)
{
	switch (stage) {
		case VK_SHADER_STAGE_VERTEX_BIT: return DX12_STAGE_VS;
		case VK_SHADER_STAGE_FRAGMENT_BIT: return DX12_STAGE_PS;
		case VK_SHADER_STAGE_GEOMETRY_BIT: return DX12_STAGE_GS;
		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return DX12_STAGE_HS;
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return DX12_STAGE_DS;
		case VK_SHADER_STAGE_COMPUTE_BIT: return DX12_STAGE_CS;
	}
	return -1;
}

struct Pipeline {
	// By Dx12ShaderStage, 0 where it has none
	UINT64 shaders[DX12_STAGE_COUNT];
	// Whether a draw with it is the engine's business, as of the
	// wanted_generation this was worked out for
	volatile LONG generation;
	volatile bool wanted;
	// What its command lists would do, see Dx12WorkOutFilter
	Dx12DrawFilter filter;
};

struct SeenShader {
	UINT64 hash;
	int stage;
};

static SRWLOCK registry_lock = SRWLOCK_INIT;
static std::unordered_map<VkShaderModule, UINT64> shader_modules;
static std::unordered_map<VkPipeline, Pipeline*> pipelines;
// A command buffer may be left pointing at the pipeline it had bound when
// that is destroyed, so these are never freed, only used again
static std::vector<Pipeline*> spare_pipelines;
// Every shader a pipeline was created with, in the order they turned up, and
// the stages each was seen for
static std::vector<SeenShader> seen_shaders;
static std::unordered_map<UINT64, UINT> seen_stages;

// The hashes of the [ShaderOverride] sections, as in dx12/DrawPath
static std::unordered_set<UINT64> wanted_hashes;
static SRWLOCK wanted_lock = SRWLOCK_INIT;
static unsigned seen_override_generation;
// With hunting enabled every draw is the engine's business
static volatile bool want_all;
// How many of seen_shaders the ShaderRegex sections have been linked to
static size_t linked_shaders;

void VkModsShaderModule(VkShaderModule module, const VkShaderModuleCreateInfo *info)
{
	UINT64 hash;

	if (!info || !info->pCode || !info->codeSize)
		return;
	hash = fnv_64_buf(info->pCode, info->codeSize);

	AcquireSRWLockExclusive(&registry_lock);
	shader_modules[module] = hash;
	ReleaseSRWLockExclusive(&registry_lock);
}

void VkModsForgetShaderModule(VkShaderModule module)
{
	AcquireSRWLockExclusive(&registry_lock);
	shader_modules.erase(module);
	ReleaseSRWLockExclusive(&registry_lock);
}

static const void* find_in_chain(const void *chain, VkStructureType type)
{
	const VkBaseInStructure *next = (const VkBaseInStructure*)chain;

	while (next && next->sType != type)
		next = next->pNext;
	return next;
}

void VkModsGraphicsPipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo *info)
{
	const VkGraphicsPipelineLibraryCreateInfoEXT *library = (const VkGraphicsPipelineLibraryCreateInfoEXT*)
		find_in_chain(info->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
	// A part of a pipeline only has to have valid pointers for what it is of
	bool has_shaders = !library || (library->flags & (VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
		| VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT));
	UINT64 shaders[DX12_STAGE_COUNT] = {};
	Pipeline *entry;
	uint32_t i;
	int stage;

	AcquireSRWLockExclusive(&registry_lock);

	for (i = 0; has_shaders && info->pStages && i < info->stageCount; i++) {
		const VkPipelineShaderStageCreateInfo *created = &info->pStages[i];
		// Without a module the SPIR-V may come with the stage instead
		const VkShaderModuleCreateInfo *code = (const VkShaderModuleCreateInfo*)
			find_in_chain(created->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);

		stage = stage_of(created->stage);
		if (stage < 0)
			continue;

		if (created->module) {
			auto module = shader_modules.find(created->module);
			if (module != shader_modules.end())
				shaders[stage] = module->second;
		} else if (code && code->pCode && code->codeSize) {
			shaders[stage] = fnv_64_buf(code->pCode, code->codeSize);
		}

		if (shaders[stage]) {
			UINT *stages = &seen_stages[shaders[stage]];

			if (!(*stages & 1u << stage)) {
				*stages |= 1u << stage;
				seen_shaders.push_back({ shaders[stage], stage });
			}
		}
	}

	if (spare_pipelines.empty()) {
		entry = new Pipeline();
	} else {
		entry = spare_pipelines.back();
		spare_pipelines.pop_back();
	}
	memcpy(entry->shaders, shaders, sizeof(shaders));
	InterlockedExchange(&entry->filter.generation, 0);
	entry->generation = 0;
	entry->wanted = false;
	pipelines[pipeline] = entry;

	ReleaseSRWLockExclusive(&registry_lock);
}

void VkModsForgetPipeline(VkPipeline pipeline)
{
	AcquireSRWLockExclusive(&registry_lock);
	auto found = pipelines.find(pipeline);
	if (found != pipelines.end()) {
		spare_pipelines.push_back(found->second);
		pipelines.erase(found);
	}
	ReleaseSRWLockExclusive(&registry_lock);
}

static bool pipeline_wanted(Pipeline *pipeline)
{
	LONG generation = wanted_generation;
	bool wanted;
	int stage;

	if (pipeline->generation == generation)
		return pipeline->wanted;

	wanted = want_all;
	if (!wanted) {
		AcquireSRWLockShared(&wanted_lock);
		for (stage = 0; stage < DX12_STAGE_COUNT; stage++)
			wanted |= pipeline->shaders[stage] && wanted_hashes.count(pipeline->shaders[stage]);
		ReleaseSRWLockShared(&wanted_lock);
	}

	// Whoever reads the generation finds the answer that goes with it
	pipeline->wanted = wanted;
	MemoryBarrier();
	pipeline->generation = generation;
	return wanted;
}

// -----------------------------------------------------------------------------
// Buffers, and the memory behind them

struct Buffer {
	VkDeviceSize size;
	VkDeviceMemory memory;
	VkDeviceSize memory_offset;
};

struct Mapping {
	const BYTE *data;
	VkDeviceSize offset, size;
};

// Held while mapped memory is read, which keeps it from being unmapped
static SRWLOCK memory_lock = SRWLOCK_INIT;
static std::unordered_map<VkBuffer, Buffer> buffers;
static std::unordered_map<VkDeviceMemory, Mapping> mappings;

// A game with its meshes in a few large buffers writes those through memory
// it keeps mapped, and nothing tells when it does: the place of a mesh that
// was unloaded is given to another. So a region that was hashed is looked at
// again once every frame it is asked about, by a few bytes from across it
// that are compared with what was there when it was hashed, and hashed again
// if those differ. Every so many frames it is hashed again regardless, for
// the change those bytes would not show.
const UINT REGION_SAMPLES = 4;
const UINT REGION_SAMPLE_BYTES = 16;
const UINT REGION_REHASH_FRAMES = 600;

struct Region {
	uint32_t hash;
	bool hashed;
	UINT hashed_frame, checked_frame;
	BYTE samples[REGION_SAMPLES * REGION_SAMPLE_BYTES];
};

// A buffer of the game as the engine is given it
struct PublishedBuffer {
	ShimBuffer *object;
	// By offset and size
	std::map<std::pair<VkDeviceSize, UINT>, Region> regions;
};

// With the lock of dx12/Bridge.h held
static std::unordered_map<VkBuffer, PublishedBuffer> published;
// The buffers destroyed since anything was published, which may be on
// whatever thread. With memory_lock held.
static std::vector<VkBuffer> dead_buffers;
static volatile bool any_published;

void VkModsBufferCreated(VkBuffer buffer, const VkBufferCreateInfo *info)
{
	Buffer created = {};

	created.size = info->size;

	AcquireSRWLockExclusive(&memory_lock);
	buffers[buffer] = created;
	ReleaseSRWLockExclusive(&memory_lock);
}

void VkModsBufferBound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
	AcquireSRWLockExclusive(&memory_lock);
	auto found = buffers.find(buffer);
	if (found != buffers.end()) {
		found->second.memory = memory;
		found->second.memory_offset = offset;
	}
	ReleaseSRWLockExclusive(&memory_lock);
}

void VkModsForgetBuffer(VkBuffer buffer)
{
	AcquireSRWLockExclusive(&memory_lock);
	buffers.erase(buffer);
	if (any_published)
		dead_buffers.push_back(buffer);
	ReleaseSRWLockExclusive(&memory_lock);
}

void VkModsMemoryMapped(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, void *data)
{
	Mapping mapping = { (const BYTE*)data, offset, size };

	AcquireSRWLockExclusive(&memory_lock);
	mappings[memory] = mapping;
	ReleaseSRWLockExclusive(&memory_lock);
}

void VkModsMemoryUnmapped(VkDeviceMemory memory)
{
	AcquireSRWLockExclusive(&memory_lock);
	mappings.erase(memory);
	ReleaseSRWLockExclusive(&memory_lock);
}

// With the lock of dx12/Bridge.h held, before anything is looked up in
// published: the handle of a buffer that is gone may be another's by now
static void forget_dead_buffers()
{
	std::vector<VkBuffer> dead;

	if (!any_published)
		return;

	AcquireSRWLockExclusive(&memory_lock);
	dead.swap(dead_buffers);
	ReleaseSRWLockExclusive(&memory_lock);

	for (VkBuffer buffer : dead) {
		auto found = published.find(buffer);
		if (found == published.end())
			continue;
		found->second.object->Release();
		published.erase(found);
	}
}

// With memory_lock held. NULL if the game does not have the bytes mapped.
static const BYTE* mapped_bytes(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize bytes)
{
	VkDeviceSize start;

	auto info = buffers.find(buffer);
	if (info == buffers.end() || !info->second.memory || offset + bytes > info->second.size)
		return NULL;

	auto mapping = mappings.find(info->second.memory);
	start = info->second.memory_offset + offset;
	if (mapping == mappings.end() || !mapping->second.data || start < mapping->second.offset
	    || (mapping->second.size != VK_WHOLE_SIZE && start + bytes > mapping->second.offset + mapping->second.size))
		return NULL;

	return mapping->second.data + (start - mapping->second.offset);
}

static void take_samples(const BYTE *data, UINT bytes, BYTE *samples)
{
	UINT i;

	memset(samples, 0, REGION_SAMPLES * REGION_SAMPLE_BYTES);
	if (bytes <= REGION_SAMPLE_BYTES) {
		memcpy(samples, data, bytes);
		return;
	}
	for (i = 0; i < REGION_SAMPLES; i++) {
		memcpy(samples + i * REGION_SAMPLE_BYTES,
			data + (UINT64)(bytes - REGION_SAMPLE_BYTES) * i / (REGION_SAMPLES - 1), REGION_SAMPLE_BYTES);
	}
}

// 0 if there is no telling
static uint32_t region_hash(VkBuffer buffer, PublishedBuffer *entry, VkDeviceSize offset, UINT bytes)
{
	Region *region = &entry->regions[std::make_pair(offset, bytes)];
	BYTE samples[sizeof(region->samples)];
	UINT frame = vk_frame;
	const BYTE *data;

	if (region->hashed && region->checked_frame == frame)
		return region->hash;

	AcquireSRWLockShared(&memory_lock);

	data = mapped_bytes(buffer, offset, bytes);
	if (!data) {
		ReleaseSRWLockShared(&memory_lock);
		stats.regions_unmapped++;
		return 0;
	}

	take_samples(data, bytes, samples);
	if (!region->hashed || memcmp(samples, region->samples, sizeof(samples))
	    || frame - region->hashed_frame >= REGION_REHASH_FRAMES) {
		region->hash = Dx12Crc32cWriteCombined(0, data, bytes);
		region->hashed = true;
		// Not all of them in the same frame
		region->hashed_frame = frame - (UINT)(offset >> 4) % (REGION_REHASH_FRAMES / 4);
		memcpy(region->samples, samples, sizeof(samples));
		stats.regions_hashed++;
	} else {
		stats.regions_checked++;
	}

	ReleaseSRWLockShared(&memory_lock);

	region->checked_frame = frame;
	return region->hash;
}

static PublishedBuffer* publish_buffer(VkBuffer buffer, UINT bind_flags)
{
	VkDeviceSize size = 0;
	ShimBuffer *object;

	auto found = published.find(buffer);
	if (found != published.end())
		return &found->second;

	AcquireSRWLockShared(&memory_lock);
	auto info = buffers.find(buffer);
	if (info != buffers.end())
		size = info->second.size;
	any_published = true;
	ReleaseSRWLockShared(&memory_lock);
	if (!size)
		return NULL;

	object = new ShimBuffer();
	// No resource: there is no D3D12 one, and nothing to read it by
	object->r.dimension = D3D11_RESOURCE_DIMENSION_BUFFER;
	object->r.desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	object->r.desc.Width = size;
	object->r.foreign = true;
	object->mDesc = {};
	object->mDesc.ByteWidth = (UINT)min(size, (VkDeviceSize)UINT_MAX);
	object->mDesc.BindFlags = bind_flags;
	// Has the engine's record of the object removed along with it
	new ResourceReleaseTracker(object);

	// None to match by: the buffer as a whole is not what a mesh is
	EnterCriticalSectionPretty(&G->mResourcesLock);
	ResourceHandleInfo *handle = &G->mResources[object];
	handle->type = D3D11_RESOURCE_DIMENSION_BUFFER;
	handle->hash = 0;
	handle->orig_hash = 0;
	handle->data_hash = 0;
	handle->texture_override_candidates_valid = false;
	LeaveCriticalSection(&G->mResourcesLock);

	PublishedBuffer *entry = &published[buffer];
	entry->object = object;
	return entry;
}

// -----------------------------------------------------------------------------
// What is set on a command buffer

struct CmdState {
	Pipeline *pipeline;
	VkBuffer ib;
	VkDeviceSize ib_offset;
	VkIndexType index_type;
};

// One for every handle there has been, which is what lets a thread remember
// where the one it is recording to is
static SRWLOCK cmd_lock = SRWLOCK_INIT;
static std::unordered_map<VkCommandBuffer, CmdState*> cmd_states;

static CmdState* cmd_state(VkCommandBuffer cmd)
{
	static thread_local VkCommandBuffer last_cmd;
	static thread_local CmdState *last_state;
	CmdState *state = NULL;

	if (last_cmd == cmd && last_state)
		return last_state;

	AcquireSRWLockShared(&cmd_lock);
	auto found = cmd_states.find(cmd);
	if (found != cmd_states.end())
		state = found->second;
	ReleaseSRWLockShared(&cmd_lock);

	if (!state) {
		AcquireSRWLockExclusive(&cmd_lock);
		CmdState **slot = &cmd_states[cmd];
		if (!*slot)
			*slot = new CmdState();
		state = *slot;
		ReleaseSRWLockExclusive(&cmd_lock);
	}

	last_cmd = cmd;
	last_state = state;
	return state;
}

void VkModsBeginCommands(VkCommandBuffer cmd)
{
	*cmd_state(cmd) = {};
}

void VkModsBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
	Pipeline *entry = NULL;

	if (bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return;

	AcquireSRWLockShared(&registry_lock);
	auto found = pipelines.find(pipeline);
	if (found != pipelines.end())
		entry = found->second;
	ReleaseSRWLockShared(&registry_lock);

	cmd_state(cmd)->pipeline = entry;
}

void VkModsBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
	CmdState *state = cmd_state(cmd);

	state->ib = buffer;
	state->ib_offset = offset;
	state->index_type = type;
}

// -----------------------------------------------------------------------------
// The draws not worth stopping for
//
// A mod that goes by meshes has a command list run for every draw that asks
// for the TextureOverride section of its index buffer, which few have. The
// threads recording find that out themselves where it is certain, as in
// dx12/DrawPath: the command lists of the shaders can do nothing but ask, and
// with track_region_hashes no section is looked for by the hash of the
// indices unless it has a match_index_count that is the draw's.

// A bit for every match_index_count there is, by a hash of it: a draw whose
// bit is not set has no section. Two of them, for the threads recording to
// read one while the other is filled in.
const UINT INDEX_COUNT_BITS = 1 << 16;
static volatile LONG64 index_count_bits[2][INDEX_COUNT_BITS / 64];
static volatile LONG64 *volatile wanted_index_counts;
// Sections that match by the description of a resource may match any buffer
static volatile bool fuzzy_sections;

static UINT index_count_bit(UINT count)
{
	return (count * 0x9e3779b1u) >> 16;
}

// With the lock of dx12/Bridge.h held
static void collect_index_counts()
{
	volatile LONG64 *bits = index_count_bits[wanted_index_counts == index_count_bits[0]];
	UINT bit;

	memset((void*)bits, 0, sizeof(index_count_bits[0]));
	for (const auto &entry : G->mTextureOverrideDrawIndexMap) {
		bit = index_count_bit(entry.first);
		bits[bit / 64] |= 1ll << (bit % 64);
	}
	fuzzy_sections = !G->mFuzzyTextureOverrides.empty();
	MemoryBarrier();
	wanted_index_counts = bits;
}

static volatile LONG fast_draws;

// Without the lock. True only if the engine would look at the draw and leave
// it at that.
static bool draw_is_boring(Pipeline *pipeline, bool indexed, uint32_t count, SlowReason *reason)
{
	volatile LONG64 *bits = wanted_index_counts;
	Dx12DrawFilter filter;
	UINT bit;

	// A frame being analysed has the dumps of the command lists do
	// something, which the filter takes them not to
	*reason = SLOW_SETTINGS;
	if (want_all || G->analyse_frame || !bits)
		return false;

	*reason = SLOW_LISTS;
	if (!Dx12FilterOnlyChecks(&pipeline->filter, &filter))
		return false;

	// The index buffer is all of the game's the engine is given yet: what
	// else the lists ask for they find nothing bound for
	if (!filter.ib || !indexed)
		return true;

	*reason = SLOW_INDEX_COUNT;
	if (fuzzy_sections)
		return false;
	if (!G->track_region_hashes)
		return true;
	bit = index_count_bit(count);
	return !(bits[bit / 64] & 1ll << (bit % 64));
}

// -----------------------------------------------------------------------------
// The engine, for a draw of the game

struct Scope {
	CmdState state;
	bool indexed;
	// The index buffer once the engine has asked for it
	PublishedBuffer *ib;
};

static Scope *scope;

bool VkInDrawScope()
{
	return !!scope;
}

bool VkScopeHasShader(int stage)
{
	return !!scope->state.pipeline->shaders[stage];
}

ID3D11Buffer* VkScopeIndexBuffer(DXGI_FORMAT *format)
{
	if (!scope->indexed || !scope->state.ib)
		return NULL;
	if (scope->state.index_type != VK_INDEX_TYPE_UINT16 && scope->state.index_type != VK_INDEX_TYPE_UINT32)
		return NULL;

	if (!scope->ib)
		scope->ib = publish_buffer(scope->state.ib, D3D11_BIND_INDEX_BUFFER);
	if (!scope->ib)
		return NULL;

	*format = scope->state.index_type == VK_INDEX_TYPE_UINT16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
	return scope->ib->object;
}

void VkScopeDraw()
{
	static bool reported;

	stats.mod_draws++;
	if (reported)
		return;
	reported = true;
	VkDumpNote("*** A mod drew for a draw of the game, which is not possible on Vulkan yet. Ignored.\n");
}

bool VkScopeRegionHash(ID3D11Buffer *buffer, UINT offset, UINT size, uint32_t *hash)
{
	if (!scope || !scope->ib || buffer != scope->ib->object)
		return false;

	*hash = region_hash(scope->state.ib, scope->ib, scope->state.ib_offset + offset, size);
	return true;
}

bool VkModsDraw(VkCommandBuffer cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
		int32_t vertex_offset, uint32_t first_instance, PFN_vkCmdDraw draw, PFN_vkCmdDrawIndexed draw_indexed)
{
	static Scope opened;
	bool timed = G->vk_dump_hashes;
	static thread_local LONG uncounted;
	UINT64 times[3] = {};
	CmdState *state;
	SlowReason reason;

	if (!draw_path_active)
		return false;
	state = cmd_state(cmd);
	if (!state->pipeline || !pipeline_wanted(state->pipeline))
		return false;
	if (draw_is_boring(state->pipeline, indexed, count, &reason)) {
		if (timed && ++uncounted == 256) {
			InterlockedAdd(&fast_draws, uncounted);
			uncounted = 0;
		}
		return false;
	}

	if (timed)
		times[0] = ticks();
	Dx12ShimLock();
	if (!dx12_shim || dx12_mods_disabled || Dx12InDrawScope()) {
		Dx12ShimUnlock();
		return false;
	}
	if (timed)
		times[1] = ticks();

	forget_dead_buffers();
	Dx12WorkOutFilter(&state->pipeline->filter, state->pipeline->shaders);
	stats.slow[reason]++;

	opened.state = *state;
	opened.indexed = indexed;
	opened.ib = NULL;
	scope = &opened;
	Dx12OpenForeignScope();

	// Vulkan has the one call whether there are instances or not, which is
	// the instanced one of D3D11
	DrawContext c = indexed
		? DrawContext(DrawCall::DrawIndexedInstanced, 0, count, instances, vertex_offset, first, first_instance, NULL, 0)
		: DrawContext(DrawCall::DrawInstanced, count, 0, instances, first, 0, first_instance, NULL, 0);

	dx12_shim->hacker_context->Dx12BeforeDraw(c, opened.state.pipeline->shaders);
	stats.draws++;
	stats.skipped += c.call_info.skip;
	if (!c.call_info.skip) {
		if (indexed)
			draw_indexed(cmd, count, instances, first, vertex_offset, first_instance);
		else
			draw(cmd, count, instances, first, first_instance);
	}
	dx12_shim->hacker_context->Dx12AfterDraw(c);

	scope = NULL;
	Dx12CloseForeignScope();

	if (timed) {
		times[2] = ticks();
		stats.wait_ticks += times[1] - times[0];
		stats.engine_ticks += times[2] - times[1];
	}
	Dx12ShimUnlock();
	return true;
}

// -----------------------------------------------------------------------------

static void link_shader_regex()
{
	std::vector<SeenShader> found;

	AcquireSRWLockShared(&registry_lock);
	if (linked_shaders < seen_shaders.size())
		found.assign(seen_shaders.begin() + linked_shaders, seen_shaders.end());
	ReleaseSRWLockShared(&registry_lock);

	linked_shaders += found.size();
	if (shader_regex_groups.empty())
		return;

	for (const SeenShader &shader : found) {
		// Nothing of what a section matches a D3D12 shader by is known,
		// which leaves the sections that take every shader of a stage
		Dx12ShaderInfo info = {};

		info.hash = shader.hash;
		info.stage = (Dx12ShaderStage)shader.stage;
		link_shader_regex_groups_dx12(&info);
	}
}

// With the lock of dx12/Bridge.h held: what the threads recording go by of
// the configuration and of the shaders there are
static void refresh_wanted()
{
	link_shader_regex();

	if (seen_override_generation == G->shader_override_generation)
		return;
	seen_override_generation = G->shader_override_generation;

	AcquireSRWLockExclusive(&wanted_lock);
	wanted_hashes.clear();
	for (const auto &entry : G->mShaderOverrideMap)
		wanted_hashes.insert(entry.first);
	ReleaseSRWLockExclusive(&wanted_lock);
	InterlockedIncrement(&wanted_generation);
}

void VkModsConfigReloaded()
{
	if (!vk_mods_active)
		return;

	// The sections the shaders were linked to are gone
	linked_shaders = 0;
	refresh_wanted();
	collect_index_counts();
}

static void report()
{
	LARGE_INTEGER frequency;
	double per_frame, per_draw;

	QueryPerformanceFrequency(&frequency);
	per_frame = 1000.0 / frequency.QuadPart / 300;
	per_draw = stats.draws ? 1000000.0 / frequency.QuadPart / stats.draws : 0;

	VkDumpNote("Mods, frame %u, since the last of these lines: the engine saw %u draws and skipped %u, mods tried to draw %u times; "
		"index regions hashed %u, found as they were %u, not mapped %u; per frame the draws took %.2f ms waiting for the lock "
		"and %.2f ms in the engine, %.2f us a draw; %Iu shaders linked to ShaderRegex, %Iu ShaderOverride hashes, %Iu buffers published\n"
		"  The draws were stopped for because of: settings %u, a pipeline not looked at yet, command lists doing more than asking "
		"or a condition of one %u, an index count a section asks for %u; about %u more were not stopped for\n",
		vk_frame, stats.draws, stats.skipped, stats.mod_draws, stats.regions_hashed, stats.regions_checked,
		stats.regions_unmapped, stats.wait_ticks * per_frame, stats.engine_ticks * per_frame, stats.engine_ticks * per_draw,
		linked_shaders, G->mShaderOverrideMap.size(), published.size(),
		stats.slow[SLOW_SETTINGS], stats.slow[SLOW_LISTS], stats.slow[SLOW_INDEX_COUNT], (unsigned)InterlockedExchange(&fast_draws, 0));
	stats = {};
}

void VkModsPresent()
{
	static bool no_engine_reported, engine_reported;
	bool all;

	Dx12ShimLock();

	if (!dx12_shim) {
		draw_path_active = false;
		Dx12ShimUnlock();
		if (vk_frame >= 600 && !no_engine_reported) {
			no_engine_reported = true;
			VkDumpNote("*** vulkan = 1, but no D3D12 device presents the frames of the game, which is where the engine "
				"for the mods comes from. Nothing is applied. With an NVIDIA driver, see \"Vulkan/OpenGL present method\".\n");
		}
		return;
	}

	if (!engine_reported) {
		engine_reported = true;
		VkDumpNote("vulkan = 1: the draws of the game are shown to the engine of the D3D12 device that presents its frames\n");
	}

	forget_dead_buffers();
	refresh_wanted();
	// The configuration as it was read when the DLL was loaded
	if (!wanted_index_counts)
		collect_index_counts();

	all = G->hunting == HUNTING_MODE_ENABLED && !dx12_mods_disabled;
	if (want_all != all) {
		want_all = all;
		InterlockedIncrement(&wanted_generation);
	}

	draw_path_active = all || (!dx12_mods_disabled && G->fix_enabled && !wanted_hashes.empty());

	if (G->vk_dump_hashes && !(vk_frame % 300))
		report();

	Dx12ShimUnlock();
}
