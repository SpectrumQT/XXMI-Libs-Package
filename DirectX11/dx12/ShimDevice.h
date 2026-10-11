#pragma once

#include "ShimObjects.h"

// The D3D11 device the engine creates its objects on. There is one, and it
// lives as long as the process.
class ShimDevice final : public ID3D11Device1
{
public:
	volatile LONG mRef;
	ShimPrivateData mPrivate;

	ShimDevice();

#include "generated/ShimDevice.decl.inl"
};

// Copy CPU memory into a resource of ours. For a texture the data is one
// whole subresource with rows row_pitch apart.
bool ShimWriteBuffer(ShimResource *resource, UINT64 offset, const void *data, UINT64 size);
// Gives a buffer that only had its contents in CPU memory a resource with
// those contents, for the GPU to write to
bool ShimBufferToGpu(ShimResource *resource);
bool ShimWriteSubresource(ShimResource *resource, UINT subresource, const void *data, UINT row_pitch, UINT slice_pitch);
// Copies the shadow of a texture with write_pending to it. Dx12ShimTransition
// does, which whatever uses a resource goes through first.
void ShimFlushWrite(ShimResource *resource);
// Gives the resource a fresh D3D12 resource with the contents of the one it
// has, which the game's lists that use it keep: for a write those lists are
// not to see, see Bridge.h. Dx12ShimTransition does it for any write.
bool ShimRenameResource(ShimResource *resource);

static inline UINT64 ShimAlign(UINT64 value, UINT64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}
