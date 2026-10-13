#include "Bridge.h"

#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "Dx12CommandList.h"
#include "Dx12CommandQueue.h"
#include "Dx12Device.h"
#include "DrawPath.h"
#include "ShimContext.h"
#include "ShimDevice.h"
#include "ShimPipeline.h"
#include "HashDump.h"

#include "util.h"

#include "../globals.h"
#include "../HackerContext.h"
#include "../HackerDevice.h"
#include "../IniHandler.h"

Dx12Shim *dx12_shim;
bool dx12_unloading;
std::atomic<bool> dx12_d3d11_presented;

// dx12_shim for Dx12IsD3D11Game, which is called without the shim's lock
static std::atomic<bool> shim_created;

bool Dx12IsD3D11Game()
{
	return dx12_d3d11_presented && !shim_created && G->dx12 != Dx12Mode::ON;
}
UINT dx12_shim_epoch = 1;
UINT dx12_shim_view_increment;

static Dx12Shim shim;

struct Allocator {
	ID3D12CommandAllocator *allocator;
	// Of the last submission recorded with it
	UINT64 fence;
};

static std::vector<Allocator> allocators;
static size_t current_allocator;
static ID3D12GraphicsCommandList *shim_list;
static bool list_open;
// Read without the lock by Dx12ShimFlush
static volatile bool list_used;

static ID3D12Fence *fence;
static UINT64 fence_signalled;
static HANDLE fence_event;

struct Retired {
	IUnknown *object;
	UINT64 fence;
};
static std::deque<Retired> retired;

// What the lists executed on another queue than ours pinned is held until a
// fence of our own that queue signals says they are done with it
struct ForeignQueue {
	// Referenced
	ID3D12CommandQueue *queue;
	// NULL if none could be made, which is not tried again
	ID3D12Fence *fence;
	UINT64 signalled;
	std::deque<Retired> retired;
	// Not referenced, each is in retired with the same value: what its
	// lists use of our resources until it is done with them, see game_uses
	std::deque<Retired> uses;
	// The value of our own fence it was last made to wait for
	UINT64 waited;
	// Whether it is one of dx12_unordered_queues, the frame until which
	// it stays one at least, and for how many frames it was made one
	// the last time, which is twice that the next time
	bool unordered;
	UINT unordered_until;
	UINT unordered_frames;
};
static std::vector<ForeignQueue*> foreign_queues;
static ForeignQueue* foreign_queue(ID3D12CommandQueue *queue);
// What was pinned by lists executed on a queue that could not be given a
// fence: nothing tells when that queue is done with it, so it is kept for
// good rather than handed out again too soon
static std::vector<IUnknown*> stranded;
static void release_retired();

static std::vector<ShimResource*> touched;

// How many unexecuted lists of the game use each D3D12 resource of ours,
// counting only what Dx12ShimUseOnGameList pinned. An execution on another
// queue than ours counts too until that queue is done with it: what we write
// after handing it the list is not after it for the GPU.
static std::unordered_map<IUnknown*, UINT> game_uses;

// With the lock held
static void end_game_use(IUnknown *object)
{
	auto found = game_uses.find(object);

	if (found != game_uses.end() && !--found->second)
		game_uses.erase(found);
}

static void end_game_uses(const std::vector<IUnknown*> *pins)
{
	for (IUnknown *object : *pins)
		end_game_use(object);
}

static bool game_list_uses(IUnknown *object)
{
	return !game_uses.empty() && game_uses.count(object);
}

static CRITICAL_SECTION shim_lock;
static INIT_ONCE shim_lock_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_shim_lock(PINIT_ONCE once, PVOID parameter, PVOID *context)
{
	// Held for a draw of the game at a time by the threads recording its
	// command lists, which is short enough to wait out without sleeping
	InitializeCriticalSectionAndSpinCount(&shim_lock, 4000);
	return TRUE;
}

void Dx12ShimLock()
{
	InitOnceExecuteOnce(&shim_lock_once, init_shim_lock, NULL, NULL);
	EnterCriticalSection(&shim_lock);
}

void Dx12ShimUnlock()
{
	LeaveCriticalSection(&shim_lock);
}

void Dx12ShimNotImplemented(const char *method, bool *reported)
{
	if (*reported)
		return;
	*reported = true;
	Dx12LogInfo("*** %s is not implemented on D3D12\n", method);
}

