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
//
// Not yet: anything a mod binds or draws, the game's vertex buffers, constant
// buffers and textures, indirect draws, dispatches.

// Set once a device has been taken on, for the hooks to tell whether to
// report here
extern bool vk_mods_active;

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
void VkModsBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);

// True if the draw was dealt with, which includes making it with whichever of
// the two functions it is for: the caller then has nothing left to do
bool VkModsDraw(VkCommandBuffer cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
		int32_t vertex_offset, uint32_t first_instance, PFN_vkCmdDraw draw, PFN_vkCmdDrawIndexed draw_indexed);

void VkModsPresent();
