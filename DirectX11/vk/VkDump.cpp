#include "VkDump.h"

#include <share.h>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <d3d11.h>

#include "util.h"
#include "../globals.h"
#include "../dx12/Dx12Common.h" // For Dx12Crc32cWriteCombined

#include "Spirv.h"

bool vk_dump_active;
volatile UINT vk_frame;

// True while the draws being recorded go to vk_draws.csv
static volatile bool dump_draws;

enum {
	STAGE_VS,
	STAGE_HS,
	STAGE_DS,
	STAGE_GS,
	STAGE_PS,
	STAGE_CS,
	STAGE_COUNT
};
static const char *const stage_names[STAGE_COUNT] = { "vs", "hs", "ds", "gs", "ps", "cs" };

static int stage_index(uint32_t stage)
{
	switch (stage) {
		case VK_SHADER_STAGE_VERTEX_BIT: return STAGE_VS;
		case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return STAGE_HS;
		case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return STAGE_DS;
		case VK_SHADER_STAGE_GEOMETRY_BIT: return STAGE_GS;
		case VK_SHADER_STAGE_FRAGMENT_BIT: return STAGE_PS;
		case VK_SHADER_STAGE_COMPUTE_BIT: return STAGE_CS;
	}
	return -1;
}

// Vulkan does not record what D3D11 would have been told a buffer is for, and
// the bind flags are part of the D3D11 hash. These are the ones a renderer
// gives its mesh buffers.
static const UINT buffer_bind_flags[] = {
	D3D11_BIND_VERTEX_BUFFER,
	D3D11_BIND_INDEX_BUFFER,
	D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE,
	D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE,
};
static const char *const buffer_bind_names[ARRAYSIZE(buffer_bind_flags)] = { "vb", "ib", "vb+srv", "ib+srv" };

enum {
	CSV_SHADERS,
	CSV_PIPELINES,
	CSV_BUFFERS,
	CSV_OBJECTS,
	CSV_DRAWS,
	CSV_PROBE,
	// Not a CSV: what the log is told, for when logging is disabled
	CSV_REPORT,
	CSV_COUNT
};
static const wchar_t *const csv_names[CSV_COUNT] = {
	L"vk_shaders.csv", L"vk_pipelines.csv", L"vk_buffers.csv", L"vk_buffer_objects.csv", L"vk_draws.csv", L"vk_probe.csv", L"vk_report.txt",
};
static const char *const csv_headers[CSV_COUNT] = {
	"hash,stage,bytes,spirv_version,generator,entry,named,unnamed,inputs,resources\n",
	"frame,pipeline,kind,vs,hs,ds,gs,ps,cs,topology,layout,vertex_bindings,vertex_attributes\n",
	"frame,buffer,buffer_bytes,usage,offset,bytes,data_hash,hash_vb,hash_ib,hash_vb_srv,hash_ib_srv\n",
	"frame,buffer,bytes,usage,memory,memory_offset\n",
	"frame,cmd,draw,type,count,instances,first,base_vertex,pipeline,vs,ps,cs,"
		"ib,ib_offset,index_type,ib_range,ib_bytes,ib_hash,"
		"vb0,vb0_offset,vb0_stride,vb0_range,vb0_bytes,vb0_hash,vb0_hash_srv,"
		"ib_buffer_bytes,ib_usage,ib_mapped,vb0_buffer_bytes,vb0_usage,vb0_mapped\n",
	"frame,buffer,offset,bytes,first_index,index_count,region_hash,previous_hash\n",
	"",
};

const UINT64 REPORT_INTERVAL_MS = 5000;
const UINT64 MAX_HASHED_BYTES = 256 << 20;
// A range written again within this many frames, this many times in a row,
// is a dynamic buffer
const UINT HOT_FRAMES = 30;
const UINT HOT_LIMIT = 4;

struct ShaderInfo {
	UINT64 hash;
	int stage;
};

struct PipelineInfo {
	UINT64 shaders[STAGE_COUNT];
	uint32_t stride0;
};

// Guards the files and everything about shaders and pipelines
static SRWLOCK lock = SRWLOCK_INIT;
static FILE *csv_files[CSV_COUNT];
static bool csv_failed[CSV_COUNT];
static std::unordered_map<VkShaderModule, ShaderInfo> shader_modules;
static std::unordered_map<VkPipeline, PipelineInfo> pipelines;
// What was seen is only compared with the configuration when reporting, as
// the mods may not be loaded yet when the game creates its first resources
static std::unordered_set<UINT64> shader_hashes;
static std::unordered_set<uint32_t> buffer_hashes[ARRAYSIZE(buffer_bind_flags)];
static unsigned modules_created, modules_unnamed, modules_invalid;
static unsigned graphics_pipelines, compute_pipelines;
static volatile LONG stages_without_module;