void Dx12LogDebugMessages()
{
	static const char *const severities[] = { "corruption", "error", "warning", "info", "message" };
	static unsigned logged;
	static bool said;
	static std::unordered_map<int, unsigned> noted;
	ID3D12InfoQueue *queue;
	D3D12_MESSAGE *message;
	std::vector<BYTE> storage;
	UINT64 count, i;
	SIZE_T length;

	if (!G->dx12_debug_layer)
		return;
	if (FAILED(shim.device->QueryInterface(IID_PPV_ARGS(&queue)))) {
		if (!said)
			Dx12DumpNote("*** dx12_debug_layer: the device has no debug layer, which comes with the Graphics Tools optional feature of Windows\n");
		said = true;
		return;
	}
	if (!said)
		Dx12DumpNote("dx12_debug_layer: the debug layer is on, its errors and warnings follow as they come\n");
	said = true;

	count = queue->GetNumStoredMessages();
	for (i = 0; i < count; i++) {
		length = 0;
		queue->GetMessage(i, NULL, &length);
		storage.resize(length);
		message = (D3D12_MESSAGE*)storage.data();
		if (FAILED(queue->GetMessage(i, message, &length)))
			continue;
		// A mistake repeated every frame would fill the disk, and one
		// kind of them is not to keep the others out
		if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
			if (noted[message->ID]++ >= 8)
				continue;
			// The log may be off where the report is on
			Dx12DumpNote("*** debug layer %s %d: %s\n", severities[min(message->Severity, 4)], message->ID, message->pDescription);
		} else if (logged++ >= 1000) {
			continue;
		}
		Dx12LogInfo("debug layer %s %d: %s\n", severities[min(message->Severity, 4)], message->ID, message->pDescription);
	}
	queue->ClearStoredMessages();
	queue->Release();
}

// -----------------------------------------------------------------------------
// Upload memory

const UINT64 UPLOAD_PAGE_SIZE = 4 << 20;
const size_t UPLOAD_PAGES_KEPT = 8;

struct UploadPage {
	ID3D12Resource *buffer;
	BYTE *cpu;
	UINT64 va;
	UINT64 used;
	UINT64 fence;
};
static std::vector<UploadPage> upload_active;
static std::vector<UploadPage> upload_idle;

static bool create_upload_buffer(UINT64 size, UploadPage *page)
{
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_DESC desc = {};
	D3D12_RANGE no_read = {};

	heap.Type = D3D12_HEAP_TYPE_UPLOAD;
	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = size;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	*page = {};
	if (FAILED(shim.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&page->buffer)))) {
		Dx12LogInfo("*** Failed to create a %llu byte upload buffer\n", size);
		return false;
	}
	if (FAILED(page->buffer->Map(0, &no_read, (void**)&page->cpu))) {
		page->buffer->Release();
		return false;
	}
	page->va = page->buffer->GetGPUVirtualAddress();
	return true;
}

BYTE* Dx12ShimUpload(UINT64 size, UINT64 alignment, ID3D12Resource **buffer, UINT64 *offset, UINT64 *va)
{
	UINT64 completed, start;
	UploadPage page;
	size_t i;

	if (size > UPLOAD_PAGE_SIZE / 4) {
		if (!create_upload_buffer(size, &page))
			return NULL;
		Dx12ShimDeferRelease(page.buffer);
		page.buffer->Release();
		if (buffer)
			*buffer = page.buffer;
		if (offset)
			*offset = 0;
		if (va)
			*va = page.va;
		return page.cpu;
	}

	start = UPLOAD_PAGE_SIZE;
	if (!upload_active.empty())
		start = (upload_active.back().used + alignment - 1) / alignment * alignment;

	if (start + size > UPLOAD_PAGE_SIZE) {
		completed = fence->GetCompletedValue();
		for (i = 0; i < upload_idle.size(); i++) {
			if (upload_idle[i].fence <= completed)
				break;
		}
		if (i < upload_idle.size()) {
			page = upload_idle[i];
			upload_idle.erase(upload_idle.begin() + i);
		} else if (!create_upload_buffer(UPLOAD_PAGE_SIZE, &page)) {
			return NULL;
		} else {
			InterlockedIncrement64(&dx12_stats.upload_pages_created);
		}
		page.used = 0;
		upload_active.push_back(page);
		start = 0;
	}

	UploadPage &active = upload_active.back();
	active.used = start + size;
	if (buffer)
		*buffer = active.buffer;
	if (offset)
		*offset = start;
	if (va)
		*va = active.va + start;
	return active.cpu + start;
}

