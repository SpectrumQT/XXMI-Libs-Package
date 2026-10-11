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

// For telling whether a queue can be made to wait for a fence without two
// queues ending up waiting for each other. That takes knowing every signal and
// wait the queues were handed: the proxies note the game's, and whoever else
// hands a queue a wait, or a signal another queue is made to wait for, has to
// as well (a signal after the queue has it, a wait before).
void Dx12QueueSignalled(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value);
void Dx12QueueWaits(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value);
// Whether the signal that takes the fence to the value has been handed to a
// queue that gets to it with what the queues have been handed so far. Then a
// wait for it handed to a queue now cannot be one of a circle.
bool Dx12QueueCanWait(ID3D12Fence *fence, UINT64 value);
