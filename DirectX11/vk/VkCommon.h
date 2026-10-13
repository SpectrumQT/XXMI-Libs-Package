#pragma once

#include <stdint.h>

#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "log.h"

#define VkLogInfo(fmt, ...) LogInfo("[VK] " fmt, __VA_ARGS__)
#define VkLogDebug(fmt, ...) LogDebug("[VK] " fmt, __VA_ARGS__)

// Presents so far, for telling how long ago something happened
extern volatile UINT vk_frame;
