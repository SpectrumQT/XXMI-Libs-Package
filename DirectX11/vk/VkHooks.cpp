#include "VkHooks.h"

#include <vector>

#include "VkCommon.h"
#include "VkDrawPath.h"
#include "VkDump.h"

#include "util.h"
#include "../cursor.h" // For InstallHookLate
#include "../D3D11Wrapper.h"
#include "../DLLMainHook.h"
#include "../Globals.h"

// The functions of a device we step into. All of them take a device, queue or
// command buffer first, which is what the continuation is found by.
#define VK_DEVICE_FUNCS(X) \
	X(DestroyDevice) \
	X(GetDeviceQueue) \
	X(GetDeviceQueue2) \
	X(CreateShaderModule) \
	X(DestroyShaderModule) \
	X(CreateGraphicsPipelines) \
	X(CreateComputePipelines) \
	X(DestroyPipeline) \
	X(DestroyPipelineLayout) \
	X(DestroyRenderPass) \
	X(CreateBuffer) \
	X(DestroyBuffer) \
	X(BindBufferMemory) \
	X(BindBufferMemory2) \
	X(MapMemory) \
	X(UnmapMemory) \
	X(FreeMemory) \
	X(BeginCommandBuffer) 	X(CmdExecuteCommands) \
	X(FlushMappedMemoryRanges) \
	X(CmdCopyBuffer) \
	X(CmdCopyBuffer2) \
	X(CreateImage) \
	X(DestroyImage) \
	X(CreateImageView) \
	X(DestroyImageView) \
	X(CmdCopyBufferToImage) \
	X(CmdUpdateBuffer) \
	X(CmdBindPipeline) \
	X(CmdBindVertexBuffers) \
	X(CmdBindIndexBuffer) \
	X(CreateDescriptorSetLayout) \
	X(DestroyDescriptorSetLayout) \
	X(AllocateDescriptorSets) \
	X(UpdateDescriptorSets) \
	X(CreateDescriptorUpdateTemplate) \
	X(UpdateDescriptorSetWithTemplate) \
	X(CmdBindDescriptorSets) \
	X(QueueSubmit) \
	X(QueueSubmit2) \
	X(CmdDraw) \
	X(CmdDrawIndexed) \
	X(CmdDrawIndirect) \
	X(CmdDrawIndexedIndirect) \
	X(CmdDispatch) \
	X(QueuePresentKHR)

struct VkFuncs {
#define X(name) PFN_vk##name name;
	VK_DEVICE_FUNCS(X)
#undef X
};

// What vkGetDeviceProcAddr says of a device, which is where a call to it
// continues. key is what the loader has put at the start of the device and of
// everything that belongs to it, NULL for a slot that is free.
struct VkDispatch {
	void *volatile key;
	VkDevice device;
	VkFuncs next;
};

static VkDispatch dispatch_tables[8];
static SRWLOCK dispatch_lock = SRWLOCK_INIT;

// The originals of the exports, which find the device's functions like the
// loader does for whoever calls an export: where a call goes for a device we
// have no table of
static VkFuncs exports;

volatile UINT vk_frame;

static PFN_vkCreateInstance fnOrigCreateInstance;
static PFN_vkCreateDevice fnOrigCreateDevice;
static PFN_vkGetDeviceProcAddr fnOrigGetDeviceProcAddr;
// The instance has a messenger of ours for VkModsAddressBinding
static bool binding_messenger;

static inline const VkDispatch* find_dispatch(void *handle)
{
	void *key = *(void**)handle;
	unsigned i;

	for (i = 0; i < ARRAYSIZE(dispatch_tables); i++) {
		if (dispatch_tables[i].key == key)
			return &dispatch_tables[i];
	}
	return NULL;
}

#define X(name) \
	static inline PFN_vk##name next_##name(void *handle) \
	{ \
		const VkDispatch *dispatch = find_dispatch(handle); \
		return dispatch && dispatch->next.name ? dispatch->next.name : exports.name; \
	}
VK_DEVICE_FUNCS(X)
#undef X

// -----------------------------------------------------------------------------

static void VKAPI_CALL Hooked_vkDestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
	PFN_vkDestroyDevice next = next_DestroyDevice(device);
	unsigned i;

	if (device) {
		AcquireSRWLockExclusive(&dispatch_lock);
		for (i = 0; i < ARRAYSIZE(dispatch_tables); i++) {
			if (dispatch_tables[i].device == device) {
				dispatch_tables[i].key = NULL;
				dispatch_tables[i].device = NULL;
			}
		}
		ReleaseSRWLockExclusive(&dispatch_lock);
		if (vk_mods_active)
			VkModsForgetDevice(device);
		VkLogInfo("vkDestroyDevice(%p)\n", device);
	}

	next(device, allocator);
}

static void VKAPI_CALL Hooked_vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
	next_GetDeviceQueue(device)(device, family, index, queue);
	if (vk_mods_active && queue && *queue)
		VkModsQueue(device, *queue, family);
}

static void VKAPI_CALL Hooked_vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
	next_GetDeviceQueue2(device)(device, info, queue);
	if (vk_mods_active && info && queue && *queue)
		VkModsQueue(device, *queue, info->queueFamilyIndex);
}

static VkResult VKAPI_CALL Hooked_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkShaderModule *module)
{
	VkResult result = next_CreateShaderModule(device)(device, info, allocator, module);

	if (result == VK_SUCCESS && module && *module) {
		if (vk_dump_active)
			VkDumpShaderModule(*module, info);
		if (vk_mods_active)
			VkModsShaderModule(*module, info);
	}
	return result;
}