struct BufferRange {
	VkDeviceSize bytes;
	uint32_t data_hash;
	bool valid;
	UINT frame;
	UINT hot;
};

struct BufferInfo {
	VkDeviceSize size;
	VkBufferUsageFlags usage;
	VkDeviceMemory memory;
	VkDeviceSize memory_offset;
	// What copies have written, by offset. They do not overlap.
	std::map<VkDeviceSize, BufferRange> ranges;
};

struct Mapping {
	const BYTE *data;
	VkDeviceSize offset, size;
};

// Held while mapped memory is read, which keeps it from being unmapped
static SRWLOCK buffer_lock = SRWLOCK_INIT;
static std::unordered_map<VkBuffer, BufferInfo> buffers;
static std::unordered_map<VkDeviceMemory, Mapping> mappings;

static struct {
	volatile LONG64 hashed;
	volatile LONG64 hashed_bytes;
	volatile LONG64 dynamic;
	volatile LONG64 not_mapped;
	volatile LONG64 untracked;
	volatile LONG64 out_of_bounds;
	volatile LONG64 maps;
	volatile LONG64 flushes;
	volatile LONG64 flushed_bytes;
} buffer_stats;

// Every way a buffer gets written that goes through Vulkan, by the usage of
// the buffer written. With the buffer lock held.
struct WriteCount {
	LONG64 copies, copied_bytes;
	LONG64 updates, updated_bytes;
};
static std::map<VkBufferUsageFlags, WriteCount> writes_by_usage;

struct CmdState {
	VkPipeline graphics, compute;
	VkBuffer ib;
	VkDeviceSize ib_offset;
	VkIndexType index_type;
	VkBuffer vb0;
	VkDeviceSize vb0_offset;
	// All of them, vb0 included, for where meshes start in a shared buffer
	VkBuffer vb[8];
	VkDeviceSize vb_offset[8];
	UINT draws;
};

// Command buffers are recorded on several threads at once
const UINT CMD_SHARDS = 16;
static SRWLOCK cmd_locks[CMD_SHARDS];
static std::unordered_map<VkCommandBuffer, CmdState> cmd_states[CMD_SHARDS];

static UINT cmd_shard(VkCommandBuffer cmd)
{
	return (UINT)(((UINT64)(uintptr_t)cmd * 0x9e3779b97f4a7c15ull) >> 60);
}

struct DrawSample {
	UINT frame;
	unsigned draws;
	unsigned unknown_pipeline;
	unsigned indexed;
	unsigned ib_hashed;
	unsigned vb_hashed;
};
static DrawSample sample;
static bool sample_requested, sample_finished;

// A game that keeps its meshes in a few large buffers binds an index buffer
// as an offset into one of those, and the buffer as a whole says nothing about
// the mesh. 3DMigoto has track_region_hashes for that under D3D11: the hash a
// mod matches is of the indices the draw uses and nothing else, which it reads
// back from the GPU. Here they are read from the memory behind the buffer, if
// the game has it mapped, for the draws of a sampled frame
// (probe_index_buffers).
struct IndexRegion {
	VkBuffer buffer;
	VkDeviceSize offset, bytes;

	bool operator<(const IndexRegion &other) const
	{
		if (buffer != other.buffer)
			return buffer < other.buffer;
		if (offset != other.offset)
			return offset < other.offset;
		return bytes < other.bytes;
	}
};
struct ProbedRegion {
	uint32_t first, count;
	// What the region hashed to the last time, as the game may write it
	// again without anything telling
	uint32_t hash;
	bool hashed;
};
static std::map<IndexRegion, ProbedRegion> probe_regions;
static std::unordered_set<uint32_t> region_hashes;
static unsigned probed, probe_unmapped, probe_changed;
const VkDeviceSize PROBE_MAX_BYTES = 64 << 20;

static bool dump_path(const wchar_t *name, wchar_t *path)
{
	if (!GetModuleFileName(migoto_handle, path, MAX_PATH))
		return false;
	wcsrchr(path, L'\\')[1] = 0;
	return !wcscat_s(path, MAX_PATH, name);
}

// With the lock held
static FILE* csv(int which)
{
	wchar_t path[MAX_PATH];

	if (csv_failed[which])
		return NULL;
	if (csv_files[which])
		return csv_files[which];

	csv_failed[which] = true;
	if (!dump_path(csv_names[which], path))
		return NULL;

	csv_files[which] = _wfsopen(path, L"w", _SH_DENYNO);
	if (!csv_files[which]) {
		VkLogInfo("*** Unable to create %S\n", path);
		return NULL;
	}

	csv_failed[which] = false;
	fputs(csv_headers[which], csv_files[which]);
	return csv_files[which];
}

