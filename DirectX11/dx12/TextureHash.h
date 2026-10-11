#pragma once

#include "ResourceTracker.h"

// Reproduces the hashes [TextureOverride] sections were written against under
// D3D11, where 3DMigoto hashes the top mip passed to CreateTexture2D and then
// the D3D11_TEXTURE2D_DESC.
//
// D3D12 textures are created empty and filled by copying from an upload
// buffer, so the data is hashed when that copy is recorded: the upload buffer
// is mappable and holds the same bytes the game would have handed to D3D11.
// Later writes and copies follow the track_texture_updates rules of
// HackerContext.
//
// The description is rebuilt from the D3D12 one. What D3D12 does not record
// (bind flags, typeless formats chosen by a D3D11 renderer) is a guess, so the
// format has variants that HashDump.cpp compares against the hashes in the
// mods to tell which one a game needs.

enum Dx12FormatRule {
	// The format of the D3D12 resource
	DX12_FORMAT_AS_IS,
	// Block compressed formats as their typeless family
	DX12_FORMAT_BC_TYPELESS,
	// The formats Unreal Engine creates typeless under D3D11:
	// BC1, BC2, BC3, BC7, R8G8B8A8 and B8G8R8A8
	DX12_FORMAT_UNREAL,
	DX12_FORMAT_RULE_COUNT
};
extern const char *const dx12_format_rule_names[DX12_FORMAT_RULE_COUNT];

// The hash 3DMigoto would have given the D3D11 counterpart of a 2D texture
uint32_t Dx12TextureHash(const Dx12ResourceInfo *info, Dx12FormatRule rule);

// For the command list proxy, before it forwards the call
void Dx12CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT x, UINT y, UINT z,
		const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *box);
void Dx12CopyResource(ID3D12Resource *dst, ID3D12Resource *src);

// Why uploads were or were not hashed, since the last report
struct Dx12TextureStats {
	volatile LONG64 hashed;
	volatile LONG64 hashed_bytes;
	// Mips below the top one and array slices after the first, which the
	// D3D11 hash does not cover either
	volatile LONG64 other_subresource;
	volatile LONG64 partial;
	volatile LONG64 update_ignored;
	volatile LONG64 not_2d;
	volatile LONG64 unknown_format;
	volatile LONG64 untracked;
	volatile LONG64 not_mappable;
	volatile LONG64 out_of_bounds;
	volatile LONG64 copy_propagated;
	volatile LONG64 copy_partial;
};
extern Dx12TextureStats dx12_texture_stats;