static void VKAPI_CALL Hooked_vkDestroyShaderModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks *allocator)
{
	// Kept, like the two below: a pipeline of the game may be made again
	// for the vertices of a mod, of what it was created from
	if (vk_mods_active)
		return;
	if (module && vk_dump_active)
		VkDumpForgetShaderModule(module);
	if (module && vk_mods_active)
		VkModsForgetShaderModule(module);
	next_DestroyShaderModule(device)(device, module, allocator);
}

static VkResult VKAPI_CALL Hooked_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
		const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
	VkResult result = next_CreateGraphicsPipelines(device)(device, cache, count, infos, allocator, pipelines);
	uint32_t i;

	// Also where only some were created
	for (i = 0; pipelines && i < count; i++) {
		if (result < 0 || !pipelines[i])
			continue;
		if (vk_dump_active)
			VkDumpGraphicsPipeline(pipelines[i], &infos[i]);
		if (vk_mods_active)
			VkModsGraphicsPipeline(pipelines[i], &infos[i]);
	}
	return result;
}

static VkResult VKAPI_CALL Hooked_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
		const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
	VkResult result = next_CreateComputePipelines(device)(device, cache, count, infos, allocator, pipelines);
	uint32_t i;

	for (i = 0; pipelines && i < count; i++) {
		if (result >= 0 && pipelines[i] && vk_dump_active)
			VkDumpComputePipeline(pipelines[i], &infos[i]);
	}
	return result;
}

static void VKAPI_CALL Hooked_vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *allocator)
{
	if (pipeline && vk_dump_active)
		VkDumpForgetPipeline(pipeline);
	if (pipeline && vk_mods_active)
		VkModsForgetPipeline(pipeline);
	next_DestroyPipeline(device)(device, pipeline, allocator);
}

static void VKAPI_CALL Hooked_vkDestroyPipelineLayout(VkDevice device, VkPipelineLayout layout, const VkAllocationCallbacks *allocator)
{
	if (!vk_mods_active)
		next_DestroyPipelineLayout(device)(device, layout, allocator);
}

static void VKAPI_CALL Hooked_vkDestroyRenderPass(VkDevice device, VkRenderPass pass, const VkAllocationCallbacks *allocator)
{
	if (!vk_mods_active)
		next_DestroyRenderPass(device)(device, pass, allocator);
}

static VkResult VKAPI_CALL Hooked_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
	VkResult result = next_CreateBuffer(device)(device, info, allocator, buffer);

	if (result == VK_SUCCESS && buffer && *buffer) {
		if (vk_dump_active)
			VkDumpBufferCreated(*buffer, info);
		if (vk_mods_active)
			VkModsBufferCreated(*buffer, info);
	}
	return result;
}

static void VKAPI_CALL Hooked_vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
	if (buffer && vk_dump_active)
		VkDumpForgetBuffer(buffer);
	if (buffer && vk_mods_active)
		VkModsForgetBuffer(buffer);
	next_DestroyBuffer(device)(device, buffer, allocator);
}

static void buffer_bound(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
	if (vk_dump_active)
		VkDumpBufferBound(buffer, memory, offset);
	if (vk_mods_active)
		VkModsBufferBound(buffer, memory, offset);
}

static VkResult VKAPI_CALL Hooked_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
	VkResult result = next_BindBufferMemory(device)(device, buffer, memory, offset);

	if (result == VK_SUCCESS)
		buffer_bound(buffer, memory, offset);
	return result;
}

static VkResult VKAPI_CALL Hooked_vkBindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
	VkResult result = next_BindBufferMemory2(device)(device, count, infos);
	uint32_t i;

	for (i = 0; result == VK_SUCCESS && i < count; i++)
		buffer_bound(infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
	return result;
}

static VkResult VKAPI_CALL Hooked_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
		VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
	VkResult result = next_MapMemory(device)(device, memory, offset, size, flags, data);

	if (result == VK_SUCCESS && data && *data) {
		if (vk_dump_active)
			VkDumpMemoryMapped(memory, offset, size, *data);
		if (vk_mods_active)
			VkModsMemoryMapped(memory, offset, size, *data);
	}
	return result;
}

static void memory_unmapped(VkDeviceMemory memory)
{
	if (vk_dump_active)
		VkDumpMemoryUnmapped(memory);
	if (vk_mods_active)
		VkModsMemoryUnmapped(memory);
}

static void VKAPI_CALL Hooked_vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
	memory_unmapped(memory);
	next_UnmapMemory(device)(device, memory);
}

static void VKAPI_CALL Hooked_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
	if (memory)
		memory_unmapped(memory);
	next_FreeMemory(device)(device, memory, allocator);
}

static VkResult VKAPI_CALL Hooked_vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{
	if (vk_dump_active)
		VkDumpBeginCommands(cmd);
	if (vk_mods_active)
		VkModsBeginCommands(cmd);
	VkResult result = next_BeginCommandBuffer(cmd)(cmd, info);
	if (vk_mods_active && result == VK_SUCCESS)
		VkModsCommandsBegun(cmd);
	return result;
}

static void VKAPI_CALL Hooked_vkCmdExecuteCommands(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer *cmds)
{
	if (vk_mods_active)
		VkModsExecuteCommands(cmd, count, cmds);
	next_CmdExecuteCommands(cmd)(cmd, count, cmds);
}

static VkResult VKAPI_CALL Hooked_vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkImage *image)
{
	VkResult result = next_CreateImage(device)(device, info, allocator, image);

	if (result == VK_SUCCESS && image && *image && vk_mods_active)
		VkModsImageCreated(*image, info);
	return result;
}

