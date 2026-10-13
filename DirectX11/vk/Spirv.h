#pragma once

#include <string>

#include "VkCommon.h"

// What can be read out of a SPIR-V module without executing anything: which
// stage it is for and what it expects to be bound where. Vulkan has no
// registers, a shader names a descriptor set and a binding in it, so this is
// what a D3D11 register of a mod has to be mapped from.
struct SpirvInfo {
	bool valid;
	uint32_t version;
	uint32_t generator;
	// VkShaderStageFlagBits of the first entry point, 0 if there is none
	uint32_t stage;
	std::string entry;
	// How many of the variables below still have a name
	unsigned named, unnamed;
	// "location:name" for every input of the stage
	std::string inputs;
	// "s<set>b<binding>:<kind>[:<bytes>][:<name>]" for every resource, and
	// "pc:<bytes>" for push constants. Kinds: ubo, ssbo, tex, samp, texsamp,
	// img (storage image), tbuf and itbuf (texel buffers), as (acceleration
	// structure)
	std::string resources;
};

void SpirvReflect(const uint32_t *code, size_t words, SpirvInfo *info);