void VkDumpNote(const char *format, ...)
{
	char line[1024];
	va_list args;
	FILE *report;

	va_start(args, format);
	vsnprintf_s(line, _TRUNCATE, format, args);
	va_end(args);

	VkLogInfo("%s", line);
	if (!vk_dump_active)
		return;

	AcquireSRWLockExclusive(&lock);
	report = csv(CSV_REPORT);
	if (report) {
		fputs(line, report);
		fflush(report);
	}
	ReleaseSRWLockExclusive(&lock);
}

// -----------------------------------------------------------------------------

static void write_spirv(UINT64 hash, int stage, const void *code, size_t bytes)
{
	static bool directory_made;
	wchar_t name[64], path[MAX_PATH];
	FILE *file;

	if (!directory_made) {
		directory_made = true;
		if (dump_path(L"vk_shaders", path))
			CreateDirectory(path, NULL);
	}

	swprintf_s(name, ARRAYSIZE(name), L"vk_shaders\\%016llx-%S.spv", hash, stage >= 0 ? stage_names[stage] : "xx");
	if (!dump_path(name, path))
		return;

	file = _wfsopen(path, L"wb", _SH_DENYNO);
	if (!file)
		return;
	fwrite(code, 1, bytes, file);
	fclose(file);
}

static ShaderInfo shader_seen(const VkShaderModuleCreateInfo *create)
{
	ShaderInfo shader = {};
	SpirvInfo info;
	FILE *file;

	shader.hash = fnv_64_buf(create->pCode, create->codeSize);
	SpirvReflect(create->pCode, create->codeSize / sizeof(uint32_t), &info);
	shader.stage = stage_index(info.stage);

	AcquireSRWLockExclusive(&lock);

	modules_created++;
	if (shader_hashes.insert(shader.hash).second) {
		modules_invalid += !info.valid;
		modules_unnamed += info.valid && !info.named && info.unnamed;

		file = csv(CSV_SHADERS);
		if (file) {
			fprintf(file, "%016llx,%s,%Iu,0x%x,0x%x,%s,%u,%u,%s,%s\n",
				shader.hash, shader.stage >= 0 ? stage_names[shader.stage] : "", create->codeSize,
				info.version, info.generator, info.entry.c_str(), info.named, info.unnamed,
				info.inputs.c_str(), info.resources.c_str());
		}
		write_spirv(shader.hash, shader.stage, create->pCode, create->codeSize);
	}

	ReleaseSRWLockExclusive(&lock);

	return shader;
}

void VkDumpShaderModule(VkShaderModule module, const VkShaderModuleCreateInfo *info)
{
	ShaderInfo shader;

	if (!info || !info->pCode || !info->codeSize)
		return;

	shader = shader_seen(info);

	AcquireSRWLockExclusive(&lock);
	shader_modules[module] = shader;
	ReleaseSRWLockExclusive(&lock);
}

void VkDumpForgetShaderModule(VkShaderModule module)
{
	AcquireSRWLockExclusive(&lock);
	shader_modules.erase(module);
	ReleaseSRWLockExclusive(&lock);
}

static const void* find_in_chain(const void *chain, VkStructureType type)
{
	const VkBaseInStructure *next = (const VkBaseInStructure*)chain;

	while (next && next->sType != type)
		next = next->pNext;
	return next;
}