static VkResult VKAPI_CALL Hooked_vkCreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkImageView *view)
{
	VkResult result = next_CreateImageView(device)(device, info, allocator, view);

	if (result == VK_SUCCESS && view && *view && vk_mods_active)
		VkModsImageView(*view, info);
	return result;
}

static void VKAPI_CALL Hooked_vkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator)
{
	if (view && vk_mods_active)
		VkModsForgetImageView(view);
	next_DestroyImageView(device)(device, view, allocator);
}

static void VKAPI_CALL Hooked_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *allocator)
{
	if (image && vk_mods_active)
		VkModsForgetImage(image);
	next_DestroyImage(device)(device, image, allocator);
}

static void VKAPI_CALL Hooked_vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer src, VkImage dst, VkImageLayout layout,
		uint32_t count, const VkBufferImageCopy *regions)
{
	if (vk_mods_active)
		VkModsCopyBufferToImage(src, dst, count, regions);
	next_CmdCopyBufferToImage(cmd)(cmd, src, dst, layout, count, regions);
}

static void VKAPI_CALL Hooked_vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions)
{
	if (vk_dump_active)
		VkDumpCopyBuffer(src, dst, count, regions);
	next_CmdCopyBuffer(cmd)(cmd, src, dst, count, regions);
}

static void VKAPI_CALL Hooked_vkCmdCopyBuffer2(VkCommandBuffer cmd, const VkCopyBufferInfo2 *info)
{
	uint32_t i;

	if (vk_dump_active) {
		std::vector<VkBufferCopy> regions(info->regionCount);

		for (i = 0; i < info->regionCount; i++)
			regions[i] = { info->pRegions[i].srcOffset, info->pRegions[i].dstOffset, info->pRegions[i].size };
		VkDumpCopyBuffer(info->srcBuffer, info->dstBuffer, info->regionCount, regions.data());
	}
	next_CmdCopyBuffer2(cmd)(cmd, info);
}

static void VKAPI_CALL Hooked_vkCmdUpdateBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize offset, VkDeviceSize bytes, const void *data)
{
	if (vk_dump_active)
		VkDumpUpdateBuffer(dst, bytes);
	next_CmdUpdateBuffer(cmd)(cmd, dst, offset, bytes, data);
}

static VkResult VKAPI_CALL Hooked_vkFlushMappedMemoryRanges(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges)
{
	if (vk_dump_active)
		VkDumpFlush(count, ranges);
	return next_FlushMappedMemoryRanges(device)(device, count, ranges);
}

static void VKAPI_CALL Hooked_vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
	if (vk_dump_active)
		VkDumpBindPipeline(cmd, bind_point, pipeline);
	if (vk_mods_active)
		VkModsBindPipeline(cmd, bind_point, pipeline);
	next_CmdBindPipeline(cmd)(cmd, bind_point, pipeline);
}

static void VKAPI_CALL Hooked_vkCmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count,
		const VkBuffer *buffers, const VkDeviceSize *offsets)
{
	if (vk_dump_active)
		VkDumpBindVertexBuffers(cmd, first, count, buffers, offsets);
	if (vk_mods_active)
		VkModsBindVertexBuffers(cmd, first, count, buffers, offsets);
	next_CmdBindVertexBuffers(cmd)(cmd, first, count, buffers, offsets);
}

static void VKAPI_CALL Hooked_vkCmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
	if (vk_dump_active)
		VkDumpBindIndexBuffer(cmd, buffer, offset, type);
	if (vk_mods_active)
		VkModsBindIndexBuffer(cmd, buffer, offset, type);
	next_CmdBindIndexBuffer(cmd)(cmd, buffer, offset, type);
}

static VkResult VKAPI_CALL Hooked_vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout)
{
	VkResult result = next_CreateDescriptorSetLayout(device)(device, info, allocator, layout);

	if (result == VK_SUCCESS && layout && *layout && vk_mods_active)
		VkModsSetLayout(*layout, info);
	return result;
}

// Kept while mods draw: a set the game still draws with may have been made of
// it, and a set of ours like it is then written, which Vulkan only allows
// while the layout lives. Its handle is not given out again either.
static void VKAPI_CALL Hooked_vkDestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout layout, const VkAllocationCallbacks *allocator)
{
	if (!vk_mods_active) {
		next_DestroyDescriptorSetLayout(device)(device, layout, allocator);
		return;
	}
	if (layout)
		VkModsSetLayoutKept();
}

static VkResult VKAPI_CALL Hooked_vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *info, VkDescriptorSet *sets)
{
	VkResult result = next_AllocateDescriptorSets(device)(device, info, sets);

	if (result == VK_SUCCESS && vk_mods_active)
		VkModsSetsAllocated(info, sets);
	return result;
}

static void VKAPI_CALL Hooked_vkUpdateDescriptorSets(VkDevice device, uint32_t write_count, const VkWriteDescriptorSet *writes,
		uint32_t copy_count, const VkCopyDescriptorSet *copies)
{
	if (vk_mods_active)
		VkModsSetsUpdated(write_count, writes, copy_count, copies);
	next_UpdateDescriptorSets(device)(device, write_count, writes, copy_count, copies);
}

static VkResult VKAPI_CALL Hooked_vkCreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkDescriptorUpdateTemplate *update_template)
{
	VkResult result = next_CreateDescriptorUpdateTemplate(device)(device, info, allocator, update_template);

	if (result == VK_SUCCESS && update_template && *update_template && vk_mods_active)
		VkModsUpdateTemplate(*update_template, info);
	return result;
}

static void VKAPI_CALL Hooked_vkUpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set,
		VkDescriptorUpdateTemplate update_template, const void *data)
{
	if (vk_mods_active)
		VkModsSetUpdatedWithTemplate(set, update_template, data);
	next_UpdateDescriptorSetWithTemplate(device)(device, set, update_template, data);
}

