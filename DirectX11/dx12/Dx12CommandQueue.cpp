#include "Dx12CommandQueue.h"

#include <deque>
#include <unordered_map>
#include <vector>

#include "Bridge.h"
#include "Dx12CommandList.h"
#include "Dx12Device.h"
#include "ResourceTracker.h"

// {1B67B403-353E-45FD-B468-760207CA7886}
const GUID Dx12CommandQueue::ProxyIID = { 0x1b67b403, 0x353e, 0x45fd, { 0xb4, 0x68, 0x76, 0x02, 0x07, 0xca, 0x78, 0x86 } };
void *Dx12CommandQueue::sVtbl;

Dx12CommandQueue::Dx12CommandQueue(Dx12Device *device, ID3D12CommandQueue *orig) :
	mOrig(orig),
	mDevice(device),
	mRef(1)
{
	mOrig->AddRef();
	mDevice->AddRef();
	sVtbl = *(void**)this;
}

bool Dx12CommandQueue::Usable(REFIID riid)
{
	return IsEqualIID(riid, ProxyIID)
	    || IsEqualIID(riid, __uuidof(IUnknown))
	    || IsEqualIID(riid, __uuidof(ID3D12Object))
	    || IsEqualIID(riid, __uuidof(ID3D12DeviceChild))
	    || IsEqualIID(riid, __uuidof(ID3D12Pageable))
	    || IsEqualIID(riid, __uuidof(ID3D12CommandQueue));
}

