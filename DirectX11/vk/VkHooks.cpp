#include "VkHooks.h"

#include <vector>

#include "VkCommon.h"
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
	X(CreateShaderModule) \
	X(DestroyShaderModule) \
	X(CreateGraphicsPipelines) \
	X(CreateComputePipelines) \
	X(DestroyPipeline) \
	X(CreateBuffer) \
	X(DestroyBuffer) \
	X(BindBufferMemory) \
	X(BindBufferMemory2) \
	X(MapMemory) \
	X(UnmapMemory) \
	X(FreeMemory) \
	X(BeginCommandBuffer) \
	X(FlushMappedMemoryRanges) \
	X(CmdCopyBuffer) \
	X(CmdCopyBuffer2) \
	X(CmdUpdateBuffer) \
	X(CmdBindPipeline) \
	X(CmdBindVertexBuffers) \
	X(CmdBindIndexBuffer) \
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

static PFN_vkCreateDevice fnOrigCreateDevice;
static PFN_vkGetDeviceProcAddr fnOrigGetDeviceProcAddr;

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
		VkLogInfo("vkDestroyDevice(%p)\n", device);
	}

	next(device, allocator);
}

static VkResult VKAPI_CALL Hooked_vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkShaderModule *module)
{
	VkResult result = next_CreateShaderModule(device)(device, info, allocator, module);

	if (result == VK_SUCCESS && module && *module)
		VkDumpShaderModule(*module, info);
	return result;
}

static void VKAPI_CALL Hooked_vkDestroyShaderModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks *allocator)
{
	if (module)
		VkDumpForgetShaderModule(module);
	next_DestroyShaderModule(device)(device, module, allocator);
}

static VkResult VKAPI_CALL Hooked_vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
		const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
	VkResult result = next_CreateGraphicsPipelines(device)(device, cache, count, infos, allocator, pipelines);
	uint32_t i;

	// Also where only some were created
	for (i = 0; pipelines && i < count; i++) {
		if (result >= 0 && pipelines[i])
			VkDumpGraphicsPipeline(pipelines[i], &infos[i]);
	}
	return result;
}

static VkResult VKAPI_CALL Hooked_vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
		const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *allocator, VkPipeline *pipelines)
{
	VkResult result = next_CreateComputePipelines(device)(device, cache, count, infos, allocator, pipelines);
	uint32_t i;

	for (i = 0; pipelines && i < count; i++) {
		if (result >= 0 && pipelines[i])
			VkDumpComputePipeline(pipelines[i], &infos[i]);
	}
	return result;
}

static void VKAPI_CALL Hooked_vkDestroyPipeline(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *allocator)
{
	if (pipeline)
		VkDumpForgetPipeline(pipeline);
	next_DestroyPipeline(device)(device, pipeline, allocator);
}

static VkResult VKAPI_CALL Hooked_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkBuffer *buffer)
{
	VkResult result = next_CreateBuffer(device)(device, info, allocator, buffer);

	if (result == VK_SUCCESS && buffer && *buffer)
		VkDumpBufferCreated(*buffer, info);
	return result;
}

static void VKAPI_CALL Hooked_vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator)
{
	if (buffer)
		VkDumpForgetBuffer(buffer);
	next_DestroyBuffer(device)(device, buffer, allocator);
}

static VkResult VKAPI_CALL Hooked_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
	VkResult result = next_BindBufferMemory(device)(device, buffer, memory, offset);

	if (result == VK_SUCCESS)
		VkDumpBufferBound(buffer, memory, offset);
	return result;
}

static VkResult VKAPI_CALL Hooked_vkBindBufferMemory2(VkDevice device, uint32_t count, const VkBindBufferMemoryInfo *infos)
{
	VkResult result = next_BindBufferMemory2(device)(device, count, infos);
	uint32_t i;

	for (i = 0; result == VK_SUCCESS && i < count; i++)
		VkDumpBufferBound(infos[i].buffer, infos[i].memory, infos[i].memoryOffset);
	return result;
}

static VkResult VKAPI_CALL Hooked_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
		VkDeviceSize size, VkMemoryMapFlags flags, void **data)
{
	VkResult result = next_MapMemory(device)(device, memory, offset, size, flags, data);

	if (result == VK_SUCCESS && data && *data)
		VkDumpMemoryMapped(memory, offset, size, *data);
	return result;
}

static void VKAPI_CALL Hooked_vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
	VkDumpMemoryUnmapped(memory);
	next_UnmapMemory(device)(device, memory);
}

static void VKAPI_CALL Hooked_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *allocator)
{
	if (memory)
		VkDumpMemoryUnmapped(memory);
	next_FreeMemory(device)(device, memory, allocator);
}

static VkResult VKAPI_CALL Hooked_vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *info)
{
	VkDumpBeginCommands(cmd);
	return next_BeginCommandBuffer(cmd)(cmd, info);
}