static void VKAPI_CALL Hooked_vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout,
		uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamic_count, const uint32_t *dynamic_offsets)
{
	if (vk_mods_active)
		VkModsBindDescriptorSets(cmd, bind_point, layout, first, count, sets, dynamic_count, dynamic_offsets);
	next_CmdBindDescriptorSets(cmd)(cmd, bind_point, layout, first, count, sets, dynamic_count, dynamic_offsets);
}

static void VKAPI_CALL Hooked_vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
	PFN_vkCmdDraw next = next_CmdDraw(cmd);

	if (vk_dump_active)
		VkDumpDraw(cmd, "draw", vertices, instances, first_vertex, 0);
	if (vk_mods_active && VkModsDraw(cmd, false, vertices, instances, first_vertex, 0, first_instance, next, NULL))
		return;
	next(cmd, vertices, instances, first_vertex, first_instance);
}

static void VKAPI_CALL Hooked_vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances, uint32_t first_index,
		int32_t vertex_offset, uint32_t first_instance)
{
	PFN_vkCmdDrawIndexed next = next_CmdDrawIndexed(cmd);

	if (vk_dump_active)
		VkDumpDraw(cmd, "indexed", indices, instances, first_index, vertex_offset);
	if (vk_mods_active && VkModsDraw(cmd, true, indices, instances, first_index, vertex_offset, first_instance, NULL, next))
		return;
	next(cmd, indices, instances, first_index, vertex_offset, first_instance);
}

static void VKAPI_CALL Hooked_vkCmdDrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t draws, uint32_t stride)
{
	if (vk_dump_active)
		VkDumpDraw(cmd, "indirect", 0, draws, 0, 0);
	next_CmdDrawIndirect(cmd)(cmd, buffer, offset, draws, stride);
}

static void VKAPI_CALL Hooked_vkCmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t draws, uint32_t stride)
{
	if (vk_dump_active)
		VkDumpDraw(cmd, "indexed_indirect", 0, draws, 0, 0);
	next_CmdDrawIndexedIndirect(cmd)(cmd, buffer, offset, draws, stride);
}

static void VKAPI_CALL Hooked_vkCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
{
	if (vk_dump_active)
		VkDumpDraw(cmd, "dispatch", x, y, z, 0);
	next_CmdDispatch(cmd)(cmd, x, y, z);
}

// Whether a submission can be given a fence of the D3D12 device to wait for:
// one batch, the command buffers of which are then all that waits, and no
// values for its semaphores yet, which would have to be put together with ours
static bool can_wait_for_fence(uint32_t count, const VkSubmitInfo *submits)
{
	const VkBaseInStructure *next;

	if (count != 1)
		return false;
	for (next = (const VkBaseInStructure*)submits->pNext; next; next = next->pNext) {
		if (next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO
		    || next->sType == VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR)
			return false;
	}
	return true;
}

static void dump_submit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence, unsigned shown, bool counted)
{
	VkFrameEvent event = { "submit", queue, shown, fence != VK_NULL_HANDLE, false, counted, count };
	const VkBaseInStructure *next;
	uint32_t i;

	for (i = 0; i < count; i++) {
		if (submits[i].waitSemaphoreCount && !event.first_wait)
			event.first_wait = submits[i].pWaitSemaphores[0];
		if (submits[i].signalSemaphoreCount && !event.first_signal)
			event.first_signal = submits[i].pSignalSemaphores[0];
		event.cbs += submits[i].commandBufferCount;
		event.waits += submits[i].waitSemaphoreCount;
		event.signals += submits[i].signalSemaphoreCount;
		for (next = (const VkBaseInStructure*)submits[i].pNext; next; next = next->pNext)
			event.timeline |= next->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	}
	event.frame_no = G->frame_no;
	VkDumpFrameEvent(&event);
}

static inline VkResult check_lost(VkQueue queue, VkResult result)
{
	if (result == VK_ERROR_DEVICE_LOST && vk_mods_active)
		VkModsDeviceLost(queue);
	return result;
}

static VkResult VKAPI_CALL Hooked_vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence)
{
	static thread_local std::vector<VkSemaphore> semaphores;
	static thread_local std::vector<VkPipelineStageFlags> stages;
	static thread_local std::vector<uint64_t> values, signals;
	VkD3D12FenceSubmitInfoKHR fences = { VK_STRUCTURE_TYPE_D3D12_FENCE_SUBMIT_INFO_KHR };
	VkSemaphore semaphore = VK_NULL_HANDLE;
	VkSubmitInfo waiting;
	UINT64 value = 0;
	unsigned shown = 0;
	bool counted = false;
	uint32_t i, j;

	if (vk_mods_active) {
		for (i = 0; i < count; i++) {
			for (j = 0; j < submits[i].commandBufferCount; j++)
				shown += VkModsCommandsShown(submits[i].pCommandBuffers[j]);
		}
		if (shown)
			counted = VkModsFrameSubmitted(shown);
	}
	if (vk_dump_active)
		dump_submit(queue, count, submits, fence, shown, counted);
	if (vk_mods_active) {
		semaphore = VkModsBeforeSubmit(queue, can_wait_for_fence(count, submits) ? &value : NULL, !!shown);
	}
	if (!semaphore)
		return check_lost(queue, next_QueueSubmit(queue)(queue, count, submits, fence));

	waiting = *submits;
	semaphores.assign(submits->pWaitSemaphores, submits->pWaitSemaphores + submits->waitSemaphoreCount);
	stages.assign(submits->pWaitDstStageMask, submits->pWaitDstStageMask + submits->waitSemaphoreCount);
	values.assign(submits->waitSemaphoreCount, 0);
	signals.assign(submits->signalSemaphoreCount, 0);
	semaphores.push_back(semaphore);
	// Before anything of the command buffers
	stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	values.push_back(value);

	fences.pNext = submits->pNext;
	fences.waitSemaphoreValuesCount = (uint32_t)values.size();
	fences.pWaitSemaphoreValues = values.data();
	fences.signalSemaphoreValuesCount = (uint32_t)signals.size();
	fences.pSignalSemaphoreValues = signals.data();
	waiting.pNext = &fences;
	waiting.waitSemaphoreCount = (uint32_t)semaphores.size();
	waiting.pWaitSemaphores = semaphores.data();
	waiting.pWaitDstStageMask = stages.data();
	return check_lost(queue, next_QueueSubmit(queue)(queue, 1, &waiting, fence));
}

