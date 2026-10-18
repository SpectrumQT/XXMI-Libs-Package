#pragma once

#include <set>
#include <string>
#include <vector>

#include "../ShaderRegex.h"

struct ShaderRegexSource;

// SPIR-V versions of the D3D11 ShaderRegex patches mods carry, made for a
// Vulkan game by recipes that know those patches (see
// docs/ini-language-extensions/spirv-shader-regex). They are kept apart from
// shader_regex_groups so that the D3D11 and D3D12 paths, the hash of the
// shader cache and the ids of its groups never see them.
//
// Read with the ShaderRegexConfigRead of ShaderRegex.h held.
extern ShaderRegexGroups vk_converted_regex_groups;

// Makes them again from the loaded config and notes in vk_report.txt what was
// converted and what was not. Call on the thread that reloads the config.
void VkConvertShaderRegexGroups();

// What a conversion of the D3D11 groups of those sources makes, as ini lines
// with each file's group prefix before them, and why groups were not
// converted. The order of the groups is that of the sources.
struct SpirvConverted {
	std::wstring prefix;
	std::vector<std::wstring> from;
	std::string recipe;
	std::vector<std::string> lines;
};
void SpirvConvertSources(const std::vector<ShaderRegexSource> &sources, const std::set<std::wstring> &files_with_vulkan_sections,
		std::vector<SpirvConverted> *converted, std::vector<std::pair<std::wstring, std::string>> *not_converted);