static void pipeline_stage(const VkPipelineShaderStageCreateInfo *stage, PipelineInfo *pipeline)
{
	// Without a module the SPIR-V may come with the stage instead
	const VkShaderModuleCreateInfo *inline_module = (const VkShaderModuleCreateInfo*)
		find_in_chain(stage->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
	int index = stage_index(stage->stage);
	UINT64 hash = 0;

	if (index < 0)
		return;

	if (stage->module) {
		AcquireSRWLockShared(&lock);
		auto i = shader_modules.find(stage->module);
		if (i != shader_modules.end())
			hash = i->second.hash;
		ReleaseSRWLockShared(&lock);
	} else if (inline_module && inline_module->pCode && inline_module->codeSize) {
		hash = shader_seen(inline_module).hash;
	}

	pipeline->shaders[index] = hash;
	if (!hash)
		InterlockedIncrement(&stages_without_module);
}

void VkDumpGraphicsPipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo *create)
{
	const VkGraphicsPipelineLibraryCreateInfoEXT *library = (const VkGraphicsPipelineLibraryCreateInfoEXT*)
		find_in_chain(create->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
	// A part of a pipeline only has to have valid pointers for what it is of
	bool has_vertex_input = !library || (library->flags & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT);
	bool has_shaders = !library || (library->flags & (VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
		| VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT));
	const VkPipelineVertexInputStateCreateInfo *input = has_vertex_input ? create->pVertexInputState : NULL;
	const VkPipelineInputAssemblyStateCreateInfo *assembly = has_vertex_input ? create->pInputAssemblyState : NULL;
	std::string bindings, attributes;
	PipelineInfo info = {};
	char text[64];
	FILE *file;
	uint32_t i;

	if (has_shaders && create->pStages) {
		for (i = 0; i < create->stageCount; i++)
			pipeline_stage(&create->pStages[i], &info);
	}

	if (input) {
		for (i = 0; i < input->vertexBindingDescriptionCount; i++) {
			const VkVertexInputBindingDescription *binding = &input->pVertexBindingDescriptions[i];

			if (binding->binding == 0)
				info.stride0 = binding->stride;
			_snprintf_s(text, _TRUNCATE, "%u:%u:%s ", binding->binding, binding->stride,
				binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? "instance" : "vertex");
			bindings += text;
		}
		for (i = 0; i < input->vertexAttributeDescriptionCount; i++) {
			const VkVertexInputAttributeDescription *attribute = &input->pVertexAttributeDescriptions[i];

			// location:binding:VkFormat:offset
			_snprintf_s(text, _TRUNCATE, "%u:%u:%d:%u ", attribute->location, attribute->binding,
				attribute->format, attribute->offset);
			attributes += text;
		}
	}

	AcquireSRWLockExclusive(&lock);

	graphics_pipelines++;
	pipelines[pipeline] = info;

	file = csv(CSV_PIPELINES);
	if (file) {
		fprintf(file, "%u,%p,%s,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,%d,%p,%s,%s\n",
			vk_frame, pipeline, library ? "graphics_part" : "graphics",
			info.shaders[STAGE_VS], info.shaders[STAGE_HS], info.shaders[STAGE_DS],
			info.shaders[STAGE_GS], info.shaders[STAGE_PS], info.shaders[STAGE_CS],
			assembly ? assembly->topology : -1, create->layout, bindings.c_str(), attributes.c_str());
	}

	ReleaseSRWLockExclusive(&lock);
}

void VkDumpComputePipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo *create)
{
	PipelineInfo info = {};
	FILE *file;

	pipeline_stage(&create->stage, &info);

	AcquireSRWLockExclusive(&lock);

	compute_pipelines++;
	pipelines[pipeline] = info;

	file = csv(CSV_PIPELINES);
	if (file) {
		fprintf(file, "%u,%p,compute,%016llx,%016llx,%016llx,%016llx,%016llx,%016llx,-1,%p,,\n",
			vk_frame, pipeline, 0ull, 0ull, 0ull, 0ull, 0ull, info.shaders[STAGE_CS], create->layout);
	}

	ReleaseSRWLockExclusive(&lock);
}

void VkDumpForgetPipeline(VkPipeline pipeline)
{
	AcquireSRWLockExclusive(&lock);
	pipelines.erase(pipeline);
	ReleaseSRWLockExclusive(&lock);
}

// -----------------------------------------------------------------------------

// The hash HackerDevice::CreateBuffer would have calculated for a default
// usage buffer of this size with these D3D11_BIND_FLAGs
static uint32_t buffer_desc_hash(uint32_t data_hash, VkDeviceSize bytes, UINT bind_flags)
{
	D3D11_BUFFER_DESC desc = {};

	desc.ByteWidth = (UINT)bytes;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;

	return crc32c_hw(data_hash, &desc, sizeof(desc));
}

void VkDumpBufferCreated(VkBuffer buffer, const VkBufferCreateInfo *info)
{
	BufferInfo created = {};

	created.size = info->size;
	created.usage = info->usage;

	AcquireSRWLockExclusive(&buffer_lock);
	buffers[buffer] = std::move(created);
	ReleaseSRWLockExclusive(&buffer_lock);
}

void VkDumpBufferBound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
	VkBufferUsageFlags usage = 0;
	VkDeviceSize bytes = 0;
	FILE *file;

	AcquireSRWLockExclusive(&buffer_lock);
	auto i = buffers.find(buffer);
	if (i != buffers.end()) {
		i->second.memory = memory;
		i->second.memory_offset = offset;
		bytes = i->second.size;
		usage = i->second.usage;
	}
	ReleaseSRWLockExclusive(&buffer_lock);

	AcquireSRWLockExclusive(&lock);
	file = csv(CSV_OBJECTS);
	if (file)
		fprintf(file, "%u,%p,%llu,0x%x,%p,%llu\n", vk_frame, buffer, bytes, usage, memory, offset);
	ReleaseSRWLockExclusive(&lock);
}

void VkDumpForgetBuffer(VkBuffer buffer)
{
	AcquireSRWLockExclusive(&buffer_lock);
	buffers.erase(buffer);
	ReleaseSRWLockExclusive(&buffer_lock);
}

void VkDumpMemoryMapped(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, void *data)
{
	Mapping mapping = { (const BYTE*)data, offset, size };

	AcquireSRWLockExclusive(&buffer_lock);
	mappings[memory] = mapping;
	ReleaseSRWLockExclusive(&buffer_lock);

	InterlockedIncrement64(&buffer_stats.maps);
}

