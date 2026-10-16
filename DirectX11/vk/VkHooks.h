#pragma once

// A Vulkan game gets its functions from vulkan-1.dll, the loader: either its
// exports, which vkGetInstanceProcAddr hands out as well, or what
// vkGetDeviceProcAddr returns, which leads past those exports straight into
// the first layer or the driver. Both are covered here, the exports with code
// hooks and the latter by answering vkGetDeviceProcAddr with our functions.
//
// Registering as a layer of the loader would be the documented way in, but
// that is done in the registry for every Vulkan program on the machine, or
// with environment variables the loader ignores in an elevated process.
//
// Vulkan handles are not wrapped: the game keeps the loader's, and our
// functions find where to continue from the dispatch table pointer the loader
// keeps at the start of every device, queue and command buffer.

// Hooks the vulkan-1.dll exports we need if the DLL is in the process and
// they are not hooked yet, or no longer: it may have been unloaded and loaded
// again since. Called from DllMain and from the LoadLibraryExW hook, so it
// has to stay within what is permitted under the loader lock.
void VkHookExports();
// For DLL_PROCESS_DETACH: no more being told of vulkan-1.dll being unloaded
void VkUnwatchUnloads();
