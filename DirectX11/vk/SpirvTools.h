#pragma once

#include <stdint.h>

#include <string>
#include <vector>

// SPIRV-Tools-shared.dll, from next to this DLL (see tools/build-spirv-tools.ps1):
// SPIR-V as text and back, and the validator. Loaded the first time it is
// asked for. All of these are safe to call from any thread.

bool SpirvToolsAvailable();

// friendly: types and constants by names like %float rather than numbers
bool SpirvDisassemble(const uint32_t *code, size_t words, bool friendly, std::string *text, std::string *error);

// version is that of a module header (0x00010300 for SPIR-V 1.3), which the
// result gets. preserve_ids keeps the number of an id written as one.
bool SpirvAssemble(const std::string &text, uint32_t version, bool preserve_ids, std::vector<uint32_t> *code, std::string *error);

enum SpirvLayoutRules {
	SPIRV_LAYOUT_STRICT,
	SPIRV_LAYOUT_RELAXED,
	SPIRV_LAYOUT_SCALAR,
	SPIRV_LAYOUT_SKIP,
	SPIRV_LAYOUT_COUNT,
};

// For Vulkan, with the version the module header says. A game's shaders may
// only pass with laxer rules for block layouts than the default ones.
bool SpirvValidate(const uint32_t *code, size_t words, SpirvLayoutRules rules, std::string *error);