void VkDumpMemoryUnmapped(VkDeviceMemory memory)
{
	AcquireSRWLockExclusive(&buffer_lock);
	mappings.erase(memory);
	ReleaseSRWLockExclusive(&buffer_lock);
}

// With the buffer lock held exclusively. Returns false if the range turned
// out to be dynamic.
static bool note_write(BufferInfo *buffer, VkDeviceSize offset, VkDeviceSize bytes)
{
	std::map<VkDeviceSize, BufferRange> &map = buffer->ranges;
	UINT frame = vk_frame;

	// Whatever this write overlaps is no longer what was hashed
	auto i = map.lower_bound(offset);
	if (i != map.begin()) {
		auto previous = std::prev(i);
		if (previous->first + previous->second.bytes > offset)
			map.erase(previous);
	}
	while (i != map.end() && i->first < offset + bytes) {
		if (i->first == offset && i->second.bytes == bytes)
			++i;
		else
			i = map.erase(i);
	}

	BufferRange *range = &map[offset];
	if (range->bytes == bytes && frame - range->frame < HOT_FRAMES)
		range->hot++;
	else
		range->hot = 0;
	range->bytes = bytes;
	range->frame = frame;
	range->valid = false;

	return range->hot < HOT_LIMIT;
}

void VkDumpCopyBuffer(VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions)
{
	struct Hashed {
		VkDeviceSize offset, bytes;
		uint32_t data_hash;
	};
	uint32_t hashes[ARRAYSIZE(buffer_bind_flags)];
	std::vector<Hashed> hashed;
	VkDeviceSize buffer_bytes = 0;
	VkBufferUsageFlags usage = 0;
	Mapping mapping = {};
	bool mapped = false;
	FILE *file;
	uint32_t r;
	int i;

	AcquireSRWLockExclusive(&buffer_lock);

	auto to = buffers.find(dst);
	if (to == buffers.end()) {
		InterlockedAdd64(&buffer_stats.untracked, count);
		ReleaseSRWLockExclusive(&buffer_lock);
		return;
	}
	buffer_bytes = to->second.size;
	usage = to->second.usage;
	for (r = 0; r < count; r++) {
		WriteCount *written = &writes_by_usage[usage];
		written->copies++;
		written->copied_bytes += regions[r].size;
	}
	// Constants and the like are uploaded every frame and no mod matches them
	if (!(usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT))) {
		ReleaseSRWLockExclusive(&buffer_lock);
		return;
	}

	auto from = buffers.find(src);
	if (from != buffers.end() && from->second.memory) {
		auto found = mappings.find(from->second.memory);
		if (found != mappings.end() && found->second.data) {
			mapping = found->second;
			mapped = true;
		}
	}

	for (r = 0; r < count; r++) {
		const VkBufferCopy *region = &regions[r];
		VkDeviceSize start;

		if (!note_write(&to->second, region->dstOffset, region->size)) {
			InterlockedIncrement64(&buffer_stats.dynamic);
			continue;
		}
		if (!mapped) {
			InterlockedIncrement64(&buffer_stats.not_mapped);
			continue;
		}

		start = from->second.memory_offset + region->srcOffset;
		if (!region->size || region->size > MAX_HASHED_BYTES || start < mapping.offset
		    || (mapping.size != VK_WHOLE_SIZE && start + region->size > mapping.offset + mapping.size)
		    || region->srcOffset + region->size > from->second.size
		    || region->dstOffset + region->size > buffer_bytes) {
			InterlockedIncrement64(&buffer_stats.out_of_bounds);
			continue;
		}

		Hashed done = { region->dstOffset, region->size,
			Dx12Crc32cWriteCombined(0, mapping.data + (start - mapping.offset), (size_t)region->size) };
		hashed.push_back(done);

		BufferRange *range = &to->second.ranges[region->dstOffset];
		range->data_hash = done.data_hash;
		range->valid = true;

		InterlockedIncrement64(&buffer_stats.hashed);
		InterlockedAdd64(&buffer_stats.hashed_bytes, region->size);
	}

	ReleaseSRWLockExclusive(&buffer_lock);

	if (hashed.empty())
		return;

	AcquireSRWLockExclusive(&lock);

	file = csv(CSV_BUFFERS);
	for (const Hashed &done : hashed) {
		for (i = 0; i < ARRAYSIZE(buffer_bind_flags); i++) {
			hashes[i] = buffer_desc_hash(done.data_hash, done.bytes, buffer_bind_flags[i]);
			buffer_hashes[i].insert(hashes[i]);
		}
		if (file) {
			fprintf(file, "%u,%p,%llu,0x%x,%llu,%llu,%08x,%08x,%08x,%08x,%08x\n",
				vk_frame, dst, buffer_bytes, usage, done.offset, done.bytes, done.data_hash,
				hashes[0], hashes[1], hashes[2], hashes[3]);
		}
	}

	ReleaseSRWLockExclusive(&lock);
}

