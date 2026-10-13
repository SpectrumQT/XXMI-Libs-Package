#include "VkDrawPath.h"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "VkDump.h"
#include "VkScope.h"

#include "../dx12/Bridge.h"
#include "../dx12/DrawPath.h"
#include "../dx12/ShimContext.h"
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
	unsigned draws, skipped, mod_draws, mod_draws_failed;
	unsigned slow[SLOW_REASONS];
	unsigned regions_hashed, regions_checked, regions_unmapped;
	unsigned mirrored;
	UINT64 mirrored_bytes;
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

// The vertex bindings kept track of, which are more than a mesh has streams
const UINT MAX_VBS = 16;

struct Pipeline {
	// By Dx12ShaderStage, 0 where it has none
	UINT64 shaders[DX12_STAGE_COUNT];
	// The vertex bindings it was created with, if that is known: a bit for
	// each, and their strides
	bool vbs_known;
	UINT vbs, vbs_per_instance;
	UINT vb_strides[MAX_VBS];
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
	const VkPipelineVertexInputStateCreateInfo *input = library ? NULL : info->pVertexInputState;
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
	entry->vbs = 0;
	entry->vbs_per_instance = 0;
	// Set while the pipeline is bound instead, the pointer is not looked at
	for (i = 0; input && info->pDynamicState && i < info->pDynamicState->dynamicStateCount; i++) {
		if (info->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT
		    || info->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)
			input = NULL;
	}
	for (i = 0; input && i < input->vertexBindingDescriptionCount; i++) {
		const VkVertexInputBindingDescription *binding = &input->pVertexBindingDescriptions[i];

		if (binding->binding >= MAX_VBS) {
			input = NULL;
			break;
		}
		entry->vbs |= 1u << binding->binding;
		if (binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX)
			entry->vbs_per_instance |= 1u << binding->binding;
		entry->vb_strides[binding->binding] = binding->stride;
	}
	entry->vbs_known = !!input;
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
// The buffers of the mods
//
// A buffer a mod draws with is a D3D11 one of the shim. What the game's
// command buffer can be given is a buffer of the game's device, so that is
// what the contents are copied to, for as long as they are known here: read
// from a file or written by the engine, not by a shader.
//
// All of this with the lock of dx12/Bridge.h held.

static VkDevice mods_device;
// What the loader has at the start of everything of the device
static void *mods_device_key;
static VkModsApi api;
static VkPhysicalDeviceMemoryProperties memory_properties;

// A device only allows so many allocations, and rounds each up
const VkDeviceSize MIRROR_BLOCK_BYTES = 32 << 20;

struct MirrorBlock {
	VkDeviceMemory memory;
	BYTE *data;
	uint32_t type;
	VkDeviceSize size, used;
	// Buffers in it. Its space is given out again once there are none.
	UINT live;
};

struct Mirror {
	VkBuffer buffer;
	MirrorBlock *block;
	VkDeviceSize size;
	// ShimResource::version of the contents
	UINT64 version;
};

struct RetiredMirror {
	VkBuffer buffer;
	MirrorBlock *block;
	UINT frame;
};

// Nothing tells when the command buffers drawing from a buffer have been
// executed. The game is taken to have submitted what it recorded this many
// frames later.
const UINT MIRROR_KEEP_FRAMES = 8;

static std::vector<MirrorBlock*> mirror_blocks;
static std::unordered_map<ShimResource*, Mirror> mirrors;
static std::vector<RetiredMirror> retired_mirrors;

static MirrorBlock* new_mirror_block(const VkMemoryRequirements *needed)
{
	const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	// Memory of the GPU that can be written from here, where there is such
	const VkMemoryPropertyFlags wanted[] = { host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, host };
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkDeviceMemory memory;
	MirrorBlock *block;
	void *data;
	uint32_t type;

	info.allocationSize = max(MIRROR_BLOCK_BYTES, needed->size);
	for (VkMemoryPropertyFlags flags : wanted) {
		for (type = 0; type < memory_properties.memoryTypeCount; type++) {
			if (!(needed->memoryTypeBits & 1u << type)
			    || (memory_properties.memoryTypes[type].propertyFlags & (flags | VK_MEMORY_PROPERTY_PROTECTED_BIT)) != flags)
				continue;

			info.memoryTypeIndex = type;
			if (api.AllocateMemory(mods_device, &info, NULL, &memory) != VK_SUCCESS)
				continue;
			if (api.MapMemory(mods_device, memory, 0, VK_WHOLE_SIZE, 0, &data) != VK_SUCCESS) {
				api.FreeMemory(mods_device, memory, NULL);
				continue;
			}

			block = new MirrorBlock();
			block->memory = memory;
			block->data = (BYTE*)data;
			block->type = type;
			block->size = info.allocationSize;
			mirror_blocks.push_back(block);
			VkDumpNote("Mods: %llu MB of memory type %u (flags 0x%x) for the buffers of the mods, block %Iu\n",
				block->size >> 20, type, memory_properties.memoryTypes[type].propertyFlags, mirror_blocks.size());
			return block;
		}
	}
	return NULL;
}

static bool place_mirror(Mirror *mirror, const VkMemoryRequirements *needed, VkDeviceSize *offset)
{
	VkDeviceSize alignment = max(needed->alignment, (VkDeviceSize)1);
	MirrorBlock *block = NULL;

	for (MirrorBlock *candidate : mirror_blocks) {
		if (!candidate->live)
			candidate->used = 0;
		*offset = (candidate->used + alignment - 1) / alignment * alignment;
		if ((needed->memoryTypeBits & 1u << candidate->type) && *offset + needed->size <= candidate->size) {
			block = candidate;
			break;
		}
	}
	if (!block) {
		block = new_mirror_block(needed);
		*offset = 0;
	}
	if (!block)
		return false;

	block->used = *offset + needed->size;
	block->live++;
	mirror->block = block;
	return true;
}

static void retire_mirror(const Mirror *mirror)
{
	retired_mirrors.push_back({ mirror->buffer, mirror->block, vk_frame });
}

static void destroy_retired_mirrors(bool all)
{
	size_t kept = 0;

	for (const RetiredMirror &retired : retired_mirrors) {
		if (!all && vk_frame - retired.frame < MIRROR_KEEP_FRAMES) {
			retired_mirrors[kept++] = retired;
			continue;
		}
		api.DestroyBuffer(mods_device, retired.buffer, NULL);
		retired.block->live--;
	}
	retired_mirrors.resize(kept);
}

// NULL if what the buffer holds is not known here
static const BYTE* buffer_contents(const ShimResource *resource)
{
	if (resource->shadow.empty() || resource->readback_pending)
		return NULL;
	if (!resource->cpu_only && resource->shadow_version != resource->version)
		return NULL;
	return resource->shadow.data();
}

static Mirror* mirror_of(ShimBuffer *buffer)
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	ShimResource *resource = &buffer->r;
	const BYTE *data = buffer_contents(resource);
	VkMemoryRequirements needed;
	VkDeviceSize offset;
	Mirror mirror = {};

