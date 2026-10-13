#pragma once

#include "VkCommon.h"

// Mods for a Vulkan game, with vulkan = 1 in [Rendering].
//
// The engine only knows D3D11, and there is no Vulkan version of what gives
// it a D3D11 device to work with. What there is is the D3D12 one (dx12/): a
// driver that presents Vulkan through a DXGI swap chain creates a D3D12
// device and queue in the process for it, which get the shim like those of a
// D3D12 game do. That engine is the one used here. Its frame is the swap
// chain's, and a draw of the game is shown to it the way dx12/DrawPath shows
// one of a D3D12 game: as a scope in which the shim context answers for what
// the game has bound, here from what was recorded to the Vulkan command
// buffer.
//
// What the engine can do from there so far:
//
//   - ShaderOverride and ShaderRegex sections run for the shaders of the
//     draw. A shader's hash is that of its SPIR-V, and a ShaderRegex section
//     can only match by stage: one without a pattern has all the shaders of
//     the stages of its shader_model.
//   - checktextureoverride = ib finds the sections of a game that keeps its
//     meshes in shared buffers (track_region_hashes): the hash is of the
//     indices drawn, read from the memory behind the buffer, which the game
//     has to have mapped.
//   - handling = skip
//   - A mod draws with vertex and index buffers of its own, as long as their
//     contents came from the CPU (a file, or data in the ini): they are
//     copied to buffers of the game's device, and the draw is recorded to the
//     game's command buffer with everything else as the game has it. The
//     pipeline is the game's, so a vertex buffer has to have the stride the
//     game's has in its place.
//
//   - The constant buffers of the game's shaders, for a mod to read: cbN of
//     a stage is the Nth uniform block its shader declares, and holds what
//     the draw has there, which the game has to have in mapped memory.
//     vs->cb_size(N) and the like answer by the same numbering.
//
// Not yet: anything else a mod binds, buffers a mod has the GPU write, the
// game's vertex buffers and textures, indirect draws, dispatches.

// Set once a device has been taken on, for the hooks to tell whether to
// report here
extern bool vk_mods_active;

// The functions of a device as they are past the hooks, for what is made and
// recorded here
struct VkModsApi {
	PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
	PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
	PFN_vkAllocateMemory AllocateMemory;
	PFN_vkFreeMemory FreeMemory;
	PFN_vkMapMemory MapMemory;
	PFN_vkCreateBuffer CreateBuffer;
	PFN_vkDestroyBuffer DestroyBuffer;
	PFN_vkBindBufferMemory BindBufferMemory;
	PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
	PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
	PFN_vkCmdDraw CmdDraw;
	PFN_vkCmdDrawIndexed CmdDrawIndexed;
};

void VkModsDevice(VkDevice device, VkPhysicalDevice physical_device, const VkModsApi *api);
void VkModsForgetDevice(VkDevice device);

void VkModsShaderModule(VkShaderModule module, const VkShaderModuleCreateInfo *info);
void VkModsForgetShaderModule(VkShaderModule module);
void VkModsGraphicsPipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo *info);
void VkModsForgetPipeline(VkPipeline pipeline);

void VkModsBufferCreated(VkBuffer buffer, const VkBufferCreateInfo *info);
void VkModsBufferBound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset);
void VkModsForgetBuffer(VkBuffer buffer);
void VkModsMemoryMapped(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, void *data);
void VkModsMemoryUnmapped(VkDeviceMemory memory);

void VkModsBeginCommands(VkCommandBuffer cmd);
void VkModsBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline);
void VkModsBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets);
void VkModsBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
void VkModsSetLayout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutCreateInfo *info);
void VkModsSetsAllocated(const VkDescriptorSetAllocateInfo *info, const VkDescriptorSet *sets);
void VkModsSetsUpdated(uint32_t write_count, const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *copies);
void VkModsUpdateTemplate(VkDescriptorUpdateTemplate update_template, const VkDescriptorUpdateTemplateCreateInfo *info);
void VkModsSetUpdatedWithTemplate(VkDescriptorSet set, VkDescriptorUpdateTemplate update_template, const void *data);
void VkModsBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, uint32_t first, uint32_t count,
		const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *dynamic_offsets);

// True if the draw was dealt with, which includes making it with whichever of
// the two functions it is for: the caller then has nothing left to do
bool VkModsDraw(VkCommandBuffer cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
		int32_t vertex_offset, uint32_t first_instance, PFN_vkCmdDraw draw, PFN_vkCmdDrawIndexed draw_indexed);

void VkModsPresent();