void VkDumpUpdateBuffer(VkBuffer dst, VkDeviceSize bytes)
{
	AcquireSRWLockExclusive(&buffer_lock);
	auto to = buffers.find(dst);
	if (to != buffers.end()) {
		WriteCount *written = &writes_by_usage[to->second.usage];
		written->updates++;
		written->updated_bytes += bytes;
	}
	ReleaseSRWLockExclusive(&buffer_lock);
}

void VkDumpFlush(uint32_t count, const VkMappedMemoryRange *ranges)
{
	uint32_t i;

	for (i = 0; i < count; i++) {
		InterlockedIncrement64(&buffer_stats.flushes);
		if (ranges[i].size != VK_WHOLE_SIZE)
			InterlockedAdd64(&buffer_stats.flushed_bytes, ranges[i].size);
	}
}

// -----------------------------------------------------------------------------

void VkDumpBeginCommands(VkCommandBuffer cmd)
{
	UINT shard = cmd_shard(cmd);

	AcquireSRWLockExclusive(&cmd_locks[shard]);
	cmd_states[shard][cmd] = {};
	ReleaseSRWLockExclusive(&cmd_locks[shard]);
}

void VkDumpBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
	UINT shard = cmd_shard(cmd);

	AcquireSRWLockExclusive(&cmd_locks[shard]);
	CmdState *state = &cmd_states[shard][cmd];
	if (bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS)
		state->graphics = pipeline;
	else if (bind_point == VK_PIPELINE_BIND_POINT_COMPUTE)
		state->compute = pipeline;
	ReleaseSRWLockExclusive(&cmd_locks[shard]);
}

void VkDumpBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets)
{
	UINT shard = cmd_shard(cmd);
	uint32_t i;

	if (!count)
		return;

	AcquireSRWLockExclusive(&cmd_locks[shard]);
	CmdState *state = &cmd_states[shard][cmd];
	if (first == 0) {
		state->vb0 = buffers[0];
		state->vb0_offset = offsets[0];
	}
	for (i = 0; i < count && first + i < ARRAYSIZE(state->vb); i++) {
		state->vb[first + i] = buffers[i];
		state->vb_offset[first + i] = offsets[i];
	}
	ReleaseSRWLockExclusive(&cmd_locks[shard]);
}

void VkDumpBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
	UINT shard = cmd_shard(cmd);

	AcquireSRWLockExclusive(&cmd_locks[shard]);
	CmdState *state = &cmd_states[shard][cmd];
	state->ib = buffer;
	state->ib_offset = offset;
	state->index_type = type;
	ReleaseSRWLockExclusive(&cmd_locks[shard]);
}

// The range a copy wrote that the offset is in
static bool find_range(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize *start, BufferRange *range)
{
	bool found = false;

	AcquireSRWLockShared(&buffer_lock);
	auto i = buffers.find(buffer);
	if (i != buffers.end()) {
		auto next = i->second.ranges.upper_bound(offset);
		if (next != i->second.ranges.begin()) {
			auto at = std::prev(next);
			if (at->second.valid && at->first + at->second.bytes > offset) {
				*start = at->first;
				*range = at->second;
				found = true;
			}
		}
	}
	ReleaseSRWLockShared(&buffer_lock);

	return found;
}

struct BufferFacts {
	VkDeviceSize bytes;
	VkBufferUsageFlags usage;
	// Whether the game has the memory behind it mapped, and so can write it
	// without Vulkan getting to know
	bool mapped;
};

static BufferFacts buffer_facts(VkBuffer buffer)
{
	BufferFacts facts = {};

	AcquireSRWLockShared(&buffer_lock);
	auto i = buffers.find(buffer);
	if (i != buffers.end()) {
		facts.bytes = i->second.size;
		facts.usage = i->second.usage;
		facts.mapped = i->second.memory && mappings.count(i->second.memory);
	}
	ReleaseSRWLockShared(&buffer_lock);

	return facts;
}