	if (!data)
		return NULL;

	auto found = mirrors.find(resource);
	if (found != mirrors.end()) {
		if (found->second.version == resource->version)
			return &found->second;
		// The command buffers recorded so far draw what it held then
		retire_mirror(&found->second);
		mirrors.erase(found);
		resource->mirrored = false;
	}

	info.size = min((size_t)buffer->mDesc.ByteWidth, resource->shadow.size());
	info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (api.CreateBuffer(mods_device, &info, NULL, &mirror.buffer) != VK_SUCCESS)
		return NULL;

	api.GetBufferMemoryRequirements(mods_device, mirror.buffer, &needed);
	if (!place_mirror(&mirror, &needed, &offset)) {
		api.DestroyBuffer(mods_device, mirror.buffer, NULL);
		return NULL;
	}
	if (api.BindBufferMemory(mods_device, mirror.buffer, mirror.block->memory, offset) != VK_SUCCESS) {
		mirror.block->live--;
		api.DestroyBuffer(mods_device, mirror.buffer, NULL);
		return NULL;
	}

	memcpy(mirror.block->data + offset, data, (size_t)info.size);
	mirror.size = info.size;
	mirror.version = resource->version;
	resource->mirrored = true;
	stats.mirrored++;
	stats.mirrored_bytes += info.size;
	return &(mirrors[resource] = mirror);
}

