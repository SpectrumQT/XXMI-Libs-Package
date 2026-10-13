#pragma once

#include <string>
#include <vector>

#include "VkCommon.h"

// What can be read out of a SPIR-V module without executing anything: which
// stage it is for and what it expects to be bound where. Vulkan has no
// registers, a shader names a descriptor set and a binding in it, so this is
// what a D3D11 register of a mod has to be mapped from.
enum SpirvKind {
	SPIRV_UBO,
	SPIRV_SSBO,
	SPIRV_TEXTURE,
	SPIRV_SAMPLER,
	SPIRV_TEXTURE_SAMPLER,
	SPIRV_STORAGE_IMAGE,
	SPIRV_TEXEL_BUFFER,
	SPIRV_STORAGE_TEXEL_BUFFER,
	SPIRV_ACCELERATION_STRUCTURE,
};

struct SpirvBinding {
	uint32_t set, binding;
	SpirvKind kind;
	// Of the block, for a ubo
	uint32_t bytes;
	// Of the array an ssbo ends with, and whether the shader can only read
	// it
	uint32_t stride;
	bool read_only;
	// Of an image: the Dim of its type, and whether it is an array
	uint32_t dim;
	bool arrayed;
};

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
	// The same, in the order the shader declares them
	std::vector<SpirvBinding> bindings;
};

void SpirvReflect(const uint32_t *code, size_t words, SpirvInfo *info);
