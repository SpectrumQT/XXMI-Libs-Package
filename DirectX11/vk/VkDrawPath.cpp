#include "VkDrawPath.h"

#include <immintrin.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Spirv.h"
#include "VkDump.h"
#include "VkScope.h"

#include "../dx12/Bridge.h"
#include "../dx12/DrawPath.h"
#include "../dx12/ShimContext.h"
#include "../dx12/ShimDevice.h"
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
	SLOW_CONSTANTS,
	SLOW_REASONS
};

// Since the last report, with the lock of dx12/Bridge.h held
static struct {
	unsigned draws, skipped, mod_draws, mod_draws_failed;
	unsigned slow[SLOW_REASONS];
	unsigned regions_hashed, regions_checked, regions_unmapped;
	unsigned mirrored;
	UINT64 mirrored_bytes;
	unsigned constants_shown, constants_read, constants_unmapped, constants_unbound;
	UINT64 constants_bytes, constants_ticks;
	unsigned views_shown, views_missing;
	// Buffers of mods brought over for a shader of the game to read, of
	// which from the GPU, and what that took
	unsigned brought, brought_from_gpu, sets_made, submits_waited;
	UINT64 brought_bytes, submit_ticks;
	unsigned variants_made, variants_failed;
	UINT64 variant_ticks;
	unsigned textures_made, textures_failed, textures_set;
	unsigned apart_draws, textures_shown;
	UINT64 textures_bytes;
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

// What a shader reads, by the register a D3D11 one would have it at. Vulkan
// has none for a mod to name it by, a shader says which binding of which
// descriptor set. So the uniform blocks of a stage are taken for its cb0, cb1
// and so on in the order the shader declares them, and the textures and
// buffers it only reads for its t0, t1 and so on likewise. That is where the
// mods of the game this was written for expect what they look for.
struct ShaderLayout {
	std::vector<SpirvBinding> cbs, views;
	// What the assembly of the shader would declare were it a D3D11 one,
	// for the patterns of the ShaderRegex sections
	std::string declarations;
	// The same as the engine has it of a D3D11 shader, once it asked. With
	// the lock of dx12/Bridge.h held.
	ShaderBindings *declared;
};

// What a pipeline was created from, with everything it points at
struct PipelineRecipe {
	std::vector<std::unique_ptr<BYTE[]>> memory;
	VkGraphicsPipelineCreateInfo info;
};

// A pipeline made of that for a mod: by the input layout the mod has set, 0
// for the game's, and the strides of its vertex buffers. No pipeline if none
// could be made.
struct PipelineVariant {
	UINT64 layout;
	UINT strides[MAX_VBS];
	VkPipeline pipeline;
};

struct Pipeline {
	VkPipeline handle;
	// NULL if it could not be kept. This and the variants with
	// registry_lock held.
	std::shared_ptr<PipelineRecipe> recipe;
	std::vector<PipelineVariant> variants;
	// By Dx12ShaderStage, 0 where it has none
	UINT64 shaders[DX12_STAGE_COUNT];
	ShaderLayout *layouts[DX12_STAGE_COUNT];
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
// By hash, and kept: pipelines point at them
static std::unordered_map<UINT64, ShaderLayout*> shader_layouts;
static std::unordered_map<VkPipeline, Pipeline*> pipelines;
// A command buffer may be left pointing at the pipeline it had bound when
// that is destroyed, so these are never freed, only used again
static std::vector<Pipeline*> spare_pipelines;
// The variants of pipelines the game has destroyed, until the next frame
static std::vector<VkPipeline> dead_variants;
// For the report: pipelines kept to be made again, those that could not be,
// and the first structure met that is not known here
static unsigned recipes_kept, recipes_failed;
static volatile LONG recipe_unknown_structure;
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
// and how many of them a section took for what they declare
static size_t matched_shaders;

// A mod tells the shaders of a game apart by what their assembly declares:
// how far a constant buffer is read and whether with an index, the registers
// and kinds of the views, the last output. A SPIR-V shader translated from
// the same source says as much, so it is written out the way the D3D11
// disassembler would. The position is taken for the first output of a vertex
// shader and the first input of a pixel shader, the locations for the
// registers that follow.
static void declare_as_d3d11(const SpirvInfo &info, ShaderLayout *layout)
{
	static const char *const masks[] = { "x", "xy", "xyz", "xyzw" };
	std::string *text = &layout->declarations;
	int stage = stage_of((VkShaderStageFlagBits)info.stage);
	std::vector<SpirvVarying> in = info.in, out = info.out;
	UINT samplers = 0, i;
	char line[128];

	if (stage < 0)
		return;
	auto by_location = [](const SpirvVarying &a, const SpirvVarying &b) { return a.location < b.location; };
	std::sort(in.begin(), in.end(), by_location);
	std::sort(out.begin(), out.end(), by_location);

	_snprintf_s(line, _TRUNCATE, "%s_5_0\ndcl_globalFlags refactoringAllowed\n", dx12_stage_names[stage]);
	*text = line;
	for (i = 0; i < layout->cbs.size(); i++) {
		if (!layout->cbs[i].used)
			continue;
		_snprintf_s(line, _TRUNCATE, "dcl_constantbuffer CB%u[%u], %s\n", i, (layout->cbs[i].used + 15) / 16,
			layout->cbs[i].dynamic ? "dynamicIndexed" : "immediateIndexed");
		*text += line;
	}
	for (const SpirvBinding &binding : info.bindings) {
		if (binding.kind != SPIRV_SAMPLER && binding.kind != SPIRV_TEXTURE_SAMPLER)
			continue;
		_snprintf_s(line, _TRUNCATE, "dcl_sampler s%u, mode_default\n", samplers++);
		*text += line;
	}
	for (i = 0; i < layout->views.size(); i++) {
		const SpirvBinding &view = layout->views[i];

		if (view.kind == SPIRV_SSBO)
			_snprintf_s(line, _TRUNCATE, "dcl_resource_structured t%u, %u\n", i, view.stride);
		else
			_snprintf_s(line, _TRUNCATE, "dcl_resource_%s (float,float,float,float) t%u\n",
				view.kind == SPIRV_TEXEL_BUFFER ? "buffer" : view.dim == 3 ? "texturecube"
				: view.dim == 2 ? "texture3d" : view.arrayed ? "texture2darray" : "texture2d", i);
		*text += line;
	}
	if (stage == DX12_STAGE_PS)
		*text += "dcl_input_ps_siv linear noperspective v0.xyzw, position\n";
	for (const SpirvVarying &varying : in) {
		_snprintf_s(line, _TRUNCATE, "dcl_input%s v%u.%s\n", stage == DX12_STAGE_PS ? "_ps linear" : "",
			varying.location + (stage == DX12_STAGE_PS), masks[varying.components - 1]);
		*text += line;
	}
	if (stage == DX12_STAGE_VS)
		*text += "dcl_output_siv o0.xyzw, position\n";
	for (const SpirvVarying &varying : out) {
		_snprintf_s(line, _TRUNCATE, "dcl_output o%u.%s\n", varying.location + (stage == DX12_STAGE_VS),
			masks[varying.components - 1]);
		*text += line;
	}
	*text += "dcl_temps 1\n";
}

static ShaderLayout* reflect_shader(const uint32_t *code, size_t bytes)
{
	ShaderLayout *layout = new ShaderLayout();
	SpirvInfo info;

	SpirvReflect(code, bytes / sizeof(uint32_t), &info);
	for (const SpirvBinding &binding : info.bindings) {
		switch (binding.kind) {
			case SPIRV_UBO:
				layout->cbs.push_back(binding);
				break;
			case SPIRV_SSBO:
				// One the shader writes would be at a u register
				if (!binding.read_only)
					break;
			case SPIRV_TEXTURE:
			case SPIRV_TEXTURE_SAMPLER:
			case SPIRV_TEXEL_BUFFER:
				layout->views.push_back(binding);
				break;
		}
	}
	layout->declared = NULL;
	declare_as_d3d11(info, layout);
	return layout;
}

void VkModsShaderModule(VkShaderModule module, const VkShaderModuleCreateInfo *info)
{
	ShaderLayout *layout;
	UINT64 hash;
	bool known;

	if (!info || !info->pCode || !info->codeSize)
		return;
	hash = fnv_64_buf(info->pCode, info->codeSize);

	AcquireSRWLockExclusive(&registry_lock);
	shader_modules[module] = hash;
	known = !!shader_layouts.count(hash);
	ReleaseSRWLockExclusive(&registry_lock);
	if (known)
		return;

	layout = reflect_shader(info->pCode, info->codeSize);
	AcquireSRWLockExclusive(&registry_lock);
	if (!shader_layouts.emplace(hash, layout).second)
		delete layout;
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

template <typename T> static T* keep(PipelineRecipe *recipe, const T *from, size_t count = 1)
{
	BYTE *copy;

	if (!from || !count)
		return NULL;
	copy = new BYTE[count * sizeof(T)];
	memcpy(copy, from, count * sizeof(T));
	recipe->memory.emplace_back(copy);
	return (T*)copy;
}

// The structures a chain may have that are kept as they are
static const struct {
	VkStructureType type;
	size_t size;
} flat_structures[] = {
	{ VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, sizeof(VkPipelineCreateFlags2CreateInfoKHR) },
	{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT, sizeof(VkPipelineRasterizationDepthClipStateCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT, sizeof(VkPipelineRasterizationLineStateCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT, sizeof(VkPipelineRasterizationProvokingVertexStateCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT, sizeof(VkPipelineRasterizationConservativeStateCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO, sizeof(VkPipelineTessellationDomainOriginStateCreateInfo) },
	{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT, sizeof(VkPipelineColorBlendAdvancedStateCreateInfoEXT) },
	{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, sizeof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo) },
	{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, sizeof(VkPipelineViewportDepthClipControlCreateInfoEXT) },
};

// False for a chain with something in it that is not known here
static bool keep_chain(PipelineRecipe *recipe, const void **chain)
{
	const VkBaseInStructure *next = (const VkBaseInStructure*)*chain;
	VkBaseInStructure *copy, *last = NULL;
	const void *first = NULL;

	for (; next; next = next->pNext) {
		copy = NULL;
		// Written to by the device
		if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO)
			continue;
		if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
			VkPipelineRenderingCreateInfo *rendering = keep(recipe, (const VkPipelineRenderingCreateInfo*)next);

			rendering->pColorAttachmentFormats = keep(recipe, rendering->pColorAttachmentFormats, rendering->colorAttachmentCount);
			copy = (VkBaseInStructure*)rendering;
		}
		for (const auto &flat : flat_structures) {
			if (next->sType == flat.type)
				copy = (VkBaseInStructure*)keep(recipe, (const BYTE*)next, flat.size);
		}
		if (!copy) {
			InterlockedCompareExchange(&recipe_unknown_structure, next->sType, 0);
			return false;
		}

		copy->pNext = NULL;
		if (last)
			last->pNext = copy;
		else
			first = copy;
		last = copy;
	}

	*chain = first;
	return true;
}

static bool keep_recipe_parts(PipelineRecipe *recipe, const VkGraphicsPipelineCreateInfo *from)
{
	VkGraphicsPipelineCreateInfo *info = &recipe->info;
	bool dynamic_viewports = false, dynamic_scissors = false;
	VkPipelineShaderStageCreateInfo *stages;
	uint32_t i;

	*info = *from;
	// Made by itself, and not to be had only if it is there already
	info->flags &= ~(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
		| VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
	info->basePipelineHandle = VK_NULL_HANDLE;
	info->basePipelineIndex = -1;
	if (!keep_chain(recipe, &info->pNext))
		return false;
	if (!from->pStages || !from->stageCount || !from->pVertexInputState || !from->pInputAssemblyState || !from->pRasterizationState)
		return false;

	stages = keep(recipe, from->pStages, from->stageCount);
	info->pStages = stages;
	for (i = 0; i < from->stageCount; i++) {
		if (!stages[i].module || !stages[i].pName || !keep_chain(recipe, &stages[i].pNext))
			return false;
		stages[i].pName = keep(recipe, stages[i].pName, strlen(stages[i].pName) + 1);
		if (stages[i].pSpecializationInfo) {
			VkSpecializationInfo *constants = keep(recipe, stages[i].pSpecializationInfo);

			constants->pMapEntries = keep(recipe, constants->pMapEntries, constants->mapEntryCount);
			constants->pData = keep(recipe, (const BYTE*)constants->pData, constants->dataSize);
			stages[i].pSpecializationInfo = constants;
		}
	}

	VkPipelineVertexInputStateCreateInfo *input = keep(recipe, from->pVertexInputState);
	info->pVertexInputState = input;
	if (input->pNext)
		return false;
	input->pVertexBindingDescriptions = keep(recipe, input->pVertexBindingDescriptions, input->vertexBindingDescriptionCount);
	input->pVertexAttributeDescriptions = keep(recipe, input->pVertexAttributeDescriptions, input->vertexAttributeDescriptionCount);

	VkPipelineInputAssemblyStateCreateInfo *assembly = keep(recipe, from->pInputAssemblyState);
	info->pInputAssemblyState = assembly;
	if (assembly->pNext)
		return false;

	if (from->pTessellationState) {
		VkPipelineTessellationStateCreateInfo *tessellation = keep(recipe, from->pTessellationState);

		info->pTessellationState = tessellation;
		if (!keep_chain(recipe, &tessellation->pNext))
			return false;
	}

	VkPipelineRasterizationStateCreateInfo *raster = keep(recipe, from->pRasterizationState);
	info->pRasterizationState = raster;
	if (!keep_chain(recipe, &raster->pNext))
		return false;

	if (from->pDynamicState) {
		VkPipelineDynamicStateCreateInfo *dynamic = keep(recipe, from->pDynamicState);

		info->pDynamicState = dynamic;
		if (dynamic->pNext)
			return false;
		dynamic->pDynamicStates = keep(recipe, dynamic->pDynamicStates, dynamic->dynamicStateCount);
		for (i = 0; i < dynamic->dynamicStateCount; i++) {
			switch (dynamic->pDynamicStates[i]) {
				case VK_DYNAMIC_STATE_VIEWPORT:
				case VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT:
					dynamic_viewports = true;
					break;
				case VK_DYNAMIC_STATE_SCISSOR:
				case VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT:
					dynamic_scissors = true;
					break;
				// The vertex input is not the pipeline's then
				case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
				case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:
					return false;
			}
		}
	}

	if (from->pViewportState) {
		VkPipelineViewportStateCreateInfo *viewport = keep(recipe, from->pViewportState);

		info->pViewportState = viewport;
		if (!keep_chain(recipe, &viewport->pNext))
			return false;
		// Not looked at where they are set while the pipeline is bound
		viewport->pViewports = dynamic_viewports ? NULL : keep(recipe, viewport->pViewports, viewport->viewportCount);
		viewport->pScissors = dynamic_scissors ? NULL : keep(recipe, viewport->pScissors, viewport->scissorCount);
	}
	if (from->pMultisampleState) {
		VkPipelineMultisampleStateCreateInfo *multisample = keep(recipe, from->pMultisampleState);

		info->pMultisampleState = multisample;
		if (multisample->pNext)
			return false;
		multisample->pSampleMask = keep(recipe, multisample->pSampleMask, ((size_t)multisample->rasterizationSamples + 31) / 32);
	}
	if (from->pDepthStencilState) {
		VkPipelineDepthStencilStateCreateInfo *depth = keep(recipe, from->pDepthStencilState);

		info->pDepthStencilState = depth;
		if (depth->pNext)
			return false;
	}
	if (from->pColorBlendState) {
		VkPipelineColorBlendStateCreateInfo *blend = keep(recipe, from->pColorBlendState);

		info->pColorBlendState = blend;
		if (!keep_chain(recipe, &blend->pNext))
			return false;
		blend->pAttachments = keep(recipe, blend->pAttachments, blend->attachmentCount);
	}
	return true;
}

// A game need not give a valid pointer for a state its pipeline does not use
static bool keep_recipe(PipelineRecipe *recipe, const VkGraphicsPipelineCreateInfo *from)
{
	__try {
		return keep_recipe_parts(recipe, from);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
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
	ShaderLayout *layouts[DX12_STAGE_COUNT] = {};
	std::shared_ptr<PipelineRecipe> recipe;
	Pipeline *entry;
	uint32_t i;
	int stage;

	if (!library) {
		recipe = std::make_shared<PipelineRecipe>();
		if (!keep_recipe(recipe.get(), info))
			recipe.reset();
	}

	AcquireSRWLockExclusive(&registry_lock);
	if (recipe)
		recipes_kept++;
	else
		recipes_failed++;

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
			if (!shader_layouts.count(shaders[stage]))
				shader_layouts[shaders[stage]] = reflect_shader(code->pCode, code->codeSize);
		}

		if (shaders[stage]) {
			UINT *stages = &seen_stages[shaders[stage]];
			auto layout = shader_layouts.find(shaders[stage]);

			if (layout != shader_layouts.end())
				layouts[stage] = layout->second;

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
	entry->handle = pipeline;
	entry->recipe = recipe;
	memcpy(entry->shaders, shaders, sizeof(shaders));
	memcpy(entry->layouts, layouts, sizeof(layouts));
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
		for (const PipelineVariant &variant : found->second->variants) {
			if (variant.pipeline)
				dead_variants.push_back(variant.pipeline);
		}
		found->second->variants.clear();
		found->second->recipe.reset();
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
static void forget_dead_constants(VkBuffer buffer);

static void forget_dead_buffers()
{
	std::vector<VkBuffer> dead;

	if (!any_published)
		return;

	AcquireSRWLockExclusive(&memory_lock);
	dead.swap(dead_buffers);
	ReleaseSRWLockExclusive(&memory_lock);

	for (VkBuffer buffer : dead) {
		forget_dead_constants(buffer);
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

// What the engine keeps of a resource it is to know
static void register_buffer(ShimBuffer *object, uint32_t hash)
{
	// Has the engine's record of the object removed along with it
	new ResourceReleaseTracker(object);

	EnterCriticalSectionPretty(&G->mResourcesLock);
	ResourceHandleInfo *handle = &G->mResources[object];
	handle->type = D3D11_RESOURCE_DIMENSION_BUFFER;
	handle->hash = hash;
	handle->orig_hash = hash;
	handle->data_hash = 0;
	handle->texture_override_candidates_valid = false;
	LeaveCriticalSection(&G->mResourcesLock);
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
	// No hash to match by: the buffer as a whole is not what a mesh is
	register_buffer(object, 0);

	PublishedBuffer *entry = &published[buffer];
	entry->object = object;
	return entry;
}

// -----------------------------------------------------------------------------
// The constant buffers of the game
//
// A uniform block a draw reads is some bytes of a buffer the game writes
// through mapped memory, other ones for every draw. A storage buffer is the
// same with a stride, and is shown as a structured buffer with a view. The engine is given them
// as a constant buffer of the shim that has those bytes for its contents, like
// one a mod would have filled itself: a mod can read values from it, take the
// hash of a part or bind it elsewhere, also to a shader of its own that runs
// on the D3D12 device. There is one for every place and size asked about, for
// as long as the game keeps drawing with that.
//
// Most that are asked about are only asked how large they are, and reading
// all of them took longer than everything else. So the contents are read when
// something wants them: by the engine a page at a time (a lazy data cache),
// and by the shim all of it (ShimReadShadow). Once a frame at most: the game
// is taken to have written what a draw reads before recording the draw, and
// not to write there again before the frame is done.

// Frames one is kept for without a draw asking for it
const UINT CONSTANTS_KEEP_FRAMES = 4;

struct PublishedConstants {
	ShimBuffer *object;
	// Of a storage buffer
	ID3D11ShaderResourceView *srv;
	UINT used_frame;
	bool shown;
};

typedef std::tuple<VkBuffer, VkDeviceSize, UINT, UINT> ConstantsKey;

// By buffer, offset, size and the stride of a storage buffer, with the lock
// of dx12/Bridge.h held
static std::map<ConstantsKey, PublishedConstants> published_constants;

// Where the shadow of one is read from, and the pages of it that have been
const UINT SHADOW_PAGE = 4096;

struct ConstantsSource {
	ConstantsKey key;
	std::vector<bool> pages;
	UINT pages_left;
};

static std::unordered_map<ShimResource*, ConstantsSource> constants_sources;

static void forget_constants(std::map<ConstantsKey, PublishedConstants>::iterator *entry)
{
	ShimBuffer *object = (*entry)->second.object;

	// A mod may keep it
	object->r.shadow_pending = false;
	object->r.upload_pending = false;
	constants_sources.erase(&object->r);
	if ((*entry)->second.srv)
		(*entry)->second.srv->Release();
	object->Release();
	*entry = published_constants.erase(*entry);
}

static void forget_dead_constants(VkBuffer buffer)
{
	auto i = published_constants.lower_bound(std::make_tuple(buffer, (VkDeviceSize)0, 0u, 0u));

	while (i != published_constants.end() && std::get<0>(i->first) == buffer)
		forget_constants(&i);
}

static void forget_unused_constants(bool all)
{
	UINT frame = vk_frame;

	for (auto i = published_constants.begin(); i != published_constants.end();) {
		if (all || frame - i->second.used_frame > CONSTANTS_KEEP_FRAMES)
			forget_constants(&i);
		else
			++i;
	}
}

// From where a part of copy_mapped is worth a thread of its own, and how many
// there are
const size_t COPY_THREAD_BYTES = 1 << 20;
const size_t COPY_THREADS = 4;

// src is aligned to 32 bytes, and bytes a multiple of 32
static void copy_mapped_wide(BYTE *dst, const BYTE *src, size_t bytes)
{
	for (; bytes; dst += 32, src += 32, bytes -= 32)
		_mm256_storeu_si256((__m256i*)dst, _mm256_stream_load_si256((const __m256i*)src));
	_mm256_zeroupper();
}

// The threads that take the parts of a large copy. They are kept: starting
// one in a game takes about as long as the copy it is for. Whoever copies has
// the lock of dx12/Bridge.h, so there is one copy at a time.
static struct CopyWorker {
	HANDLE thread, start, done;
	BYTE *dst;
	const BYTE *src;
	size_t bytes;
} copy_workers[COPY_THREADS - 1];

static DWORD WINAPI copy_worker(void *argument)
{
	CopyWorker *worker = (CopyWorker*)argument;

	for (;;) {
		WaitForSingleObject(worker->start, INFINITE);
		copy_mapped_wide(worker->dst, worker->src, worker->bytes);
		SetEvent(worker->done);
	}
}

static bool start_copy_workers()
{
	static bool tried;

	if (tried)
		return !!copy_workers[COPY_THREADS - 2].thread;
	tried = true;

	for (CopyWorker &worker : copy_workers) {
		worker.start = CreateEvent(NULL, FALSE, FALSE, NULL);
		worker.done = CreateEvent(NULL, FALSE, FALSE, NULL);
		worker.thread = worker.start && worker.done ? CreateThread(NULL, 0, copy_worker, &worker, 0, NULL) : NULL;
		// Those started stay idle
		if (!worker.thread)
			return false;
	}
	return true;
}

// The memory a device lets be mapped is write combined as a rule, which takes
// hundreds of times as long to read the ordinary way as it does like this.
// Memory of the GPU itself is slower still, and what is read of that is the
// largest there is to read: several megabytes of bones every frame. Wider
// loads and more threads each make up for a part of it.
static void copy_mapped(BYTE *dst, const BYTE *src, size_t bytes)
{
	static const bool wide = !!IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE);
	size_t head = min(bytes, (32 - ((size_t)src & 31)) & 31);
	size_t body, part, i;

	if (!wide) {
		head = min(bytes, (16 - ((size_t)src & 15)) & 15);
		memcpy(dst, src, head);
		for (dst += head, src += head, bytes -= head; bytes >= 16; dst += 16, src += 16, bytes -= 16)
			_mm_storeu_si128((__m128i*)dst, _mm_stream_load_si128((__m128i*)src));
		memcpy(dst, src, bytes);
		return;
	}

	memcpy(dst, src, head);
	dst += head;
	src += head;
	bytes -= head;
	body = bytes & ~(size_t)31;

	if (body >= COPY_THREAD_BYTES * 2 && start_copy_workers()) {
		part = (body / COPY_THREADS) & ~(size_t)31;
		for (i = 0; i < COPY_THREADS - 1; i++) {
			copy_workers[i].dst = dst + i * part;
			copy_workers[i].src = src + i * part;
			copy_workers[i].bytes = part;
			SetEvent(copy_workers[i].start);
		}
		copy_mapped_wide(dst + i * part, src + i * part, body - i * part);
		for (CopyWorker &worker : copy_workers)
			WaitForSingleObject(worker.done, INFINITE);
	} else {
		copy_mapped_wide(dst, src, body);
	}
	memcpy(dst + body, src + body, bytes - body);
}

// What the game has there now, zeroes if it no longer has it mapped. With the
// lock of dx12/Bridge.h held, as everything is that runs the engine.
static void read_constants(BYTE *dst, VkBuffer buffer, VkDeviceSize offset, size_t bytes)
{
	UINT64 start = G->vk_dump_hashes ? ticks() : 0;
	const BYTE *data;

	AcquireSRWLockShared(&memory_lock);
	data = mapped_bytes(buffer, offset, bytes);
	if (data)
		copy_mapped(dst, data, bytes);
	else
		memset(dst, 0, bytes);
	ReleaseSRWLockShared(&memory_lock);

	stats.constants_read++;
	stats.constants_bytes += bytes;
	if (start)
		stats.constants_ticks += ticks() - start;
}

void VkScopeFillShadow(ShimResource *resource, UINT64 offset, UINT64 size)
{
	auto found = constants_sources.find(resource);
	ConstantsSource *source;
	UINT bytes, page, start;
	bool read = false;

	if (found == constants_sources.end()) {
		resource->shadow_pending = false;
		return;
	}
	source = &found->second;
	bytes = std::get<2>(source->key);
	if (offset >= bytes)
		return;
	size = min(size, bytes - offset);

	for (page = (UINT)(offset / SHADOW_PAGE); (UINT64)page * SHADOW_PAGE < offset + size; page++) {
		if (source->pages[page])
			continue;
		// With those after it in one go: a large read is a faster one
		start = page * SHADOW_PAGE;
		for (; (UINT64)page * SHADOW_PAGE < offset + size && !source->pages[page]; page++) {
			source->pages[page] = true;
			source->pages_left--;
		}
		read_constants(resource->shadow.data() + start, std::get<0>(source->key), std::get<1>(source->key) + start,
			min(page * SHADOW_PAGE, bytes) - start);
		read = true;
	}
	if (read)
		resource->version++;
	if (!source->pages_left)
		resource->shadow_pending = false;
}

static PublishedConstants* publish_constants(VkBuffer buffer, VkDeviceSize offset, UINT bytes, UINT stride)
{
	ConstantsKey key = std::make_tuple(buffer, offset, bytes, stride);
	auto found = published_constants.find(key);
	D3D11_SHADER_RESOURCE_VIEW_DESC view_desc = {};
	ID3D11ShaderResourceView *srv = NULL;
	D3D11_BUFFER_DESC desc = {};
	ID3D11Buffer *created = NULL;
	ShimBuffer *object;
	bool mapped;

	if (found != published_constants.end() && found->second.shown && found->second.used_frame == vk_frame) {
		stats.constants_shown++;
		return &found->second;
	}

	AcquireSRWLockShared(&memory_lock);
	mapped = !!mapped_bytes(buffer, offset, bytes);
	any_published = true;
	ReleaseSRWLockShared(&memory_lock);
	if (!mapped) {
		stats.constants_unmapped++;
		return NULL;
	}

	if (found == published_constants.end()) {
		desc.ByteWidth = (bytes + 15) & ~15u;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (stride) {
			// With a resource: what reads it is a shader of a mod
			desc.ByteWidth = bytes;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = stride;
		}
		if (FAILED(dx12_shim->shim_device->CreateBuffer(&desc, NULL, &created)))
			return NULL;
		object = static_cast<ShimBuffer*>(created);
		if (stride) {
			view_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			view_desc.Buffer.NumElements = bytes / stride;
			if (FAILED(dx12_shim->shim_device->CreateShaderResourceView(object, &view_desc, &srv))) {
				object->Release();
				return NULL;
			}
		}
		// What a D3D11 game's buffer of the size would have for its hash
		register_buffer(object, crc32c_hw(0, &desc, sizeof(desc)));
		found = published_constants.emplace(key, PublishedConstants{ object, srv, 0, false }).first;
		constants_sources[&object->r].key = key;
	}
	object = found->second.object;
	found->second.used_frame = vk_frame;
	found->second.shown = true;
	stats.constants_shown++;

	// Other contents than the frame before, as far as anyone knows
	ConstantsSource *source = &constants_sources[&object->r];
	source->pages.assign((bytes + SHADOW_PAGE - 1) / SHADOW_PAGE, false);
	source->pages_left = (UINT)source->pages.size();
	object->r.shadow_pending = true;
	object->r.upload_pending = !object->r.cpu_only;
	object->r.version++;
	// A storage buffer is large, and nothing has asked for values of one
	if (stride)
		return &found->second;

	EnterCriticalSectionPretty(&G->mCriticalSection);
	ResourceHandleInfo *handle = GetResourceHandleInfo(object);
	if (handle) {
		handle->SetLazyDataCache(bytes, [buffer, offset](uint8_t *dst, size_t at, size_t size) {
			read_constants(dst, buffer, offset + at, size);
		});
	}
	LeaveCriticalSection(&G->mCriticalSection);
	return &found->second;
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

// -----------------------------------------------------------------------------
// What a mod has a shader of the game read
//
// Constants a mod set for the vertex shader, or a buffer with the bones of a
// skeleton it put together, in place of a buffer of the game. Those are other
// contents for every draw, and as a rule ones the GPU wrote: with a shader of
// the mod, which runs on the D3D12 device. So each gets a place of its own in
// a buffer of the game's device, given out again some frames later, and what
// is not known here is copied out of the D3D12 resource where the list of the
// shim has got to. That copy is there once the list has been executed, which
// the game's handing its command buffers to a queue waits for
// (VkModsBeforeSubmit).

const VkDeviceSize TRANSIENT_BYTES = 4 << 20;
// More than a device asks a view of a buffer to be aligned to
const VkDeviceSize TRANSIENT_ALIGNMENT = 256;

struct Transient {
	VkBuffer buffer;
	MirrorBlock *block;
	BYTE *data;
	VkDeviceSize size, used;
	// The last it had anything given out
	UINT frame;
};

static std::vector<Transient> transients;

static Transient* new_transient(VkDeviceSize size)
{
	VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	VkMemoryRequirements needed;
	Transient transient = {};
	VkDeviceSize offset;
	Mirror placed = {};

	info.size = max(size, TRANSIENT_BYTES);
	info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (api.CreateBuffer(mods_device, &info, NULL, &transient.buffer) != VK_SUCCESS)
		return NULL;

	api.GetBufferMemoryRequirements(mods_device, transient.buffer, &needed);
	if (!place_mirror(&placed, &needed, &offset)) {
		api.DestroyBuffer(mods_device, transient.buffer, NULL);
		return NULL;
	}
	if (api.BindBufferMemory(mods_device, transient.buffer, placed.block->memory, offset) != VK_SUCCESS) {
		placed.block->live--;
		api.DestroyBuffer(mods_device, transient.buffer, NULL);
		return NULL;
	}

	transient.block = placed.block;
	transient.data = placed.block->data + offset;
	transient.size = info.size;
	transient.frame = vk_frame;
	transients.push_back(transient);
	return &transients.back();
}

// Memory of a buffer for what the draws recorded this frame read. reach is
// how far from its start a shader may be told it can read, which is then
// within the buffer.
static BYTE* transient_bytes(VkDeviceSize bytes, VkDeviceSize reach, VkBuffer *buffer, VkDeviceSize *offset)
{
	Transient *found = NULL;
	VkDeviceSize start = 0;
	UINT frame = vk_frame;

	for (Transient &transient : transients) {
		if (transient.frame != frame) {
			if (frame - transient.frame <= MIRROR_KEEP_FRAMES)
				continue;
			transient.frame = frame;
			transient.used = 0;
		}
		start = (transient.used + TRANSIENT_ALIGNMENT - 1) & ~(TRANSIENT_ALIGNMENT - 1);
		if (start + reach <= transient.size) {
			found = &transient;
			break;
		}
	}
	if (!found) {
		found = new_transient(reach);
		start = 0;
	}
	if (!found)
		return NULL;

	found->used = start + bytes;
	*buffer = found->buffer;
	*offset = start;
	return found->data + start;
}

// One made for the contents of a texture is of no use to what a draw reads,
// and neither is the memory it has to itself
static void destroy_big_transients()
{
	size_t kept = 0, i;

	for (const Transient &transient : transients) {
		if (transient.size <= TRANSIENT_BYTES || vk_frame - transient.frame <= MIRROR_KEEP_FRAMES) {
			transients[kept++] = transient;
			continue;
		}
		api.DestroyBuffer(mods_device, transient.buffer, NULL);
		transient.block->live--;
	}
	transients.resize(kept);

	for (i = 0; i < mirror_blocks.size();) {
		MirrorBlock *block = mirror_blocks[i];

		if (block->live || block->size <= MIRROR_BLOCK_BYTES) {
			i++;
			continue;
		}
		api.FreeMemory(mods_device, block->memory, NULL);
		delete block;
		mirror_blocks.erase(mirror_blocks.begin() + i);
	}
}

struct Readback {
	ID3D12Resource *resource;
	const BYTE *data;
	UINT64 size, used;
};

struct Transfer {
	const BYTE *src;
	BYTE *dst;
	size_t bytes;
};

const UINT64 READBACK_BYTES = 1 << 20;

static std::vector<Readback> readbacks;
static std::vector<Transfer> transfers;
// Read without the lock by the threads handing command buffers to a queue
static volatile bool transfers_pending;

// Where a list of the shim can copy to for it to be read here once the list
// has been executed
static Readback* readback_space(UINT64 bytes, UINT64 *offset)
{
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_DESC desc = {};
	Readback *found = NULL;
	Readback created = {};

	for (Readback &readback : readbacks) {
		*offset = ShimAlign(readback.used, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		if (*offset + bytes <= readback.size) {
			found = &readback;
			break;
		}
	}
	if (!found) {
		heap.Type = D3D12_HEAP_TYPE_READBACK;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = max(bytes, READBACK_BYTES);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&created.resource))))
			return NULL;
		if (FAILED(created.resource->Map(0, NULL, (void**)&created.data))) {
			created.resource->Release();
			return NULL;
		}
		created.size = desc.Width;
		readbacks.push_back(created);
		found = &readbacks.back();
		*offset = 0;
	}

	found->used = *offset + bytes;
	return found;
}

static bool transfer_from_gpu(ShimResource *resource, UINT64 offset, UINT64 bytes, BYTE *dst)
{
	Readback *readback;
	UINT64 start;

	if (!resource->resource || offset + bytes > resource->desc.Width)
		return false;
	readback = readback_space(bytes, &start);
	if (!readback)
		return false;

	Dx12ShimTransition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Dx12ShimList()->CopyBufferRegion(readback->resource, start, resource->resource, offset, bytes);
	transfers.push_back({ readback->data + start, dst, (size_t)bytes });
	transfers_pending = true;
	return true;
}

static void release_readbacks()
{
	for (Readback &readback : readbacks)
		readback.resource->Release();
	readbacks.clear();
}

static void submit_uploads(VkQueue queue);

// With the lock of dx12/Bridge.h held
static void finish_transfers(VkQueue queue)
{
	UINT64 start;

	if (!transfers_pending)
		return;

	start = ticks();
	if (dx12_shim && !transfers.empty()) {
		Dx12ShimSubmitAndWait();
		for (const Transfer &transfer : transfers)
			memcpy(transfer.dst, transfer.src, transfer.bytes);
	}
	transfers.clear();
	for (Readback &readback : readbacks)
		readback.used = 0;
	submit_uploads(queue);
	transfers_pending = false;

	stats.submits_waited++;
	stats.submit_ticks += ticks() - start;
}

void VkModsBeforeSubmit(VkQueue queue)
{
	if (!transfers_pending)
		return;

	Dx12ShimLock();
	finish_transfers(queue);
	Dx12ShimUnlock();
}

// Where a shader of the game is to read from, and how far
struct ModBuffer {
	VkBuffer buffer;
	VkDeviceSize offset, range;
};

struct Brought {
	// ShimResource::version of the contents
	UINT64 version;
	ModBuffer buffer;
};

// Those of this frame, by resource, offset and size: a mesh is drawn in parts
// with the same of everything, and a skeleton is put together once for all
// the meshes it moves
static std::map<std::tuple<ShimResource*, UINT64, UINT64>, Brought> brought;

static bool bring_buffer(ShimResource *resource, UINT64 offset, UINT64 bytes, UINT64 reach, ModBuffer *buffer)
{
	auto key = std::make_tuple(resource, offset, bytes);
	const BYTE *data;
	BYTE *dst;

	// The game's own, or a part of it: that is where it is already
	auto source = constants_sources.find(resource);
	if (source != constants_sources.end()) {
		const ConstantsKey &game = source->second.key;

		if (offset >= std::get<2>(game))
			return false;
		buffer->buffer = std::get<0>(game);
		buffer->offset = std::get<1>(game) + offset;
		buffer->range = min(bytes, std::get<2>(game) - offset);
		return true;
	}

	auto found = brought.find(key);
	if (found != brought.end() && found->second.version == resource->version) {
		*buffer = found->second.buffer;
		return true;
	}

	reach = max(reach, bytes);
	dst = transient_bytes(bytes, reach, &buffer->buffer, &buffer->offset);
	if (!dst)
		return false;
	buffer->range = reach;

	data = buffer_contents(resource);
	if (data && offset + bytes <= resource->shadow.size()) {
		memcpy(dst, data + offset, (size_t)bytes);
	} else {
		if (!transfer_from_gpu(resource, offset, bytes, dst))
			return false;
		stats.brought_from_gpu++;
	}
	stats.brought++;
	stats.brought_bytes += bytes;

	resource->mirrored = true;
	brought[key] = { resource->version, *buffer };
	return true;
}

// -----------------------------------------------------------------------------
// The textures of the mods
//
// A texture a mod sets for the pixel shader is one of the D3D12 device, where
// the engine put what it read from a file. The shader of the game samples an
// image of the game's device, so one is made for each, and filled with what
// is copied out of the D3D12 resource the way the contents of a buffer are.
// From there to the image takes commands of our own, which go to the queue
// the game is about to hand its own to.

static const struct {
	DXGI_FORMAT dxgi;
	VkFormat vk;
} texture_formats[] = {
	{ DXGI_FORMAT_R8G8B8A8_TYPELESS, VK_FORMAT_R8G8B8A8_UNORM },
	{ DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM },
	{ DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, VK_FORMAT_R8G8B8A8_SRGB },
	{ DXGI_FORMAT_B8G8R8A8_TYPELESS, VK_FORMAT_B8G8R8A8_UNORM },
	{ DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM },
	{ DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, VK_FORMAT_B8G8R8A8_SRGB },
	{ DXGI_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM },
	{ DXGI_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_UNORM },
	{ DXGI_FORMAT_R16_UNORM, VK_FORMAT_R16_UNORM },
	{ DXGI_FORMAT_R16_FLOAT, VK_FORMAT_R16_SFLOAT },
	{ DXGI_FORMAT_R16G16_FLOAT, VK_FORMAT_R16G16_SFLOAT },
	{ DXGI_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT },
	{ DXGI_FORMAT_R10G10B10A2_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
	{ DXGI_FORMAT_R11G11B10_FLOAT, VK_FORMAT_B10G11R11_UFLOAT_PACK32 },
	{ DXGI_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM },
	{ DXGI_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT },
	{ DXGI_FORMAT_R32G32B32A32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT },
	{ DXGI_FORMAT_BC1_TYPELESS, VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
	{ DXGI_FORMAT_BC1_UNORM, VK_FORMAT_BC1_RGBA_UNORM_BLOCK },
	{ DXGI_FORMAT_BC1_UNORM_SRGB, VK_FORMAT_BC1_RGBA_SRGB_BLOCK },
	{ DXGI_FORMAT_BC2_TYPELESS, VK_FORMAT_BC2_UNORM_BLOCK },
	{ DXGI_FORMAT_BC2_UNORM, VK_FORMAT_BC2_UNORM_BLOCK },
	{ DXGI_FORMAT_BC2_UNORM_SRGB, VK_FORMAT_BC2_SRGB_BLOCK },
	{ DXGI_FORMAT_BC3_TYPELESS, VK_FORMAT_BC3_UNORM_BLOCK },
	{ DXGI_FORMAT_BC3_UNORM, VK_FORMAT_BC3_UNORM_BLOCK },
	{ DXGI_FORMAT_BC3_UNORM_SRGB, VK_FORMAT_BC3_SRGB_BLOCK },
	{ DXGI_FORMAT_BC4_TYPELESS, VK_FORMAT_BC4_UNORM_BLOCK },
	{ DXGI_FORMAT_BC4_UNORM, VK_FORMAT_BC4_UNORM_BLOCK },
	{ DXGI_FORMAT_BC4_SNORM, VK_FORMAT_BC4_SNORM_BLOCK },
	{ DXGI_FORMAT_BC5_TYPELESS, VK_FORMAT_BC5_UNORM_BLOCK },
	{ DXGI_FORMAT_BC5_UNORM, VK_FORMAT_BC5_UNORM_BLOCK },
	{ DXGI_FORMAT_BC5_SNORM, VK_FORMAT_BC5_SNORM_BLOCK },
	{ DXGI_FORMAT_BC6H_TYPELESS, VK_FORMAT_BC6H_UFLOAT_BLOCK },
	{ DXGI_FORMAT_BC6H_UF16, VK_FORMAT_BC6H_UFLOAT_BLOCK },
	{ DXGI_FORMAT_BC6H_SF16, VK_FORMAT_BC6H_SFLOAT_BLOCK },
	{ DXGI_FORMAT_BC7_TYPELESS, VK_FORMAT_BC7_UNORM_BLOCK },
	{ DXGI_FORMAT_BC7_UNORM, VK_FORMAT_BC7_UNORM_BLOCK },
	{ DXGI_FORMAT_BC7_UNORM_SRGB, VK_FORMAT_BC7_SRGB_BLOCK },
};

static VkFormat vk_texture_format(DXGI_FORMAT format)
{
	for (const auto &known : texture_formats) {
		if (known.dxgi == format)
			return known.vk;
	}
	return VK_FORMAT_UNDEFINED;
}

const VkDeviceSize IMAGE_BLOCK_BYTES = 64 << 20;

struct ImageBlock {
	VkDeviceMemory memory;
	uint32_t type;
	VkDeviceSize size, used;
	// Images in it. Its space is given out again once there are none.
	UINT live;
};

struct ModTexture {
	// None if one could not be made, which is not tried for every draw
	VkImage image;
	VkImageView view;
	VkImageViewType view_type;
	ImageBlock *block;
	// ShimResource::version of the contents
	UINT64 version;
	// It has had contents before, which is the layout it is in
	bool filled;
};

struct RetiredTexture {
	ModTexture texture;
	UINT frame;
};

// The contents of an image, where they are in a buffer of the game's device
// by the time the transfers are done
struct Upload {
	VkImage image;
	VkBuffer buffer;
	bool filled;
	UINT mips, layers;
	std::vector<VkBufferImageCopy> regions;
};

struct UploadCommands {
	VkCommandPool pool;
	VkCommandBuffer cmd;
	UINT frame;
};

static std::vector<ImageBlock*> image_blocks;
static std::unordered_map<ShimResource*, ModTexture> mod_textures;
static std::vector<RetiredTexture> retired_textures;
static std::vector<Upload> uploads;
static std::vector<UploadCommands> upload_commands;
static std::unordered_map<VkQueue, uint32_t> queue_families;
static std::unordered_map<uint32_t, VkCommandPool> upload_pools;

const UINT64 TEXTURE_BYTES_PER_FRAME = 64 << 20;
static UINT64 texture_frame_bytes;
static UINT texture_frame;

void VkModsQueue(VkDevice device, VkQueue queue, uint32_t family)
{
	Dx12ShimLock();
	if (device == mods_device)
		queue_families[queue] = family;
	Dx12ShimUnlock();
}

static ImageBlock* place_image(const VkMemoryRequirements *needed, VkDeviceSize *offset)
{
	const VkMemoryPropertyFlags wanted[] = { VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0 };
	VkMemoryAllocateInfo info = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkDeviceSize alignment = max(needed->alignment, (VkDeviceSize)1);
	ImageBlock *block;
	uint32_t type;

	for (ImageBlock *candidate : image_blocks) {
		if (!candidate->live)
			candidate->used = 0;
		*offset = (candidate->used + alignment - 1) / alignment * alignment;
		if ((needed->memoryTypeBits & 1u << candidate->type) && *offset + needed->size <= candidate->size) {
			candidate->used = *offset + needed->size;
			candidate->live++;
			return candidate;
		}
	}

	info.allocationSize = max(IMAGE_BLOCK_BYTES, needed->size);
	for (VkMemoryPropertyFlags flags : wanted) {
		for (type = 0; type < memory_properties.memoryTypeCount; type++) {
			VkMemoryPropertyFlags has = memory_properties.memoryTypes[type].propertyFlags;

			if (!(needed->memoryTypeBits & 1u << type) || (has & VK_MEMORY_PROPERTY_PROTECTED_BIT) || (has & flags) != flags)
				continue;

			block = new ImageBlock();
			info.memoryTypeIndex = type;
			if (api.AllocateMemory(mods_device, &info, NULL, &block->memory) != VK_SUCCESS) {
				delete block;
				continue;
			}
			block->type = type;
			block->size = info.allocationSize;
			block->used = needed->size;
			block->live = 1;
			image_blocks.push_back(block);
			VkDumpNote("Mods: %llu MB of memory type %u (flags 0x%x) for the textures of the mods, block %Iu\n",
				block->size >> 20, type, has, image_blocks.size());
			*offset = 0;
			return block;
		}
	}
	return NULL;
}

static void destroy_texture(const ModTexture *texture)
{
	if (texture->view)
		api.DestroyImageView(mods_device, texture->view, NULL);
	if (texture->image)
		api.DestroyImage(mods_device, texture->image, NULL);
	if (texture->block)
		texture->block->live--;
}

// A descriptor set made with the view of one is held for as long as a buffer
// is after its last draw, and the view has to be there for longer than that
// set: a view made after it may get its handle
static void destroy_retired_textures(bool all)
{
	size_t kept = 0;

	for (const RetiredTexture &retired : retired_textures) {
		if (!all && vk_frame - retired.frame < MIRROR_KEEP_FRAMES + 4) {
			retired_textures[kept++] = retired;
			continue;
		}
		destroy_texture(&retired.texture);
	}
	retired_textures.resize(kept);

	kept = 0;
	for (const UploadCommands &commands : upload_commands) {
		if (!all && vk_frame - commands.frame < MIRROR_KEEP_FRAMES) {
			upload_commands[kept++] = commands;
			continue;
		}
		api.FreeCommandBuffers(mods_device, commands.pool, 1, &commands.cmd);
	}
	upload_commands.resize(kept);
}

static bool make_image(ModTexture *texture, const D3D12_RESOURCE_DESC *desc, VkFormat format, const SpirvBinding *slot)
{
	VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkMemoryRequirements needed;
	VkDeviceSize offset;

	// What the shader of the game samples there
	if (slot->dim == 1) {
		texture->view_type = slot->arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
	} else if (slot->dim == 3 && !slot->arrayed && desc->DepthOrArraySize >= 6 && desc->Width == desc->Height) {
		texture->view_type = VK_IMAGE_VIEW_TYPE_CUBE;
		info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	} else {
		return false;
	}

	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = (uint32_t)desc->Width;
	info.extent.height = desc->Height;
	info.extent.depth = 1;
	info.mipLevels = desc->MipLevels;
	info.arrayLayers = desc->DepthOrArraySize;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (api.CreateImage(mods_device, &info, NULL, &texture->image) != VK_SUCCESS) {
		texture->image = VK_NULL_HANDLE;
		return false;
	}

	api.GetImageMemoryRequirements(mods_device, texture->image, &needed);
	texture->block = place_image(&needed, &offset);
	if (!texture->block || api.BindImageMemory(mods_device, texture->image, texture->block->memory, offset) != VK_SUCCESS)
		return false;

	view_info.image = texture->image;
	view_info.viewType = texture->view_type;
	view_info.format = format;
	view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view_info.subresourceRange.levelCount = info.mipLevels;
	view_info.subresourceRange.layerCount = texture->view_type == VK_IMAGE_VIEW_TYPE_CUBE ? 6
		: texture->view_type == VK_IMAGE_VIEW_TYPE_2D ? 1 : info.arrayLayers;
	if (api.CreateImageView(mods_device, &view_info, NULL, &texture->view) != VK_SUCCESS) {
		texture->view = VK_NULL_HANDLE;
		return false;
	}

	stats.textures_made++;
	return true;
}

// Has a list of the shim copy what the resource holds to where it can be read,
// and from there it goes to a buffer of the game's device for the image
static bool fill_image(ModTexture *texture, ShimResource *resource, const D3D12_RESOURCE_DESC *desc)
{
	UINT count = desc->MipLevels * desc->DepthOrArraySize, i, y;
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(count);
	std::vector<UINT64> row_sizes(count);
	std::vector<UINT> rows(count);
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	UINT64 total = 0, start, bytes = 0;
	VkDeviceSize staging_offset;
	Readback *readback;
	Upload upload = {};
	BYTE *staging;

	dx12_shim->device->GetCopyableFootprints(desc, 0, count, 0, layouts.data(), rows.data(), row_sizes.data(), &total);
	if (!total || total == UINT64_MAX)
		return false;
	// A row takes no more here than there, and a subresource starts at a
	// multiple of the size of any block of texels
	for (i = 0; i < count; i++)
		bytes = ShimAlign(bytes, 16) + row_sizes[i] * rows[i];

	staging = transient_bytes(bytes, bytes, &upload.buffer, &staging_offset);
	readback = staging ? readback_space(total, &start) : NULL;
	if (!readback)
		return false;

	// After this, so that what the engine has yet to write is written
	Dx12ShimTransition(resource, D3D12_RESOURCE_STATE_COPY_SOURCE);
	src.pResource = resource->resource;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.pResource = readback->resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

	bytes = 0;
	for (i = 0; i < count; i++) {
		const BYTE *from = readback->data + start + layouts[i].Offset;
		UINT mip = i % desc->MipLevels;
		VkBufferImageCopy region = {};

		src.SubresourceIndex = i;
		dst.PlacedFootprint = layouts[i];
		dst.PlacedFootprint.Offset += start;
		Dx12ShimList()->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

		bytes = ShimAlign(bytes, 16);
		if (layouts[i].Footprint.RowPitch == row_sizes[i]) {
			transfers.push_back({ from, staging + bytes, (size_t)(row_sizes[i] * rows[i]) });
		} else {
			for (y = 0; y < rows[i]; y++) {
				transfers.push_back({ from + (size_t)y * layouts[i].Footprint.RowPitch,
					staging + bytes + y * row_sizes[i], (size_t)row_sizes[i] });
			}
		}

		region.bufferOffset = staging_offset + bytes;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = mip;
		region.imageSubresource.baseArrayLayer = i / desc->MipLevels;
		region.imageSubresource.layerCount = 1;
		region.imageExtent.width = max((uint32_t)desc->Width >> mip, 1u);
		region.imageExtent.height = max(desc->Height >> mip, 1u);
		region.imageExtent.depth = 1;
		upload.regions.push_back(region);
		bytes += row_sizes[i] * rows[i];
	}

	upload.image = texture->image;
	upload.filled = texture->filled;
	upload.mips = desc->MipLevels;
	upload.layers = desc->DepthOrArraySize;
	uploads.push_back(std::move(upload));
	transfers_pending = true;

	texture->filled = true;
	texture->version = resource->version;
	stats.textures_bytes += bytes;
	return true;
}

// The image for what a mod has set where the shader of the game samples a
// texture, none if there cannot be one
static VkImageView bring_texture(const ShimView *view, const SpirvBinding *slot)
{
	ShimResource *resource = view->resource;
	D3D12_RESOURCE_DESC desc;
	ModTexture *texture;
	VkFormat format;
	UINT64 copied;

	if (!resource->resource || queue_families.empty())
		return VK_NULL_HANDLE;

	auto found = mod_textures.find(resource);
	if (found != mod_textures.end()) {
		texture = &found->second;
		if (!texture->view)
			return VK_NULL_HANDLE;
		if (texture->filled && texture->version == resource->version && !resource->write_pending)
			return texture->view;
	} else {
		desc = resource->resource->GetDesc();
		format = vk_texture_format(view->format);
		if (!format)
			format = vk_texture_format(desc.Format);

		texture = &mod_textures[resource];
		resource->mirrored = true;
		if (resource->dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || !format
		    || !make_image(texture, &desc, format, slot)) {
			destroy_texture(texture);
			*texture = {};
			stats.textures_failed++;
			return VK_NULL_HANDLE;
		}
	}

	// The rest of what a mod has just had loaded is there some frames
	// later, rather than all of it after one frame that takes that long
	if (texture_frame != vk_frame) {
		texture_frame = vk_frame;
		texture_frame_bytes = 0;
	}
	copied = stats.textures_bytes;
	desc = resource->resource->GetDesc();
	if (texture_frame_bytes >= TEXTURE_BYTES_PER_FRAME || !fill_image(texture, resource, &desc))
		return texture->filled ? texture->view : VK_NULL_HANDLE;
	texture_frame_bytes += stats.textures_bytes - copied;
	return texture->view;
}

// With the lock of dx12/Bridge.h held, by the one using the queue
static void submit_uploads(VkQueue queue)
{
	VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	VkCommandBufferAllocateInfo cmd_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	VkCommandBuffer cmd;
	VkCommandPool *pool;

	if (uploads.empty() || !mods_device)
		return;

	auto family = queue_families.find(queue);
	if (!dx12_shim || family == queue_families.end()) {
		// Nothing can be done for them later either: the buffers they
		// are in are given out again
		uploads.clear();
		return;
	}

	pool = &upload_pools[family->second];
	pool_info.queueFamilyIndex = family->second;
	cmd_info.commandBufferCount = 1;
	cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if ((!*pool && api.CreateCommandPool(mods_device, &pool_info, NULL, pool) != VK_SUCCESS)
	    || (cmd_info.commandPool = *pool, api.AllocateCommandBuffers(mods_device, &cmd_info, &cmd) != VK_SUCCESS)) {
		uploads.clear();
		return;
	}
	upload_commands.push_back({ *pool, cmd, vk_frame });

	if (api.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
		uploads.clear();
		return;
	}
	for (const Upload &upload : uploads) {
		VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

		barrier.srcAccessMask = upload.filled ? VK_ACCESS_SHADER_READ_BIT : 0;
		barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.oldLayout = upload.filled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = upload.image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.levelCount = upload.mips;
		barrier.subresourceRange.layerCount = upload.layers;
		api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
			0, NULL, 0, NULL, 1, &barrier);

		api.CmdCopyBufferToImage(cmd, upload.buffer, upload.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			(uint32_t)upload.regions.size(), upload.regions.data());

		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
			0, NULL, 0, NULL, 1, &barrier);
	}
	uploads.clear();
	if (api.EndCommandBuffer(cmd) != VK_SUCCESS)
		return;

	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	api.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
}

void VkScopeForgetResource(ShimResource *resource)
{
	auto found = mirrors.find(resource);
	auto texture = mod_textures.find(resource);

	if (texture != mod_textures.end()) {
		retired_textures.push_back({ texture->second, vk_frame });
		mod_textures.erase(texture);
	}
	auto first = brought.lower_bound(std::make_tuple(resource, (UINT64)0, (UINT64)0));
	auto last = first;

	// Its address may be that of another before the frame is over
	while (last != brought.end() && std::get<0>(last->first) == resource)
		++last;
	brought.erase(first, last);

	if (found == mirrors.end())
		return;
	retire_mirror(&found->second);
	mirrors.erase(found);
}

static void destroy_mod_sets();
static void destroy_variants(bool all);

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
		destroy_mod_sets();
		destroy_variants(true);
		// What is still to be copied would be copied to memory that goes
		transfers.clear();
		uploads.clear();
		transfers_pending = false;
		for (auto &texture : mod_textures) {
			texture.first->mirrored = false;
			destroy_texture(&texture.second);
		}
		mod_textures.clear();
		destroy_retired_textures(true);
		for (auto &pool : upload_pools)
			api.DestroyCommandPool(mods_device, pool.second, NULL);
		upload_pools.clear();
		queue_families.clear();
		for (ImageBlock *block : image_blocks) {
			api.FreeMemory(mods_device, block->memory, NULL);
			delete block;
		}
		image_blocks.clear();
		brought.clear();
		for (Transient &transient : transients)
			api.DestroyBuffer(mods_device, transient.buffer, NULL);
		transients.clear();
		for (MirrorBlock *block : mirror_blocks) {
			api.FreeMemory(mods_device, block->memory, NULL);
			delete block;
		}
		mirror_blocks.clear();
		forget_unused_constants(true);
		mods_device = NULL;
		mods_device_key = NULL;
	}
	Dx12ShimUnlock();
}

// -----------------------------------------------------------------------------
// Descriptor sets: the buffers the game has at the bindings its shaders read

// How many sets of a command buffer are kept track of, and how many of the
// offsets each is bound with
const UINT MAX_SETS = 8;
const UINT MAX_DYNAMIC = 16;
const UINT MAX_BINDINGS = 256;

struct SetBinding {
	bool buffer, dynamic;
	// Not what a set can be given from another: a sampler the layout has
	// for good
	bool fixed;
	VkDescriptorType type;
	UINT count;
	// Of its first descriptor among those of the set that are buffers, and
	// among those that are bound with an offset
	UINT first, first_dynamic;
	// A texture a shader samples, and its first among those of the set
	bool image;
	UINT first_image;
};

struct DescriptorLayout {
	// By binding number
	std::vector<SetBinding> bindings;
	UINT buffers, dynamic, images;
	// What a pool needs for a set of it. None if a set like one of the
	// game's cannot be made here.
	std::vector<VkDescriptorPoolSize> pool_sizes;
};

struct SetBuffer {
	VkBuffer buffer;
	VkDeviceSize offset, range;
};

struct DescriptorSet {
	std::shared_ptr<const DescriptorLayout> layout;
	VkDescriptorSetLayout layout_handle;
	std::vector<SetBuffer> buffers;
	std::vector<VkImageView> images;
	// Counts the times it was allocated or written
	UINT64 generation;
};

static SRWLOCK descriptor_lock = SRWLOCK_INIT;
static std::unordered_map<VkDescriptorSetLayout, std::shared_ptr<const DescriptorLayout>> set_layouts;
// One for every handle there has been: a pool is reset without a word about
// its sets, whose handles are then given out again
static std::unordered_map<VkDescriptorSet, DescriptorSet*> descriptor_sets;

// For the report, with descriptor_lock held: sets allocated, and updates of
// buffers by vkUpdateDescriptorSets and by a template
static unsigned sets_allocated, sets_written, sets_written_by_template;

static bool is_buffer_descriptor(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
		|| type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
}

static bool is_image_descriptor(VkDescriptorType type)
{
	return type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}

void VkModsSetLayout(VkDescriptorSetLayout handle, const VkDescriptorSetLayoutCreateInfo *info)
{
	std::shared_ptr<DescriptorLayout> layout = std::make_shared<DescriptorLayout>();
	const VkDescriptorSetLayoutBindingFlagsCreateInfo *flags = (const VkDescriptorSetLayoutBindingFlagsCreateInfo*)
		find_in_chain(info->pNext, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO);
	// Sets of the kinds that take a pool or an allocation of their kind
	bool plain = !info->flags;
	uint32_t i, counts[VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1] = {};

	layout->buffers = 0;
	layout->dynamic = 0;
	layout->images = 0;
	for (i = 0; info->pBindings && i < info->bindingCount; i++) {
		const VkDescriptorSetLayoutBinding *described = &info->pBindings[i];
		SetBinding *binding;

		if (flags && i < flags->bindingCount && (flags->pBindingFlags[i]
				& (VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT)))
			plain = false;
		if (described->binding >= MAX_BINDINGS || described->descriptorType > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
			plain = false;
			continue;
		}
		counts[described->descriptorType] += described->descriptorCount;

		if (layout->bindings.size() <= described->binding)
			layout->bindings.resize(described->binding + 1);
		binding = &layout->bindings[described->binding];
		binding->buffer = is_buffer_descriptor(described->descriptorType);
		binding->image = is_image_descriptor(described->descriptorType);
		binding->dynamic = described->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
			|| described->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
		binding->fixed = described->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER && described->pImmutableSamplers;
		binding->type = described->descriptorType;
		binding->count = described->descriptorCount;
	}
	for (i = 0; plain && i < ARRAYSIZE(counts); i++) {
		if (counts[i])
			layout->pool_sizes.push_back({ (VkDescriptorType)i, counts[i] });
	}
	// The offsets a set is bound with are in the order of its bindings
	for (SetBinding &binding : layout->bindings) {
		if (binding.image) {
			binding.first_image = layout->images;
			layout->images += binding.count;
		}
		if (!binding.buffer)
			continue;
		binding.first = layout->buffers;
		layout->buffers += binding.count;
		if (!binding.dynamic)
			continue;
		binding.first_dynamic = layout->dynamic;
		layout->dynamic += binding.count;
	}

	AcquireSRWLockExclusive(&descriptor_lock);
	set_layouts[handle] = layout;
	ReleaseSRWLockExclusive(&descriptor_lock);
}

void VkModsForgetSetLayout(VkDescriptorSetLayout handle)
{
	AcquireSRWLockExclusive(&descriptor_lock);
	set_layouts.erase(handle);
	ReleaseSRWLockExclusive(&descriptor_lock);
}

void VkModsSetsAllocated(const VkDescriptorSetAllocateInfo *info, const VkDescriptorSet *sets)
{
	uint32_t i;

	AcquireSRWLockExclusive(&descriptor_lock);
	sets_allocated += info->descriptorSetCount;
	for (i = 0; i < info->descriptorSetCount; i++) {
		DescriptorSet **slot = &descriptor_sets[sets[i]];
		auto layout = set_layouts.find(info->pSetLayouts[i]);

		if (!*slot)
			*slot = new DescriptorSet();
		(*slot)->layout = layout != set_layouts.end() ? layout->second : NULL;
		(*slot)->layout_handle = info->pSetLayouts[i];
		(*slot)->generation++;
		(*slot)->buffers.assign((*slot)->layout ? (*slot)->layout->buffers : 0, SetBuffer{});
		(*slot)->images.assign((*slot)->layout ? (*slot)->layout->images : 0, VK_NULL_HANDLE);
	}
	ReleaseSRWLockExclusive(&descriptor_lock);
}

// The descriptor a write or a copy has got to, which goes on into the bindings
// after the one it started at. With descriptor_lock held. False past the last
// of the set; buffer is NULL for one that is no buffer, image for one that is
// no texture.
static bool next_descriptor(DescriptorSet *set, uint32_t *binding, uint32_t *element, SetBuffer **buffer, VkImageView **image)
{
	const std::vector<SetBinding> &bindings = set->layout->bindings;

	while (*binding < bindings.size() && *element >= bindings[*binding].count) {
		*element = 0;
		++*binding;
	}
	if (*binding >= bindings.size())
		return false;

	*buffer = bindings[*binding].buffer ? &set->buffers[bindings[*binding].first + *element] : NULL;
	*image = bindings[*binding].image ? &set->images[bindings[*binding].first_image + *element] : NULL;
	++*element;
	return true;
}

static DescriptorSet* find_set(VkDescriptorSet handle)
{
	auto found = descriptor_sets.find(handle);

	return found != descriptor_sets.end() && found->second->layout ? found->second : NULL;
}

void VkModsSetsUpdated(uint32_t write_count, const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *copies)
{
	uint32_t i, j, binding, element, src_binding, src_element;
	SetBuffer *buffer, *src_buffer;
	VkImageView *image, *src_image;
	DescriptorSet *set, *src;
	bool any = copy_count != 0;

	for (i = 0; i < write_count && !any; i++)
		any = is_buffer_descriptor(writes[i].descriptorType) && writes[i].pBufferInfo;

	AcquireSRWLockExclusive(&descriptor_lock);
	sets_written += any;

	for (i = 0; i < write_count; i++) {
		const VkWriteDescriptorSet *write = &writes[i];
		bool buffers = is_buffer_descriptor(write->descriptorType) && write->pBufferInfo;
		bool images = is_image_descriptor(write->descriptorType) && write->pImageInfo;

		if (!(set = find_set(write->dstSet)))
			continue;
		set->generation++;
		if (!buffers && !images)
			continue;
		binding = write->dstBinding;
		element = write->dstArrayElement;
		for (j = 0; j < write->descriptorCount && next_descriptor(set, &binding, &element, &buffer, &image); j++) {
			if (buffer && buffers)
				*buffer = { write->pBufferInfo[j].buffer, write->pBufferInfo[j].offset, write->pBufferInfo[j].range };
			if (image && images)
				*image = write->pImageInfo[j].imageView;
		}
	}

	for (i = 0; i < copy_count; i++) {
		const VkCopyDescriptorSet *copy = &copies[i];

		if (!(set = find_set(copy->dstSet)))
			continue;
		set->generation++;
		if (!(src = find_set(copy->srcSet)))
			continue;
		binding = copy->dstBinding;
		element = copy->dstArrayElement;
		src_binding = copy->srcBinding;
		src_element = copy->srcArrayElement;
		for (j = 0; j < copy->descriptorCount && next_descriptor(set, &binding, &element, &buffer, &image)
				&& next_descriptor(src, &src_binding, &src_element, &src_buffer, &src_image); j++) {
			if (buffer && src_buffer)
				*buffer = *src_buffer;
			if (image && src_image)
				*image = *src_image;
		}
	}

	ReleaseSRWLockExclusive(&descriptor_lock);
}

// Of a template for updating a set, the entries that are for buffers or for
// textures
static std::unordered_map<VkDescriptorUpdateTemplate, std::shared_ptr<const std::vector<VkDescriptorUpdateTemplateEntry>>> update_templates;

void VkModsUpdateTemplate(VkDescriptorUpdateTemplate handle, const VkDescriptorUpdateTemplateCreateInfo *info)
{
	auto entries = std::make_shared<std::vector<VkDescriptorUpdateTemplateEntry>>();
	uint32_t i;

	// One for push descriptors is of no set
	for (i = 0; info->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET && i < info->descriptorUpdateEntryCount; i++) {
		if (is_buffer_descriptor(info->pDescriptorUpdateEntries[i].descriptorType)
		    || is_image_descriptor(info->pDescriptorUpdateEntries[i].descriptorType))
			entries->push_back(info->pDescriptorUpdateEntries[i]);
	}

	AcquireSRWLockExclusive(&descriptor_lock);
	update_templates[handle] = entries;
	ReleaseSRWLockExclusive(&descriptor_lock);
}

void VkModsSetUpdatedWithTemplate(VkDescriptorSet handle, VkDescriptorUpdateTemplate update_template, const void *data)
{
	uint32_t j, binding, element;
	DescriptorSet *set;
	VkImageView *image;
	SetBuffer *buffer;

	if (!data)
		return;

	AcquireSRWLockExclusive(&descriptor_lock);
	auto found = update_templates.find(update_template);
	set = find_set(handle);
	if (set)
		set->generation++;
	if (found != update_templates.end() && !found->second->empty() && set) {
		sets_written_by_template++;
		for (const VkDescriptorUpdateTemplateEntry &entry : *found->second) {
			bool buffers = is_buffer_descriptor(entry.descriptorType);

			binding = entry.dstBinding;
			element = entry.dstArrayElement;
			for (j = 0; j < entry.descriptorCount && next_descriptor(set, &binding, &element, &buffer, &image); j++) {
				const BYTE *at = (const BYTE*)data + entry.offset + j * entry.stride;

				if (buffer && buffers) {
					const VkDescriptorBufferInfo *info = (const VkDescriptorBufferInfo*)at;

					*buffer = { info->buffer, info->offset, info->range };
				} else if (image && !buffers) {
					*image = ((const VkDescriptorImageInfo*)at)->imageView;
				}
			}
		}
	}
	ReleaseSRWLockExclusive(&descriptor_lock);
}

// -----------------------------------------------------------------------------
// The textures of the game
//
// A mod names a texture of the game by the hash 3DMigoto has of the D3D11
// one: of its description and of the start of its first mip level, as it was
// when the game created it. The game fills its Vulkan image with the same
// bytes, out of a buffer it has mapped, so the hash can be had from those.
//
// A texture whose larger levels are not loaded yet is a smaller texture under
// D3D11, which the game replaces when they are. Here it is the one image all
// along: its hash is that of the largest level filled so far, with the
// description of a texture that starts there.

struct GameFormat {
	VkFormat vk;
	DXGI_FORMAT dxgi;
	// Of a block of 4x4 texels, or of a texel where there are no blocks
	UINT block_bytes, texel_bytes;
};

static const GameFormat game_formats[] = {
	{ VK_FORMAT_BC1_RGB_UNORM_BLOCK, DXGI_FORMAT_BC1_UNORM, 8 }, { VK_FORMAT_BC1_RGB_SRGB_BLOCK, DXGI_FORMAT_BC1_UNORM_SRGB, 8 },
	{ VK_FORMAT_BC1_RGBA_UNORM_BLOCK, DXGI_FORMAT_BC1_UNORM, 8 }, { VK_FORMAT_BC1_RGBA_SRGB_BLOCK, DXGI_FORMAT_BC1_UNORM_SRGB, 8 },
	{ VK_FORMAT_BC2_UNORM_BLOCK, DXGI_FORMAT_BC2_UNORM, 16 }, { VK_FORMAT_BC2_SRGB_BLOCK, DXGI_FORMAT_BC2_UNORM_SRGB, 16 },
	{ VK_FORMAT_BC3_UNORM_BLOCK, DXGI_FORMAT_BC3_UNORM, 16 }, { VK_FORMAT_BC3_SRGB_BLOCK, DXGI_FORMAT_BC3_UNORM_SRGB, 16 },
	{ VK_FORMAT_BC4_UNORM_BLOCK, DXGI_FORMAT_BC4_UNORM, 8 }, { VK_FORMAT_BC4_SNORM_BLOCK, DXGI_FORMAT_BC4_SNORM, 8 },
	{ VK_FORMAT_BC5_UNORM_BLOCK, DXGI_FORMAT_BC5_UNORM, 16 }, { VK_FORMAT_BC5_SNORM_BLOCK, DXGI_FORMAT_BC5_SNORM, 16 },
	{ VK_FORMAT_BC6H_UFLOAT_BLOCK, DXGI_FORMAT_BC6H_UF16, 16 }, { VK_FORMAT_BC6H_SFLOAT_BLOCK, DXGI_FORMAT_BC6H_SF16, 16 },
	{ VK_FORMAT_BC7_UNORM_BLOCK, DXGI_FORMAT_BC7_UNORM, 16 }, { VK_FORMAT_BC7_SRGB_BLOCK, DXGI_FORMAT_BC7_UNORM_SRGB, 16 },
	{ VK_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, 0, 1 }, { VK_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, 0, 2 },
	{ VK_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM, 0, 2 }, { VK_FORMAT_R16_SFLOAT, DXGI_FORMAT_R16_FLOAT, 0, 2 },
	{ VK_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 4 }, { VK_FORMAT_R8G8B8A8_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 4 },
	{ VK_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 4 }, { VK_FORMAT_B8G8R8A8_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 0, 4 },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 8 },
};

struct GameImage {
	const GameFormat *format;
	uint32_t width, height, mips, layers;
	bool cube;
	// The level the hash is of
	uint32_t level;
	bool hashed;
	uint32_t hash, data_hash;
	// What the engine is shown for it, once it asked, and the hash that
	// has. With the lock of dx12/Bridge.h held.
	ShimTexture2D *object;
	ShimShaderResourceView *srv;
	uint32_t shown_hash;
};

static SRWLOCK image_lock = SRWLOCK_INIT;
static std::unordered_map<VkImage, GameImage> game_images;
static std::unordered_map<VkImageView, VkImage> game_views;
// What the engine was shown of images that are gone. Let go of with the lock
// of dx12/Bridge.h held.
static std::vector<IUnknown*> retired_images;
static volatile LONG images_hashed, images_not_mapped;

void VkModsImageCreated(VkImage image, const VkImageCreateInfo *info)
{
	GameImage created = {};

	if (info->imageType != VK_IMAGE_TYPE_2D || info->samples != VK_SAMPLE_COUNT_1_BIT
	    || !(info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
		return;
	for (const GameFormat &format : game_formats) {
		if (format.vk == info->format)
			created.format = &format;
	}
	if (!created.format)
		return;
	created.width = info->extent.width;
	created.height = info->extent.height;
	created.mips = info->mipLevels;
	created.layers = info->arrayLayers;
	created.cube = !!(info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && info->arrayLayers == 6;

	AcquireSRWLockExclusive(&image_lock);
	game_images[image] = created;
	ReleaseSRWLockExclusive(&image_lock);
}

void VkModsForgetImage(VkImage image)
{
	AcquireSRWLockExclusive(&image_lock);
	auto found = game_images.find(image);
	if (found != game_images.end()) {
		if (found->second.srv)
			retired_images.push_back(found->second.srv);
		if (found->second.object)
			retired_images.push_back(found->second.object);
		game_images.erase(found);
	}
	ReleaseSRWLockExclusive(&image_lock);
}

void VkModsImageView(VkImageView view, const VkImageViewCreateInfo *info)
{
	AcquireSRWLockExclusive(&image_lock);
	if (game_images.count(info->image))
		game_views[view] = info->image;
	ReleaseSRWLockExclusive(&image_lock);
}

void VkModsForgetImageView(VkImageView view)
{
	AcquireSRWLockExclusive(&image_lock);
	game_views.erase(view);
	ReleaseSRWLockExclusive(&image_lock);
}

// With the lock of dx12/Bridge.h held
static void release_retired_images()
{
	std::vector<IUnknown*> gone;

	AcquireSRWLockExclusive(&image_lock);
	gone.swap(retired_images);
	ReleaseSRWLockExclusive(&image_lock);
	for (IUnknown *object : gone)
		object->Release();
}

void VkModsCopyBufferToImage(VkBuffer src, VkImage dst, uint32_t count, const VkBufferImageCopy *regions)
{
	const VkBufferImageCopy *top = NULL;
	D3D11_TEXTURE2D_DESC desc = {};
	VkDeviceSize start, bytes, level_bytes;
	uint32_t data_hash, level, r;
	bool known = false;
	GameImage image;

	for (r = 0; r < count; r++) {
		if (regions[r].imageSubresource.baseArrayLayer || regions[r].imageOffset.x || regions[r].imageOffset.y
		    || regions[r].imageOffset.z || regions[r].bufferRowLength || regions[r].bufferImageHeight)
			continue;
		if (!top || regions[r].imageSubresource.mipLevel < top->imageSubresource.mipLevel)
			top = &regions[r];
	}
	if (!top)
		return;
	level = top->imageSubresource.mipLevel;

	AcquireSRWLockShared(&image_lock);
	auto found = game_images.find(dst);
	if (found != game_images.end() && level < found->second.mips && (!found->second.hashed || level < found->second.level)) {
		image = found->second;
		known = true;
	}
	ReleaseSRWLockShared(&image_lock);
	if (!known)
		return;

	desc.Width = max(image.width >> level, 1u);
	desc.Height = max(image.height >> level, 1u);
	desc.MipLevels = image.mips - level;
	desc.ArraySize = image.layers;
	desc.Format = image.format->dxgi;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = image.cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

	// As CalcTexture2DDataHash has it without texture_hash: as many bytes
	// as the level has texels, or the level if that is less
	level_bytes = image.format->block_bytes
		? (VkDeviceSize)((desc.Width + 3) / 4) * ((desc.Height + 3) / 4) * image.format->block_bytes
		: (VkDeviceSize)desc.Width * desc.Height * image.format->texel_bytes;
	bytes = min((VkDeviceSize)desc.Width * desc.Height * desc.ArraySize, level_bytes);

	AcquireSRWLockShared(&memory_lock);
	auto from = buffers.find(src);
	auto mapping = from != buffers.end() ? mappings.find(from->second.memory) : mappings.end();
	known = mapping != mappings.end() && mapping->second.data && top->bufferOffset + bytes <= from->second.size;
	if (known) {
		start = from->second.memory_offset + top->bufferOffset;
		known = start >= mapping->second.offset
			&& (mapping->second.size == VK_WHOLE_SIZE || start + bytes <= mapping->second.offset + mapping->second.size);
	}
	if (known)
		data_hash = Dx12Crc32cWriteCombined(0, mapping->second.data + (start - mapping->second.offset), (size_t)bytes);
	ReleaseSRWLockShared(&memory_lock);
	if (!known) {
		InterlockedIncrement(&images_not_mapped);
		return;
	}

	AcquireSRWLockExclusive(&image_lock);
	found = game_images.find(dst);
	if (found != game_images.end() && (!found->second.hashed || level < found->second.level)) {
		found->second.level = level;
		found->second.hashed = true;
		found->second.data_hash = data_hash;
		found->second.hash = CalcTexture2DDescHash(data_hash, &desc);
		InterlockedIncrement(&images_hashed);
	}
	ReleaseSRWLockExclusive(&image_lock);
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
	// By set number, and the offsets each was bound with for its dynamic
	// buffers
	VkDescriptorSet sets[MAX_SETS];
	// What each was bound with for the layout of the pipeline, which
	// binding another in its place takes
	VkPipelineLayout layouts[MAX_SETS];
	BYTE dynamic_count[MAX_SETS];
	uint32_t dynamic[MAX_SETS][MAX_DYNAMIC];
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

void VkModsBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first,
		uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *dynamic_offsets)
{
	bool locked = false, lost = false;
	uint32_t i, index, dynamic;
	CmdState *state;

	if (bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS)
		return;
	state = cmd_state(cmd);

	for (i = 0; i < count; i++) {
		index = first + i;
		dynamic = 0;
		// The offsets are those of all the sets, of each as many as its
		// layout has dynamic buffers
		if (dynamic_count && count == 1) {
			dynamic = dynamic_count;
		} else if (dynamic_count && !lost) {
			if (!locked)
				AcquireSRWLockShared(&descriptor_lock);
			locked = true;

			auto found = descriptor_sets.find(sets[i]);
			if (found != descriptor_sets.end() && found->second->layout)
				dynamic = min(found->second->layout->dynamic, dynamic_count);
			else
				lost = true;
		}

		if (index < MAX_SETS) {
			state->sets[index] = sets[i];
			state->layouts[index] = layout;
			state->dynamic_count[index] = lost ? 0 : (BYTE)min(dynamic, MAX_DYNAMIC);
			if (state->dynamic_count[index])
				memcpy(state->dynamic[index], dynamic_offsets, state->dynamic_count[index] * sizeof(uint32_t));
		}
		dynamic_offsets += dynamic;
		dynamic_count -= dynamic;
	}

	if (locked)
		ReleaseSRWLockShared(&descriptor_lock);
}

// -----------------------------------------------------------------------------
// Descriptor sets for the draws of mods
//
// A draw reads a buffer of a mod through a set that is the game's but for
// the binding of that buffer: one of the game's layout, with the rest copied
// from the game's as that is when the draw is recorded. All of this with the
// lock of dx12/Bridge.h held.

const UINT SETS_PER_POOL = 16;

// What a set of the game is, for one like it to be made
struct GameSet {
	VkDescriptorSet set;
	std::shared_ptr<const DescriptorLayout> layout;
	VkDescriptorSetLayout layout_handle;
	UINT64 generation;
};

// Binding, and either buffer, offset and range or the view of an image
typedef std::tuple<UINT, VkBuffer, VkDeviceSize, VkDeviceSize, VkImageView> SetReplacement;
// The set of the game, how often that had been written, and what is other
// than in it
typedef std::tuple<VkDescriptorSet, UINT64, std::vector<SetReplacement>> ModSetKey;

struct ModSet {
	VkDescriptorSet set;
	std::shared_ptr<const DescriptorLayout> layout;
	UINT used_frame;
};

// The sets not in use, of a layout that is kept from going so that its
// address stays its own
struct SpareSets {
	std::shared_ptr<const DescriptorLayout> layout;
	std::vector<VkDescriptorSet> sets;
};

static std::map<ModSetKey, ModSet> mod_sets;
static std::unordered_map<const DescriptorLayout*, SpareSets> spare_sets;
static std::vector<VkDescriptorPool> set_pools;

static VkDescriptorSet spare_set(const GameSet *game)
{
	VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	VkDescriptorSetAllocateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
	std::vector<VkDescriptorPoolSize> sizes = game->layout->pool_sizes;
	SpareSets *spare = &spare_sets[game->layout.get()];
	VkDescriptorSetLayout layouts[SETS_PER_POOL];
	VkDescriptorSet sets[SETS_PER_POOL];
	VkDescriptorPool pool;
	VkDescriptorSet set;
	bool alive;
	UINT i;

	spare->layout = game->layout;
	if (spare->sets.empty()) {
		// More can only be made while the game still has the layout
		AcquireSRWLockShared(&descriptor_lock);
		auto found = set_layouts.find(game->layout_handle);
		alive = found != set_layouts.end() && found->second == game->layout;
		ReleaseSRWLockShared(&descriptor_lock);
		if (!alive || sizes.empty())
			return VK_NULL_HANDLE;

		for (VkDescriptorPoolSize &size : sizes)
			size.descriptorCount *= SETS_PER_POOL;
		pool_info.maxSets = SETS_PER_POOL;
		pool_info.poolSizeCount = (uint32_t)sizes.size();
		pool_info.pPoolSizes = sizes.data();
		if (api.CreateDescriptorPool(mods_device, &pool_info, NULL, &pool) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		set_pools.push_back(pool);

		for (i = 0; i < SETS_PER_POOL; i++)
			layouts[i] = game->layout_handle;
		info.descriptorPool = pool;
		info.descriptorSetCount = SETS_PER_POOL;
		info.pSetLayouts = layouts;
		if (api.AllocateDescriptorSets(mods_device, &info, sets) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		spare->sets.assign(sets, sets + SETS_PER_POOL);
	}

	set = spare->sets.back();
	spare->sets.pop_back();
	return set;
}

static VkDescriptorSet mod_set(const GameSet *game, const std::vector<SetReplacement> &replacements)
{
	const std::vector<SetBinding> &bindings = game->layout->bindings;
	ModSetKey key(game->set, game->generation, replacements);
	std::vector<VkDescriptorBufferInfo> buffers;
	std::vector<VkDescriptorImageInfo> images;
	std::vector<VkWriteDescriptorSet> writes;
	std::vector<VkCopyDescriptorSet> copies;
	VkDescriptorSet set;
	UINT binding;

	auto found = mod_sets.find(key);
	if (found != mod_sets.end()) {
		found->second.used_frame = vk_frame;
		return found->second.set;
	}

	set = spare_set(game);
	if (!set)
		return VK_NULL_HANDLE;

	// Pointed at by the writes
	buffers.reserve(replacements.size());
	images.reserve(replacements.size());
	for (binding = 0; binding < bindings.size(); binding++) {
		const SetReplacement *replaced = NULL;

		if (!bindings[binding].count)
			continue;
		for (const SetReplacement &replacement : replacements) {
			if (std::get<0>(replacement) == binding)
				replaced = &replacement;
		}

		if (replaced) {
			VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };

			write.dstSet = set;
			write.dstBinding = binding;
			write.descriptorCount = 1;
			write.descriptorType = bindings[binding].type;
			if (std::get<4>(*replaced)) {
				images.push_back({ VK_NULL_HANDLE, std::get<4>(*replaced), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
				write.pImageInfo = &images.back();
			} else {
				buffers.push_back({ std::get<1>(*replaced), std::get<2>(*replaced), std::get<3>(*replaced) });
				write.pBufferInfo = &buffers.back();
			}
			writes.push_back(write);
		} else if (!bindings[binding].fixed) {
			VkCopyDescriptorSet copy = { VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET };

			copy.srcSet = game->set;
			copy.srcBinding = binding;
			copy.dstSet = set;
			copy.dstBinding = binding;
			copy.descriptorCount = bindings[binding].count;
			copies.push_back(copy);
		}
	}
	api.UpdateDescriptorSets(mods_device, (uint32_t)writes.size(), writes.data(), (uint32_t)copies.size(), copies.data());

	mod_sets[key] = { set, game->layout, vk_frame };
	stats.sets_made++;
	return set;
}

// Those the command buffers that were recorded with them are taken to be
// done with, like the buffers of the mods
static void release_unused_mod_sets()
{
	UINT frame = vk_frame;

	for (auto i = mod_sets.begin(); i != mod_sets.end();) {
		if (frame - i->second.used_frame <= MIRROR_KEEP_FRAMES) {
			++i;
			continue;
		}
		spare_sets[i->second.layout.get()].sets.push_back(i->second.set);
		i = mod_sets.erase(i);
	}
}

static void destroy_mod_sets()
{
	for (VkDescriptorPool pool : set_pools)
		api.DestroyDescriptorPool(mods_device, pool, NULL);
	set_pools.clear();
	spare_sets.clear();
	mod_sets.clear();
}

// -----------------------------------------------------------------------------
// Pipelines for the draws of mods
//
// The vertex input is part of a pipeline. A mod whose vertex buffers have
// other strides than the game's, or elements of another format (a skeleton
// with more bones than the indices of the game's vertices can name), draws
// with a pipeline made of what the game's was created from, with that for
// its vertex input. With the lock of dx12/Bridge.h held.

static const struct {
	VkFormat vk;
	DXGI_FORMAT dxgi;
} vertex_formats[] = {
	{ VK_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM },
	{ VK_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT },
	{ VK_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM },
	{ VK_FORMAT_R8G8_UINT, DXGI_FORMAT_R8G8_UINT },
	{ VK_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM },
	{ VK_FORMAT_R8G8B8A8_SNORM, DXGI_FORMAT_R8G8B8A8_SNORM },
	{ VK_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_UINT },
	{ VK_FORMAT_R8G8B8A8_SINT, DXGI_FORMAT_R8G8B8A8_SINT },
	{ VK_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, DXGI_FORMAT_R10G10B10A2_UNORM },
	{ VK_FORMAT_R16_UNORM, DXGI_FORMAT_R16_UNORM },
	{ VK_FORMAT_R16_UINT, DXGI_FORMAT_R16_UINT },
	{ VK_FORMAT_R16_SFLOAT, DXGI_FORMAT_R16_FLOAT },
	{ VK_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_UNORM },
	{ VK_FORMAT_R16G16_SNORM, DXGI_FORMAT_R16G16_SNORM },
	{ VK_FORMAT_R16G16_UINT, DXGI_FORMAT_R16G16_UINT },
	{ VK_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT },
	{ VK_FORMAT_R16G16_SFLOAT, DXGI_FORMAT_R16G16_FLOAT },
	{ VK_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_UNORM },
	{ VK_FORMAT_R16G16B16A16_SNORM, DXGI_FORMAT_R16G16B16A16_SNORM },
	{ VK_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_UINT },
	{ VK_FORMAT_R16G16B16A16_SINT, DXGI_FORMAT_R16G16B16A16_SINT },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT },
	{ VK_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT },
	{ VK_FORMAT_R32_SINT, DXGI_FORMAT_R32_SINT },
	{ VK_FORMAT_R32_SFLOAT, DXGI_FORMAT_R32_FLOAT },
	{ VK_FORMAT_R32G32_UINT, DXGI_FORMAT_R32G32_UINT },
	{ VK_FORMAT_R32G32_SINT, DXGI_FORMAT_R32G32_SINT },
	{ VK_FORMAT_R32G32_SFLOAT, DXGI_FORMAT_R32G32_FLOAT },
	{ VK_FORMAT_R32G32B32_UINT, DXGI_FORMAT_R32G32B32_UINT },
	{ VK_FORMAT_R32G32B32_SINT, DXGI_FORMAT_R32G32B32_SINT },
	{ VK_FORMAT_R32G32B32_SFLOAT, DXGI_FORMAT_R32G32B32_FLOAT },
	{ VK_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_UINT },
	{ VK_FORMAT_R32G32B32A32_SINT, DXGI_FORMAT_R32G32B32A32_SINT },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT },
};

static DXGI_FORMAT dxgi_format(VkFormat format)
{
	for (const auto &known : vertex_formats) {
		if (known.vk == format)
			return known.dxgi;
	}
	return DXGI_FORMAT_UNKNOWN;
}

static VkFormat vk_format(DXGI_FORMAT format)
{
	for (const auto &known : vertex_formats) {
		if (known.dxgi == format)
			return known.vk;
	}
	return VK_FORMAT_UNDEFINED;
}

static VkPipeline make_variant(const PipelineRecipe *recipe, const ShimInputLayout *layout, const UINT *strides)
{
	VkGraphicsPipelineCreateInfo info = recipe->info;
	VkPipelineVertexInputStateCreateInfo input = *info.pVertexInputState;
	std::vector<VkVertexInputBindingDescription> bindings(input.pVertexBindingDescriptions,
		input.pVertexBindingDescriptions + input.vertexBindingDescriptionCount);
	std::vector<VkVertexInputAttributeDescription> attributes(input.pVertexAttributeDescriptions,
		input.pVertexAttributeDescriptions + input.vertexAttributeDescriptionCount);
	VkPipeline variant = VK_NULL_HANDLE;
	size_t i;

	for (VkVertexInputBindingDescription &binding : bindings) {
		if (binding.binding < MAX_VBS)
			binding.stride = strides[binding.binding];
	}

	// Its elements are the attributes in their order, see
	// VkScopeInputElements
	if (layout && layout->mElements.size() != attributes.size())
		return VK_NULL_HANDLE;
	for (i = 0; layout && i < attributes.size(); i++) {
		const D3D12_INPUT_ELEMENT_DESC *element = &layout->mElements[i];

		if (element->Format != dxgi_format(attributes[i].format)) {
			attributes[i].format = vk_format(element->Format);
			if (!attributes[i].format)
				return VK_NULL_HANDLE;
		}
		if (element->AlignedByteOffset != D3D12_APPEND_ALIGNED_ELEMENT)
			attributes[i].offset = element->AlignedByteOffset;
	}

	input.pVertexBindingDescriptions = bindings.data();
	input.pVertexAttributeDescriptions = attributes.data();
	info.pVertexInputState = &input;
	if (api.CreateGraphicsPipelines(mods_device, VK_NULL_HANDLE, 1, &info, NULL, &variant) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return variant;
}

static VkPipeline pipeline_variant(Pipeline *pipeline, const ShimInputLayout *layout, const UINT *strides)
{
	UINT64 layout_id = layout ? layout->mId : 0;
	std::shared_ptr<PipelineRecipe> recipe;
	PipelineVariant made = {};
	bool found = false;
	UINT64 start;

	AcquireSRWLockShared(&registry_lock);
	for (const PipelineVariant &variant : pipeline->variants) {
		if (variant.layout == layout_id && !memcmp(variant.strides, strides, sizeof(variant.strides))) {
			made = variant;
			found = true;
			break;
		}
	}
	recipe = pipeline->recipe;
	ReleaseSRWLockShared(&registry_lock);
	if (found || !recipe)
		return made.pipeline;

	start = ticks();
	made.layout = layout_id;
	memcpy(made.strides, strides, sizeof(made.strides));
	made.pipeline = make_variant(recipe.get(), layout, strides);
	stats.variants_made++;
	stats.variants_failed += !made.pipeline;
	stats.variant_ticks += ticks() - start;

	AcquireSRWLockExclusive(&registry_lock);
	if (pipeline->recipe == recipe) {
		// Also one that could not be made, which is not tried again
		pipeline->variants.push_back(made);
	} else if (made.pipeline) {
		// The game has destroyed its pipeline since
		dead_variants.push_back(made.pipeline);
		made.pipeline = VK_NULL_HANDLE;
	}
	ReleaseSRWLockExclusive(&registry_lock);
	return made.pipeline;
}

static void destroy_variants(bool all)
{
	std::vector<VkPipeline> dead;

	AcquireSRWLockExclusive(&registry_lock);
	dead.swap(dead_variants);
	for (auto &entry : pipelines) {
		if (!all)
			break;
		for (const PipelineVariant &variant : entry.second->variants) {
			if (variant.pipeline)
				dead.push_back(variant.pipeline);
		}
		entry.second->variants.clear();
	}
	ReleaseSRWLockExclusive(&registry_lock);

	for (VkPipeline pipeline : dead)
		api.DestroyPipeline(mods_device, pipeline, NULL);
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
	UINT bit, i;

	// A frame being analysed has the dumps of the command lists do
	// something, which the filter takes them not to
	*reason = SLOW_SETTINGS;
	if (want_all || G->analyse_frame || !bits)
		return false;

	*reason = SLOW_LISTS;
	if (!Dx12FilterOnlyChecks(&pipeline->filter, &filter))
		return false;

	// A section may be for a constant buffer by what it is described as
	*reason = SLOW_CONSTANTS;
	for (i = 0; i < filter.cb_count; i++) {
		const ShaderLayout *shader = filter.cbs[i].stage < DX12_STAGE_COUNT ? pipeline->layouts[filter.cbs[i].stage] : NULL;

		if (shader && filter.cbs[i].slot < shader->cbs.size())
			return false;
	}

	// The index buffer and the constant buffers are all of the game's the
	// engine is given yet: what else the lists ask for they find nothing
	// bound for
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

const UINT SCOPE_SRVS = 32;

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
	UINT sets_changed;
	VkDescriptorSet sets[MAX_SETS];
	uint32_t dynamic[MAX_SETS][MAX_DYNAMIC];
	VkPipeline bound_pipeline;
	// The input layout the engine was given for the pipeline's, see
	// VkScopeInputElements
	ID3D11InputLayout *game_layout;
	// Draws of mods that could not be made, see mod_draws
	UINT mod_draws_failed;
	// The constant buffers the engine has asked for, a bit for each by
	// stage, and what it was given
	UINT cbs_asked[DX12_STAGE_COUNT];
	ID3D11Buffer *cbs[DX12_STAGE_COUNT][SHIM_CBV_SLOTS];
	// The same of the buffers at the first t registers
	UINT srvs_asked[DX12_STAGE_COUNT];
	ID3D11ShaderResourceView *srvs[DX12_STAGE_COUNT][SCOPE_SRVS];
	// One of those could not be shown. What a mod makes of nothing is not
	// worth drawing.
	bool srv_missing;
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
	// Descriptor sets, and the offsets each is bound with: as many as the
	// game's in its place
	UINT sets_set;
	VkDescriptorSet sets[MAX_SETS];
	uint32_t dynamic[MAX_SETS][MAX_DYNAMIC];
	// Not the game's if set
	VkPipeline pipeline;
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

bool VkScopeInputElements(std::vector<D3D11_INPUT_ELEMENT_DESC> *elements)
{
	const VkPipelineVertexInputStateCreateInfo *input;
	std::shared_ptr<PipelineRecipe> recipe;
	UINT indices = 0;
	uint32_t i, j;

	if (!scope)
		return false;

	AcquireSRWLockShared(&registry_lock);
	recipe = scope->state.pipeline->recipe;
	ReleaseSRWLockShared(&registry_lock);
	if (!recipe)
		return false;

	input = recipe->info.pVertexInputState;
	for (i = 0; i < input->vertexAttributeDescriptionCount; i++) {
		const VkVertexInputAttributeDescription *attribute = &input->pVertexAttributeDescriptions[i];
		D3D11_INPUT_ELEMENT_DESC element = {};

		if (attribute->format == VK_FORMAT_R8G8B8A8_UINT || attribute->format == VK_FORMAT_R16G16B16A16_UINT
		    || attribute->format == VK_FORMAT_R32G32B32A32_UINT) {
			element.SemanticName = "BLENDINDICES";
			element.SemanticIndex = indices++;
		} else {
			element.SemanticName = "ATTRIBUTE";
			element.SemanticIndex = attribute->location;
		}
		element.Format = dxgi_format(attribute->format);
		element.InputSlot = attribute->binding;
		element.AlignedByteOffset = attribute->offset;
		for (j = 0; j < input->vertexBindingDescriptionCount; j++) {
			if (input->pVertexBindingDescriptions[j].binding == attribute->binding
			    && input->pVertexBindingDescriptions[j].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
				element.InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
				element.InstanceDataStepRate = 1;
			}
		}
		elements->push_back(element);
	}
	return !elements->empty();
}

void VkScopeSetGameLayout(ID3D11InputLayout *layout)
{
	if (scope)
		scope->game_layout = layout;
}

ID3D11InputLayout* VkScopeGameLayout()
{
	return scope ? scope->game_layout : NULL;
}

// The buffer the draw has at a binding. If the game has not been seen to set
// one there, why not.
static const char* bound_buffer(const SpirvBinding *slot, SetBuffer *bound)
{
	const CmdState *game = &scope->state;
	const SetBinding *binding = NULL;
	const char *why = NULL;

	if (slot->set >= MAX_SETS || !game->sets[slot->set])
		return "no descriptor set was bound at the number of its set";

	AcquireSRWLockShared(&descriptor_lock);
	DescriptorSet *set = find_set(game->sets[slot->set]);
	if (set && slot->binding < set->layout->bindings.size())
		binding = &set->layout->bindings[slot->binding];

	if (!set) {
		why = "its descriptor set was not seen being allocated";
	} else if (!binding || !binding->buffer || !binding->count) {
		why = "the layout of its descriptor set has no buffer at the binding";
	} else {
		*bound = set->buffers[binding->first];
		if (!bound->buffer)
			why = "the binding was not seen being written";
		else if (binding->dynamic && binding->first_dynamic >= game->dynamic_count[slot->set])
			why = "the offset its descriptor set was bound with is not known";
		else if (binding->dynamic)
			bound->offset += game->dynamic[slot->set][binding->first_dynamic];
	}
	ReleaseSRWLockShared(&descriptor_lock);
	return why;
}

// For the report: the first few constant buffers there are to show, and what
// keeps one from being shown
static void note_constants(int stage, UINT slot, const SpirvBinding *block, const char *why)
{
	static std::unordered_set<std::string> noted;
	char text[256];

	if (!G->vk_dump_hashes || noted.size() >= 48)
		return;
	_snprintf_s(text, _TRUNCATE, "Mods: %cs-cb%u is %u bytes at set %u binding %u%s%s", "vpghdc"[stage], slot, block->bytes,
		block->set, block->binding, why ? ", NOT SHOWN: " : "", why ? why : "");
	if (noted.insert(text).second)
		VkDumpNote("%s\n", text);
}

ID3D11Buffer* VkScopeConstantBuffer(int stage, UINT slot)
{
	const ShaderLayout *shader;
	const SpirvBinding *block;
	PublishedConstants *published;
	const char *why;
	SetBuffer bound;
	UINT bytes;

	if (!scope || stage >= DX12_STAGE_COUNT || slot >= SHIM_CBV_SLOTS)
		return NULL;
	if (scope->cbs_asked[stage] & 1u << slot)
		return scope->cbs[stage][slot];
	scope->cbs_asked[stage] |= 1u << slot;
	scope->cbs[stage][slot] = NULL;

	shader = scope->state.pipeline->layouts[stage];
	if (!shader || slot >= shader->cbs.size() || !shader->cbs[slot].bytes)
		return NULL;
	block = &shader->cbs[slot];
	why = bound_buffer(block, &bound);
	if (why) {
		stats.constants_unbound++;
		note_constants(stage, slot, block, why);
		return NULL;
	}

	// As much as the shader reads, of what the game says is there
	bytes = block->bytes;
	if (bound.range != VK_WHOLE_SIZE && bound.range < bytes)
		bytes = (UINT)bound.range;
	if (!bytes)
		return NULL;

	published = publish_constants(bound.buffer, bound.offset, bytes, 0);
	scope->cbs[stage][slot] = published ? published->object : NULL;
	note_constants(stage, slot, block, published ? NULL : "the game does not have the memory of the buffer mapped");
	return scope->cbs[stage][slot];
}

// A storage buffer larger than this is not shown: all of it is read every
// frame a shader of a mod is given it
const VkDeviceSize VIEW_MAX_BYTES = 64 << 20;

// For the report, like note_constants
static void note_view(int stage, UINT slot, const SpirvBinding *view, VkDeviceSize bytes, VkDeviceSize buffer_bytes, const char *why)
{
	static std::unordered_set<std::string> noted;
	char text[320];

	if (!G->vk_dump_hashes || noted.size() >= 48)
		return;
	_snprintf_s(text, _TRUNCATE, "Mods: %cs-t%u is a buffer with a stride of %u, %llu bytes at set %u binding %u, of a buffer of %llu bytes%s%s",
		"vpghdc"[stage], slot, view->stride, bytes, view->set, view->binding, buffer_bytes, why ? ", NOT SHOWN: " : "", why ? why : "");
	if (noted.insert(text).second)
		VkDumpNote("%s\n", text);
}

// With the lock of dx12/Bridge.h held: the texture of the game a shader samples
// at one of its bindings, as an object the engine finds the hash of. There is
// nothing behind it, a mod cannot read the texture or bind it elsewhere.
static ID3D11ShaderResourceView* game_texture(const SpirvBinding *slot)
{
	const CmdState *game = &scope->state;
	ID3D11ShaderResourceView *shown = NULL;
	VkImageView bound = VK_NULL_HANDLE;
	D3D11_TEXTURE2D_DESC desc = {};
	GameImage *image;

	if (slot->set >= MAX_SETS || !game->sets[slot->set])
		return NULL;
	AcquireSRWLockShared(&descriptor_lock);
	DescriptorSet *set = find_set(game->sets[slot->set]);
	if (set && slot->binding < set->layout->bindings.size() && set->layout->bindings[slot->binding].image
	    && set->layout->bindings[slot->binding].count)
		bound = set->images[set->layout->bindings[slot->binding].first_image];
	ReleaseSRWLockShared(&descriptor_lock);
	if (!bound)
		return NULL;

	AcquireSRWLockExclusive(&image_lock);
	auto view = game_views.find(bound);
	auto found = view != game_views.end() ? game_images.find(view->second) : game_images.end();
	image = found != game_images.end() && found->second.hashed ? &found->second : NULL;
	if (image && (!image->object || image->shown_hash != image->hash)) {
		desc.Width = max(image->width >> image->level, 1u);
		desc.Height = max(image->height >> image->level, 1u);
		desc.MipLevels = image->mips - image->level;
		desc.ArraySize = image->layers;
		desc.Format = image->format->dxgi;
		desc.SampleDesc.Count = 1;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.MiscFlags = image->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;

		if (!image->object) {
			image->object = new ShimTexture2D();
			image->object->r.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
			image->object->r.foreign = true;
			// Has the engine's record of the object removed along with it
			new ResourceReleaseTracker(image->object);

			image->srv = new ShimShaderResourceView();
			image->srv->v.heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			image->srv->v.resource11 = image->object;
			image->srv->v.resource11->AddRef();
			image->srv->v.resource = &image->object->r;
			image->srv->v.format = desc.Format;
			image->srv->mDesc = {};
			image->srv->mDesc.Format = desc.Format;
			image->srv->mDesc.ViewDimension = image->cube ? D3D11_SRV_DIMENSION_TEXTURECUBE
				: image->layers > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;
		}
		image->object->mDesc = desc;
		image->object->r.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		image->object->r.desc.Width = desc.Width;
		image->object->r.desc.Height = desc.Height;
		image->object->r.desc.DepthOrArraySize = (UINT16)desc.ArraySize;
		image->object->r.desc.MipLevels = (UINT16)desc.MipLevels;
		image->object->r.desc.Format = desc.Format;
		image->object->r.desc.SampleDesc.Count = 1;

		EnterCriticalSectionPretty(&G->mResourcesLock);
		ResourceHandleInfo *handle = &G->mResources[image->object];
		handle->type = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
		handle->hash = image->hash;
		handle->orig_hash = image->hash;
		handle->data_hash = image->data_hash;
		handle->texture_override_candidates_valid = false;
		handle->desc2D = desc;
		LeaveCriticalSection(&G->mResourcesLock);
		ForgetTextureOverrideMiss(image->object);
		image->shown_hash = image->hash;
	}
	if (image)
		shown = image->srv;
	ReleaseSRWLockExclusive(&image_lock);
	return shown;
}

ID3D11ShaderResourceView* VkScopeShaderResource(int stage, UINT slot)
{
	PublishedConstants *published = NULL;
	const ShaderLayout *shader;
	const SpirvBinding *view;
	VkDeviceSize bytes = 0, buffer_bytes = 0;
	const char *why;
	SetBuffer bound;

	if (!scope || stage >= DX12_STAGE_CS || slot >= SCOPE_SRVS)
		return NULL;
	if (scope->srvs_asked[stage] & 1u << slot)
		return scope->srvs[stage][slot];
	scope->srvs_asked[stage] |= 1u << slot;
	scope->srvs[stage][slot] = NULL;

	shader = scope->state.pipeline->layouts[stage];
	if (!shader || slot >= shader->views.size())
		return NULL;
	view = &shader->views[slot];
	// Of the pixel shader, the textures by their hashes. Of the stages
	// before it, a storage buffer, which is where one is what moves a mesh.
	if (stage == DX12_STAGE_PS) {
		if (view->kind == SPIRV_TEXTURE || view->kind == SPIRV_TEXTURE_SAMPLER) {
			scope->srvs[stage][slot] = game_texture(view);
			stats.textures_shown += !!scope->srvs[stage][slot];
		}
		return scope->srvs[stage][slot];
	}
	if (view->kind != SPIRV_SSBO || !view->stride)
		return NULL;

	why = bound_buffer(view, &bound);
	if (!why) {
		AcquireSRWLockShared(&memory_lock);
		auto info = buffers.find(bound.buffer);
		if (info != buffers.end())
			buffer_bytes = info->second.size;
		ReleaseSRWLockShared(&memory_lock);

		bytes = bound.range;
		if (bytes == VK_WHOLE_SIZE)
			bytes = buffer_bytes > bound.offset ? buffer_bytes - bound.offset : 0;
		bytes -= bytes % view->stride;
		if (!bytes)
			why = "the size of the buffer is not known";
		else if (bytes > VIEW_MAX_BYTES)
			why = "it is larger than what is read for a mod";
		else if (!(published = publish_constants(bound.buffer, bound.offset, (UINT)bytes, view->stride)))
			why = "the game does not have the memory of the buffer mapped";
	}
	note_view(stage, slot, view, bytes, buffer_bytes, why);

	if (!published) {
		stats.views_missing++;
		scope->srv_missing = true;
		return NULL;
	}
	stats.views_shown++;
	scope->srvs[stage][slot] = published->srv;
	return published->srv;
}

static D3D_SRV_DIMENSION view_dimension(const SpirvBinding *view)
{
	if (view->kind == SPIRV_SSBO || view->kind == SPIRV_TEXEL_BUFFER)
		return D3D_SRV_DIMENSION_BUFFER;
	// The Dim of the image type
	switch (view->dim) {
		case 0: return view->arrayed ? D3D_SRV_DIMENSION_TEXTURE1DARRAY : D3D_SRV_DIMENSION_TEXTURE1D;
		case 1: return view->arrayed ? D3D_SRV_DIMENSION_TEXTURE2DARRAY : D3D_SRV_DIMENSION_TEXTURE2D;
		case 2: return D3D_SRV_DIMENSION_TEXTURE3D;
		case 3: return view->arrayed ? D3D_SRV_DIMENSION_TEXTURECUBEARRAY : D3D_SRV_DIMENSION_TEXTURECUBE;
	}
	return D3D_SRV_DIMENSION_UNKNOWN;
}

const ShaderBindings* VkScopeShaderBindings(int stage)
{
	ShaderLayout *shader = scope && stage < DX12_STAGE_COUNT ? scope->state.pipeline->layouts[stage] : NULL;
	ShaderBindings *declared;
	size_t i;

	if (!shader || shader->declared)
		return shader ? shader->declared : NULL;

	declared = new ShaderBindings();
	for (i = 0; i < shader->cbs.size() && i < declared->constant_buffers.size(); i++) {
		declared->constant_buffers[i].type = ShaderConstantBufferType::IMMEDIATE_INDEXED;
		// In vectors of four, as a D3D11 shader declares it
		declared->constant_buffers[i].size = (shader->cbs[i].bytes + 15) / 16;
	}
	for (i = 0; i < shader->views.size() && i < declared->resources.size(); i++) {
		const SpirvBinding *view = &shader->views[i];

		declared->resources[i].type = view->kind == SPIRV_SSBO ? ShaderResourceType::STRUCTURED : ShaderResourceType::TYPED;
		declared->resources[i].dimension = view_dimension(view);
		declared->resources[i].stride = view->kind == SPIRV_SSBO ? view->stride : 0;
	}
	shader->declared = declared;
	return declared;
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

static void bind_pipeline(VkPipeline pipeline)
{
	if (scope->bound_pipeline == pipeline)
		return;
	api.CmdBindPipeline(scope->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	scope->bound_pipeline = pipeline;
}

static void bind_set(UINT slot, VkDescriptorSet set, const uint32_t *dynamic)
{
	const CmdState *game = &scope->state;
	size_t dynamic_bytes = game->dynamic_count[slot] * sizeof(uint32_t);

	if (scope->sets[slot] == set && !memcmp(scope->dynamic[slot], dynamic, dynamic_bytes))
		return;
	api.CmdBindDescriptorSets(scope->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, game->layouts[slot], slot, 1, &set,
		game->dynamic_count[slot], dynamic);
	scope->sets[slot] = set;
	memcpy(scope->dynamic[slot], dynamic, dynamic_bytes);
	scope->sets_changed |= 1u << slot;
}

// The game's bindings, where the draws of mods left others
static void restore_bindings()
{
	const CmdState *game = &scope->state;
	unsigned long slot;
	UINT vbs, sets;

	for (vbs = scope->vbs_changed; _BitScanForward(&slot, vbs); vbs &= vbs - 1) {
		if (game->vbs[slot])
			bind_vb(slot, game->vbs[slot], game->vb_offsets[slot]);
	}
	if (scope->ib_changed && game->ib)
		bind_ib(game->ib, game->ib_offset, game->index_type);
	for (sets = scope->sets_changed; _BitScanForward(&slot, sets); sets &= sets - 1)
		bind_set(slot, game->sets[slot], game->dynamic[slot]);
	bind_pipeline(game->pipeline->handle);
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
	const ShimInputLayout *layout = NULL;
	Mirror *replaced[MAX_VBS] = {};
	UINT64 vertices = UINT64_MAX;
	UINT strides[MAX_VBS];
	VkDeviceSize size;
	Mirror *mirror;
	UINT slot;

	memcpy(strides, pipeline->vb_strides, sizeof(strides));
	if (mod->input_layout && scope->game_layout && mod->input_layout != scope->game_layout)
		layout = static_cast<const ShimInputLayout*>(mod->input_layout);

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
		strides[slot] = mod->vb_strides[slot];
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
			strides[slot] = strides[other];
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
	if (layout || memcmp(strides, pipeline->vb_strides, sizeof(strides))) {
		draw->pipeline = pipeline_variant(game->pipeline, layout, strides);
		if (!draw->pipeline) {
			mod_draw_failed("the vertex buffers have other strides or another layout than the game's pipeline reads, "
				"and no pipeline could be made of the game's that reads them");
			return false;
		}
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
		bind_pipeline(draw.pipeline ? draw.pipeline : game->pipeline->handle);
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
		for (slot = 0; slot < MAX_SETS; slot++) {
			if (draw.sets_set & 1u << slot)
				bind_set(slot, draw.sets[slot], draw.dynamic[slot]);
			else if (scope->sets_changed & 1u << slot)
				bind_set(slot, game->sets[slot], game->dynamic[slot]);
		}

		if (draw.indexed)
			api.CmdDrawIndexed(scope->cmd, draw.count, draw.instances, draw.start, draw.base_vertex, draw.start_instance);
		else
			api.CmdDraw(scope->cmd, draw.count, draw.instances, draw.start, draw.start_instance);
	}
	restore_bindings();
}

// The shaders are the game's: a mod with one of its own for the draw needs a
// pipeline with it, which there is none of
static bool mod_draw_possible(const ShimState *mod)
{
	int stage;

	for (stage = 0; stage < DX12_STAGE_COUNT; stage++) {
		if (stage != DX12_STAGE_CS && (mod->stages[stage].shader || mod->stages[stage].shader_unbound)) {
			mod_draw_failed("the mod draws with a shader of its own");
			return false;
		}
	}
	return true;
}

const UINT MAX_REPLACED = 32;

// A buffer of a mod where a shader of the game reads one, or a texture of a
// mod where it samples one
struct Replaced {
	const SpirvBinding *slot;
	ModBuffer buffer;
	VkImageView view;
};

// What the mod has set for the stages before the pixel shader goes with its
// mesh: the bones of a skeleton it put together itself, say, which its
// vertices name by their place in that. Drawn with the game's instead, such a
// mesh is anywhere, and vertices that are not numbers stay in what the game
// keeps from frame to frame. A texture that is missing only looks wrong.
static bool collect_replaced(const ShimState *mod, Replaced *replaced, UINT *count)
{
	const ID3D11ShaderResourceView *ini_params = dx12_shim->hacker_device->mIniResourceView;
	const CmdState *game = &scope->state;
	UINT64 offset, bytes, stride;
	unsigned long slot;
	UINT i, cbs;
	int stage;

	for (stage = 0; stage < DX12_STAGE_COUNT; stage++) {
		const ShimStageState *state = &mod->stages[stage];
		const ShaderLayout *shader = game->pipeline->layouts[stage];

		if (stage == DX12_STAGE_CS)
			continue;

		for (cbs = state->cb_mask; _BitScanForward(&slot, cbs); cbs &= cbs - 1) {
			ShimBuffer *buffer = static_cast<ShimBuffer*>(state->cbs[slot]);
			bool taken = false;

			// The game's own, set again by the mod
			if ((scope->cbs_asked[stage] & 1u << slot) && state->cbs[slot] == scope->cbs[stage][slot]
			    && !state->cb_first[slot] && !state->cb_count[slot])
				continue;
			// Nothing reads it
			if (!shader || slot >= shader->cbs.size() || !shader->cbs[slot].bytes)
				continue;
			// What an earlier stage reads there as well is that stage's
			for (i = 0; i < *count; i++)
				taken |= replaced[i].slot->set == shader->cbs[slot].set && replaced[i].slot->binding == shader->cbs[slot].binding;
			if (taken)
				continue;

			offset = (UINT64)state->cb_first[slot] * 16;
			bytes = state->cb_count[slot] ? (UINT64)state->cb_count[slot] * 16 : buffer->mDesc.ByteWidth;
			if (offset < buffer->mDesc.ByteWidth)
				bytes = min(bytes, min(buffer->mDesc.ByteWidth - offset, (UINT64)shader->cbs[slot].bytes));
			if (*count == MAX_REPLACED || offset >= buffer->mDesc.ByteWidth
			    || !bring_buffer(&buffer->r, offset, bytes, shader->cbs[slot].bytes, &replaced[*count].buffer)) {
				// Like a texture that is missing
				if (stage == DX12_STAGE_PS)
					continue;
				mod_draw_failed("a constant buffer the mod has set for a stage before the pixel shader could not be "
					"brought to the device of the game");
				return false;
			}
			replaced[(*count)++].slot = &shader->cbs[slot];
		}

		for (i = 0; stage == DX12_STAGE_PS && shader && i < shader->views.size() && i < SHIM_SRV_SLOTS; i++) {
			ShimView *view;

			if (!(state->srv_mask[i / 32] & 1u << (i % 32)) || state->srvs[i] == ini_params)
				continue;
			view = ShimViewOf(state->srvs[i]);
			if (shader->views[i].kind != SPIRV_TEXTURE || !view || *count == MAX_REPLACED)
				continue;
			replaced[*count].view = bring_texture(view, &shader->views[i]);
			if (!replaced[*count].view)
				continue;
			stats.textures_set++;
			replaced[(*count)++].slot = &shader->views[i];
		}

		for (i = 0; stage != DX12_STAGE_PS && i < SHIM_SRV_SLOTS; i++) {
			ShimView *view;

			if (!(state->srv_mask[i / 32] & 1u << (i % 32)) || state->srvs[i] == ini_params)
				continue;
			if (i < SCOPE_SRVS && (scope->srvs_asked[stage] & 1u << i) && state->srvs[i] == scope->srvs[stage][i])
				continue;
			if (!shader || i >= shader->views.size())
				continue;
			if (shader->views[i].kind != SPIRV_SSBO) {
				mod_draw_failed("the mod has set a buffer or texture for a stage before the pixel shader where the shader "
					"of the game reads a texture");
				return false;
			}

			view = ShimViewOf(state->srvs[i]);
			if (!view || view->resource->dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
				mod_draw_failed("the mod has set a texture for a stage before the pixel shader where the shader of the "
					"game reads a buffer");
				return false;
			}
			// A view without one is of a buffer of values of a format:
			// all of it then, if that is what the view is of
			stride = view->desc12.srv.Buffer.StructureByteStride;
			offset = view->desc12.srv.Buffer.FirstElement * stride;
			bytes = view->desc12.srv.Buffer.NumElements * stride;
			if (!stride && !view->desc12.srv.Buffer.FirstElement)
				bytes = view->resource->desc.Width;
			if (*count == MAX_REPLACED || !bytes || !bring_buffer(view->resource, offset, bytes, bytes, &replaced[*count].buffer)) {
				mod_draw_failed("a buffer the mod has set for a stage before the pixel shader could not be brought to the "
					"device of the game");
				return false;
			}
			replaced[(*count)++].slot = &shader->views[i];
		}
	}
	return true;
}

// The descriptor sets the draw of a mod is made with, where they are not the
// game's as the game has them bound
static bool replace_buffers(const ShimState *mod, ModDraw *draw)
{
	const CmdState *game = &scope->state;
	Replaced replaced[MAX_REPLACED] = {};
	std::vector<SetReplacement> descriptors;
	const char *why = NULL;
	UINT i, count = 0, set;
	GameSet tracked;

	if (!collect_replaced(mod, replaced, &count))
		return false;

	for (set = 0; set < MAX_SETS && count && !why; set++) {
		for (i = 0; i < count && replaced[i].slot->set != set; i++);
		if (i == count)
			continue;
		if (!game->sets[set] || !game->layouts[set]) {
			why = "no descriptor set was bound at the number of the set of a buffer the mod has set";
			break;
		}

		descriptors.clear();
		memcpy(draw->dynamic[set], game->dynamic[set], sizeof(draw->dynamic[set]));

		AcquireSRWLockShared(&descriptor_lock);
		const DescriptorSet *found = find_set(game->sets[set]);
		if (!found) {
			why = "the descriptor set of a buffer the mod has set was not seen being allocated";
		} else if (found->layout->dynamic != game->dynamic_count[set]) {
			why = "the offsets the descriptor set of a buffer the mod has set was bound with are not known";
		} else {
			tracked = { game->sets[set], found->layout, found->layout_handle, found->generation };
		}
		for (i = 0; i < count && !why; i++) {
			const ModBuffer *buffer = &replaced[i].buffer;
			UINT number = replaced[i].slot->binding;
			const SetBinding *binding;
			const SetBuffer *bound;
			VkDeviceSize size = 0;

			if (replaced[i].slot->set != set)
				continue;
			binding = number < found->layout->bindings.size() ? &found->layout->bindings[number] : NULL;
			if (replaced[i].view) {
				// Without it the draw only looks wrong
				if (binding && binding->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && binding->count == 1)
					descriptors.emplace_back(number, VkBuffer(), (VkDeviceSize)0, (VkDeviceSize)0, replaced[i].view);
				continue;
			}
			if (!binding || !binding->buffer || binding->count != 1) {
				why = "the layout of the descriptor set has no buffer by itself at the binding of one the mod has set";
				break;
			}
			if (!binding->dynamic) {
				descriptors.emplace_back(number, buffer->buffer, buffer->offset, buffer->range, VkImageView());
				continue;
			}

			// A part of what the game has there is only another offset
			bound = &found->buffers[binding->first];
			if (bound->buffer == buffer->buffer && bound->range != VK_WHOLE_SIZE && buffer->offset >= bound->offset) {
				AcquireSRWLockShared(&memory_lock);
				auto info = buffers.find(bound->buffer);
				if (info != buffers.end())
					size = info->second.size;
				ReleaseSRWLockShared(&memory_lock);
			}
			if (size && buffer->offset + bound->range <= size && buffer->offset - bound->offset <= UINT32_MAX) {
				draw->dynamic[set][binding->first_dynamic] = (uint32_t)(buffer->offset - bound->offset);
			} else if (buffer->offset <= UINT32_MAX) {
				draw->dynamic[set][binding->first_dynamic] = (uint32_t)buffer->offset;
				descriptors.emplace_back(number, buffer->buffer, (VkDeviceSize)0, buffer->range, VkImageView());
			} else {
				why = "a buffer the mod has set is too far into the buffer it is in for an offset";
			}
		}
		ReleaseSRWLockShared(&descriptor_lock);
		if (why)
			break;

		draw->sets[set] = descriptors.empty() ? game->sets[set] : mod_set(&tracked, descriptors);
		if (!draw->sets[set])
			why = "no descriptor set like the game's could be made for a buffer the mod has set";
		draw->sets_set |= 1u << set;
	}

	if (why)
		mod_draw_failed(why);
	return !why;
}

void VkScopeDraw(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
	const ShimState *mod = dx12_shim->shim_context->mState;
	ModDraw draw = {};

	stats.mod_draws++;
	// Nothing of this is the game's: what it writes to a buffer of the
	// mod's, which is all it can reach, is made on the D3D12 device as a
	// dispatch of the mod's is
	if (mod->stages[DX12_STAGE_VS].shader && mod->stages[DX12_STAGE_PS].shader) {
		if (dx12_shim->shim_context->DrawApart(indexed, count, instances, start, base_vertex, start_instance))
			stats.apart_draws++;
		else
			mod_draw_failed("the mod draws with shaders of its own, which need a target of the game or could not be put together");
		return;
	}
	// D3D11 reads every index as 0 without an index buffer, which draws
	// nothing
	if (indexed && (mod->ib_unbound || (!mod->ib && !scope->state.ib)))
		return;

	// The game's is drawn anyway
	if (scope->mod_draws_failed)
		return;
	if (scope->srv_missing) {
		mod_draw_failed("a buffer of the game the mod asked for could not be shown");
		return;
	}

	draw.indexed = indexed;
	draw.count = count;
	draw.instances = instances;
	draw.start = start;
	draw.base_vertex = base_vertex;
	draw.start_instance = start_instance;
	if (mod_draw_possible(mod) && bind_for_mod(mod, &draw) && replace_buffers(mod, &draw))
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
	opened.sets_changed = 0;
	opened.bound_pipeline = state->pipeline->handle;
	opened.game_layout = NULL;
	memcpy(opened.sets, state->sets, sizeof(opened.sets));
	memcpy(opened.dynamic, state->dynamic, sizeof(opened.dynamic));
	opened.mod_draws_failed = 0;
	memset(opened.cbs_asked, 0, sizeof(opened.cbs_asked));
	memset(opened.srvs_asked, 0, sizeof(opened.srvs_asked));
	opened.srv_missing = false;
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
		ShaderLayout *layout = NULL;

		info.hash = shader.hash;
		info.stage = (Dx12ShaderStage)shader.stage;
		link_shader_regex_groups_dx12(&info);

		// and those with patterns that what the shader declares is enough for
		AcquireSRWLockShared(&registry_lock);
		auto known = shader_layouts.find(shader.hash);
		if (known != shader_layouts.end())
			layout = known->second;
		ReleaseSRWLockShared(&registry_lock);
		if (layout && !layout->declarations.empty()
		    && link_shader_regex_groups_declared(dx12_stage_names[shader.stage], shader.hash, &layout->declarations))
			matched_shaders++;
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
	linked_shaders = matched_shaders = 0;
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

	// A note is cut off past a thousand characters
	VkDumpNote("Mods, frame %u (D3D12 frame %u), since the last of these lines: the engine saw %u draws and skipped %u, mods drew %u times "
		"of which %u could not be made, %u buffers of mods copied (%llu KB), %Iu held; "
		"index regions hashed %u, found as they were %u, not mapped %u; per frame the draws took %.2f ms waiting for the lock "
		"and %.2f ms in the engine, %.2f us a draw; %Iu shaders linked to ShaderRegex (%Iu by what they declare), %Iu ShaderOverride hashes, %Iu buffers published\n",
		vk_frame, dx12_frame, stats.draws, stats.skipped, stats.mod_draws, stats.mod_draws_failed, stats.mirrored,
		stats.mirrored_bytes >> 10, mirrors.size(), stats.regions_hashed, stats.regions_checked,
		stats.regions_unmapped, stats.wait_ticks * per_frame, stats.engine_ticks * per_frame, stats.engine_ticks * per_draw,
		linked_shaders, matched_shaders, G->mShaderOverrideMap.size(), published.size());
	VkDumpNote("  Constant buffers of the game: shown %u times, read %u times (%llu KB, %.2f ms per frame), not mapped %u, no buffer seen set at the "
		"binding %u; %Iu held. Descriptor sets: %u allocated, buffers written %u times by vkUpdateDescriptorSets and %u times "
		"by a template\n",
		stats.constants_shown, stats.constants_read, stats.constants_bytes >> 10, stats.constants_ticks * per_frame, stats.constants_unmapped,
		stats.constants_unbound, published_constants.size(), sets_allocated, sets_written, sets_written_by_template);
	VkDumpNote("  Storage buffers of the game: shown %u times, not shown %u. Buffers of mods for the shaders of the game: %u brought over "
		"(%llu KB), %u of them from the GPU, which %u submissions of the game waited %.2f ms per frame for; %u descriptor sets made, "
		"%Iu held, %Iu MB of buffers for them\n",
		stats.views_shown, stats.views_missing, stats.brought, stats.brought_bytes >> 10, stats.brought_from_gpu,
		stats.submits_waited, stats.submit_ticks * per_frame, stats.sets_made, mod_sets.size(),
		(size_t)(transients.size() * TRANSIENT_BYTES >> 20));
	VkDumpNote("  Pipelines made again for the vertex buffers of mods: %u, %u of them not, %.2f ms per frame; %u pipelines of the game "
		"kept to be made again, %u could not be, the first structure not known: %d\n",
		stats.variants_made, stats.variants_failed, stats.variant_ticks * per_frame, recipes_kept, recipes_failed,
		(int)recipe_unknown_structure);
	VkDumpNote("  Textures of mods for the pixel shaders of the game: set %u times, %u images made (%llu KB copied), %u could not be; "
		"%Iu held in %Iu blocks of memory; draws of mods with shaders of their own: %u; textures of the game shown by their hashes "
		"%u times, %d hashed, %d filled from memory that is not mapped\n",
		stats.textures_set, stats.textures_made, stats.textures_bytes >> 10, stats.textures_failed, mod_textures.size(),
		image_blocks.size(), stats.apart_draws, stats.textures_shown, (int)images_hashed, (int)images_not_mapped);
	VkDumpNote("  The draws were stopped for because of: settings %u, a pipeline not looked at yet, command lists doing more than asking "
		"or a condition of one %u, an index count a section asks for %u, a constant buffer a list asks for %u; "
		"about %u more were not stopped for\n",
		stats.slow[SLOW_SETTINGS], stats.slow[SLOW_LISTS], stats.slow[SLOW_INDEX_COUNT], stats.slow[SLOW_CONSTANTS],
		(unsigned)InterlockedExchange(&fast_draws, 0));
	stats = {};
	AcquireSRWLockExclusive(&descriptor_lock);
	sets_allocated = sets_written = sets_written_by_template = 0;
	ReleaseSRWLockExclusive(&descriptor_lock);
}

void VkModsPresent(VkQueue queue)
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

	release_retired_images();

	if (!engine_reported) {
		engine_reported = true;
		VkDumpNote("vulkan = 1: the draws of the game are shown to the engine of the D3D12 device that presents its frames\n");
	}

	if (!device_removed_reported && FAILED(removed = dx12_shim->device->GetDeviceRemovedReason())) {
		device_removed_reported = true;
		VkDumpNote("*** Frame %u: the D3D12 device that presents the frames has been removed, reason 0x%x. "
			"Nothing new is shown from here on.\n", vk_frame, removed);
	}

	// Whatever was recorded with them has been handed to a queue by now
	finish_transfers(queue);
	brought.clear();
	if (!(vk_frame & 255))
		release_readbacks();

	forget_dead_buffers();
	forget_unused_constants(false);
	if (mods_device) {
		destroy_retired_mirrors(false);
		destroy_retired_textures(false);
		destroy_big_transients();
		release_unused_mod_sets();
		destroy_variants(false);
	}
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