static void retire_upload_pages()
{
	UINT64 completed = fence->GetCompletedValue();
	size_t i, keep = 0;

	// Keep filling the last page where this submission left off, or every
	// submission with something small to upload would take a page of its
	// own. It gets the fence value of the last submission that used it
	// when it is retired.
	if (!upload_active.empty() && upload_active.back().used < UPLOAD_PAGE_SIZE)
		keep = 1;

	for (i = 0; i + keep < upload_active.size(); i++) {
		upload_active[i].fence = fence_signalled;
		upload_idle.push_back(upload_active[i]);
	}
	upload_active.erase(upload_active.begin(), upload_active.end() - keep);

	for (i = upload_idle.size(); i-- > 0 && upload_idle.size() > UPLOAD_PAGES_KEPT;) {
		if (upload_idle[i].fence > completed)
			continue;
		upload_idle[i].buffer->Release();
		upload_idle.erase(upload_idle.begin() + i);
		InterlockedIncrement64(&dx12_stats.upload_pages_released);
	}
}

// -----------------------------------------------------------------------------
// Descriptors shaders can use

const UINT VIEW_PAGE_SIZE = 4096;
const UINT SAMPLER_PAGE_SIZE = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE;

struct DescriptorPage {
	ID3D12DescriptorHeap *heap;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	UINT used;
	UINT64 fence;
};

struct DescriptorPages {
	D3D12_DESCRIPTOR_HEAP_TYPE type;
	UINT size;
	UINT increment;
	// heap is NULL until the first allocation
	DescriptorPage current;
	std::vector<DescriptorPage> idle;
};
static DescriptorPages view_pages = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, VIEW_PAGE_SIZE };
static DescriptorPages sampler_pages = { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SAMPLER_PAGE_SIZE };

static std::map<std::vector<SIZE_T>, UINT> sampler_tables;
static ID3D12DescriptorHeap *bound_view_heap;
static ID3D12DescriptorHeap *bound_sampler_heap;

static bool next_descriptor_page(DescriptorPages *pages)
{
	UINT64 completed = fence->GetCompletedValue();
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	DescriptorPage page = {};
	size_t i;

	if (pages->current.heap) {
		pages->current.fence = Dx12ShimPendingFence();
		pages->idle.push_back(pages->current);
		pages->current = {};
	}

	for (i = 0; i < pages->idle.size(); i++) {
		if (pages->idle[i].fence <= completed) {
			pages->current = pages->idle[i];
			pages->current.used = 0;
			pages->idle.erase(pages->idle.begin() + i);
			return true;
		}
	}

	desc.Type = pages->type;
	desc.NumDescriptors = pages->size;
	desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(shim.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&page.heap)))) {
		Dx12LogInfo("*** Failed to create a shader visible descriptor heap of type %d\n", pages->type);
		return false;
	}
	page.cpu = page.heap->GetCPUDescriptorHandleForHeapStart();
	page.gpu = page.heap->GetGPUDescriptorHandleForHeapStart();
	pages->current = page;
	return true;
}

