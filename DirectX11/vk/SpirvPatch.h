#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "../ShaderRegex.h"

// The ShaderRegex groups with SpirvPattern subsections, applied to the text
// SPIRV-Tools makes of a SPIR-V shader. Ids are numbers there (%123), which
// differ from shader to shader, so before any pattern sees it the text gets
// these names the patterns can count on:
//
//   %zz_main                  the function of the entry point
//   %zz_in_loc<N>             the input at Location N, %zz_out_loc<N> output
//   %zz_in_<BuiltIn>          a built-in input (%zz_in_FragCoord), _out_ output
//   %zz_glsl                  the GLSL.std.450 instructions (added if missing)
//   %zz_void %zz_bool %zz_int %zz_uint %zz_float, %zz_v<2-4><scalar>
//                             always there
//   %zz_image2d_float         a sampled 2D float image, %zz_sampler, and
//   %zz_sampled_image2d_float always there when declared to be used
//   %zz_ptr_<Storage>_<type>  a pointer to one of those (%zz_ptr_Input_v2float),
//                             where the shader has one, and those to what is
//                             declared below
//
// and lines a pattern can find a place by, which are taken out again:
//
//   @zz_globals               before the first function: types, constants and
//                             variables go there
//   @zz_entry                 the end of a block that is now the first of
//                             %zz_main, which then goes on to what the shader
//                             had as its first: code put there runs before
//                             anything else, and may branch and, in a pixel
//                             shader, OpKill
//
// What InsertDeclarations asks for is declared in a descriptor set of its own:
//
//   %IniParams                the IniParams, a uniform block of an array of v4float
//                             (OpAccessChain %zz_ptr_Uniform_v4float %IniParams
//                             <int 0> <index>)
//   %t<N>                     a 2D float texture
//   %s<N>                     a sampler
//
// The ${name} of a temps name is %zz<group>_<name>, the group being the place
// of the group among those for SPIR-V. A group whose patterns do not all match
// leaves the shader as it was.

// The binding a declaration has in the set of the patched shader
uint32_t SpirvPatchBinding(const SpirvDeclaration &declaration);

struct SpirvPatched {
	std::vector<uint32_t> code;
	// The words to write the number of that set into
	std::vector<size_t> set_words;
	// What it was given, without duplicates
	std::vector<SpirvDeclaration> declarations;
	// The groups that were applied, by their sections
	std::vector<const ShaderRegexGroup*> groups;
	// The text the patterns were applied to and what they made of it, if
	// asked for
	std::string before, after;
};

enum class SpirvPatchOutcome {
	PATCHED,
	// No group is for it, or none matched
	NO_MATCH,
	// One did, and what came of it is no valid shader, or the shader was
	// not one to begin with
	FAILED,
};

// stage: "ps", "vs" and so on, which a group has to name a shader model of.
// entry: the name of the entry point of that stage the pipeline uses, NULL for
// the first entry point of the module.
// declarations: what the shader declares were it a D3D11 one, which the
// Pattern subsections of a group have to match; NULL takes it that they do.
// ini_params: how many there are. Call with the ShaderRegex sections kept from
// changing.
SpirvPatchOutcome SpirvPatchShader(const uint32_t *code, size_t words, const char *stage, const char *entry,
		const std::string *declarations, unsigned ini_params, bool keep_text,
		SpirvPatched *patched, std::string *why);

// The code of a patched shader with its set at that number
std::vector<uint32_t> SpirvPatchedCode(const SpirvPatched &patched, uint32_t set);