static VkResult VKAPI_CALL Hooked_vkQueueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence)
{
	VkFrameEvent event = { "submit2", queue, 0, fence != VK_NULL_HANDLE, true, false, count };
	uint32_t i, j;

	if (vk_mods_active) {
		for (i = 0; i < count; i++) {
			for (j = 0; j < submits[i].commandBufferInfoCount; j++)
				event.shown += VkModsCommandsShown(submits[i].pCommandBufferInfos[j].commandBuffer);
		}
		if (event.shown)
			event.counted = VkModsFrameSubmitted(event.shown);
	}
	if (vk_dump_active) {
		for (i = 0; i < count; i++) {
			if (submits[i].waitSemaphoreInfoCount && !event.first_wait)
				event.first_wait = submits[i].pWaitSemaphoreInfos[0].semaphore;
			if (submits[i].signalSemaphoreInfoCount && !event.first_signal)
				event.first_signal = submits[i].pSignalSemaphoreInfos[0].semaphore;
			event.cbs += submits[i].commandBufferInfoCount;
			event.waits += submits[i].waitSemaphoreInfoCount;
			event.signals += submits[i].signalSemaphoreInfoCount;
		}
		event.frame_no = G->frame_no;
		VkDumpFrameEvent(&event);
	}
	if (vk_mods_active)
		VkModsBeforeSubmit(queue, NULL, !!event.shown);
	return check_lost(queue, next_QueueSubmit2(queue)(queue, count, submits, fence));
}

static VkResult VKAPI_CALL Hooked_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
	InterlockedIncrement((volatile LONG*)&vk_frame);
	if (vk_dump_active) {
		VkFrameEvent event = { "present", queue, 0, false, false, false, info->swapchainCount, 0, info->waitSemaphoreCount };
		const VkBaseInStructure *next;

		if (info->waitSemaphoreCount)
			event.first_wait = info->pWaitSemaphores[0];
		for (next = (const VkBaseInStructure*)info->pNext; next; next = next->pNext) {
			if (next->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR && ((const VkPresentIdKHR*)next)->pPresentIds)
				event.present_id = ((const VkPresentIdKHR*)next)->pPresentIds[0];
		}
		event.frame_no = G->frame_no;
		VkDumpFrameEvent(&event);
		VkDumpPresent();
	}
	if (vk_mods_active)
		VkModsPresent(queue);
	return check_lost(queue, next_QueuePresentKHR(queue)(queue, info));
}

// -----------------------------------------------------------------------------