static bool reserve_descriptors(DescriptorPages *pages, UINT count)
{
	if (pages->current.heap && pages->current.used + count <= pages->size)
		return true;
	return count <= pages->size && next_descriptor_page(pages);
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimAllocViews(UINT count, D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
	D3D12_CPU_DESCRIPTOR_HANDLE cpu = {};

	*gpu = {};
	if (!reserve_descriptors(&view_pages, count))
		return cpu;

	cpu.ptr = view_pages.current.cpu.ptr + (SIZE_T)view_pages.current.used * view_pages.increment;
	gpu->ptr = view_pages.current.gpu.ptr + (UINT64)view_pages.current.used * view_pages.increment;
	view_pages.current.used += count;
	return cpu;
}

void Dx12ShimReserveSamplers(UINT count)
{
	ID3D12DescriptorHeap *heap = sampler_pages.current.heap;

	reserve_descriptors(&sampler_pages, count);
	if (sampler_pages.current.heap != heap)
		sampler_tables.clear();
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12ShimSamplerTable(const D3D12_CPU_DESCRIPTOR_HANDLE *samplers, UINT count)
{
	DescriptorPage *page = &sampler_pages.current;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu = {};
	std::vector<SIZE_T> key(count);
	UINT i, start;

	if (!page->heap || page->used + count > sampler_pages.size)
		return gpu;

	for (i = 0; i < count; i++)
		key[i] = samplers[i].ptr;

	auto found = sampler_tables.find(key);
	if (found != sampler_tables.end()) {
		start = found->second;
	} else {
		start = page->used;
		page->used += count;
		cpu.ptr = page->cpu.ptr + (SIZE_T)start * sampler_pages.increment;
		shim.device->CopyDescriptors(1, &cpu, &count, count, samplers, NULL, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
		sampler_tables[key] = start;
	}

	gpu.ptr = page->gpu.ptr + (UINT64)start * sampler_pages.increment;
	return gpu;
}

void Dx12ShimBindHeaps()
{
	ID3D12DescriptorHeap *heaps[2];
	UINT count = 0;

	if (view_pages.current.heap == bound_view_heap && sampler_pages.current.heap == bound_sampler_heap)
		return;

	bound_view_heap = view_pages.current.heap;
	bound_sampler_heap = sampler_pages.current.heap;
	if (bound_view_heap)
		heaps[count++] = bound_view_heap;
	if (bound_sampler_heap)
		heaps[count++] = bound_sampler_heap;
	if (count)
		Dx12ShimList()->SetDescriptorHeaps(count, heaps);
}

// -----------------------------------------------------------------------------
// Descriptors of views and samplers

struct DescriptorPool {
	UINT chunk;
	UINT increment;
	std::vector<ID3D12DescriptorHeap*> heaps;
	std::vector<SIZE_T> free;
};
static DescriptorPool pools[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES] = { { 4096 }, { 256 }, { 256 }, { 64 } };

D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimAllocDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	DescriptorPool *pool = &pools[type];
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
	ID3D12DescriptorHeap *heap;
	SIZE_T start;
	UINT i;

	if (pool->free.empty()) {
		desc.Type = type;
		desc.NumDescriptors = pool->chunk;
		if (FAILED(shim.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) {
			Dx12LogInfo("*** Failed to create a descriptor heap of type %d\n", type);
			return handle;
		}
		pool->heaps.push_back(heap);
		start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
		for (i = pool->chunk; i-- > 0;)
			pool->free.push_back(start + (SIZE_T)i * pool->increment);
	}

	handle.ptr = pool->free.back();
	pool->free.pop_back();
	return handle;
}

void Dx12ShimFreeDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE type, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	if (!handle.ptr)
		return;

	pools[type].free.push_back(handle.ptr);
	// The next sampler to get this descriptor must not be taken for the
	// one that had it
	if (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
		sampler_tables.clear();
}

// -----------------------------------------------------------------------------
// Resource states

const D3D12_RESOURCE_STATES READ_STATES = D3D12_RESOURCE_STATE_GENERIC_READ
	| D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_RESOLVE_SOURCE;

static bool read_only(D3D12_RESOURCE_STATES state)
{
	return state && !(state & ~READ_STATES);
}

void Dx12ShimTransition(ShimResource *resource, D3D12_RESOURCE_STATES state)
{
	D3D12_RESOURCE_BARRIER barrier = {};

	if (!resource->resource)
		return;

	if (resource->write_pending)
		ShimFlushWrite(resource);

	// What is about to be written is in use by lists of the game that
	// were recorded before and will be executed after: they keep it as it
	// is, see Bridge.h
	if ((state & ~READ_STATES) && !resource->foreign && game_list_uses(resource->resource))
		ShimRenameResource(resource);
	if (state & ~READ_STATES)
		resource->version++;

	if (!resource->foreign && resource->touched_epoch != dx12_shim_epoch) {
		resource->touched_epoch = dx12_shim_epoch;
		touched.push_back(resource);
	}

	if (resource->state == state) {
		if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = resource->resource;
			Dx12ShimList()->ResourceBarrier(1, &barrier);
		}
		return;
	}

	if (read_only(resource->state) && read_only(state)) {
		if ((resource->state & state) == state)
			return;
		state |= resource->state;
	}

	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource->resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = resource->state;
	barrier.Transition.StateAfter = state;
	Dx12ShimList()->ResourceBarrier(1, &barrier);
	resource->state = state;
}

void Dx12ShimForgetResource(ShimResource *resource)
{
	size_t i;

	for (i = 0; i < touched.size(); i++) {
		if (touched[i] == resource) {
			touched.erase(touched.begin() + i);
			return;
		}
	}
}

// -----------------------------------------------------------------------------
// Game lists

const UINT64 GAME_UPLOAD_BLOCK_SIZE = 1 << 20;

// For how long the lists of a queue go without the mods the first time it
// could not be made to wait for them
const UINT UNORDERED_FRAMES = 600;

const size_t GAME_UPLOAD_BLOCKS_KEPT = 8;

// The upload buffer being handed out for game lists. Those that are full are
// kept alive by the lists and buffers using them, and a few of them by a
// reference of ours as well: once that is the only one left, the lists have
// been executed and the buffers have their contents elsewhere, and the block
// is handed out again.
static UploadPage game_upload;
static std::vector<UploadPage> game_upload_full;

static bool next_game_upload(UploadPage *page)
{
	size_t i;

	for (i = 0; i < game_upload_full.size(); i++) {
		game_upload_full[i].buffer->AddRef();
		if (game_upload_full[i].buffer->Release() != 1)
			continue;
		*page = game_upload_full[i];
		page->used = 0;
		game_upload_full.erase(game_upload_full.begin() + i);
		return true;
	}
	return create_upload_buffer(GAME_UPLOAD_BLOCK_SIZE, page);
}

bool Dx12ShimPin(Dx12GraphicsCommandList *list, IUnknown *object)
{
	IUnknown **pinned = &list->mPinned[((UINT64)(uintptr_t)object * 0x9e3779b97f4a7c15ull) >> 59];

	// What was done to the object for the list is in the submission being
	// recorded, if not in one before it
	list->mShimFence = Dx12ShimPendingFence();
	if (*pinned == object)
		return false;
	*pinned = object;
	object->AddRef();
	list->mPins.push_back(object);
	return true;
}

void Dx12ShimPinsExecuted(ID3D12CommandQueue *queue, std::vector<IUnknown*> *pins, std::vector<IUnknown*> *first)
{
	Dx12ShimLock();
	if (dx12_shim && queue == shim.queue) {
		end_game_uses(first);
		// What another thread recorded since the queue proxy had our
		// list executed was promised the value signalled next
		Dx12ShimSubmit();
		shim.queue->Signal(fence, ++fence_signalled);
		Dx12QueueSignalled(shim.queue, fence, fence_signalled);
		InterlockedIncrement64(&dx12_stats.pin_signals);
		for (IUnknown *object : *pins)
			retired.push_back({ object, fence_signalled });
	} else {
		ForeignQueue *foreign = foreign_queue(queue);

		InterlockedIncrement64(&dx12_stats.pin_lates);
		if (foreign->fence) {
			queue->Signal(foreign->fence, ++foreign->signalled);
			for (IUnknown *object : *pins) {
				game_uses[object]++;
				foreign->uses.push_back({ object, foreign->signalled });
				foreign->retired.push_back({ object, foreign->signalled });
			}
			end_game_uses(first);
		} else {
			// Never is not an option for these: every write would
			// take another resource, kept for good as well
			end_game_uses(first);
			stranded.insert(stranded.end(), pins->begin(), pins->end());
		}
	}
	pins->clear();
	Dx12ShimUnlock();
}

ID3D12CommandQueue *volatile dx12_unordered_queues[4];

// With the lock held
static void set_queue_unordered(ForeignQueue *foreign, bool unordered)
{
	ID3D12CommandQueue *wanted = unordered ? NULL : foreign->queue;

	for (ID3D12CommandQueue *volatile &slot : dx12_unordered_queues) {
		if (slot != wanted)
			continue;
		slot = unordered ? foreign->queue : NULL;
		foreign->unordered = unordered;
		// What the lists had worked out for their draws went by this
		dx12_draw_generation++;
		if (unordered) {
			Dx12LogInfo("*** ID3D12CommandQueue %p cannot be made to wait for what the mods have done for its lists, "
				"they are left out of those for %u frames at least\n", foreign->queue, foreign->unordered_frames);
		} else {
			Dx12LogInfo("ID3D12CommandQueue %p can be made to wait for the mods again\n", foreign->queue);
		}
		return;
	}
}

void Dx12ShimQueueWaits(ID3D12CommandQueue *queue, UINT64 needed, bool uses_mods)
{
	ForeignQueue *foreign;
	bool ordered = true;

	Dx12ShimLock();
	if (dx12_shim && queue != shim.queue) {
		foreign = foreign_queue(queue);
		// Nothing was recorded since for a value not signalled yet
		needed = needed ? min(needed, fence_signalled) : fence_signalled;
		if (foreign->waited < needed && fence->GetCompletedValue() < needed) {
			// Our queue may be behind a wait for what this one is
			// yet to be handed, and would then never get to it
			if (!Dx12QueueCanWait(fence, needed)) {
				ordered = false;
			} else if (uses_mods) {
				Dx12QueueWaits(queue, fence, needed);
				if (SUCCEEDED(queue->Wait(fence, needed))) {
					foreign->waited = needed;
					InterlockedIncrement64(&dx12_stats.queue_waits);
				} else {
					ordered = false;
				}
			}
		}

		if (!ordered) {
			if (uses_mods)
				InterlockedIncrement64(&dx12_stats.queue_waits_skipped);
			if (!foreign->unordered) {
				foreign->unordered_frames = foreign->unordered_frames ? min(foreign->unordered_frames * 2, 1u << 20) : UNORDERED_FRAMES;
				set_queue_unordered(foreign, true);
			}
			foreign->unordered_until = dx12_frame + foreign->unordered_frames;
		} else if (foreign->unordered && (int)(dx12_frame - foreign->unordered_until) >= 0) {
			set_queue_unordered(foreign, false);
		}
	}
	Dx12ShimUnlock();
}

void Dx12ShimPinsInherited(Dx12GraphicsCommandList *list, Dx12GraphicsCommandList *bundle)
{
	Dx12ShimLock();
	for (IUnknown *object : bundle->mPins) {
		ID3D12Resource *resource;

		if (!Dx12ShimPin(list, object))
			continue;
		// A resource is in use by the list now, see game_uses. The
		// bundle's own uses stay until it is reset: it may be
		// executed again by another list, with what it was recorded
		// with.
		if (SUCCEEDED(object->QueryInterface(IID_PPV_ARGS(&resource)))) {
			game_uses[object]++;
			resource->Release();
		}
	}
	Dx12ShimUnlock();
}

void Dx12ShimPinsAbandoned(std::vector<IUnknown*> *pins, bool executed)
{
	if (!executed) {
		Dx12ShimLock();
		end_game_uses(pins);
		Dx12ShimUnlock();
	}
	for (IUnknown *object : *pins)
		object->Release();
	pins->clear();
}

void Dx12ShimFramePresented()
{
	release_retired();
}

bool Dx12ShimUseOnGameList(ShimResource *resource, Dx12GraphicsCommandList *list)
{
	if (!resource->resource || resource->foreign)
		return false;

	Dx12ShimTransition(resource, D3D12_RESOURCE_STATE_COMMON);
	if (Dx12ShimPin(list, resource->resource))
		game_uses[resource->resource]++;
	return true;
}

// Copies the shadow of a cpu_only buffer to where the game's lists can read
// it, bytes of it with the padding. alone is for a buffer of its own, where
// the contents start at the start of a resource.
static bool upload_for_game_lists(ShimResource *resource, UINT64 bytes, bool alone)
{
	UploadPage page;
	UINT64 start;
	BYTE *cpu;

	if (alone || bytes > GAME_UPLOAD_BLOCK_SIZE / 4) {
		if (!create_upload_buffer(bytes, &page))
			return false;
		start = 0;
	} else {
		if (!game_upload.buffer || game_upload.used + bytes > GAME_UPLOAD_BLOCK_SIZE) {
			if (!next_game_upload(&page))
				return false;
			if (game_upload.buffer && game_upload_full.size() < GAME_UPLOAD_BLOCKS_KEPT)
				game_upload_full.push_back(game_upload);
			else if (game_upload.buffer)
				game_upload.buffer->Release();
			game_upload = page;
		}
		page = game_upload;
		page.buffer->AddRef();
		start = game_upload.used;
		game_upload.used += bytes;
	}

	cpu = page.cpu + start;
	ShimReadShadow(resource);
	memcpy(cpu, resource->shadow.data(), resource->shadow.size());
	memset(cpu + resource->shadow.size(), 0, bytes - resource->shadow.size());

	if (resource->game_upload)
		resource->game_upload->Release();
	resource->game_upload = page.buffer;
	resource->game_upload_va = page.va + start;
	resource->game_upload_version = resource->version;
	return true;
}

UINT64 Dx12ShimGameBufferAddress(ID3D11Buffer *buffer, Dx12GraphicsCommandList *list, UINT *size)
{
	ShimResource *resource = ShimResourceOf(buffer);
	UINT64 bytes;

	if (!resource->cpu_only) {
		if (!Dx12ShimUseOnGameList(resource, list))
			return 0;
		*size = (UINT)min(resource->desc.Width, (UINT64)UINT_MAX);
		return resource->resource->GetGPUVirtualAddress();
	}

	bytes = (resource->shadow.size() + 255) & ~(UINT64)255;
	*size = (UINT)bytes;
	if (!bytes)
		return 0;

	if ((!resource->game_upload || resource->game_upload_version != resource->version)
	    && !upload_for_game_lists(resource, bytes, false))
		return 0;

	Dx12ShimPin(list, resource->game_upload);
	return resource->game_upload_va;
}

bool Dx12ShimGameBufferView(ID3D11Buffer *buffer, Dx12GraphicsCommandList *list, UINT stride, ID3D12Resource **view_resource,
		UINT64 *offset, UINT *size)
{
	ShimResource *resource = ShimResourceOf(buffer);
	D3D11_BUFFER_DESC desc;
	UINT64 bytes;

	if (!stride)
		return false;

	if (!resource->cpu_only) {
		if (!Dx12ShimUseOnGameList(resource, list))
			return false;
		buffer->GetDesc(&desc);
		*view_resource = resource->resource;
		*offset = 0;
		*size = desc.ByteWidth;
		return true;
	}

	bytes = (resource->shadow.size() + 255) & ~(UINT64)255;
	if (!bytes)
		return false;

	if (!resource->game_upload || resource->game_upload_version != resource->version
	    || (resource->game_upload_va - resource->game_upload->GetGPUVirtualAddress()) % stride) {
		if (!upload_for_game_lists(resource, bytes, true))
			return false;
	}

	Dx12ShimPin(list, resource->game_upload);
	*view_resource = resource->game_upload;
	*offset = resource->game_upload_va - resource->game_upload->GetGPUVirtualAddress();
	*size = (UINT)resource->shadow.size();
	return true;
}

// -----------------------------------------------------------------------------
// Command list and fence

void Dx12ShimDeferRelease(IUnknown *object)
{
	object->AddRef();
	retired.push_back({ object, Dx12ShimPendingFence() });
}

static void release_retired()
{
	UINT64 completed = fence->GetCompletedValue();

	while (!retired.empty() && retired.front().fence <= completed) {
		retired.front().object->Release();
		retired.pop_front();
	}

	for (ForeignQueue *foreign : foreign_queues) {
		if (!foreign->fence || foreign->retired.empty())
			continue;
		completed = foreign->fence->GetCompletedValue();
		while (!foreign->uses.empty() && foreign->uses.front().fence <= completed) {
			end_game_use(foreign->uses.front().object);
			foreign->uses.pop_front();
		}
		while (!foreign->retired.empty() && foreign->retired.front().fence <= completed) {
			foreign->retired.front().object->Release();
			foreign->retired.pop_front();
		}
	}
}

static ForeignQueue* foreign_queue(ID3D12CommandQueue *queue)
{
	ForeignQueue *foreign;
	ID3D12Device *device;
	ID3D12Fence *queue_fence = NULL;

	for (ForeignQueue *known : foreign_queues) {
		if (known->queue == queue)
			return known;
	}

	if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue_fence))))
			queue_fence = NULL;
		device->Release();
	}
	if (queue_fence)
		Dx12LogInfo("Lists executed on ID3D12CommandQueue %p use what mods gave them, which a fence of ours on it now guards\n", queue);
	else
		Dx12LogInfo("*** Failed to create a fence for ID3D12CommandQueue %p, what its lists use of the mods is kept for good\n", queue);

	foreign = new ForeignQueue();
	foreign->queue = queue;
	foreign->fence = queue_fence;
	foreign->signalled = 0;
	foreign->waited = 0;
	foreign->unordered = false;
	foreign->unordered_until = 0;
	foreign->unordered_frames = 0;
	queue->AddRef();
	foreign_queues.push_back(foreign);
	return foreign;
}

