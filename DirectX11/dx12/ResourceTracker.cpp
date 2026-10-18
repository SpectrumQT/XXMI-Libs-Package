#include "ResourceTracker.h"

#include <map>
#include <unordered_map>
#include <unordered_set>

#include "BufferHash.h"

// {5D0C2A8E-93B4-4F0E-9C57-3E1B7A64D2F1}
static const GUID DestructionNotifierGuid = { 0x5d0c2a8e, 0x93b4, 0x4f0e, { 0x9c, 0x57, 0x3e, 0x1b, 0x7a, 0x64, 0xd2, 0xf1 } };

// D3D12 objects release the interfaces stored as their private data when they
// are destroyed, which is the only notification of that there is (this is how
// ReShade tracks resource lifetime as well).
class DestructionNotifier final : public IUnknown
{
	volatile LONG mRef;
	void *mObject;
	void (*mCallback)(void *object);

public:
	DestructionNotifier(void *object, void (*callback)(void *object)) :
		mRef(1), mObject(object), mCallback(callback)
	{}

	void Disarm()
	{
		mCallback = NULL;
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
	{
		if (!ppvObject)
			return E_POINTER;
		if (!IsEqualIID(riid, __uuidof(IUnknown))) {
			*ppvObject = NULL;
			return E_NOINTERFACE;
		}
		AddRef();
		*ppvObject = this;
		return S_OK;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return InterlockedIncrement(&mRef);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG ref = InterlockedDecrement(&mRef);

		if (!ref) {
			if (mCallback)
				mCallback(mObject);
			delete this;
		}
		return ref;
	}
};

bool Dx12NotifyDestruction(ID3D12Object *object, REFGUID guid, void (*callback)(void *context), void *context)
{
	DestructionNotifier *notifier = new DestructionNotifier(context, callback);
	bool armed = SUCCEEDED(object->SetPrivateDataInterface(guid, notifier));

	if (!armed)
		notifier->Disarm();
	notifier->Release();
	return armed;
}

void Dx12NotifyDestruction(ID3D12Object *object, void (*callback)(void *object))
{
	Dx12NotifyDestruction(object, DestructionNotifierGuid, callback, object);
}

static SRWLOCK gone_lock = SRWLOCK_INIT;
static std::unordered_set<const void*> gone;

void Dx12KeepGone(void *storage)
{
	AcquireSRWLockExclusive(&gone_lock);
	gone.insert(storage);
	ReleaseSRWLockExclusive(&gone_lock);
}

bool Dx12IsGone(const void *object)
{
	bool result;

	AcquireSRWLockShared(&gone_lock);
	result = gone.count(object) != 0;
	ReleaseSRWLockShared(&gone_lock);
	return result;
}

struct ResourceShard {
	SRWLOCK lock;
	std::unordered_map<ID3D12Resource*, Dx12ResourceInfo> resources;
};

// Resources are created and destroyed from several threads at once while
// textures stream in, hence more than one lock.
static ResourceShard shards[64];

struct BufferRange {
	UINT64 end;
	ID3D12Resource *resource;
};

static SRWLOCK buffer_lock = SRWLOCK_INIT;
static std::map<UINT64, BufferRange> buffers;

// See Dx12FindBufferStamp. A counter per 64 KiB of address space, which is
// what D3D12 aligns buffers to, shared with whatever else hashes to it, and
// one for buffers covering too many of those. Engines create and destroy
// buffers all the time: counters that many buffers shared would have every
// one of them looked up again for each.
const UINT VA_STAMP_SHIFT = 16;
const UINT64 VA_STAMP_MAX_SPAN = 256;
static volatile LONG64 va_stamps[16384];
static volatile LONG64 va_stamp_all;

static volatile LONG64* va_stamp(UINT64 granule)
{
	return &va_stamps[(granule * 0x9e3779b97f4a7c15ull) >> 50];
}

// With buffer_lock held
static void stamp_buffer(UINT64 va, UINT64 end)
{
	UINT64 first = va >> VA_STAMP_SHIFT, last = (end > va ? end - 1 : va) >> VA_STAMP_SHIFT;

	if (last - first >= VA_STAMP_MAX_SPAN) {
		InterlockedIncrement64(&va_stamp_all);
		return;
	}
	for (; first <= last; first++)
		InterlockedIncrement64(va_stamp(first));
}

UINT64 Dx12FindBufferStamp(UINT64 va)
{
	return (UINT64)va_stamp_all + (UINT64)*va_stamp(va >> VA_STAMP_SHIFT);
}

void Dx12FindBufferStampCounters(UINT64 va, const volatile LONG64 *counters[2])
{
	counters[0] = &va_stamp_all;
	counters[1] = va_stamp(va >> VA_STAMP_SHIFT);
}

static volatile LONG64 resource_serial;

// See Dx12TextureStamp. Counted up with the lock of the resource's shard held.
static volatile LONG64 texture_stamps[4096];

static volatile LONG64* texture_stamp(ID3D12Resource *resource)
{
	return &texture_stamps[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 52];
}

UINT64 Dx12TextureStamp(ID3D12Resource *resource)
{
	return (UINT64)*texture_stamp(resource);
}

static ResourceShard* shard_of(ID3D12Resource *resource)
{
	return &shards[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 58];
}

static void resource_destroyed(void *object)
{
	ID3D12Resource *resource = (ID3D12Resource*)object;
	ResourceShard *shard = shard_of(resource);
	UINT64 va = 0;

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		va = i->second.va;
		shard->resources.erase(i);
	}
	InterlockedIncrement64(texture_stamp(resource));
	ReleaseSRWLockExclusive(&shard->lock);