void VkDumpDraw(VkCommandBuffer cmd, const char *type, uint32_t count, uint32_t instances, uint32_t first, int32_t base_vertex)
{
	bool dispatch = !strcmp(type, "dispatch"), indexed = !strncmp(type, "indexed", 7);
	VkDeviceSize ib_start = 0, vb_start = 0;
	BufferRange ib_range = {}, vb_range = {};
	bool ib_hashed = false, vb_hashed = false, known;
	UINT shard = cmd_shard(cmd);
	PipelineInfo pipeline = {};
	BufferFacts ib = {}, vb = {};
	VkPipeline bound;
	CmdState state;
	FILE *file;

	AcquireSRWLockExclusive(&cmd_locks[shard]);
	CmdState *live = &cmd_states[shard][cmd];
	live->draws++;
	state = *live;
	ReleaseSRWLockExclusive(&cmd_locks[shard]);

	if (!dump_draws)
		return;

	if (!dispatch) {
		if (indexed && state.ib)
			ib_hashed = find_range(state.ib, state.ib_offset, &ib_start, &ib_range);
		if (state.vb0)
			vb_hashed = find_range(state.vb0, state.vb0_offset, &vb_start, &vb_range);
		if (state.ib)
			ib = buffer_facts(state.ib);
		if (state.vb0)
			vb = buffer_facts(state.vb0);
	}

	bound = dispatch ? state.compute : state.graphics;

	AcquireSRWLockExclusive(&lock);

	auto i = pipelines.find(bound);
	known = i != pipelines.end();
	if (known)
		pipeline = i->second;

	// An indirect draw does not say how much it draws
	if (!strcmp(type, "indexed") && state.ib && count) {
		UINT index_bytes = state.index_type == VK_INDEX_TYPE_UINT16 ? 2 : 4;
		IndexRegion region = { state.ib, state.ib_offset + (VkDeviceSize)first * index_bytes,
			(VkDeviceSize)count * index_bytes };
		ProbedRegion *probe = &probe_regions[region];

		probe->first = first;
		probe->count = count;
	}

	sample.draws++;
	sample.unknown_pipeline += !known;
	sample.indexed += indexed;
	sample.ib_hashed += ib_hashed;
	sample.vb_hashed += vb_hashed;

	file = csv(CSV_DRAWS);
	if (file) {
		fprintf(file, "%u,%p,%u,%s,%u,%u,%u,%d,%p,%016llx,%016llx,%016llx,"
			"%p,%llu,%d,%llu,%llu,%08x,"
			"%p,%llu,%u,%llu,%llu,%08x,%08x,"
			"%llu,0x%x,%d,%llu,0x%x,%d\n",
			vk_frame, cmd, state.draws, type, count, instances, first, base_vertex, bound,
			pipeline.shaders[STAGE_VS], pipeline.shaders[STAGE_PS], pipeline.shaders[STAGE_CS],
			dispatch ? NULL : state.ib, state.ib_offset, indexed ? state.index_type : -1, ib_start, ib_range.bytes,
			ib_hashed ? buffer_desc_hash(ib_range.data_hash, ib_range.bytes, D3D11_BIND_INDEX_BUFFER) : 0,
			dispatch ? NULL : state.vb0, state.vb0_offset, pipeline.stride0, vb_start, vb_range.bytes,
			vb_hashed ? buffer_desc_hash(vb_range.data_hash, vb_range.bytes, D3D11_BIND_VERTEX_BUFFER) : 0,
			vb_hashed ? buffer_desc_hash(vb_range.data_hash, vb_range.bytes,
				D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE) : 0,
			ib.bytes, ib.usage, ib.mapped, vb.bytes, vb.usage, vb.mapped);
	}

	ReleaseSRWLockExclusive(&lock);
}

// -----------------------------------------------------------------------------

// With the lock held, once the draws of a sampled frame are in
static void probe_index_buffers()
{
	FILE *file = csv(CSV_PROBE);

	AcquireSRWLockShared(&buffer_lock);

	for (auto &entry : probe_regions) {
		const IndexRegion *region = &entry.first;
		ProbedRegion *probe = &entry.second;
		VkDeviceSize start;
		uint32_t hash;

		auto info = buffers.find(region->buffer);
		if (info == buffers.end() || !info->second.memory || region->bytes > PROBE_MAX_BYTES
		    || region->offset + region->bytes > info->second.size) {
			probe_unmapped += !probe->hashed;
			continue;
		}
		auto mapping = mappings.find(info->second.memory);
		start = info->second.memory_offset + region->offset;
		if (mapping == mappings.end() || !mapping->second.data || start < mapping->second.offset
		    || (mapping->second.size != VK_WHOLE_SIZE
			&& start + region->bytes > mapping->second.offset + mapping->second.size)) {
			probe_unmapped += !probe->hashed;
			continue;
		}

		hash = Dx12Crc32cWriteCombined(0, mapping->second.data + (start - mapping->second.offset), (size_t)region->bytes);
		if (probe->hashed && probe->hash == hash)
			continue;
		region_hashes.insert(hash);

		// A line for every hash a region has had
		if (file) {
			fprintf(file, "%u,%p,%llu,%llu,%u,%u,%08x,%08x\n", vk_frame, region->buffer, region->offset, region->bytes,
				probe->first, probe->count, hash, probe->hashed ? probe->hash : 0);
		}

		if (probe->hashed)
			probe_changed++;
		else
			probed++;
		probe->hash = hash;
		probe->hashed = true;
	}

	ReleaseSRWLockShared(&buffer_lock);

	if (file)
		fflush(file);
}