UINT64 Dx12ShimPendingFence()
{
	return fence_signalled + 1;
}

bool Dx12ShimFenceReached(UINT64 value)
{
	return fence->GetCompletedValue() >= value;
}

ID3D12GraphicsCommandList* Dx12ShimList()
{
	UINT64 completed;
	Allocator allocator;
	size_t i;

	list_used = true;
	if (list_open)
		return shim_list;

	completed = fence->GetCompletedValue();
	for (i = 0; i < allocators.size(); i++) {
		if (allocators[i].fence <= completed)
			break;
	}
	if (i == allocators.size()) {
		allocator.fence = 0;
		// Recording to the allocator of an executing list is not an
		// option, so there is nothing to fall back to
		if (FAILED(shim.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.allocator)))) {
			Dx12LogInfo("*** Failed to create a command allocator\n");
			DoubleBeepExit();
		}
		allocators.push_back(allocator);
	}

	current_allocator = i;
	allocators[i].allocator->Reset();
	shim_list->Reset(allocators[i].allocator, NULL);
	list_open = true;
	bound_view_heap = NULL;
	bound_sampler_heap = NULL;
	return shim_list;
}

void Dx12ShimSubmit()
{
	ID3D12CommandList *lists[1];

	if (!list_used) {
		// Nothing recorded since the last submission can be using
		// what was retired since
		if (!retired.empty() && fence->GetCompletedValue() >= fence_signalled) {
			for (Retired &entry : retired)
				entry.object->Release();
			retired.clear();
		}
		release_retired();
		return;
	}

	// By index: the first transition of a resource in a submission is
	// what adds it, which none of these is
	for (size_t i = 0; i < touched.size(); i++)
		Dx12ShimTransition(touched[i], D3D12_RESOURCE_STATE_COMMON);
	touched.clear();

	shim_list->Close();
	lists[0] = shim_list;
	shim.queue->ExecuteCommandLists(1, lists);
	shim.queue->Signal(fence, ++fence_signalled);
	Dx12QueueSignalled(shim.queue, fence, fence_signalled);
	allocators[current_allocator].fence = fence_signalled;
	list_open = false;
	list_used = false;
	dx12_shim_epoch++;

	retire_upload_pages();
	release_retired();
}

