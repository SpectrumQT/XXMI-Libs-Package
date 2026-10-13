#pragma once

#include "VkCommon.h"

// With vk_dump_hashes in [Hunting], what a Vulkan game creates and draws with
// is identified the way the D3D11 version of it would be and written next to
// the DLL, for telling how much of what mods match on is there to be found:
//
//   vk_shaders.csv    every shader module, with what its SPIR-V declares
//   vk_shaders\       the SPIR-V of each, named by hash and stage
//   vk_pipelines.csv  every pipeline: its shaders and vertex layout
//   vk_buffers.csv    every upload to a vertex or index buffer that was hashed
//   vk_draws.csv      what the draws of one frame use, a frame every few
//                     seconds
//   vk_report.txt     the totals, and how many of the hashes are ones the
//                     installed mods ask for
//
// A buffer is hashed when the game copies to it from a staging buffer whose
// memory is mapped, from the bytes found there: the same bytes D3D11 would
// have been given as initial data, if the game uploads the same way.

// Whether any of the below does anything. Set once, when the first device is
// created.
extern bool vk_dump_active;

void VkDumpNote(const char *format, ...);

void VkDumpShaderModule(VkShaderModule module, const VkShaderModuleCreateInfo *info);
void VkDumpForgetShaderModule(VkShaderModule module);

void VkDumpGraphicsPipeline(VkPipeline pipeline, const VkGraphicsPipelineCreateInfo *info);
void VkDumpComputePipeline(VkPipeline pipeline, const VkComputePipelineCreateInfo *info);
void VkDumpForgetPipeline(VkPipeline pipeline);

void VkDumpBufferCreated(VkBuffer buffer, const VkBufferCreateInfo *info);
void VkDumpBufferBound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset);
void VkDumpForgetBuffer(VkBuffer buffer);
void VkDumpMemoryMapped(VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, void *data);
// Before the memory is unmapped or freed, as an upload may be being read
void VkDumpMemoryUnmapped(VkDeviceMemory memory);

// When the copy is recorded
void VkDumpCopyBuffer(VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions);

// For telling how the game fills its buffers where it is not such a copy
void VkDumpUpdateBuffer(VkBuffer dst, VkDeviceSize bytes);
void VkDumpFlush(uint32_t count, const VkMappedMemoryRange *ranges);

// Nothing is bound to a command buffer that is begun
void VkDumpBeginCommands(VkCommandBuffer cmd);
void VkDumpBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline);
void VkDumpBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count, const VkBuffer *buffers, const VkDeviceSize *offsets);
void VkDumpBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type);
void VkDumpDraw(VkCommandBuffer cmd, const char *type, uint32_t count, uint32_t instances, uint32_t first, int32_t base_vertex);

void VkDumpPresent();
