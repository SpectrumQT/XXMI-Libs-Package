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
//     pipeline is the game's, or one made of what that was created from
//     where the vertex buffers of the mod have other strides or another
//     layout (ElementFormat). For that the shader modules, render passes and
//     pipeline layouts the game destroys are kept.
//
//   - The constant buffers of the game's shaders, for a mod to read: cbN of
//     a stage is the Nth uniform block its shader declares, and holds what
//     the draw has there, which the game has to have in mapped memory.
//     vs->cb_size(N) and the like answer by the same numbering. A storage
//     buffer the shader only reads is at a t register the same way, counted
//     along with its textures.
//   - What a mod sets in place of such a buffer for the stages before the
//     pixel shader goes with its draw: constants, or a buffer a compute
//     shader of the mod wrote on the D3D12 device, as a mod that puts a
//     skeleton together does. The contents are brought to a buffer of the
//     game's device, and the draw gets a descriptor set that is the game's
//     but for that buffer.
//
//   - A texture a mod sets for the pixel shader goes with its draw the same
//     way: as an image of the game's device, filled once with what the
//     engine loaded on the D3D12 device.
//
//   - A draw of a mod with a vertex and a pixel shader of its own is recorded
//     on the D3D12 device. It has none of the targets of the game there, so
//     all it is good for is writing a buffer of the mod through a UAV.
//
//   - The ShaderRegex sections that tell shaders apart by their declarations
//     are matched against those of the SPIR-V shader, written out as the
//     D3D11 disassembler would.
//
//   - A texture of the game is shown to the engine by the hash its D3D11
//     texture would have, worked out from what the game fills the image
//     with, so a TextureOverride section finds it. Its contents are not: a
//     mod cannot read it or bind it elsewhere.
//
// Not yet: shaders of its own for a draw to the targets of the game, the
// game's vertex buffers, indirect draws, dispatches.

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
	PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
	PFN_vkDestroyPipeline DestroyPipeline;
	PFN_vkCmdBindPipeline CmdBindPipeline;
	PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers;
	PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer;
	PFN_vkCreateDescriptorPool CreateDescriptorPool;
	PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
	PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
	PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
	PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
	PFN_vkCreateImage CreateImage;
	PFN_vkDestroyImage DestroyImage;
	PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
	PFN_vkBindImageMemory BindImageMemory;
	PFN_vkCreateImageView CreateImageView;
	PFN_vkDestroyImageView DestroyImageView;
	PFN_vkCreateCommandPool CreateCommandPool;
	PFN_vkDestroyCommandPool DestroyCommandPool;
	PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
	PFN_vkFreeCommandBuffers FreeCommandBuffers;
	PFN_vkBeginCommandBuffer BeginCommandBuffer;
	PFN_vkEndCommandBuffer EndCommandBuffer;
	PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
	PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage;
	PFN_vkQueueSubmit QueueSubmit;
	PFN_vkCreateSemaphore CreateSemaphore;
	PFN_vkDestroySemaphore DestroySemaphore;
	PFN_vkCmdDraw CmdDraw;
	PFN_vkCmdDrawIndexed CmdDrawIndexed;
};

// handle_properties is NULL for a device without VK_KHR_external_memory_win32,
// which then shares no memory with the D3D12 device, and import_semaphore for
// one without VK_KHR_external_semaphore_win32, which cannot wait for it
void VkModsDevice(VkDevice device, VkPhysicalDevice physical_device, const VkModsApi *api,
		PFN_vkGetMemoryWin32HandlePropertiesKHR handle_properties, PFN_vkImportSemaphoreWin32HandleKHR import_semaphore);
void VkModsForgetDevice(VkDevice device);
// A queue the game got, and the family it is of
void VkModsImageCreated(VkImage image, const VkImageCreateInfo *info);
void VkModsForgetImage(VkImage image);
void VkModsImageView(VkImageView view, const VkImageViewCreateInfo *info);
void VkModsForgetImageView(VkImageView view);
void VkModsCopyBufferToImage(VkBuffer src, VkImage dst, uint32_t count, const VkBufferImageCopy *regions);
void VkModsQueue(VkDevice device, VkQueue queue, uint32_t family);

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
void VkModsExecuteCommands(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer *cmds);
void VkModsBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline);
void VkModsBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets);
void VkModsBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
void VkModsSetLayout(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutCreateInfo *info);
// The game destroyed a layout, which is kept: see Hooked_vkDestroyDescriptorSetLayout
void VkModsSetLayoutKept();
void VkModsSetsAllocated(const VkDescriptorSetAllocateInfo *info, const VkDescriptorSet *sets);
void VkModsSetsUpdated(uint32_t write_count, const VkWriteDescriptorSet *writes, uint32_t copy_count, const VkCopyDescriptorSet *copies);
void VkModsUpdateTemplate(VkDescriptorUpdateTemplate update_template, const VkDescriptorUpdateTemplateCreateInfo *info);
void VkModsSetUpdatedWithTemplate(VkDescriptorSet set, VkDescriptorUpdateTemplate update_template, const void *data);
void VkModsBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout, uint32_t first,
		uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *dynamic_offsets);

// Before command buffers are handed to a queue: what their draws read of the
// mods has to be where they read it by then. The caller is the one using the
// queue, as the game is by calling.
//
// One who can have the command buffers wait for a fence of the D3D12 device
// asks with a value: what is returned then is a semaphore that is such a
// fence, if what the draws read is there once the fence has reached the value
// and not before. Nothing is waited for here in that case.
//
// shown is whether any of the command buffers has draws the engine was shown,
// see VkModsCommandsShown: no other submission is one to wait.
VkSemaphore VkModsBeforeSubmit(VkQueue queue, UINT64 *value, bool shown);

// How many draws of the command buffer the engine was shown, and that a
// submission with such is handed to a queue, with how many it has: the
// game's frame, which the engine counts by. True if it counted a frame.
unsigned VkModsCommandsShown(VkCommandBuffer cmd);
bool VkModsFrameSubmitted(unsigned draws);

// True if the draw was dealt with, which includes making it with whichever of
// the two functions it is for: the caller then has nothing left to do
bool VkModsDraw(VkCommandBuffer cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
		int32_t vertex_offset, uint32_t first_instance, PFN_vkCmdDraw draw, PFN_vkCmdDrawIndexed draw_indexed);

void VkModsPresent(VkQueue queue);

// With vk_dump_hashes the device is created with VK_EXT_device_fault and
// VK_NV_device_diagnostic_checkpoints where the driver has them, for
// VkModsDeviceLost to say where the GPU was. Either function may be NULL.
void VkModsDiagnostics(VkDevice device, PFN_vkGetDeviceFaultInfoEXT fault_info, PFN_vkCmdSetCheckpointNV set_checkpoint,
		PFN_vkGetQueueCheckpointDataNV checkpoint_data);
void VkModsCommandsBegun(VkCommandBuffer cmd);
// The game enabled VK_EXT_memory_budget, which shared buffers are kept within
void VkModsMemoryBudget(VkDevice device);
// With VK_EXT_device_address_binding_report, which the instance is given
// VK_EXT_debug_utils for: GPU addresses bound to an object or let go of, for
// VkModsDeviceLost to say what was at the address of a fault. From any thread.
void VkModsAddressBinding(bool bound, UINT64 base, UINT64 size, VkObjectType type, UINT64 handle);
// A call on the queue returned VK_ERROR_DEVICE_LOST. The game may well go on
// recording and submitting, which nothing of the mods is part of from then on.
void VkModsDeviceLost(VkQueue queue);
