#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <Windows.h>

#include "../../DirectX11/vk/VkObjectLifetime.h"

FILE *LogFile;
LogVerbosity gLogVerbosity = LogVerbosity::DISABLED;
std::atomic<bool> vk_mods_active;
bool vk_dump_active;
static std::atomic<bool> d3d11;
static std::atomic<unsigned> destroyed, forgotten;
static VkDevice first_device = (VkDevice)1;
static VkDevice second_device = (VkDevice)2;
struct AllocatorState {
	unsigned destroys;
};

bool Dx12IsD3D11Game() { return d3d11; }
void VkModsForgetShaderModule(VkShaderModule) { forgotten++; }
void VkDumpForgetShaderModule(VkShaderModule) {}

static void require(bool condition)
{
	if (!condition) {
		std::fputs("FAIL: Vulkan retained-object lifetime\n", stderr);
		std::exit(1);
	}
}

template <typename Handle>
static void VKAPI_CALL destroy(VkDevice device, Handle handle, const VkAllocationCallbacks *allocator)
{
	require(device == first_device || device == second_device);
	require(handle != VK_NULL_HANDLE);
	if (allocator) {
		auto state = static_cast<AllocatorState*>(allocator->pUserData);
		require(state != NULL);
		state->destroys++;
	}
	destroyed++;
}

template <typename Handle>
static void game_destroy(std::vector<RetainedObject<Handle>> *objects, VkDevice device,
		Handle handle, const VkAllocationCallbacks *allocator)
{
	if (!retain_object(objects, device, handle, allocator, destroy<Handle>))
		destroy(device, handle, allocator);
}

int main()
{
	vk_mods_active = true;
	auto allocator_state = new AllocatorState{};
	VkAllocationCallbacks allocator = {};
	allocator.pUserData = allocator_state;
	game_destroy(&retained_shaders, first_device, (VkShaderModule)101, &allocator);
	game_destroy(&retained_pipeline_layouts, first_device, (VkPipelineLayout)102, &allocator);
	game_destroy(&retained_render_passes, first_device, (VkRenderPass)103, &allocator);
	game_destroy(&retained_set_layouts, first_device, (VkDescriptorSetLayout)104, &allocator);
	require(allocator_state->destroys == 4 && destroyed == 4);
	delete allocator_state;
	allocator.pUserData = NULL;
	d3d11 = true;
	VkDisableModsForD3D11();
	require(destroyed == 4);
	destroyed = 0;
	d3d11 = false;
	vk_mods_active = true;
	game_destroy(&retained_shaders, first_device, (VkShaderModule)1, NULL);
	game_destroy(&retained_pipeline_layouts, first_device, (VkPipelineLayout)2, NULL);
	game_destroy(&retained_render_passes, first_device, (VkRenderPass)3, NULL);
	game_destroy(&retained_set_layouts, first_device, (VkDescriptorSetLayout)4, NULL);
	require(destroyed == 0);
	VkDisableModsForD3D11();
	require(vk_mods_active && destroyed == 0);
	d3d11 = true;
	VkDisableModsForD3D11();
	require(!vk_mods_active && destroyed == 4 && forgotten == 1);
	VkDisableModsForD3D11();
	require(destroyed == 4);
	game_destroy(&retained_shaders, first_device, (VkShaderModule)5, NULL);
	game_destroy(&retained_pipeline_layouts, first_device, (VkPipelineLayout)6, NULL);
	game_destroy(&retained_render_passes, first_device, (VkRenderPass)7, NULL);
	game_destroy(&retained_set_layouts, first_device, (VkDescriptorSetLayout)8, NULL);
	require(destroyed == 8);

	d3d11 = false;
	vk_mods_active = true;
	game_destroy(&retained_shaders, first_device, (VkShaderModule)9, NULL);
	game_destroy(&retained_shaders, second_device, (VkShaderModule)10, NULL);
	AcquireSRWLockExclusive(&retained_lock);
	release_retained(first_device);
	ReleaseSRWLockExclusive(&retained_lock);
	require(destroyed == 9);
	d3d11 = true;
	VkDisableModsForD3D11();
	require(destroyed == 10);

	d3d11 = false;
	vk_mods_active = true;
	std::thread worker([] {
		for (unsigned i = 0; i < 10000; i++)
			game_destroy(&retained_shaders, first_device, (VkShaderModule)(uintptr_t)(i + 11), NULL);
	});
	d3d11 = true;
	VkDisableModsForD3D11();
	worker.join();
	require(!vk_mods_active && destroyed == 10010);
	std::puts("PASS: D3D11 transition releases all four object kinds; later destroys pass through");
	std::puts("PASS: custom allocators destroy synchronously before their state is released");
	std::puts("PASS: device-scoped cleanup, repeat cleanup and concurrent destroys");
}