static void hook_device_exports()
{
	static bool hooked;
	HINSTANCE vulkan;

	if (hooked)
		return;
	hooked = true;

	vulkan = NktHookLibHelpers::GetModuleBaseAddress(L"vulkan-1.dll");
	if (!vulkan)
		return;

#define X(name) \
	if (InstallHookLate(vulkan, "vk" #name, (void**)&exports.name, Hooked_vk##name)) \
		VkDumpNote("*** Failed to hook the export vk" #name "\n");
	VK_DEVICE_FUNCS(X)
#undef X
}

static void register_device(VkDevice device, VkPhysicalDevice physical_device, bool external_memory, bool external_semaphore)
{
	HINSTANCE vulkan = NktHookLibHelpers::GetModuleBaseAddress(L"vulkan-1.dll");
	PFN_vkGetDeviceProcAddr get = fnOrigGetDeviceProcAddr;
	VkDispatch *slot = NULL;
	VkFuncs next;
	unsigned i;

	if (!get)
		get = (PFN_vkGetDeviceProcAddr)NktHookLibHelpers::GetProcedureAddress(vulkan, "vkGetDeviceProcAddr");
	if (!get)
		return;

	// For a few functions the loader has work of its own to do and answers
	// with its export, which is hooked: going there would lead back here.
#define X(name) \
	next.name = (PFN_vk##name)get(device, "vk" #name); \
	if (exports.name && (void*)next.name == NktHookLibHelpers::GetProcedureAddress(vulkan, "vk" #name)) \
		next.name = exports.name;
	VK_DEVICE_FUNCS(X)
#undef X

	AcquireSRWLockExclusive(&dispatch_lock);

	for (i = 0; i < ARRAYSIZE(dispatch_tables) && !slot; i++) {
		if (!dispatch_tables[i].key)
			slot = &dispatch_tables[i];
	}
	if (slot) {
		slot->device = device;
		slot->next = next;
		MemoryBarrier();
		// Whoever looks for it finds it complete
		slot->key = *(void**)device;
	}

	ReleaseSRWLockExclusive(&dispatch_lock);

	if (!slot)
		VkDumpNote("*** Too many Vulkan devices, calls of %p are only seen where they go through the exports\n", device);

	if (slot && vk_mods_active) {
		VkModsApi api = {};

		api.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
			NktHookLibHelpers::GetProcedureAddress(vulkan, "vkGetPhysicalDeviceMemoryProperties");
		api.GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)get(device, "vkGetBufferMemoryRequirements");
		api.AllocateMemory = (PFN_vkAllocateMemory)get(device, "vkAllocateMemory");
		api.CreateBuffer = next.CreateBuffer;
		api.DestroyBuffer = next.DestroyBuffer;
		api.BindBufferMemory = next.BindBufferMemory;
		api.MapMemory = next.MapMemory;
		api.FreeMemory = next.FreeMemory;
		api.CreateGraphicsPipelines = next.CreateGraphicsPipelines;
		api.DestroyPipeline = next.DestroyPipeline;
		api.CmdBindPipeline = next.CmdBindPipeline;
		api.CmdBindVertexBuffers = next.CmdBindVertexBuffers;
		api.CmdBindIndexBuffer = next.CmdBindIndexBuffer;
		api.CreateDescriptorPool = (PFN_vkCreateDescriptorPool)get(device, "vkCreateDescriptorPool");
		api.DestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)get(device, "vkDestroyDescriptorPool");
		api.AllocateDescriptorSets = next.AllocateDescriptorSets;
		api.UpdateDescriptorSets = next.UpdateDescriptorSets;
		api.CmdBindDescriptorSets = next.CmdBindDescriptorSets;
		api.CreateImage = (PFN_vkCreateImage)get(device, "vkCreateImage");
		api.DestroyImage = (PFN_vkDestroyImage)get(device, "vkDestroyImage");
		api.GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)get(device, "vkGetImageMemoryRequirements");
		api.BindImageMemory = (PFN_vkBindImageMemory)get(device, "vkBindImageMemory");
		api.CreateImageView = (PFN_vkCreateImageView)get(device, "vkCreateImageView");
		api.DestroyImageView = (PFN_vkDestroyImageView)get(device, "vkDestroyImageView");
		api.CreateCommandPool = (PFN_vkCreateCommandPool)get(device, "vkCreateCommandPool");
		api.DestroyCommandPool = (PFN_vkDestroyCommandPool)get(device, "vkDestroyCommandPool");
		api.AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)get(device, "vkAllocateCommandBuffers");
		api.FreeCommandBuffers = (PFN_vkFreeCommandBuffers)get(device, "vkFreeCommandBuffers");
		api.BeginCommandBuffer = next.BeginCommandBuffer;
		api.EndCommandBuffer = (PFN_vkEndCommandBuffer)get(device, "vkEndCommandBuffer");
		api.CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)get(device, "vkCmdPipelineBarrier");
		api.CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)get(device, "vkCmdCopyBufferToImage");
		api.QueueSubmit = next.QueueSubmit;
		api.CreateSemaphore = (PFN_vkCreateSemaphore)get(device, "vkCreateSemaphore");
		api.DestroySemaphore = (PFN_vkDestroySemaphore)get(device, "vkDestroySemaphore");
		api.CmdDraw = next.CmdDraw;
		api.CmdDrawIndexed = next.CmdDrawIndexed;
		VkModsDevice(device, physical_device, &api, !external_memory ? NULL
			: (PFN_vkGetMemoryWin32HandlePropertiesKHR)get(device, "vkGetMemoryWin32HandlePropertiesKHR"),
			!external_semaphore ? NULL
			: (PFN_vkImportSemaphoreWin32HandleKHR)get(device, "vkImportSemaphoreWin32HandleKHR"));
	}
}

static VkBool32 VKAPI_CALL address_bound(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
		const VkDebugUtilsMessengerCallbackDataEXT *data, void *user)
{
	const VkBaseInStructure *next;

	if (!(types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !data)
		return VK_FALSE;
	for (next = (const VkBaseInStructure*)data->pNext; next; next = next->pNext) {
		if (next->sType != VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT)
			continue;
		const VkDeviceAddressBindingCallbackDataEXT *binding = (const VkDeviceAddressBindingCallbackDataEXT*)next;
		VkModsAddressBinding(binding->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT, binding->baseAddress,
			binding->size, data->objectCount ? data->pObjects[0].objectType : VK_OBJECT_TYPE_UNKNOWN,
			data->objectCount ? data->pObjects[0].objectHandle : 0);
		break;
	}
	return VK_FALSE;
}

// With vulkan and vk_dump_hashes the instance gets VK_EXT_debug_utils and a
// messenger of ours, which is how the device tells what its GPU addresses are
// bound to (VkModsAddressBinding). Failing that it is made as the game asks.
static VkResult VKAPI_CALL Hooked_vkCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
		VkInstance *instance)
{
	HINSTANCE vulkan = NktHookLibHelpers::GetModuleBaseAddress(L"vulkan-1.dll");
	PFN_vkEnumerateInstanceExtensionProperties enumerate = (PFN_vkEnumerateInstanceExtensionProperties)
		NktHookLibHelpers::GetProcedureAddress(vulkan, "vkEnumerateInstanceExtensionProperties");
	PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)NktHookLibHelpers::GetProcedureAddress(vulkan, "vkGetInstanceProcAddr");
	VkDebugUtilsMessengerCreateInfoEXT messenger_info = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
	PFN_vkCreateDebugUtilsMessengerEXT create_messenger;
	VkDebugUtilsMessengerEXT messenger;
	std::vector<VkExtensionProperties> available;
	std::vector<const char*> names;
	VkInstanceCreateInfo extended;
	bool asked = false, has = false;
	uint32_t count = 0;
	VkResult result;

	InitCommon();
	if (!info || !G->bIntendedTargetExe || !G->vulkan || !G->vk_dump_hashes || !enumerate || !get)
		return fnOrigCreateInstance(info, allocator, instance);

	names.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
	for (const char *name : names)
		asked |= !strcmp(name, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (!asked && enumerate(NULL, &count, NULL) == VK_SUCCESS) {
		available.resize(count);
		if (enumerate(NULL, &count, available.data()) < VK_SUCCESS)
			count = 0;
		available.resize(count);
		for (const VkExtensionProperties &extension : available)
			has |= !strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		if (!has)
			return fnOrigCreateInstance(info, allocator, instance);
		names.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	}
	extended = *info;
	extended.enabledExtensionCount = (uint32_t)names.size();
	extended.ppEnabledExtensionNames = names.data();
	result = fnOrigCreateInstance(&extended, allocator, instance);
	if (result != VK_SUCCESS)
		return fnOrigCreateInstance(info, allocator, instance);

	create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)get(*instance, "vkCreateDebugUtilsMessengerEXT");
	messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
	messenger_info.pfnUserCallback = address_bound;
	// Kept for as long as the instance is
	if (create_messenger && create_messenger(*instance, &messenger_info, NULL, &messenger) == VK_SUCCESS)
		binding_messenger = true;
	return result;
}

