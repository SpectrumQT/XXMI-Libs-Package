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
bool ShimWriteSubresource(ShimResource *resource, UINT subresource, const void *data, UINT row_pitch, UINT slice_pitch);

static inline UINT64 ShimAlign(UINT64 value, UINT64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}
