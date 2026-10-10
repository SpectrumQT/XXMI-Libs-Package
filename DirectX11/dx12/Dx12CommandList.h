#pragma once

#include <vector>

#include "CmdState.h"

class Dx12Device;

struct Dx12Transition {
	ID3D12Resource *resource;
	UINT subresource;
	LONG state;
};

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

	Dx12CmdState mState;

	// The resource states this list leaves behind, in recording order.
	// They only become true when the list is executed, which is when the
	// queue hands them to the resource tracker.
	std::vector<Dx12Transition> mTransitions;

	Dx12GraphicsCommandList(Dx12Device *device, ID3D12GraphicsCommandList *orig);

	bool Usable(REFIID riid);
	void Abandon();

#include "generated/Dx12GraphicsCommandList.decl.inl"
};
