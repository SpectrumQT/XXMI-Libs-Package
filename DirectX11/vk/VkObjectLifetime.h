#pragma once

#include <vector>

#include "VkDrawPath.h"
#include "VkDump.h"
#include "../dx12/Bridge.h"

template <typename Handle>
struct RetainedObject {
	using Destroy = void (VKAPI_PTR *)(VkDevice, Handle, const VkAllocationCallbacks*);
	VkDevice device;
	Handle handle;
	Destroy destroy;
};

static SRWLOCK retained_lock = SRWLOCK_INIT;
static std::vector<RetainedObject<VkShaderModule>> retained_shaders;
static std::vector<RetainedObject<VkPipelineLayout>> retained_pipeline_layouts;
static std::vector<RetainedObject<VkRenderPass>> retained_render_passes;
static std::vector<RetainedObject<VkDescriptorSetLayout>> retained_set_layouts;

template <typename Handle>
static bool retain_object(std::vector<RetainedObject<Handle>> *objects, VkDevice device, Handle handle,
		const VkAllocationCallbacks *allocator, typename RetainedObject<Handle>::Destroy destroy)
{
	// The caller may release its allocator state as soon as vkDestroy* returns.
	if (!handle || allocator)
		return false;
	AcquireSRWLockExclusive(&retained_lock);
	bool keep = vk_mods_active && !Dx12IsD3D11Game();
	if (keep)
		objects->push_back({ device, handle, destroy });
	ReleaseSRWLockExclusive(&retained_lock);
	return keep;
}

// With retained_lock held: device destruction and the D3D11 transition must
// not both release an object, or release it after its device has gone away.
template <typename Handle>
static void release_objects(std::vector<RetainedObject<Handle>> *objects, VkDevice device)
{
	for (size_t i = 0; i < objects->size();) {
		const auto &object = (*objects)[i];
		if (device && object.device != device) {
			i++;
			continue;
		}
		object.destroy(object.device, object.handle, NULL);
		(*objects)[i] = objects->back();
		objects->pop_back();
	}
}

static void release_retained(VkDevice device)
{
	for (const auto &shader : retained_shaders) {
		if (device && shader.device != device)
			continue;
		VkModsForgetShaderModule(shader.handle);
		if (vk_dump_active)
			VkDumpForgetShaderModule(shader.handle);
	}
	release_objects(&retained_shaders, device);
	release_objects(&retained_pipeline_layouts, device);
	release_objects(&retained_render_passes, device);
	release_objects(&retained_set_layouts, device);
}

void VkDisableModsForD3D11()
{
	if (!Dx12IsD3D11Game())
		return;
	AcquireSRWLockExclusive(&retained_lock);
	vk_mods_active = false;
	release_retained(VK_NULL_HANDLE);
	ReleaseSRWLockExclusive(&retained_lock);
}
