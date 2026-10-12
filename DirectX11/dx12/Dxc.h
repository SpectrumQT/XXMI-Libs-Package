#pragma once

#include <d3d12shader.h>
#include <d3dcommon.h>

#include <string>
#include <vector>

// The DirectX Shader Compiler, for shaders of mods on a D3D12 game whose own
// shaders are DXIL: a pipeline state does not take shader model 5 and 6
// together. dxcompiler.dll and dxil.dll, which signs what it compiles, are
// loaded from next to d3d11.dll the first time they are needed.

bool Dx12DxcAvailable();

// Compiles HLSL to DXIL for the shader model 6.0 profile of the stage (the
// first letter of the target, as D3DCompile takes it) with the same macros and
// compile flags as D3DCompile would, as far as the compiler has them. What
// the compiler said goes to errors whether it succeeded or not.
bool Dx12DxcCompile(const char *source, size_t size, const wchar_t *path, const wchar_t *target,
		const D3D_SHADER_MACRO *macros, UINT compile_flags, ID3DBlob **bytecode, std::string *errors);

// What D3DReflect gives for DXBC
bool Dx12DxcReflect(const void *bytecode, size_t size, ID3D12ShaderReflection **reflection);

// Where the bytecode came from, for compiling it again another way
void Dx12DxcRemember(const void *bytecode, size_t size, const wchar_t *path, const wchar_t *target,
		const D3D_SHADER_MACRO *macros, UINT compile_flags);

struct Dx12ShaderInfo;

// A pixel shader of a mod was written for the vertex shader of the D3D11 game,
// and the D3D12 game's may write fewer components of a semantic or not have it
// at all, which D3D12 refuses to link. This compiles the mod's again with its
// inputs cut down to what upstream (the stage before it) writes, the missing
// components read as 0. Returns 0 where the shader fits as it is, 1 with
// *fitted set to one that does (kept for good, with the hash, size and
// bytecode of the new one and the rest of ps), -1 with *why set if it cannot
// be done.
int Dx12DxcFitPixelShader(const Dx12ShaderInfo *ps, const Dx12ShaderInfo *upstream, const Dx12ShaderInfo **fitted, const char **why);