void VkScopeForgetResource(ShimResource *resource)
{
	auto found = mirrors.find(resource);

	if (found == mirrors.end())
		return;
	retire_mirror(&found->second);
	mirrors.erase(found);
}

void VkModsDevice(VkDevice device, VkPhysicalDevice physical_device, const VkModsApi *functions)
{
	const void *const *function = (const void *const *)functions;
	size_t i;

	for (i = 0; i < sizeof(*functions) / sizeof(void*); i++) {
		if (!function[i]) {
			VkDumpNote("*** Mods: a function of the Vulkan device was not found, mods cannot draw\n");
			return;
		}
	}

	Dx12ShimLock();
	// The first there is: the buffers are of one device
	if (!mods_device) {
		api = *functions;
		api.GetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
		mods_device_key = *(void**)device;
		mods_device = device;
	}
	Dx12ShimUnlock();
}

void VkModsForgetDevice(VkDevice device)
{
	Dx12ShimLock();
	if (device == mods_device) {
		for (auto &mirror : mirrors) {
			mirror.first->mirrored = false;
			api.DestroyBuffer(mods_device, mirror.second.buffer, NULL);
		}
		mirrors.clear();
		destroy_retired_mirrors(true);
		for (MirrorBlock *block : mirror_blocks) {
			api.FreeMemory(mods_device, block->memory, NULL);
			delete block;
		}
		mirror_blocks.clear();
		mods_device = NULL;
		mods_device_key = NULL;
	}
	Dx12ShimUnlock();
}

// -----------------------------------------------------------------------------
// What is set on a command buffer

struct CmdState {
	Pipeline *pipeline;
	VkBuffer ib;
	VkDeviceSize ib_offset;
	VkIndexType index_type;
	VkBuffer vbs[MAX_VBS];
	VkDeviceSize vb_offsets[MAX_VBS];
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

void VkModsBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)
{
	CmdState *state = cmd_state(cmd);
	uint32_t i;

	for (i = 0; i < count && first + i < MAX_VBS; i++) {
		state->vbs[first + i] = buffers[i];
		state->vb_offsets[first + i] = offsets[i];
	}
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
	VkCommandBuffer cmd;
	CmdState state;
	bool indexed;
	// The index buffer once the engine has asked for it
	PublishedBuffer *ib;
	// What the draws of mods have left bound, where that is not what the
	// game has
	UINT vbs_changed;
	bool ib_changed;
	VkBuffer vbs[MAX_VBS];
	VkDeviceSize vb_offsets[MAX_VBS];
	VkBuffer bound_ib;
	VkDeviceSize bound_ib_offset;
	VkIndexType bound_index_type;
	// Draws of mods that could not be made, see mod_draws
	UINT mod_draws_failed;
};

// A draw of a mod, with the bindings that are not to be the game's
struct ModDraw {
	UINT vbs_set;
	VkBuffer vbs[MAX_VBS];
	VkDeviceSize vb_offsets[MAX_VBS];
	bool ib_set;
	VkBuffer ib;
	VkDeviceSize ib_offset;
	VkIndexType index_type;
	bool indexed;
	UINT count, instances, start, start_instance;
	INT base_vertex;
};

// Those of the scope. A mod draws a mesh in parts, and what stands in for a
// part that cannot be drawn is the game's mesh, all of it: so nothing is
// recorded before the last of them is known to be possible.
static std::vector<ModDraw> mod_draws;

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

static void mod_draw_failed(const char *why)
{
	static std::unordered_set<std::string> reported;

	scope->mod_draws_failed++;
	stats.mod_draws_failed++;
	if (reported.insert(why).second)
		VkDumpNote("*** Mods: a draw of a mod could not be made: %s. None of the mod's are made for that draw of "
			"the game then, and the game's is.\n", why);
}

static void bind_vb(UINT slot, VkBuffer buffer, VkDeviceSize offset)
{
	if (scope->vbs[slot] == buffer && scope->vb_offsets[slot] == offset)
		return;
	api.CmdBindVertexBuffers(scope->cmd, slot, 1, &buffer, &offset);
	scope->vbs[slot] = buffer;
	scope->vb_offsets[slot] = offset;
	scope->vbs_changed |= 1u << slot;
}

