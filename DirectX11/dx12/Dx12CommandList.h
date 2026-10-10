#pragma once

#include "Dx12Common.h"

class Dx12Device;

// Wraps every kind of list that is recorded through ID3D12GraphicsCommandList:
// direct, bundle, compute and copy. Video lists are left to the game.
class Dx12GraphicsCommandList final : public ID3D12GraphicsCommandList10
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	ID3D12GraphicsCommandList10 *mOrig;
	Dx12Device *mDevice;
	// Highest N of the ID3D12GraphicsCommandListN the original implements
	int mVersion;
	volatile LONG mRef;

	// Since the last Reset. A list is recorded by one thread at a time.
	UINT mDraws;
	UINT mDispatches;

	Dx12GraphicsCommandList(Dx12Device *device, ID3D12GraphicsCommandList *orig);

	bool Usable(REFIID riid);
	void Abandon();

#include "generated/Dx12GraphicsCommandList.decl.inl"
};
