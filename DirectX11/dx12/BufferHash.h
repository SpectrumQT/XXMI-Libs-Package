#pragma once

#include "ResourceTracker.h"

// Under D3D11 a vertex or index buffer is one resource, hashed from the data
// and the D3D11_BUFFER_DESC passed to CreateBuffer. A D3D12 renderer carves
// its buffers out of large pooled resources and fills each with a copy from
// an upload buffer, so here a "buffer" is the range written by one such copy,
// hashed when the copy is recorded and looked up by resource and offset.
//
// This only identifies buffers whose upload is a single copy of the same
// bytes D3D11 would have been given. Where a game uploads differently, the
// range has to be hashed from the GPU copy instead, which needs command lists
// of our own and comes with the engine shim.

struct Dx12BufferHash {
	UINT64 bytes;
	uint32_t data_hash;
	// dx12_frame when the copy was recorded
	UINT frame;
};

// For the command list proxy, before it forwards the call
void Dx12CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT64 bytes);

bool Dx12FindBufferHash(ID3D12Resource *resource, UINT64 offset, Dx12BufferHash *hash);
// For remembering what Dx12FindBufferHash said about a range: a number that
// has changed if anything was written to these bytes of the resource, or
// forgotten about them, since. It may change without that as well. Taken
// before asking, or what was written in between goes unnoticed. False if the
// range is too large to be told apart from the rest of its resource.
bool Dx12BufferHashStamp(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, UINT64 *stamp);
void Dx12ForgetBufferHashes(ID3D12Resource *resource);

// The hash HackerDevice::CreateBuffer would have calculated for a default
// usage buffer of this size with these D3D11_BIND_FLAGs
uint32_t Dx12BufferDescHash(const Dx12BufferHash *hash, UINT bind_flags);

struct Dx12BufferStats {
	volatile LONG64 hashed;
	volatile LONG64 hashed_bytes;
	// Ranges rewritten every few frames are not buffers a mod would match
	volatile LONG64 dynamic;
	volatile LONG64 not_mappable;
	volatile LONG64 untracked;
	volatile LONG64 out_of_bounds;
};
extern Dx12BufferStats dx12_buffer_stats;