static void bind_ib(VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
	if (scope->bound_ib == buffer && scope->bound_ib_offset == offset && scope->bound_index_type == type)
		return;
	api.CmdBindIndexBuffer(scope->cmd, buffer, offset, type);
	scope->bound_ib = buffer;
	scope->bound_ib_offset = offset;
	scope->bound_index_type = type;
	scope->ib_changed = true;
}

// The game's bindings, where the draws of mods left others
static void restore_bindings()
{
	const CmdState *game = &scope->state;
	unsigned long slot;
	UINT vbs;

	for (vbs = scope->vbs_changed; _BitScanForward(&slot, vbs); vbs &= vbs - 1) {
		if (game->vbs[slot])
			bind_vb(slot, game->vbs[slot], game->vb_offsets[slot]);
	}
	if (scope->ib_changed && game->ib)
		bind_ib(game->ib, game->ib_offset, game->index_type);
}

// For the report: what the first few draws of mods with other vertex bindings
// than those before found bound, and what became of each binding
static void note_mod_bindings(const ModDraw *draw, Mirror *const *replaced)
{
	static std::unordered_set<std::string> noted;
	const CmdState *game = &scope->state;
	const Pipeline *pipeline = game->pipeline;
	char text[1024] = "", part[128];
	std::string key;
	UINT slot;

	if (noted.size() >= 16)
		return;

	for (slot = 0; slot < MAX_VBS; slot++) {
		if (!(pipeline->vbs & 1u << slot))
			continue;
		_snprintf_s(part, _TRUNCATE, " %u: stride %u,", slot, pipeline->vb_strides[slot]);
		key += part;
		_snprintf_s(part, _TRUNCATE, " %u: stride %u, the game's %p+%llu, %s;", slot, pipeline->vb_strides[slot],
			(void*)game->vbs[slot], game->vb_offsets[slot],
			replaced[slot] ? "the mod's" : (draw->vbs_set & 1u << slot) ? "follows the mod's" : "left");
		strncat_s(text, part, _TRUNCATE);
	}
	if (noted.insert(key).second)
		VkDumpNote("Mods: a draw of a mod with the vertex bindings%s\n", text);
}