	if (va) {
		AcquireSRWLockExclusive(&buffer_lock);
		auto range = buffers.find(va);
		if (range != buffers.end() && range->second.resource == resource) {
			stamp_buffer(va, range->second.end);
			buffers.erase(range);
		}
		ReleaseSRWLockExclusive(&buffer_lock);

		Dx12ForgetBufferHashes(resource);
	}
}

void Dx12TrackResource(REFIID riid, void **ppvResource, LONG initial_state)
{
	ID3D12Resource *resource = NULL;
	D3D12_HEAP_PROPERTIES heap = {};
	Dx12ResourceInfo info = {};
	ResourceShard *shard;

	if (!ppvResource || !*ppvResource)
		return;
	if (FAILED(((IUnknown*)*ppvResource)->QueryInterface(IID_PPV_ARGS(&resource))))
		return;

	info.desc = resource->GetDesc();
	info.state = initial_state;
	info.top_state = initial_state;
	info.serial = InterlockedIncrement64(&resource_serial);
	// Fails for reserved resources, which have no heap of their own
	if (SUCCEEDED(resource->GetHeapProperties(&heap, NULL))) {
		info.heap_type = heap.Type;
		info.cpu_accessible = heap.Type == D3D12_HEAP_TYPE_UPLOAD
			|| heap.Type == D3D12_HEAP_TYPE_READBACK
			|| heap.Type == D3D12_HEAP_TYPE_GPU_UPLOAD
			|| (heap.Type == D3D12_HEAP_TYPE_CUSTOM && heap.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE);
	}
	if (info.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		info.va = resource->GetGPUVirtualAddress();

	shard = shard_of(resource);
	AcquireSRWLockExclusive(&shard->lock);
	shard->resources[resource] = info;
	InterlockedIncrement64(texture_stamp(resource));
	ReleaseSRWLockExclusive(&shard->lock);

	if (info.va) {
		AcquireSRWLockExclusive(&buffer_lock);
		// One that was at the address before and is still in the map
		// may have reached further
		auto previous = buffers.find(info.va);
		if (previous != buffers.end())
			stamp_buffer(info.va, previous->second.end);
		buffers[info.va] = { info.va + info.desc.Width, resource };
		stamp_buffer(info.va, info.va + info.desc.Width);
		ReleaseSRWLockExclusive(&buffer_lock);
	}

	Dx12NotifyDestruction(resource, resource_destroyed);

	resource->Release();
}

bool Dx12GetResourceInfo(ID3D12Resource *resource, Dx12ResourceInfo *info)
{
	ResourceShard *shard = shard_of(resource);
	bool found = false;

	if (!resource)
		return false;

	AcquireSRWLockShared(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		*info = i->second;
		found = true;
	}
	ReleaseSRWLockShared(&shard->lock);

	return found;
}

void Dx12ApplyTransition(ID3D12Resource *resource, UINT subresource, LONG state)
{
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockShared(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end()) {
		const D3D12_RESOURCE_DESC *desc = &i->second.desc;

		if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || !subresource)
			InterlockedExchange(&i->second.top_state, state);

		// A single state is all that is kept per resource, so a
		// transition of one subresource out of several leaves it
		// unknown. Depth stencil formats have a subresource per plane.
		if (subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
		    && desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER
		    && (desc->MipLevels != 1
		     || (desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D && desc->DepthOrArraySize != 1)
		     || (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
			state = DX12_STATE_UNKNOWN;

		InterlockedExchange(&i->second.state, state);
	}
	ReleaseSRWLockShared(&shard->lock);
}

void Dx12SetTextureDataHash(ID3D12Resource *resource, bool has_data_hash, uint32_t data_hash)
{
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end() && (i->second.has_data_hash != has_data_hash || i->second.data_hash != data_hash)) {
		i->second.has_data_hash = has_data_hash;
		i->second.data_hash = data_hash;
		InterlockedIncrement64(texture_stamp(resource));
	}
	ReleaseSRWLockExclusive(&shard->lock);
}

void Dx12MarkCubeTexture(ID3D12Resource *resource)
{
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end() && !i->second.cube) {
		i->second.cube = true;
		InterlockedIncrement64(texture_stamp(resource));
	}
	ReleaseSRWLockExclusive(&shard->lock);
}

// The buffers found before, by the 64 KiB of address space asked about, for
// the threads recording: they ask for the buffers of every draw that are not
// the same from frame to frame, with the map behind a lock they all share.
// What a slot has goes while the stamp of where it was asked is the one taken
// before asking. seq is odd while a slot is being written.
struct FoundBuffer {
	volatile LONG seq;
	UINT64 granule, start, end, stamp;
	ID3D12Resource *resource;
};
static FoundBuffer found_buffers[4096];

bool Dx12FindBuffer(UINT64 va, ID3D12Resource **resource, UINT64 *offset)
{
	FoundBuffer *slot, remembered;
	UINT64 stamp;
	bool found = false;
	LONG seq;

	if (!va)
		return false;

	stamp = Dx12FindBufferStamp(va);
	slot = &found_buffers[((va >> VA_STAMP_SHIFT) * 0x9e3779b97f4a7c15ull) >> 52];
	seq = slot->seq;
	if (!(seq & 1)) {
		remembered.granule = slot->granule;
		remembered.start = slot->start;
		remembered.end = slot->end;
		remembered.stamp = slot->stamp;
		remembered.resource = slot->resource;
		if (slot->seq == seq && remembered.granule == va >> VA_STAMP_SHIFT && remembered.stamp == stamp
		    && va >= remembered.start && va < remembered.end) {
			*resource = remembered.resource;
			*offset = va - remembered.start;
			return true;
		}
	}

	AcquireSRWLockShared(&buffer_lock);
	auto i = buffers.upper_bound(va);
	if (i != buffers.begin()) {
		--i;
		if (va < i->second.end) {
			*resource = i->second.resource;
			*offset = va - i->first;
			remembered.start = i->first;
			remembered.end = i->second.end;
			found = true;
		}
	}
	ReleaseSRWLockShared(&buffer_lock);

	if (found && !(seq & 1) && InterlockedCompareExchange(&slot->seq, seq + 1, seq) == seq) {
		slot->granule = va >> VA_STAMP_SHIFT;
		slot->start = remembered.start;
		slot->end = remembered.end;
		slot->stamp = stamp;
		slot->resource = *resource;
		InterlockedExchange(&slot->seq, seq + 2);
	}

	return found;
}
