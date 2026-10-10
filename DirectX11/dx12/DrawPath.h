#pragma once

#include <d3d11_1.h>

#include "Dx12CommandList.h"

// What HackerContext does around every Draw and Dispatch of a D3D11 game, for
// the draws and dispatches a D3D12 game records: run the [ShaderOverride]
// sections of the shaders in use, with everything those can do from there.
//
// D3D12 has no context whose bindings the engine could read and change, so
// for the duration of one such call the shim context stands in for it (a
// "scope"):
//
//   - What the engine asks it for (ps-t0, vb0, ib, ...) is answered from
//     what the game has set on the command list, as D3D11 objects made up
//     for the purpose. They carry the hashes of the D3D11 resources they
//     correspond to, which is all checktextureoverride wants from them, and
//     cannot be used for anything else.
//   - What the engine binds stays with the shim context and is applied to the
//     game's list when something is drawn: the game's own call, unless
//     handling = skip said otherwise, and any draw of the mod's. Those use the
//     game's pipeline state, so a mod's buffers have to be laid out like the
//     ones they replace.
//   - Afterwards the list is put back the way the game left it.
//
// A compute shader of a mod dispatched from there is no different from one
// dispatched at present: it goes to our own list, which is executed before
// the game's. It can read a constant buffer of the game that way, as those are
// filled in by the time the game binds them.
//
// Not in here yet: drawing with custom shaders, render targets and unordered
// access views for these draws, and reading the game's textures.

// Whether any draw could be of interest at all. Read without the lock by the
// threads recording, like dx12_draw_generation, which changes when what was
// worked out for a pipeline state no longer holds.
extern volatile bool dx12_draw_path_active;
extern volatile UINT dx12_draw_generation;

bool Dx12DrawPipelineWanted(Dx12GraphicsCommandList *list);

// The check every draw of the game pays for
static inline bool Dx12DrawWanted(Dx12GraphicsCommandList *list)
{
	if (!dx12_draw_path_active)
		return false;
	if (list->mWantedPipeline != list->mState.pipeline || list->mWantedGeneration != dx12_draw_generation)
		return Dx12DrawPipelineWanted(list);
	return list->mWanted;
}

// For the list proxy. True if the call was dealt with, which includes making
// it: the proxy then has nothing left to do.
bool Dx12Draw(Dx12GraphicsCommandList *list, bool indexed, UINT count, UINT instances, UINT start,
		INT base_vertex, UINT start_instance);
bool Dx12Dispatch(Dx12GraphicsCommandList *list, UINT x, UINT y, UINT z);

// Once per present, with the lock of Bridge.h held
void Dx12DrawPathFramePresented();
// After the configuration was read again, which threw away what was worked
// out for the game's shaders
void Dx12DrawPathConfigReloaded();

// For the key that compares against the game as it is without us: whether
// nothing of the mods is to reach a frame, neither through the game's draws
// nor at present
extern bool dx12_mods_disabled;
void Dx12ToggleMods();

// What marking a shader does while hunting: writes the shader as the game
// gave it to ShaderFixes, for dxc or a disassembler to take from there
void Dx12DumpMarkedShader(UINT64 hash, const char *type);

// For the shim context. All but the first are only for while a scope is open,
// and what they return is not referenced.
bool Dx12InDrawScope();
ID3D11Buffer* Dx12ScopeVertexBuffer(UINT slot, UINT *stride);
ID3D11Buffer* Dx12ScopeIndexBuffer(DXGI_FORMAT *format);
ID3D11Buffer* Dx12ScopeConstantBuffer(Dx12ShaderStage stage, UINT slot);
ID3D11ShaderResourceView* Dx12ScopeShaderResource(Dx12ShaderStage stage, UINT slot);
ID3D11UnorderedAccessView* Dx12ScopeUnorderedAccess(UINT slot);
void Dx12ScopeDraw(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance);
void Dx12ScopeUnsupported(const char *what);
void Dx12ScopeDispatched();
