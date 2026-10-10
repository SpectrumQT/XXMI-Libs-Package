#pragma once

#include <vector>

#include "Dx12Common.h"

// The one thing about a DXIL shader that is only in its LLVM bitcode and that
// we need: how large its constant buffers are. D3D12 binds a constant buffer
// by address, so what the shader declares is all there is to tell how much of
// the memory at that address belongs to the buffer.

struct Dx12DxilCBuffer {
	UINT space;
	UINT reg;
	// More than one for an array of constant buffers
	UINT count;
	UINT bytes;
};

// data is the DXIL part of a shader container. False if it could not be read,
// which leaves buffers as it was.
bool Dx12ReadDxilCBuffers(const BYTE *data, size_t size, std::vector<Dx12DxilCBuffer> *buffers);
