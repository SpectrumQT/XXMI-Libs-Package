#pragma once

#include "Dx12Common.h"

class Dx12Device;

class Dx12CommandQueue final : public ID3D12CommandQueue
{
public:
	static const GUID ProxyIID;
	static void *sVtbl;

	ID3D12CommandQueue *mOrig;
	Dx12Device *mDevice;
	volatile LONG mRef;

	Dx12CommandQueue(Dx12Device *device, ID3D12CommandQueue *orig);

	bool Usable(REFIID riid);
	void Abandon();

#include "generated/Dx12CommandQueue.decl.inl"
};