void Dx12CommandQueue::Abandon()
{
	mOrig->Release();
	mDevice->Release();
	delete this;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::QueryInterface(REFIID riid, void **ppvObject)
{
	HRESULT hr;

	if (!ppvObject)
		return E_POINTER;

	if (Usable(riid)) {
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	hr = mOrig->QueryInterface(riid, ppvObject);
	Dx12LogForwardedQuery("Dx12CommandQueue", riid, hr);
	return hr;
}

ULONG STDMETHODCALLTYPE Dx12CommandQueue::AddRef()
{
	mOrig->AddRef();
	return InterlockedIncrement(&mRef);
}

ULONG STDMETHODCALLTYPE Dx12CommandQueue::Release()
{
	// Read first: once our reference is gone, so may be this object.
	ID3D12CommandQueue *orig = mOrig;
	Dx12Device *device = mDevice;
	ULONG ref;

	ref = InterlockedDecrement(&mRef);
	if (ref) {
		orig->Release();
		return ref;
	}

	Dx12LogInfo("Dx12CommandQueue %p destroyed\n", this);
	delete this;
	orig->Release();
	device->Release();
	return 0;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::GetDevice(REFIID riid, void **ppvDevice)
{
	return mDevice->QueryInterface(riid, ppvDevice);
}

void STDMETHODCALLTYPE Dx12CommandQueue::ExecuteCommandLists(UINT NumCommandLists, ID3D12CommandList *const *ppCommandLists)
{
	ID3D12CommandList *stack[64], **lists = stack;
	std::vector<ID3D12CommandList*> large;
	std::vector<IUnknown*> pins, first;
	UINT64 needed = 0;
	UINT i;

	if (!ppCommandLists) {
		mOrig->ExecuteCommandLists(NumCommandLists, ppCommandLists);
		return;
	}

	if (NumCommandLists > ARRAYSIZE(stack)) {
		large.resize(NumCommandLists);
		lists = large.data();
	}

	// What the mods had done for the draws of these lists comes first, and
	// was recorded with the states their resources had before them
	Dx12ShimFlush();

	for (i = 0; i < NumCommandLists; i++) {
		lists[i] = Dx12Unwrap<Dx12GraphicsCommandList>(ppCommandLists[i]);
		if (lists[i] == ppCommandLists[i])
			continue;

		// From here on the states the list was recorded to leave its
		// resources in are the ones they have, as far as anything
		// executed later is concerned
		Dx12GraphicsCommandList *proxy = static_cast<Dx12GraphicsCommandList*>(ppCommandLists[i]);
		for (const Dx12Transition &transition : proxy->mTransitions)
			Dx12ApplyTransition(transition.resource, transition.subresource, transition.state);
		// The list keeps what it pinned, for being executed again:
		// this execution gets references of its own, together so
		// that one signal of the fence does for all of them
		for (IUnknown *object : proxy->mPins) {
			object->AddRef();
			pins.push_back(object);
		}
		if (!proxy->mPins.empty())
			needed = max(needed, proxy->mShimFence);
		// What was worked out for its draws went by the queue, see
		// Dx12DrawPipelineWanted
		if (proxy->mQueue != mOrig) {
			proxy->mQueue = mOrig;
			proxy->mWantedGeneration = 0;
		}
		if (!proxy->mExecuted) {
			proxy->mExecuted = true;
			first.insert(first.end(), proxy->mPins.begin(), proxy->mPins.end());
		}
	}

	InterlockedIncrement64(&dx12_stats.execute_calls);
	InterlockedAdd64(&dx12_stats.lists_executed, NumCommandLists);

	if (!pins.empty() || Dx12ShimQueueUnordered(mOrig))
		Dx12ShimQueueWaits(mOrig, needed, !pins.empty());

	Dx12ShimResidencyWait(mOrig);
	mOrig->ExecuteCommandLists(NumCommandLists, lists);

	if (!pins.empty())
		Dx12ShimPinsExecuted(mOrig, &pins, &first);
}

// The signals and waits the queues were handed, in order: the waits their
// fences have not got to yet, and for each fence the signals of values it has
// not got to yet. That tells whether a queue gets to a signal with what all of
// them have been handed so far, see Dx12QueueCanWait. A fence signalled other
// than by a queue of ours is not known to be, which errs on the side of not
// waiting.
struct QueuedWait {
	// Referenced
	ID3D12Fence *fence;
	UINT64 value;
	ID3D12CommandQueue *queue;
	UINT64 order;
};
struct QueuedSignal {
	UINT64 value;
	ID3D12CommandQueue *queue;
	UINT64 order;
};
static SRWLOCK fence_lock = SRWLOCK_INIT;
static UINT64 queue_order;
static std::vector<QueuedWait> queued_waits;
static std::unordered_map<ID3D12Fence*, std::deque<QueuedSignal>> queued_signals;

const size_t QUEUED_SIGNALS_KEPT = 4096;
const int QUEUED_WAIT_DEPTH = 8;

static void fence_destroyed(void *object)
{
	AcquireSRWLockExclusive(&fence_lock);
	queued_signals.erase((ID3D12Fence*)object);
	ReleaseSRWLockExclusive(&fence_lock);
}

// With fence_lock held. The fences in done are for releasing without it:
// that may be the end of one, which takes the lock.
static void forget_reached_waits(std::vector<ID3D12Fence*> *done)
{
	size_t i, kept = 0;

	for (i = 0; i < queued_waits.size(); i++) {
		if (queued_waits[i].fence->GetCompletedValue() >= queued_waits[i].value)
			done->push_back(queued_waits[i].fence);
		else
			queued_waits[kept++] = queued_waits[i];
	}
	queued_waits.resize(kept);
}

// The first signal handed to a queue that takes the fence to the value
static const QueuedSignal* queued_signal(ID3D12Fence *fence, UINT64 value)
{
	auto found = queued_signals.find(fence);

	if (found == queued_signals.end())
		return NULL;
	for (const QueuedSignal &signal : found->second) {
		if (signal.value >= value)
			return &signal;
	}
	return NULL;
}

// Whether the queue gets through what it was handed before order without
// anything that no queue has been handed yet
static bool queue_reaches(ID3D12CommandQueue *queue, UINT64 order, int depth)
{
	for (const QueuedWait &wait : queued_waits) {
		const QueuedSignal *signal;

		if (wait.queue != queue || wait.order >= order || wait.fence->GetCompletedValue() >= wait.value)
			continue;
		// Queues of a game that wait in a circle are not ours to wait for
		if (depth >= QUEUED_WAIT_DEPTH)
			return false;
		signal = queued_signal(wait.fence, wait.value);
		if (!signal || !queue_reaches(signal->queue, signal->order, depth + 1))
			return false;
	}
	return true;
}

void Dx12QueueSignalled(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
	UINT64 completed = fence->GetCompletedValue();
	bool added;

	AcquireSRWLockExclusive(&fence_lock);
	added = !queued_signals.count(fence);
	std::deque<QueuedSignal> &signals = queued_signals[fence];
	while (!signals.empty() && (signals.front().value <= completed || signals.size() >= QUEUED_SIGNALS_KEPT))
		signals.pop_front();
	signals.push_back({ value, queue, ++queue_order });
	ReleaseSRWLockExclusive(&fence_lock);

	// Or the next fence at its address would be taken to have been
	// signalled as this one was
	if (added)
		Dx12NotifyDestruction(fence, fence_destroyed);
}

void Dx12QueueWaits(ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
	std::vector<ID3D12Fence*> done;

	if (fence->GetCompletedValue() >= value)
		return;

	fence->AddRef();
	AcquireSRWLockExclusive(&fence_lock);
	forget_reached_waits(&done);
	queued_waits.push_back({ fence, value, queue, ++queue_order });
	ReleaseSRWLockExclusive(&fence_lock);

	for (ID3D12Fence *reached : done)
		reached->Release();
}

bool Dx12QueueCanWait(ID3D12Fence *fence, UINT64 value)
{
	std::vector<ID3D12Fence*> done;
	const QueuedSignal *signal;
	bool reached;

	if (fence->GetCompletedValue() >= value)
		return true;

	AcquireSRWLockExclusive(&fence_lock);
	forget_reached_waits(&done);
	signal = queued_signal(fence, value);
	reached = signal && queue_reaches(signal->queue, signal->order, 0);
	ReleaseSRWLockExclusive(&fence_lock);

	for (ID3D12Fence *reached_fence : done)
		reached_fence->Release();
	return reached;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::Signal(ID3D12Fence *pFence, UINT64 Value)
{
	HRESULT hr = mOrig->Signal(pFence, Value);

	// After the queue has it and not before, as a wait is noted before:
	// either way round, what is not known for a moment is that the queue
	// gets further than it is taken to
	if (SUCCEEDED(hr) && pFence)
		Dx12QueueSignalled(mOrig, pFence, Value);
	return hr;
}

HRESULT STDMETHODCALLTYPE Dx12CommandQueue::Wait(ID3D12Fence *pFence, UINT64 Value)
{
	if (pFence)
		Dx12QueueWaits(mOrig, pFence, Value);
	return mOrig->Wait(pFence, Value);
}

#include "generated/Dx12CommandQueue.fwd.inl"