static void VKAPI_CALL Hooked_vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t count, const VkBufferCopy *regions)
{
	VkDumpCopyBuffer(src, dst, count, regions);
	next_CmdCopyBuffer(cmd)(cmd, src, dst, count, regions);
}

static void VKAPI_CALL Hooked_vkCmdCopyBuffer2(VkCommandBuffer cmd, const VkCopyBufferInfo2 *info)
{
	std::vector<VkBufferCopy> regions(info->regionCount);
	uint32_t i;

	for (i = 0; i < info->regionCount; i++)
		regions[i] = { info->pRegions[i].srcOffset, info->pRegions[i].dstOffset, info->pRegions[i].size };
	VkDumpCopyBuffer(info->srcBuffer, info->dstBuffer, info->regionCount, regions.data());
	next_CmdCopyBuffer2(cmd)(cmd, info);
}

static void VKAPI_CALL Hooked_vkCmdUpdateBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize offset, VkDeviceSize bytes, const void *data)
{
	VkDumpUpdateBuffer(dst, bytes);
	next_CmdUpdateBuffer(cmd)(cmd, dst, offset, bytes, data);
}

static VkResult VKAPI_CALL Hooked_vkFlushMappedMemoryRanges(VkDevice device, uint32_t count, const VkMappedMemoryRange *ranges)
{
	VkDumpFlush(count, ranges);
	return next_FlushMappedMemoryRanges(device)(device, count, ranges);
}

static void VKAPI_CALL Hooked_vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind_point, VkPipeline pipeline)
{
	VkDumpBindPipeline(cmd, bind_point, pipeline);
	next_CmdBindPipeline(cmd)(cmd, bind_point, pipeline);
}

static void VKAPI_CALL Hooked_vkCmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count,
		const VkBuffer *buffers, const VkDeviceSize *offsets)
{
	VkDumpBindVertexBuffers(cmd, first, count, buffers, offsets);
	next_CmdBindVertexBuffers(cmd)(cmd, first, count, buffers, offsets);
}

static void VKAPI_CALL Hooked_vkCmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type)
{
	VkDumpBindIndexBuffer(cmd, buffer, offset, type);
	next_CmdBindIndexBuffer(cmd)(cmd, buffer, offset, type);
}

static void VKAPI_CALL Hooked_vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance)
{
	VkDumpDraw(cmd, "draw", vertices, instances, first_vertex, 0);
	next_CmdDraw(cmd)(cmd, vertices, instances, first_vertex, first_instance);
}

static void VKAPI_CALL Hooked_vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances, uint32_t first_index,
		int32_t vertex_offset, uint32_t first_instance)
{
	VkDumpDraw(cmd, "indexed", indices, instances, first_index, vertex_offset);
	next_CmdDrawIndexed(cmd)(cmd, indices, instances, first_index, vertex_offset, first_instance);
}

static void VKAPI_CALL Hooked_vkCmdDrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t draws, uint32_t stride)
{
	VkDumpDraw(cmd, "indirect", 0, draws, 0, 0);
	next_CmdDrawIndirect(cmd)(cmd, buffer, offset, draws, stride);
}

static void VKAPI_CALL Hooked_vkCmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t draws, uint32_t stride)
{
	VkDumpDraw(cmd, "indexed_indirect", 0, draws, 0, 0);
	next_CmdDrawIndexedIndirect(cmd)(cmd, buffer, offset, draws, stride);
}

static void VKAPI_CALL Hooked_vkCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z)
{
	VkDumpDraw(cmd, "dispatch", x, y, z, 0);
	next_CmdDispatch(cmd)(cmd, x, y, z);
}

static VkResult VKAPI_CALL Hooked_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
	VkDumpPresent();
	return next_QueuePresentKHR(queue)(queue, info);
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

static void register_device(VkDevice device)
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
}

static bool vk_enabled()
{
	InitCommon();

	return G->bIntendedTargetExe && G->vk_dump_hashes;
}

static VkResult VKAPI_CALL Hooked_vkCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
		const VkAllocationCallbacks *allocator, VkDevice *device)
{
	VkResult result = fnOrigCreateDevice(physical_device, info, allocator, device);
	uint32_t i;

	if (result != VK_SUCCESS || !device || !*device)
		return result;

	if (!vk_enabled()) {
		VkLogInfo("vkCreateDevice = %p, left alone without vk_dump_hashes\n", *device);
		return result;
	}

	vk_dump_active = true;
	hook_device_exports();
	register_device(*device);

	VkDumpNote("vkCreateDevice = %p, %u extensions\n", *device, info ? info->enabledExtensionCount : 0);
	for (i = 0; info && i < info->enabledExtensionCount; i++)
		VkDumpNote("  %s\n", info->ppEnabledExtensionNames[i]);

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

	InstallHookLate(vulkan, "vkCreateDevice", (void**)&fnOrigCreateDevice, Hooked_vkCreateDevice);
	InstallHookLate(vulkan, "vkGetDeviceProcAddr", (void**)&fnOrigGetDeviceProcAddr, Hooked_vkGetDeviceProcAddr);
}