// The vertex and index buffers the mod has set, in place of the game's. The
// pipeline stays the game's: a buffer is read the way the game's in its place
// would be.
static bool bind_for_mod(const ShimState *mod, ModDraw *draw)
{
	const CmdState *game = &scope->state;
	const Pipeline *pipeline = game->pipeline;
	Mirror *replaced[MAX_VBS] = {};
	UINT64 vertices = UINT64_MAX;
	VkDeviceSize size;
	Mirror *mirror;
	UINT slot;

	if (!mods_device || *(void**)scope->cmd != mods_device_key) {
		mod_draw_failed("the command buffer is of another device than the one the buffers of the mods are on");
		return false;
	}

	for (slot = 0; mod->vbs_set && slot < MAX_VBS; slot++) {
		ShimBuffer *vb = static_cast<ShimBuffer*>(mod->vbs[slot]);

		if (!vb)
			continue;
		if (!pipeline->vbs_known) {
			mod_draw_failed("the vertex bindings of the game's pipeline are not known");
			return false;
		}
		// Nothing reads it
		if (!(pipeline->vbs & 1u << slot))
			continue;
		if (pipeline->vb_strides[slot] != mod->vb_strides[slot]) {
			mod_draw_failed("a vertex buffer has another stride than the game's pipeline reads its binding with");
			return false;
		}
		if (mod->vb_offsets[slot] >= vb->mDesc.ByteWidth || !(replaced[slot] = mirror_of(vb))) {
			mod_draw_failed("the contents of a vertex buffer are not known to the CPU, or no Vulkan buffer could be made of them");
			return false;
		}
		if (mod->vb_strides[slot] && !(pipeline->vbs_per_instance & 1u << slot))
			vertices = min(vertices, (replaced[slot]->size - mod->vb_offsets[slot]) / mod->vb_strides[slot]);
	}

	// What the game has bound to more than one binding is one stream to the
	// mod: the others follow. What is left of the game's is read as far as
	// the mod's vertices go, which has to be within the buffer.
	for (slot = 0; slot < MAX_VBS; slot++) {
		UINT other;

		if (replaced[slot] || !(pipeline->vbs & 1u << slot) || !game->vbs[slot])
			continue;
		for (other = 0; other < MAX_VBS; other++) {
			if (replaced[other] && game->vbs[other] == game->vbs[slot]
			    && game->vb_offsets[other] == game->vb_offsets[slot]
			    && pipeline->vb_strides[other] == pipeline->vb_strides[slot])
				break;
		}
		if (other < MAX_VBS) {
			draw->vbs_set |= 1u << slot;
			draw->vbs[slot] = replaced[other]->buffer;
			draw->vb_offsets[slot] = mod->vb_offsets[other];
			continue;
		}
		if (vertices == UINT64_MAX || !pipeline->vb_strides[slot] || (pipeline->vbs_per_instance & 1u << slot))
			continue;

		AcquireSRWLockShared(&memory_lock);
		auto info = buffers.find(game->vbs[slot]);
		size = info != buffers.end() ? info->second.size : 0;
		ReleaseSRWLockShared(&memory_lock);
		if (game->vb_offsets[slot] + vertices * pipeline->vb_strides[slot] > size) {
			mod_draw_failed("a vertex buffer of the game the mod leaves in place is too small for the vertices of the mod");
			return false;
		}
	}
	for (slot = 0; slot < MAX_VBS; slot++) {
		if (!replaced[slot])
			continue;
		draw->vbs_set |= 1u << slot;
		draw->vbs[slot] = replaced[slot]->buffer;
		draw->vb_offsets[slot] = mod->vb_offsets[slot];
	}
	if (G->vk_dump_hashes)
		note_mod_bindings(draw, replaced);

	if (!draw->indexed || !mod->ib)
		return true;
	draw->ib_set = true;

	if (scope->ib && mod->ib == scope->ib->object) {
		// The game's own, set again by the mod
		draw->ib = game->ib;
		draw->ib_offset = game->ib_offset + mod->ib_offset;
		draw->index_type = game->index_type;
		return true;
	}
	if (mod->ib_format != DXGI_FORMAT_R16_UINT && mod->ib_format != DXGI_FORMAT_R32_UINT) {
		mod_draw_failed("the index buffer has a format that is neither R16_UINT nor R32_UINT");
		return false;
	}
	mirror = mirror_of(static_cast<ShimBuffer*>(mod->ib));
	if (!mirror || mod->ib_offset >= mirror->size) {
		mod_draw_failed("the contents of the index buffer are not known to the CPU, or no Vulkan buffer could be made of them");
		return false;
	}
	draw->ib = mirror->buffer;
	draw->ib_offset = mod->ib_offset;
	draw->index_type = mod->ib_format == DXGI_FORMAT_R16_UINT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
	return true;
}

static void record_mod_draws()
{
	const CmdState *game = &scope->state;
	UINT slot;

	for (const ModDraw &draw : mod_draws) {
		for (slot = 0; slot < MAX_VBS; slot++) {
			if (draw.vbs_set & 1u << slot)
				bind_vb(slot, draw.vbs[slot], draw.vb_offsets[slot]);
			else if (game->vbs[slot])
				bind_vb(slot, game->vbs[slot], game->vb_offsets[slot]);
		}
		if (draw.ib_set)
			bind_ib(draw.ib, draw.ib_offset, draw.index_type);
		else if (draw.indexed)
			bind_ib(game->ib, game->ib_offset, game->index_type);

		if (draw.indexed)
			api.CmdDrawIndexed(scope->cmd, draw.count, draw.instances, draw.start, draw.base_vertex, draw.start_instance);
		else
			api.CmdDraw(scope->cmd, draw.count, draw.instances, draw.start, draw.start_instance);
	}
	if (scope->vbs_changed || scope->ib_changed)
		restore_bindings();
}