void Dx12ShimFlush()
{
	// Whatever the lists about to be executed depend on was recorded
	// before they were closed, by the threads the game had them from
	if (!list_used)
		return;

	Dx12ShimLock();
	if (dx12_shim)
		Dx12ShimSubmit();
	Dx12ShimUnlock();
}

void Dx12ShimSubmitAndWait()
{
	Dx12ShimSubmit();

	if (fence->GetCompletedValue() >= fence_signalled)
		return;

	if (FAILED(fence->SetEventOnCompletion(fence_signalled, fence_event))
	    || WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0)
		Dx12LogInfo("*** Gave up waiting for the GPU to reach fence value %llu\n", fence_signalled);

	release_retired();
}

ID3D12Fence* Dx12ShimFence(UINT64 *signalled)
{
	*signalled = fence_signalled;
	return fence;
}

void Dx12ShimSetQueue(Dx12CommandQueue *queue)
{
	if (shim.queue == queue->mOrig)
		return;

	// The fence must have been signalled by the old queue before the new
	// one may signal a higher value
	Dx12ShimSubmitAndWait();

	Dx12LogInfo("Shim commands now go to ID3D12CommandQueue %p\n", queue->mOrig);
	shim.queue->Release();
	shim.queue = queue->mOrig;
	shim.queue->AddRef();
}

