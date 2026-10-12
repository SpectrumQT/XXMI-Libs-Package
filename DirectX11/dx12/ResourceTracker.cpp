#include "ResourceTracker.h"

#include <map>
#include <unordered_map>

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

void Dx12NotifyDestruction(ID3D12Object *object, void (*callback)(void *object))
{
	DestructionNotifier *notifier = new DestructionNotifier(object, callback);

	if (FAILED(object->SetPrivateDataInterface(DestructionNotifierGuid, notifier)))
		notifier->Disarm();
	notifier->Release();
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

// See Dx12FindBufferStamp. A counter per 64 MiB of address space, shared with
// whatever else hashes to it, and one for buffers covering too many of those.
const UINT VA_STAMP_SHIFT = 26;
const UINT64 VA_STAMP_MAX_SPAN = 64;
static volatile LONG64 va_stamps[1024];
static volatile LONG64 va_stamp_all;

static volatile LONG64* va_stamp(UINT64 granule)
{
	return &va_stamps[(granule * 0x9e3779b97f4a7c15ull) >> 54];
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

static volatile LONG64 resource_serial;

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
	if (i != shard->resources.end()) {
		i->second.has_data_hash = has_data_hash;
		i->second.data_hash = data_hash;
	}
	ReleaseSRWLockExclusive(&shard->lock);
}

void Dx12MarkCubeTexture(ID3D12Resource *resource)
{
	ResourceShard *shard = shard_of(resource);

	AcquireSRWLockExclusive(&shard->lock);
	auto i = shard->resources.find(resource);
	if (i != shard->resources.end())
		i->second.cube = true;
	ReleaseSRWLockExclusive(&shard->lock);
}

bool Dx12FindBuffer(UINT64 va, ID3D12Resource **resource, UINT64 *offset)
{
	bool found = false;

	if (!va)
		return false;

	AcquireSRWLockShared(&buffer_lock);
	auto i = buffers.upper_bound(va);
	if (i != buffers.begin()) {
		--i;
		if (va < i->second.end) {
			*resource = i->second.resource;
			*offset = va - i->first;
			found = true;
		}
	}
	ReleaseSRWLockShared(&buffer_lock);

	return found;
}