// What a mod sets for the stages before the pixel shader goes with its mesh:
// the bones of a skeleton it put together itself, say, which its vertices
// name by their place in that. Drawn with the game's instead, such a mesh is
// anywhere, and vertices that are not numbers stay in what the game keeps
// from frame to frame. A texture that is missing only looks wrong.
static bool mod_draw_possible(const ShimState *mod)
{
	const ID3D11ShaderResourceView *ini_params = dx12_shim->hacker_device->mIniResourceView;
	int stage;
	UINT i;

	for (stage = 0; stage < DX12_STAGE_COUNT; stage++) {
		const ShimStageState *state = &mod->stages[stage];

		if (stage == DX12_STAGE_CS)
			continue;
		if (state->shader || state->shader_unbound) {
			mod_draw_failed("the mod draws with a shader of its own");
			return false;
		}
		if (stage == DX12_STAGE_PS)
			continue;
		if (state->cb_mask) {
			mod_draw_failed("the mod has set a constant buffer for a stage before the pixel shader");
			return false;
		}
		for (i = 0; i < SHIM_SRV_SLOTS; i++) {
			if (state->srvs[i] && state->srvs[i] != ini_params) {
				mod_draw_failed("the mod has set a buffer or texture for a stage before the pixel shader");
				return false;
			}
		}
	}
	return true;
}

void VkScopeDraw(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
	const ShimState *mod = dx12_shim->shim_context->mState;
	ModDraw draw = {};

	stats.mod_draws++;
	// D3D11 reads every index as 0 without an index buffer, which draws
	// nothing
	if (indexed && (mod->ib_unbound || (!mod->ib && !scope->state.ib)))
		return;

	// The game's is drawn anyway
	if (scope->mod_draws_failed)
		return;

	draw.indexed = indexed;
	draw.count = count;
	draw.instances = instances;
	draw.start = start;
	draw.base_vertex = base_vertex;
	draw.start_instance = start_instance;
	if (mod_draw_possible(mod) && bind_for_mod(mod, &draw))
		mod_draws.push_back(draw);
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

	opened.cmd = cmd;
	opened.state = *state;
	opened.indexed = indexed;
	opened.ib = NULL;
	opened.vbs_changed = 0;
	opened.ib_changed = false;
	memcpy(opened.vbs, state->vbs, sizeof(opened.vbs));
	memcpy(opened.vb_offsets, state->vb_offsets, sizeof(opened.vb_offsets));
	opened.bound_ib = state->ib;
	opened.bound_ib_offset = state->ib_offset;
	opened.bound_index_type = state->index_type;
	opened.mod_draws_failed = 0;
	mod_draws.clear();
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
	if (!opened.mod_draws_failed)
		record_mod_draws();
	// A mesh the mod was to replace and could not is better seen as it was
	// than not at all
	if (!c.call_info.skip || opened.mod_draws_failed) {
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

	VkDumpNote("Mods, frame %u (D3D12 frame %u), since the last of these lines: the engine saw %u draws and skipped %u, mods drew %u times "
		"of which %u could not be made, %u buffers of mods copied (%llu KB), %Iu held; "
		"index regions hashed %u, found as they were %u, not mapped %u; per frame the draws took %.2f ms waiting for the lock "
		"and %.2f ms in the engine, %.2f us a draw; %Iu shaders linked to ShaderRegex, %Iu ShaderOverride hashes, %Iu buffers published\n"
		"  The draws were stopped for because of: settings %u, a pipeline not looked at yet, command lists doing more than asking "
		"or a condition of one %u, an index count a section asks for %u; about %u more were not stopped for\n",
		vk_frame, dx12_frame, stats.draws, stats.skipped, stats.mod_draws, stats.mod_draws_failed, stats.mirrored,
		stats.mirrored_bytes >> 10, mirrors.size(), stats.regions_hashed, stats.regions_checked,
		stats.regions_unmapped, stats.wait_ticks * per_frame, stats.engine_ticks * per_frame, stats.engine_ticks * per_draw,
		linked_shaders, G->mShaderOverrideMap.size(), published.size(),
		stats.slow[SLOW_SETTINGS], stats.slow[SLOW_LISTS], stats.slow[SLOW_INDEX_COUNT], (unsigned)InterlockedExchange(&fast_draws, 0));
	stats = {};
}

void VkModsPresent()
{
	static bool no_engine_reported, engine_reported, device_removed_reported;
	HRESULT removed;
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

	if (!device_removed_reported && FAILED(removed = dx12_shim->device->GetDeviceRemovedReason())) {
		device_removed_reported = true;
		VkDumpNote("*** Frame %u: the D3D12 device that presents the frames has been removed, reason 0x%x. "
			"Nothing new is shown from here on.\n", vk_frame, removed);
	}

	forget_dead_buffers();
	if (mods_device)
		destroy_retired_mirrors(false);
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
