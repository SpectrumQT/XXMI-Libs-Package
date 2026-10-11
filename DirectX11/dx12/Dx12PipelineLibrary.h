#pragma once

#include "Dx12Common.h"

class Dx12Device;

// A pipeline loaded from a library is created from the same description as
// one made by the device, but by the library. Wrapped only to see those.
class Dx12PipelineLibrary final : public ID3D12PipelineLibrary1
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	ID3D12PipelineLibrary1 *mOrig;
	Dx12Device *mDevice;
	// Highest N of the ID3D12PipelineLibraryN the original implements
	int mVersion;
	volatile LONG mRef;

	Dx12PipelineLibrary(Dx12Device *device, ID3D12PipelineLibrary *orig);

	bool Usable(REFIID riid);
	void Abandon();

#include "generated/Dx12PipelineLibrary.decl.inl"
};
