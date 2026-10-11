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
	// Counts the times the list was reset, to tell one recording of it
	// from the next
	UINT mResets;

	Dx12CmdState mState;

	// What mods had this list use, see Dx12ShimPin. Kept until the list is
	// reset: a closed list may be executed more than once.
	std::vector<IUnknown*> mPins;
	// Some of those by what they hash to, so that what every draw of a mod
	// uses is not pinned once for each of them. Emptied along with mPins.
	IUnknown *mPinned[32];
	// Whether the list has been executed since it was last reset
	bool mExecuted;

	// Whether the engine has anything to do for draws with mPipeline,
	// see Dx12DrawWanted
	ID3D12PipelineState *mWantedPipeline;
	UINT mWantedGeneration;
	bool mWanted;

	// What the registry has on the pipeline state and root signatures
	// last asked about, see Dx12ListPipeline. Forgotten at Reset, as the
	// game only keeps those alive for a list that is being recorded.
	ID3D12PipelineState *mInfoPipeline;
	const Dx12PipelineInfo *mInfo;
	ID3D12RootSignature *mInfoSignature[2];
	const Dx12RootSignatureInfo *mSignatureInfo[2];

	// The resource states this list leaves behind, in recording order.
	// They only become true when the list is executed, which is when the
	// queue hands them to the resource tracker, each time it is.
	std::vector<Dx12Transition> mTransitions;

	Dx12GraphicsCommandList(Dx12Device *device, ID3D12GraphicsCommandList *orig);

	bool Usable(REFIID riid);
	void Abandon();
	void ForgetInfo();

#include "generated/Dx12GraphicsCommandList.decl.inl"
};