bool Dx12ShimCreate(Dx12Device *device, Dx12CommandQueue *queue)
{
	static bool failed;
	Allocator allocator = {};
	UINT type;

	if (dx12_shim)
		return true;
	if (failed)
		return false;
	failed = true;

	// A device wrapped before the game's D3D11 swap chain presented
	if (Dx12IsD3D11Game()) {
		Dx12LogInfo("The game presents with D3D11, leaving the D3D12 swap chain alone\n");
		return false;
	}

	// Our objects are created on the original device: they are not the
	// game's, so nothing about them belongs in what the proxies record.
	shim.device = device->mOrig;
	shim.queue = queue->mOrig;

	if (shim.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
		Dx12LogInfo("*** The swap chain's queue is not a direct queue, mods will not be applied\n");
		return false;
	}

	if (FAILED(shim.device->CreateFence(0, G->vulkan ? D3D12_FENCE_FLAG_SHARED : D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))
	    || FAILED(shim.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator.allocator)))
	    || FAILED(shim.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.allocator, NULL, IID_PPV_ARGS(&shim_list)))) {
		Dx12LogInfo("*** Failed to create the command list for mods, they will not be applied\n");
		return false;
	}
	fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	allocators.push_back(allocator);
	list_open = true;

	shim.device->AddRef();
	shim.queue->AddRef();

	for (type = 0; type < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; type++)
		pools[type].increment = shim.device->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)type);
	view_pages.increment = pools[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV].increment;
	sampler_pages.increment = pools[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER].increment;
	dx12_shim_view_increment = view_pages.increment;

	// Everything from here on finds the device through this
	dx12_shim = &shim;
	if (!Dx12ShimPipelineInit()) {
		dx12_shim = NULL;
		return false;
	}

	shim.shim_device = new ShimDevice();
	shim.shim_context = new ShimContext();

	shim.hacker_device = new HackerDevice(shim.shim_device, shim.shim_context);
	// Not HackerContextFactory: frame analysis reads back what the game
	// renders in ways the shim has no answer to yet
	shim.hacker_context = new HackerContext(shim.shim_device, shim.shim_context);
	shim.hacker_device->SetHackerContext(shim.hacker_context);
	// The constructor leaves it uninitialised, and the engine takes
	// anything but NULL for a D3D11 swap chain
	shim.hacker_device->SetHackerSwapChain(NULL);
	shim.hacker_context->SetHackerDevice(shim.hacker_device);

	shim.hacker_device->Create3DMigotoResources();
	shim.hacker_context->Bind3DMigotoResources();
	if (!G->constants_run)
		shim.hacker_context->InitIniParams();

	Dx12LogInfo("HackerDevice %p and HackerContext %p created on the D3D11 shim of ID3D12Device %p, commands go to ID3D12CommandQueue %p\n",
		shim.hacker_device, shim.hacker_context, shim.device, shim.queue);
	shim_created = true;

	// The shader overrides were read before the game was known to be
	// D3D12, and are not read again at the first present when
	// config_initialization_delay says not to
	if (G->gConfigInitialized && !G->gReloadConfigPending && IniDx12HashesProvisional()) {
		Dx12LogInfo("Reloading the configuration for the hash_dx12 of the shader overrides\n");
		G->gReloadConfigPending = true;
	}
	return true;
}