// What makes a device with the game's create info: with what lets
// VkModsDeviceLost say where the GPU was, as far as the driver has that, if
// diagnose is set, and failing that as given
static VkResult create_diagnosed(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkDevice *device, bool diagnose, bool *fault, bool *checkpoints, bool *bindings)
{
	HINSTANCE vulkan = NktHookLibHelpers::GetModuleBaseAddress(L"vulkan-1.dll");
	PFN_vkEnumerateDeviceExtensionProperties enumerate = (PFN_vkEnumerateDeviceExtensionProperties)
		NktHookLibHelpers::GetProcedureAddress(vulkan, "vkEnumerateDeviceExtensionProperties");
	PFN_vkGetPhysicalDeviceFeatures2 get_features = (PFN_vkGetPhysicalDeviceFeatures2)
		NktHookLibHelpers::GetProcedureAddress(vulkan, "vkGetPhysicalDeviceFeatures2");
	VkPhysicalDeviceFaultFeaturesEXT fault_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
	VkPhysicalDeviceAddressBindingReportFeaturesEXT binding_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT };
	VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	std::vector<VkExtensionProperties> available;
	std::vector<const char*> names;
	VkDeviceCreateInfo diagnosing;
	const VkBaseInStructure *next;
	bool has_fault = false, has_checkpoints = false, has_bindings = false;
	uint32_t count = 0;
	VkResult result;

	*fault = *checkpoints = *bindings = false;
	if (!diagnose || !enumerate || !get_features || enumerate(physical_device, NULL, &count, NULL) != VK_SUCCESS)
		return fnOrigCreateDevice(physical_device, info, allocator, device);

	available.resize(count);
	if (enumerate(physical_device, NULL, &count, available.data()) < VK_SUCCESS)
		count = 0;
	available.resize(count);
	for (const VkExtensionProperties &extension : available) {
		has_fault |= !strcmp(extension.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
		has_checkpoints |= !strcmp(extension.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
		has_bindings |= !strcmp(extension.extensionName, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
	}
	// Reported to a messenger of the instance, which there has to be
	has_bindings &= binding_messenger;
	// What the game turns on itself is left as it has it
	names.assign(info->ppEnabledExtensionNames, info->ppEnabledExtensionNames + info->enabledExtensionCount);
	for (const char *name : names) {
		if (!strcmp(name, VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
			has_fault = false;
		if (!strcmp(name, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
			has_checkpoints = false;
		if (!strcmp(name, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME))
			has_bindings = false;
	}
	for (next = (const VkBaseInStructure*)info->pNext; next; next = next->pNext) {
		has_fault &= next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
		has_bindings &= next->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT;
	}
	if (has_fault || has_bindings) {
		features.pNext = &fault_features;
		fault_features.pNext = &binding_features;
		get_features(physical_device, &features);
		has_fault &= !!fault_features.deviceFault;
		has_bindings &= !!binding_features.reportAddressBinding;
	}
	if (!has_fault && !has_checkpoints && !has_bindings)
		return fnOrigCreateDevice(physical_device, info, allocator, device);

	diagnosing = *info;
	if (has_fault) {
		names.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
		fault_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
		fault_features.deviceFault = VK_TRUE;
		fault_features.pNext = (void*)diagnosing.pNext;
		diagnosing.pNext = &fault_features;
	}
	if (has_bindings) {
		names.push_back(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
		binding_features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT };
		binding_features.reportAddressBinding = VK_TRUE;
		binding_features.pNext = (void*)diagnosing.pNext;
		diagnosing.pNext = &binding_features;
	}
	if (has_checkpoints)
		names.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
	diagnosing.enabledExtensionCount = (uint32_t)names.size();
	diagnosing.ppEnabledExtensionNames = names.data();

	result = fnOrigCreateDevice(physical_device, &diagnosing, allocator, device);
	if (result == VK_SUCCESS) {
		*fault = has_fault;
		*checkpoints = has_checkpoints;
		*bindings = has_bindings;
		return result;
	}
	VkLogInfo("vkCreateDevice failed with %d once diagnostics were added, made without\n", result);
	return fnOrigCreateDevice(physical_device, info, allocator, device);
}

// The device as the game asks for it, and with vulkan and vk_dump_hashes also
// with what lets VkModsDeviceLost say where the GPU was
static VkResult create_device(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkDevice *device, bool *fault, bool *checkpoints, bool *bindings)
{
	return create_diagnosed(physical_device, info, allocator, device,
		info && G->bIntendedTargetExe && G->vulkan && G->vk_dump_hashes, fault, checkpoints, bindings);
}

static VkResult VKAPI_CALL Hooked_vkCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkDevice *device)
{
	bool external_memory = false, external_semaphore = false, budget = false, fault, checkpoints, bindings;
	VkResult result;
	uint32_t i;

	InitCommon();
	result = create_device(physical_device, info, allocator, device, &fault, &checkpoints, &bindings);
	if (result != VK_SUCCESS || !device || !*device)
		return result;

	if (!G->bIntendedTargetExe || (!G->vulkan && !G->vk_dump_hashes)) {
		VkLogInfo("vkCreateDevice = %p, left alone without vulkan or vk_dump_hashes\n", *device);
		return result;
	}

	vk_dump_active = G->vk_dump_hashes;
	vk_mods_active = G->vulkan;
	hook_device_exports();
	for (i = 0; info && i < info->enabledExtensionCount; i++) {
		external_memory |= !strcmp(info->ppEnabledExtensionNames[i], VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
		external_semaphore |= !strcmp(info->ppEnabledExtensionNames[i], VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
		budget |= !strcmp(info->ppEnabledExtensionNames[i], VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	}
	register_device(*device, physical_device, external_memory, external_semaphore);
	if (budget && vk_mods_active)
		VkModsMemoryBudget(*device);

	VkDumpNote("vkCreateDevice = %p, %u extensions\n", *device, info ? info->enabledExtensionCount : 0);
	for (i = 0; info && i < info->enabledExtensionCount; i++)
		VkDumpNote("  %s\n", info->ppEnabledExtensionNames[i]);
	if (fault || checkpoints || bindings) {
		VkDumpNote("  and for telling where the GPU was if the device is lost:%s%s%s\n",
			fault ? " " VK_EXT_DEVICE_FAULT_EXTENSION_NAME : "",
			checkpoints ? " " VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME : "",
			bindings ? " " VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME : "");
	}
	if (vk_mods_active) {
		PFN_vkGetDeviceProcAddr get = fnOrigGetDeviceProcAddr ? fnOrigGetDeviceProcAddr : (PFN_vkGetDeviceProcAddr)
			NktHookLibHelpers::GetProcedureAddress(NktHookLibHelpers::GetModuleBaseAddress(L"vulkan-1.dll"), "vkGetDeviceProcAddr");

		if (get) {
			VkModsDiagnostics(*device,
				fault ? (PFN_vkGetDeviceFaultInfoEXT)get(*device, "vkGetDeviceFaultInfoEXT") : NULL,
				checkpoints ? (PFN_vkCmdSetCheckpointNV)get(*device, "vkCmdSetCheckpointNV") : NULL,
				checkpoints ? (PFN_vkGetQueueCheckpointDataNV)get(*device, "vkGetQueueCheckpointDataNV") : NULL);
		}
	}

	return result;
}

static PFN_vkVoidFunction VKAPI_CALL Hooked_vkGetDeviceProcAddr(VkDevice device, const char *name)
{
	PFN_vkVoidFunction real = fnOrigGetDeviceProcAddr(device, name);
	const VkDispatch *dispatch;

	if (!real || !name || !device)
		return real;

	dispatch = find_dispatch(device);
	if (!dispatch)
		return real;

#define X(func) \
	if (!strcmp(name, "vk" #func)) \
		return dispatch->next.func ? (PFN_vkVoidFunction)Hooked_vk##func : real;
	VK_DEVICE_FUNCS(X)
#undef X

	// The name it had as an extension, for the same function
	if (!strcmp(name, "vkBindBufferMemory2KHR") && dispatch->next.BindBufferMemory2)
		return (PFN_vkVoidFunction)Hooked_vkBindBufferMemory2;
	if (!strcmp(name, "vkCmdCopyBuffer2KHR") && dispatch->next.CmdCopyBuffer2)
		return (PFN_vkVoidFunction)Hooked_vkCmdCopyBuffer2;
	if (!strcmp(name, "vkCreateDescriptorUpdateTemplateKHR") && dispatch->next.CreateDescriptorUpdateTemplate)
		return (PFN_vkVoidFunction)Hooked_vkCreateDescriptorUpdateTemplate;
	if (!strcmp(name, "vkUpdateDescriptorSetWithTemplateKHR") && dispatch->next.UpdateDescriptorSetWithTemplate)
		return (PFN_vkVoidFunction)Hooked_vkUpdateDescriptorSetWithTemplate;
	if (!strcmp(name, "vkQueueSubmit2KHR") && dispatch->next.QueueSubmit2)
		return (PFN_vkVoidFunction)Hooked_vkQueueSubmit2;

	return real;
}

void VkHookExports()
{
	static volatile LONG hooked = 0;
	HINSTANCE vulkan;

	if (hooked)
		return;

	vulkan = NktHookLibHelpers::GetModuleBaseAddress(L"vulkan-1.dll");
	if (!vulkan)
		return;

	if (InterlockedCompareExchange(&hooked, 1, 0) != 0)
		return;

	InstallHookLate(vulkan, "vkCreateInstance", (void**)&fnOrigCreateInstance, Hooked_vkCreateInstance);
	InstallHookLate(vulkan, "vkCreateDevice", (void**)&fnOrigCreateDevice, Hooked_vkCreateDevice);
	InstallHookLate(vulkan, "vkGetDeviceProcAddr", (void**)&fnOrigGetDeviceProcAddr, Hooked_vkGetDeviceProcAddr);
}