template <class Seen, class Map>
static size_t count_in_ini(const Seen &seen, const Map &map)
{
	size_t count = 0;

	for (auto hash : seen)
		count += map.count(hash) != 0;
	return count;
}

#define REPORT(fmt, ...) \
	do { \
		VkLogInfo(fmt, __VA_ARGS__); \
		if (report) \
			fprintf(report, fmt, __VA_ARGS__); \
	} while (0)

// The configuration is read here on the thread that presents with Vulkan,
// while reloading it happens on whichever thread runs the engine. Only a
// reload in the very moment of a report could trip over that.
static void hash_report()
{
	FILE *report;
	int i;

	AcquireSRWLockExclusive(&lock);

	report = csv(CSV_REPORT);
	if (report)
		fprintf(report, "Frame %u\n", vk_frame);

	REPORT("  %u shader modules created, %Iu distinct (%u not SPIR-V, %u without any names left), %u graphics and %u compute pipelines (%u stages without a known module). ShaderOverride hashes seen: %Iu of %Iu\n",
		modules_created, shader_hashes.size(), modules_invalid, modules_unnamed, graphics_pipelines, compute_pipelines,
		stages_without_module, count_in_ini(shader_hashes, G->mShaderOverrideMap), G->mShaderOverrideMap.size());

	REPORT("  Vertex and index buffer uploads: %lld hashed (%.1f MB). Not hashed: %lld dynamic, %lld source not mapped, %lld out of bounds. %lld copies to buffers we did not see created\n",
		buffer_stats.hashed, buffer_stats.hashed_bytes / 1048576.0, buffer_stats.dynamic, buffer_stats.not_mapped,
		buffer_stats.out_of_bounds, buffer_stats.untracked);

	REPORT("  Memory: %lld maps, %lld flushes of %.1f MB. Buffer writes through Vulkan by the usage of the buffer written:\n",
		buffer_stats.maps, buffer_stats.flushes, buffer_stats.flushed_bytes / 1048576.0);
	AcquireSRWLockShared(&buffer_lock);
	for (const auto &written : writes_by_usage) {
		REPORT("    usage 0x%x: %lld copies of %.1f MB, %lld updates of %.1f MB\n", written.first,
			written.second.copies, written.second.copied_bytes / 1048576.0,
			written.second.updates, written.second.updated_bytes / 1048576.0);
	}
	ReleaseSRWLockShared(&buffer_lock);

	REPORT("  TextureOverride hashes: %Iu in the configuration, %Iu distinct buffer hashes seen\n",
		G->mTextureOverrideMap.size(), buffer_hashes[0].size());
	for (i = 0; i < ARRAYSIZE(buffer_bind_flags); i++) {
		REPORT("    %Iu matched by buffers hashed as %s\n",
			count_in_ini(buffer_hashes[i], G->mTextureOverrideMap), buffer_bind_names[i]);
	}

	REPORT("  Index regions of draws read from mapped memory: %u hashed, %u not mapped, %u times one had changed since. %Iu distinct hashes, %Iu of them a TextureOverride hash\n",
		probed, probe_unmapped, probe_changed, region_hashes.size(),
		count_in_ini(region_hashes, G->mTextureOverrideMap));

	if (sample_finished) {
		sample_finished = false;
		REPORT("  Frame %u: %u draws and dispatches, %u with an unknown pipeline, %u indexed, index buffer hashed for %u, vb0 for %u\n",
			sample.frame, sample.draws, sample.unknown_pipeline, sample.indexed, sample.ib_hashed, sample.vb_hashed);
	}

	for (i = 0; i < CSV_COUNT; i++) {
		if (csv_files[i])
			fflush(csv_files[i]);
	}

	sample_requested = true;

	ReleaseSRWLockExclusive(&lock);
}

void VkDumpPresent()
{
	static UINT64 last_report;
	UINT64 now = GetTickCount64();

	InterlockedIncrement((volatile LONG*)&vk_frame);

	if (dump_draws || sample_requested) {
		AcquireSRWLockExclusive(&lock);
		if (dump_draws) {
			dump_draws = false;
			sample_finished = true;
			if (csv_files[CSV_DRAWS])
				fflush(csv_files[CSV_DRAWS]);
			probe_index_buffers();
		} else {
			sample_requested = false;
			sample = {};
			sample.frame = vk_frame;
			dump_draws = true;
		}
		ReleaseSRWLockExclusive(&lock);
	}

	if (now - last_report < REPORT_INTERVAL_MS)
		return;
	last_report = now;

	hash_report();
}
