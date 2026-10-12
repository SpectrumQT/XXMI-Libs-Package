#pragma once

#include <stdint.h>

#include "Dx12Common.h"

// Resources are not wrapped: the game holds the original ID3D12Resource. What
// we know about each one lives in a table keyed by its address, filled when
// the resource is created through the device proxy and emptied when D3D12
// destroys it. Resources that reach the game another way (swap chain buffers,
// shared handles, another layer's device) are simply not in the table.
//
// Nothing here keeps a resource alive, so a resource pointer taken from a
// descriptor or a view is only ever used as a key, never dereferenced.

const LONG DX12_STATE_UNKNOWN = -1;

struct Dx12ResourceInfo {
	D3D12_RESOURCE_DESC desc;
	D3D12_HEAP_TYPE heap_type;
	// Whether Map can be expected to work
	bool cpu_accessible;
	// Buffers only
	UINT64 va;

	// The state the executed command lists left the whole resource in, or
	// DX12_STATE_UNKNOWN: subresources in different states, enhanced
	// barriers, or a resource created with a layout.
	LONG state;
	// The same for subresource 0 alone, which is known more often: a
	// texture whose mips were transitioned one by one has no state above
	LONG top_state;

	// Textures: crc32c of the top mip as 3DMigoto calculates it for
	// D3D11, see TextureHash.h
	bool has_data_hash;
	uint32_t data_hash;
	// D3D12 does not tell a cube map from a 2D array until a view is made
	bool cube;

	// Tells the resource from one created at its address after it was
	// destroyed. Never 0.
	UINT64 serial;
};

// Calls back once when D3D12 destroys the object. One callback per object.
void Dx12NotifyDestruction(ID3D12Object *object, void (*callback)(void *object));

// For the device's Create*Resource methods, on success. ppvResource is what
// the game asked for, which may be nothing at all.
void Dx12TrackResource(REFIID riid, void **ppvResource, LONG initial_state);

bool Dx12GetResourceInfo(ID3D12Resource *resource, Dx12ResourceInfo *info);

// For a transition barrier of a command list that is being executed.
// subresource is as in D3D12_RESOURCE_TRANSITION_BARRIER.
void Dx12ApplyTransition(ID3D12Resource *resource, UINT subresource, LONG state);

void Dx12SetTextureDataHash(ID3D12Resource *resource, bool has_data_hash, uint32_t data_hash);
void Dx12MarkCubeTexture(ID3D12Resource *resource);

bool Dx12FindBuffer(UINT64 va, ID3D12Resource **resource, UINT64 *offset);
// For remembering what Dx12FindBuffer said about an address: a number that
// has changed if a buffer was created or destroyed there since, and may have
// changed without that. Taken before asking.
UINT64 Dx12FindBufferStamp(UINT64 va);
