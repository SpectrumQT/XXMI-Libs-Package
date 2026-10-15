#include "DrawPath.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Bridge.h"
#include "BufferHash.h"
#include "Dxc.h"
#include "FrameDump.h"
#include "HashDump.h"
#include "ShimContext.h"
#include "ShimDevice.h"
#include "ShimObjects.h"
#include "TextureHash.h"
#include "util.h"

#include "../globals.h"
#include "../HackerContext.h"
#include "../Hunting.h"
#include "../Overlay.h"
#include "../ResourceHash.h"
#include "../vk/VkScope.h"

volatile bool dx12_draw_path_active;
volatile UINT dx12_draw_generation = 1;
bool dx12_mods_disabled;

// What the engine did with the game's draws since the last report
static struct {
	unsigned draws, skipped, mod_draws, mod_dispatches;
	unsigned vertex_buffers_matched, constant_buffers_matched;
	unsigned reaches_known, reaches_failed;
	unsigned buffers_remembered, buffers_looked_up;
	unsigned lookups_missing, lookups_unsettled, lookups_unstamped, lookups_new;
	unsigned skin_dispatches, skin_unconfirmed, skin_replaced, skin_swapped, skin_failed;
	// Tables copied for what mods replace: in the game's reserve, and in
	// the side heap for a mod's changes and for the game's own tables
	unsigned table_copies, side_copies, side_base_copies, table_descriptors, side_switches;
	unsigned table_unbounded, table_no_room;
	unsigned skin_filter_misses;
	unsigned buffers_unpacked, skin_stood_in, skin_aliased;
	// Draws of mods given a buffer the mods wrote in place of the game's
	unsigned written_swapped;
	// Draws of mods with shaders of their own: made with those, and made
	// with the game's for want of a pipeline with them
	unsigned custom_draws, custom_failed;
	// Only counted with dx12_dump_hashes, in QueryPerformanceCounter ticks
	UINT64 wait_ticks, before_ticks, buffer_ticks, draw_ticks, after_ticks;
} stats;

static UINT64 ticks()
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

// -----------------------------------------------------------------------------
// Which draws to look at

// The hashes of the [ShaderOverride] sections, for the threads recording to
// look up without the lock of Bridge.h. Replaced as a whole when the
// configuration changes, which waits for whoever is looking at the old one.
typedef std::unordered_set<UINT64> HashSet;
static HashSet *wanted_hashes;
static SRWLOCK wanted_lock = SRWLOCK_INIT;
static unsigned seen_override_generation;
// With hunting enabled every draw is the engine's business
static volatile bool want_all;

bool Dx12DrawPipelineWanted(Dx12GraphicsCommandList *list)
{
	ID3D12PipelineState *state = list->mState.pipeline;
	UINT generation = dx12_draw_generation;
	Dx12GraphicsCommandList::PipelineMemo *memo;
	const Dx12PipelineInfo *pipeline;
	bool wanted = false;

	pipeline = Dx12ListPipeline(list);
	memo = list->MemoOf(state);
	if (memo->pipeline == state && memo->wanted_generation == generation) {
		wanted = memo->wanted;
	} else if (pipeline) {
		if (want_all) {
			wanted = true;
		} else {
			AcquireSRWLockShared(&wanted_lock);
			if (wanted_hashes) {
				for (const Dx12ShaderInfo *shader : pipeline->shaders)
					wanted |= shader && wanted_hashes->count(shader->hash);
			}
			ReleaseSRWLockShared(&wanted_lock);
		}
	}

	// Nothing of the mods goes on a list whose queue cannot be made to wait
	// for it, see Dx12ShimQueueWaits
	if (wanted && Dx12ShimQueueUnordered(list->mQueue))
		wanted = false;

	if (memo->pipeline == state) {
		memo->wanted_generation = generation;
		memo->wanted = wanted;
	}
	list->mWantedPipeline = state;
	list->mWantedGeneration = generation;
	list->mWanted = wanted;
	return wanted;
}

// -----------------------------------------------------------------------------
// The game's resources as the engine sees them

enum PublishedKind {
	PUBLISHED_RESOURCE,
	PUBLISHED_VERTEX_BUFFER,
	PUBLISHED_INDEX_BUFFER,
	// A vertex buffer as a dispatch that skins it reads it, see SkinDispatch.
	// Apart from the same buffer bound for a draw, which has an index
	// buffer to say how much of it counts.
	PUBLISHED_SKIN_BUFFER,
};

struct PublishedKey {
	ID3D12Resource *resource;
	UINT64 offset;
	UINT kind;

	bool operator==(const PublishedKey &other) const
	{
		return resource == other.resource && offset == other.offset && kind == other.kind;
	}
};

struct PublishedKeyHash {
	size_t operator()(const PublishedKey &key) const
	{
		return (size_t)(((UINT64)(uintptr_t)key.resource ^ (key.offset * 0x9e3779b97f4a7c15ull) ^ key.kind) * 0xff51afd7ed558ccdull >> 17);
	}
};

// One per resource of the game the engine asked about, or per range of one
// for the buffers a game carves its vertex and index buffers out of. Kept, so
// that the engine sees the same object for the same resource every time: it
// remembers by address which ones matched nothing.
struct Published {
	// Referenced. The views have a reference on the resource themselves.
	ID3D11Resource *resource;
	ID3D11ShaderResourceView *srv;
	ID3D11UnorderedAccessView *uav;

	// What the object and its hash were made from, to notice when the
	// address has passed on to something else
	D3D12_RESOURCE_DESC desc;
	UINT64 bytes;
	bool has_data_hash;
	uint32_t data_hash;
	// Vertex buffers: the part its index buffer reaches, see VertexReach
	UINT64 reach_bytes;
	uint32_t reach_hash;
	UINT config;
	// Buffers: counts the times any of the above changed
	UINT version;

	// Referenced. A buffer of ours that the compute shaders of mods write
	// in place of this one, see "Buffers of the game that mods write".
	ID3D11Buffer *standin;
	ID3D11UnorderedAccessView *standin_uav;
	UINT standin_frame;
	bool standin_drawn;
	// Holds what the mods wrote for what the game has in its buffer now,
	// and the scope a dispatch of a mod last wrote it in
	bool standin_valid;
	UINT64 standin_scope;
	// Dx12ResourceInfo::serial of the buffer of the game it was written for
	UINT64 standin_serial;

	UINT frame;
};

static std::unordered_map<PublishedKey, Published, PublishedKeyHash> published;
// Counts configurations, as which of the hashes a resource could have had
// under D3D11 is the one to use depends on what the mods ask for
// Read by the threads recording without the lock, see draw_is_boring
static volatile UINT config_generation = 1;

static void set_resource_hash(ID3D11Resource *resource, D3D11_RESOURCE_DIMENSION type, uint32_t hash, uint32_t data_hash,
		const D3D11_TEXTURE2D_DESC *desc)
{
	EnterCriticalSectionPretty(&G->mResourcesLock);
	ResourceHandleInfo *info = &G->mResources[resource];
	info->type = type;
	info->hash = hash;
	info->orig_hash = hash;
	info->data_hash = data_hash;
	info->texture_override_candidates_valid = false;
	if (desc)
		info->desc2D = *desc;
	LeaveCriticalSection(&G->mResourcesLock);

	ForgetTextureOverrideMiss(resource);
}

volatile UINT dx12_standins_live;

static void release_standin(Published *entry)
{
	if (entry->standin_uav)
		entry->standin_uav->Release();
	if (entry->standin) {
		entry->standin->Release();
		dx12_standins_live--;
	}
	entry->standin_uav = NULL;
	entry->standin = NULL;
	entry->standin_valid = false;
}

static void release_published(Published *entry)
{
	release_standin(entry);
	if (entry->srv)
		entry->srv->Release();
	if (entry->uav)
		entry->uav->Release();
	if (entry->resource)
		entry->resource->Release();
	entry->srv = NULL;
	entry->uav = NULL;
	entry->resource = NULL;
}

template <class Object>
static Object* new_game_resource(D3D11_RESOURCE_DIMENSION dimension, const D3D12_RESOURCE_DESC *desc)
{
	Object *object = new Object();

	// No resource: the game's may be gone at any time
	object->r.dimension = dimension;
	object->r.desc = *desc;
	object->r.foreign = true;
	// Has the engine's record of the object removed along with it
	new ResourceReleaseTracker(object);
	return object;
}

static UINT bind_flags(const D3D12_RESOURCE_DESC *desc)
{
	UINT flags = 0;

	if (!(desc->Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
		flags |= D3D11_BIND_SHADER_RESOURCE;
	if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
		flags |= D3D11_BIND_RENDER_TARGET;
	if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
		flags |= D3D11_BIND_DEPTH_STENCIL;
	if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
		flags |= D3D11_BIND_UNORDERED_ACCESS;
	return flags;
}

static ID3D11Resource* new_game_buffer(const D3D12_RESOURCE_DESC *desc, UINT64 bytes, UINT flags)
{
	ShimBuffer *buffer = new_game_resource<ShimBuffer>(D3D11_RESOURCE_DIMENSION_BUFFER, desc);

	buffer->mDesc = {};
	buffer->mDesc.ByteWidth = (UINT)min(bytes, (UINT64)UINT_MAX);
	buffer->mDesc.BindFlags = flags;
	return buffer;
}

static ID3D11Resource* new_game_texture(const Dx12ResourceInfo *info, D3D11_TEXTURE2D_DESC *desc2d)
{
	const D3D12_RESOURCE_DESC *desc = &info->desc;

	*desc2d = {};
	switch (desc->Dimension) {
	case D3D12_RESOURCE_DIMENSION_BUFFER:
		return new_game_buffer(desc, desc->Width, bind_flags(desc));
	case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
		ShimTexture1D *texture = new_game_resource<ShimTexture1D>(D3D11_RESOURCE_DIMENSION_TEXTURE1D, desc);
		texture->mDesc = {};
		texture->mDesc.Width = (UINT)desc->Width;
		texture->mDesc.MipLevels = desc->MipLevels;
		texture->mDesc.ArraySize = desc->DepthOrArraySize;
		texture->mDesc.Format = desc->Format;
		texture->mDesc.BindFlags = bind_flags(desc);
		return texture;
	}
	case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
		ShimTexture3D *texture = new_game_resource<ShimTexture3D>(D3D11_RESOURCE_DIMENSION_TEXTURE3D, desc);
		texture->mDesc = {};
		texture->mDesc.Width = (UINT)desc->Width;
		texture->mDesc.Height = desc->Height;
		texture->mDesc.Depth = desc->DepthOrArraySize;
		texture->mDesc.MipLevels = desc->MipLevels;
		texture->mDesc.Format = desc->Format;
		texture->mDesc.BindFlags = bind_flags(desc);
		return texture;
	}
	default: {
		ShimTexture2D *texture = new_game_resource<ShimTexture2D>(D3D11_RESOURCE_DIMENSION_TEXTURE2D, desc);
		desc2d->Width = (UINT)desc->Width;
		desc2d->Height = desc->Height;
		desc2d->MipLevels = desc->MipLevels;
		desc2d->ArraySize = desc->DepthOrArraySize;
		desc2d->Format = desc->Format;
		desc2d->SampleDesc = desc->SampleDesc;
		desc2d->BindFlags = bind_flags(desc);
		desc2d->MiscFlags = info->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
		texture->mDesc = *desc2d;
		return texture;
	}
	}
}

// D3D12 does not record everything that went into the hash of a D3D11
// resource, so there are a few it could have been. The one a mod asks for is
// the one the game's D3D11 renderer would have produced.
static uint32_t pick_texture_hash(const Dx12ResourceInfo *info)
{
	uint32_t hash, first = 0;
	int rule;

	for (rule = 0; rule < DX12_FORMAT_RULE_COUNT; rule++) {
		hash = Dx12TextureHash(info, (Dx12FormatRule)rule);
		if (G->mTextureOverrideMap.count(hash))
			return hash;
		if (!rule)
			first = hash;
	}
	return first;
}

// A buffer the game's compute shaders write is created without contents under
// D3D11, which leaves the description to be hashed, as for constant buffers
static uint32_t pick_written_buffer_hash(UINT64 bytes)
{
	static const UINT flags[] = {
		D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
		D3D11_BIND_UNORDERED_ACCESS,
	};
	D3D11_BUFFER_DESC desc = {};
	uint32_t hash, first = 0;
	size_t i;

	desc.ByteWidth = (UINT)bytes;
	desc.Usage = D3D11_USAGE_DEFAULT;
	for (i = 0; i < ARRAYSIZE(flags); i++) {
		desc.BindFlags = flags[i];
		hash = crc32c_hw(0, &desc, sizeof(desc));
		if (G->mTextureOverrideMap.count(hash))
			return hash;
		if (!i)
			first = hash;
	}
	return first;
}

// reach is the vertex buffer cut down to what its index buffer uses, if that
// is known
static uint32_t pick_buffer_hash(const Dx12BufferHash *data, const Dx12BufferHash *reach, UINT kind)
{
	static const UINT vertex_flags[] = { D3D11_BIND_VERTEX_BUFFER, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE };
	static const UINT index_flags[] = { D3D11_BIND_INDEX_BUFFER, D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE };
	const UINT *flags = kind == PUBLISHED_INDEX_BUFFER ? index_flags : vertex_flags;
	uint32_t hash, first = 0;
	size_t i;

	for (i = 0; i < ARRAYSIZE(vertex_flags); i++) {
		hash = Dx12BufferDescHash(data, flags[i]);
		if (G->mTextureOverrideMap.count(hash))
			return hash;
		if (!i)
			first = hash;
	}
	for (i = 0; reach && i < ARRAYSIZE(vertex_flags); i++) {
		hash = Dx12BufferDescHash(reach, flags[i]);
		if (G->mTextureOverrideMap.count(hash))
			return hash;
	}
	return first;
}

// -----------------------------------------------------------------------------
// A D3D12 renderer may upload more than the vertices of one mesh in a go, or
// bind more than it uploaded, where its D3D11 counterpart created a buffer of
// just those vertices. What tells where a mesh ends is its index buffer: the
// D3D11 buffer was the vertices up to the last one an index refers to.
//
// Finding that takes the contents of both buffers, which are only on the GPU
// by the time a draw pairs them up. They are copied to where they can be read
// the first time the engine asks about the vertex buffer, and until that has
// happened, a few frames later, the buffer goes by the hash of all of it.

struct ReachKey {
	ID3D12Resource *vb;
	UINT64 vb_offset;
	ID3D12Resource *ib;
	UINT64 ib_offset;
	UINT ib_bytes;
	UINT index_bytes;
	UINT stride;
	UINT padding;

	bool operator<(const ReachKey &other) const
	{
		return memcmp(this, &other, sizeof(*this)) < 0;
	}
};

enum ReachState {
	REACH_PENDING,
	REACH_KNOWN,
	REACH_FAILED,
};

struct VertexReach {
	ReachState state;
	// What was uploaded to the two ranges when this was worked out
	uint32_t vb_upload, ib_upload;
	UINT frame;

	// Pending: the index buffer followed by vb_bytes of the vertex buffer
	ID3D12Resource *readback;
	UINT64 vb_bytes;
	UINT64 fence;

	Dx12BufferHash hash;
};

static std::map<ReachKey, VertexReach> reaches;
// Counts the times entries of reaches or published were erased, after which
// what publish_buffer remembers about them is not to be looked at
static UINT buffer_cache_generation = 1;
static std::vector<ReachKey> pending_reaches;
static UINT reach_frame;
static UINT64 reach_frame_bytes;

const UINT64 REACH_MAX_BYTES = 64 << 20;
// Per frame, so that a scene coming into view does not copy all its meshes
// at once
const UINT64 REACH_FRAME_BYTES = 32 << 20;
const size_t REACH_MAX_PENDING = 32;
// Per frame as well: what was copied is gone through at present, with the
// lock held
const UINT64 REACH_FINISH_BYTES = 8 << 20;
// An upload recorded this recently may not have been executed yet
const UINT REACH_UPLOAD_AGE = 3;

// The buffers the copies were read from, kept for the next meshes while there
// are any: a scene coming into view has hundreds of them looked at
static std::vector<ID3D12Resource*> spare_readbacks;
const UINT64 REACH_READBACK_STEP = 1 << 16;
const UINT64 REACH_SPARE_BYTES = 4 << 20;
const size_t REACH_SPARES = 4;

// The barriers that let a list of ours copy from a buffer of the game and
// leave it as the game expects it. False if it is in no state to be read.
static bool reach_barrier(ID3D12Resource *resource, D3D12_RESOURCE_BARRIER *barrier, bool *needed)
{
	Dx12ResourceInfo info;

	if (!Dx12GetResourceInfo(resource, &info) || info.state == DX12_STATE_UNKNOWN)
		return false;
	if (info.state & (D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
		return false;

	// From the common state D3D12 promotes a buffer by itself
	*needed = info.state && !(info.state & D3D12_RESOURCE_STATE_COPY_SOURCE);
	*barrier = {};
	barrier->Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier->Transition.pResource = resource;
	barrier->Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier->Transition.StateBefore = (D3D12_RESOURCE_STATES)info.state;
	barrier->Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	return true;
}

static bool start_reach(const ReachKey *key, VertexReach *reach, UINT64 vb_bytes)
{
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_BARRIER barriers[2];
	D3D12_RESOURCE_DESC desc = {};
	ID3D12GraphicsCommandList *list;
	bool needed[2] = {};
	UINT count = 0, i;

	if (!reach_barrier(key->vb, &barriers[0], &needed[0]))
		return false;
	if (needed[0])
		count = 1;
	if (key->ib != key->vb) {
		if (!reach_barrier(key->ib, &barriers[count], &needed[1]))
			return false;
		if (needed[1])
			count++;
	}

	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	// In steps, for one that finish_reach kept to do for another mesh
	desc.Width = (key->ib_bytes + vb_bytes + REACH_READBACK_STEP - 1) & ~(REACH_READBACK_STEP - 1);
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	for (i = 0; i < spare_readbacks.size(); i++) {
		if (spare_readbacks[i]->GetDesc().Width < desc.Width)
			continue;
		reach->readback = spare_readbacks[i];
		spare_readbacks.erase(spare_readbacks.begin() + i);
		break;
	}
	if (!reach->readback && FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&reach->readback))))
		return false;

	list = Dx12ShimList();
	if (count)
		list->ResourceBarrier(count, barriers);
	list->CopyBufferRegion(reach->readback, 0, key->ib, key->ib_offset, key->ib_bytes);
	list->CopyBufferRegion(reach->readback, key->ib_bytes, key->vb, key->vb_offset, vb_bytes);
	for (i = 0; i < count; i++)
		std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
	if (count)
		list->ResourceBarrier(count, barriers);
	// The game may be rid of its buffers before our list is executed, if
	// it drops the list that draws with them
	Dx12ShimDeferRelease(key->ib);
	Dx12ShimDeferRelease(key->vb);

	reach->vb_bytes = vb_bytes;
	reach->fence = Dx12ShimPendingFence();
	return true;
}

// What vertex_reach went by besides its arguments
struct ReachLookup {
	// The answer is the same the next time, unless one of the buffers
	// changes or the entry of reaches is erased
	bool settled;
	VertexReach *reach;
	// How far it got with the index buffer
	bool ib_asked, ib_found, ib_hashed;
	ID3D12Resource *ib;
	UINT64 ib_offset;
	Dx12BufferHash ib_upload;
};

// What is known about a vertex buffer drawn with the index buffer of the view,
// or NULL. Sets off finding out if it is not known.
static const Dx12BufferHash* vertex_reach(ID3D12Resource *vb, UINT64 vb_offset, UINT64 vb_bytes, UINT stride,
		const Dx12BufferHash *vb_upload, const D3D12_INDEX_BUFFER_VIEW *view, ReachLookup *lookup)
{
	Dx12BufferHash &ib_upload = lookup->ib_upload;
	ReachKey key = {};

	*lookup = {};
	lookup->settled = true;
	if (!stride || !view->BufferLocation || !view->SizeInBytes || vb_bytes < stride)
		return NULL;
	if (view->Format != DXGI_FORMAT_R16_UINT && view->Format != DXGI_FORMAT_R32_UINT)
		return NULL;
	// Only for what was uploaded once and left alone: anything else is
	// not a mesh a mod could have a hash for
	lookup->ib_asked = true;
	if (!Dx12FindBuffer(view->BufferLocation, &key.ib, &key.ib_offset))
		return NULL;
	lookup->ib_found = true;
	lookup->ib = key.ib;
	lookup->ib_offset = key.ib_offset;
	if (!Dx12FindBufferHash(key.ib, key.ib_offset, &ib_upload))
		return NULL;
	lookup->ib_hashed = true;

	key.vb = vb;
	key.vb_offset = vb_offset;
	key.ib_bytes = (UINT)min((UINT64)view->SizeInBytes, ib_upload.bytes);
	key.index_bytes = view->Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
	key.stride = stride;

	auto found = reaches.find(key);
	if (found != reaches.end()) {
		VertexReach *reach = &found->second;

		reach->frame = dx12_frame;
		if (reach->state == REACH_PENDING) {
			lookup->settled = false;
			return NULL;
		}
		if (reach->vb_upload == vb_upload->data_hash && reach->ib_upload == ib_upload.data_hash) {
			lookup->reach = reach;
			return reach->state == REACH_KNOWN ? &reach->hash : NULL;
		}
		reaches.erase(found);
		buffer_cache_generation++;
	}

	// From here on it depends on when it is asked
	lookup->settled = false;
	if (dx12_frame - vb_upload->frame < REACH_UPLOAD_AGE || dx12_frame - ib_upload.frame < REACH_UPLOAD_AGE)
		return NULL;
	vb_bytes = min(vb_bytes, vb_upload->bytes);
	if (key.ib_bytes + vb_bytes > REACH_MAX_BYTES)
		return NULL;
	if (reach_frame != dx12_frame) {
		reach_frame = dx12_frame;
		reach_frame_bytes = 0;
	}
	if (pending_reaches.size() >= REACH_MAX_PENDING)
		return NULL;
	// The first of a frame goes ahead whatever its size, or a mesh larger
	// than the budget would never be looked at
	if (reach_frame_bytes && reach_frame_bytes + key.ib_bytes + vb_bytes > REACH_FRAME_BYTES)
		return NULL;
	reach_frame_bytes += key.ib_bytes + vb_bytes;

	VertexReach *reach = &reaches[key];
	*reach = {};
	reach->vb_upload = vb_upload->data_hash;
	reach->ib_upload = ib_upload.data_hash;
	reach->frame = dx12_frame;
	if (start_reach(&key, reach, vb_bytes)) {
		reach->state = REACH_PENDING;
		pending_reaches.push_back(key);
	} else {
		reach->state = REACH_FAILED;
		stats.reaches_failed++;
	}
	return NULL;
}

// Where the sections of a mesh end that the mods name: a mod made for the
// D3D11 game expects the vertex buffer to end with the vertices of its last
// section, and a D3D12 build of the mesh may have more sections than that.
// In ascending order, each once.
static std::vector<UINT> section_ends;
static UINT section_ends_config;

static void collect_section_ends()
{
	if (section_ends_config == config_generation)
		return;
	section_ends_config = config_generation;

	section_ends.clear();
	for (const auto &entry : G->mTextureOverrideMap) {
		for (const TextureOverride &texture_override : entry.second) {
			if (texture_override.match_first_index.op == FuzzyMatchOp::EQUAL
			    && texture_override.match_index_count.op == FuzzyMatchOp::EQUAL)
				section_ends.push_back(texture_override.match_first_index.val + texture_override.match_index_count.val);
		}
	}
	std::sort(section_ends.begin(), section_ends.end());
	section_ends.erase(std::unique(section_ends.begin(), section_ends.end()), section_ends.end());
}

static bool reach_matches(const Dx12BufferHash *hash)
{
	return G->mTextureOverrideMap.count(Dx12BufferDescHash(hash, D3D11_BIND_VERTEX_BUFFER))
		|| G->mTextureOverrideMap.count(Dx12BufferDescHash(hash, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE));
}

// For working out offline what a mod's hash was made of
static void dump_reach(const ReachKey *key, const BYTE *data, UINT64 bytes)
{
	wchar_t path[MAX_PATH], name[64];
	FILE *file = NULL;

	if (!GetModuleFileName(migoto_handle, path, MAX_PATH))
		return;
	wcsrchr(path, L'\\')[1] = 0;
	if (wcscat_s(path, L"dx12_reach"))
		return;
	CreateDirectoryW(path, NULL);
	swprintf_s(name, L"\\%llu_%u_%u_%u_%llu.bin", key->vb_offset, key->ib_bytes, key->index_bytes, key->stride, bytes);
	if (wcscat_s(path, name))
		return;

	_wfopen_s(&file, path, L"wb");
	if (!file)
		return;
	fwrite(data, 1, (size_t)bytes, file);
	fclose(file);
}

// The half Unity makes of a float: the nearest, and the one further from zero
// of two that are as near
static UINT16 unity_half(UINT32 value)
{
	UINT16 sign = (UINT16)(value >> 16 & 0x8000);
	UINT32 mantissa = value & 0x7fffff, shift;
	int exponent = (int)(value >> 23 & 0xff) - 127;

	if (exponent > 15)
		return sign | 0x7c00;
	if (exponent >= -14)
		return (UINT16)(sign | (((UINT32)(exponent + 15) << 10 | mantissa >> 13) + (mantissa >> 12 & 1)));
	if (exponent < -25)
		return sign;
	shift = (UINT32)(-1 - exponent);
	mantissa |= 0x800000;
	return (UINT16)(sign | ((mantissa >> shift) + (mantissa >> (shift - 1) & 1)));
}

// Unity keeps the positions of a mesh with compressed vertices as four halves
// under D3D11 and as three floats under D3D12. The mods have the hash of the
// former, 4 bytes shorter a vertex, which is what this makes of the latter.
// False where that is not to be tried.
static bool pack_positions(const BYTE *vertices, UINT64 count, UINT stride, std::vector<BYTE> *packed)
{
	static int unity;
	UINT16 halves[4] = { 0, 0, 0, 0x3c00 };
	const UINT32 *position;
	BYTE *out;
	UINT64 i;

	if (!unity)
		unity = GetModuleHandleW(L"UnityPlayer.dll") ? 1 : -1;
	if (unity < 0 || stride <= 12 || !count)
		return false;

	packed->resize((size_t)(count * (stride - 4)));
	out = packed->data();
	for (i = 0; i < count; i++, vertices += stride, out += stride - 4) {
		position = (const UINT32*)vertices;
		halves[0] = unity_half(position[0]);
		halves[1] = unity_half(position[1]);
		halves[2] = unity_half(position[2]);
		memcpy(out, halves, 8);
		memcpy(out + 8, vertices + 12, stride - 12);
	}
	return true;
}

static void finish_reach(const ReachKey *key, VertexReach *reach)
{
	D3D12_RANGE range = { 0, (SIZE_T)(key->ib_bytes + reach->vb_bytes) }, nothing = {};
	UINT64 last = 0, limit = reach->vb_bytes / key->stride;
	size_t next_end = 0;
	// Vertices used up to the end of each section, which never go down
	std::vector<UINT64> ends;
	Dx12BufferHash hash = {}, matched = {}, packed_hash = {};
	std::vector<BYTE> packed;
	BYTE *data = NULL;
	UINT32 index, count;
	UINT i;

	reach->state = REACH_FAILED;
	stats.reaches_failed++;
	if (SUCCEEDED(reach->readback->Map(0, &range, (void**)&data)) && data) {
		collect_section_ends();

		for (i = 0; i + key->index_bytes <= key->ib_bytes; i += key->index_bytes) {
			index = key->index_bytes == 2 ? *(const UINT16*)(data + i) : *(const UINT32*)(data + i);
			// The highest value is a strip restart, not a vertex
			if (index != (key->index_bytes == 2 ? 0xffffu : 0xffffffffu))
				last = max(last, (UINT64)index);

			count = i / key->index_bytes + 1;
			while (next_end < section_ends.size() && section_ends[next_end] < count)
				next_end++;
			if (next_end < section_ends.size() && section_ends[next_end] == count && (ends.empty() || ends.back() != last + 1))
				ends.push_back(last + 1);
		}
		if (ends.empty() || ends.back() != last + 1)
			ends.push_back(last + 1);

		for (UINT64 vertices : ends) {
			vertices = min(vertices, limit);
			hash.data_hash = crc32c_hw(hash.data_hash, data + key->ib_bytes + hash.bytes, (size_t)(vertices * key->stride - hash.bytes));
			hash.bytes = vertices * key->stride;
			if (!matched.bytes && reach_matches(&hash))
				matched = hash;
		}
		if (!matched.bytes && pack_positions(data + key->ib_bytes, min(ends.back(), limit), key->stride, &packed)) {
			for (UINT64 vertices : ends) {
				vertices = min(vertices, limit) * (key->stride - 4);
				packed_hash.data_hash = crc32c_hw(packed_hash.data_hash, packed.data() + packed_hash.bytes, (size_t)(vertices - packed_hash.bytes));
				packed_hash.bytes = vertices;
				if (!matched.bytes && reach_matches(&packed_hash))
					matched = packed_hash;
			}
		}

		reach->hash = matched.bytes ? matched : hash;
		reach->hash.frame = dx12_frame;
		reach->state = REACH_KNOWN;
		stats.reaches_failed--;
		stats.reaches_known++;

		if (G->dx12_dump_hashes) {
			Dx12DumpNote("Vertex buffer %p+%llu: stride %u, %u indices of %u bytes use %llu of %llu bytes, %Iu section ends tried. "
				"D3D11 would have hashed that as %08x or %08x%s\n",
				key->vb, key->vb_offset, key->stride, key->ib_bytes / key->index_bytes, key->index_bytes,
				reach->hash.bytes, reach->vb_bytes, ends.size(),
				Dx12BufferDescHash(&reach->hash, D3D11_BIND_VERTEX_BUFFER),
				Dx12BufferDescHash(&reach->hash, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE),
				matched.bytes ? ", which a mod asks for" : "");
			if (packed_hash.bytes) {
				Dx12DumpNote("  With its positions as halves, %llu bytes: %08x or %08x\n", packed_hash.bytes,
					Dx12BufferDescHash(&packed_hash, D3D11_BIND_VERTEX_BUFFER),
					Dx12BufferDescHash(&packed_hash, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE));
			}
			if (!matched.bytes && key->ib_bytes + reach->vb_bytes >= 65536)
				dump_reach(key, data, key->ib_bytes + reach->vb_bytes);
		}
		reach->readback->Unmap(0, &nothing);
	}

	if (spare_readbacks.size() < REACH_SPARES && reach->readback->GetDesc().Width <= REACH_SPARE_BYTES)
		spare_readbacks.push_back(reach->readback);
	else
		reach->readback->Release();
	reach->readback = NULL;
}

static void update_reaches()
{
	UINT64 finished = 0, bytes;
	size_t i;

	for (i = 0; i < pending_reaches.size();) {
		auto found = reaches.find(pending_reaches[i]);

		if (found != reaches.end()) {
			if (!Dx12ShimFenceReached(found->second.fence)) {
				i++;
				continue;
			}
			// The rest waits for the next frame. Until then their
			// buffers go by the hash of all of them, as before.
			bytes = found->first.ib_bytes + found->second.vb_bytes;
			if (finished && finished + bytes > REACH_FINISH_BYTES)
				break;
			finished += bytes;
			finish_reach(&found->first, &found->second);
		}
		pending_reaches.erase(pending_reaches.begin() + i);
	}

	if (dx12_frame & 255)
		return;
	if (pending_reaches.empty()) {
		for (ID3D12Resource *readback : spare_readbacks)
			readback->Release();
		spare_readbacks.clear();
	}
	buffer_cache_generation++;
	for (auto j = reaches.begin(); j != reaches.end();) {
		if (j->second.state != REACH_PENDING && dx12_frame - j->second.frame > 600)
			j = reaches.erase(j);
		else
			++j;
	}
}

// What publish_resource and remember_texture came to for a resource, which a
// mod's draw asks for every texture it has: good while the stamp of what is
// known of the resource is the one taken before asking, and for the pointer
// while buffer_cache_generation is the same.
struct PublishedMemo {
	ID3D12Resource *resource;
	UINT64 stamp;
	UINT config, generation;
	Published *entry;
	// remember_texture left its word in one of the two here, or found no
	// use for one with boring NULL
	bool remembered;
	volatile LONG64 *boring;
	LONG64 boring_word;
};
static PublishedMemo published_memos[4096];

static PublishedMemo* published_memo(ID3D12Resource *resource)
{
	return &published_memos[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 52];
}

static bool published_memo_holds(const PublishedMemo *memo, ID3D12Resource *resource, UINT64 stamp)
{
	return memo->resource == resource && memo->stamp == stamp && memo->config == config_generation
		&& memo->generation == buffer_cache_generation;
}

static Published* publish_resource(ID3D12Resource *resource)
{
	PublishedMemo *memo = published_memo(resource);
	UINT64 stamp = Dx12TextureStamp(resource);
	Published *entry;
	Dx12ResourceInfo info;
	D3D11_TEXTURE2D_DESC desc2d;
	D3D11_RESOURCE_DIMENSION dimension;

	if (published_memo_holds(memo, resource, stamp)) {
		memo->entry->frame = dx12_frame;
		return memo->entry;
	}

	if (!Dx12GetResourceInfo(resource, &info))
		return NULL;

	entry = &published[{ resource, 0, PUBLISHED_RESOURCE }];
	entry->frame = dx12_frame;

	memo->resource = resource;
	memo->stamp = stamp;
	memo->config = config_generation;
	memo->generation = buffer_cache_generation;
	memo->entry = entry;
	memo->remembered = false;

	if (entry->resource && !memcmp(&entry->desc, &info.desc, sizeof(info.desc))) {
		if (entry->has_data_hash == info.has_data_hash && entry->data_hash == info.data_hash && entry->config == config_generation)
			return entry;
	} else {
		release_published(entry);
		entry->resource = new_game_texture(&info, &desc2d);
		entry->desc = info.desc;
	}

	entry->has_data_hash = info.has_data_hash;
	entry->data_hash = info.data_hash;
	entry->config = config_generation;

	entry->resource->GetType(&dimension);
	if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
		static_cast<ShimTexture2D*>(entry->resource)->GetDesc(&desc2d);
		set_resource_hash(entry->resource, dimension, pick_texture_hash(&info), info.has_data_hash ? info.data_hash : 0, &desc2d);
	} else if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER && (info.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
	    && info.desc.Width <= UINT_MAX) {
		set_resource_hash(entry->resource, dimension, pick_written_buffer_hash(info.desc.Width), 0, NULL);
	}
	return entry;
}

// Nearly every draw has its vertex buffer asked about, mostly the same buffers
// frame after frame, and the answer takes several lookups under locks shared
// with the threads recording. So what publish_buffer worked out is remembered
// along with the stamps of its lookups, which tell whether it would come to
// the same again.
struct BufferCacheEntry {
	// What was asked. The index buffer is 0 where there is none.
	UINT64 va, bytes, ib_va;
	UINT kind, stride, ib_bytes, ib_format;

	UINT generation, config;
	UINT64 stamp;
	// No TextureOverride section can match the buffer
	bool boring;
	bool ib_asked, ib_found;
	bool has_hash, ib_hashed;
	// What the stamp is of
	ID3D12Resource *resource, *ib;
	UINT64 offset, span, ib_offset, ib_span;

	// Up to here is what a thread recording reads of a buffer whose stamp
	// has not changed, see buffer_is_boring: kept together for the memory
	// that has to be fetched for it
	Published *entry;
	UINT entry_version;
	VertexReach *reach;
	// What the lookups the stamp is of came to, for when it has changed:
	// the counters it is made of are shared, and the game writes
	// hundreds of other ranges every frame
	Dx12BufferHash data, ib_data;
};

// Written with the lock held. The threads recording read it without, to tell
// whether a draw is worth stopping for, and may put a newer stamp to it: seq is
// odd while data is being written, which whoever made it so does, and has
// changed if it was written since.
struct alignas(64) BufferCacheSlot {
	volatile LONG seq;
	BufferCacheEntry data;
};
// In buckets of this many: with one slot each, the few percent of a frame's
// buffers that share theirs with another would be looked up for every draw
const UINT BUFFER_CACHE_WAYS = 4;
static BufferCacheSlot buffer_cache[32768];
// A word per slot made of what the slot was asked, never 0: the slots of a
// bucket that are for something else are told by these, a bucket of them to
// the cache line, without fetching the slots. Written before the slot is.
static volatile UINT64 buffer_cache_tags[ARRAYSIZE(buffer_cache)];

// The first slot of the bucket
static BufferCacheSlot* buffer_cache_slot(BufferCacheEntry *asked, UINT64 va, UINT64 bytes, UINT kind, UINT stride,
		const D3D12_INDEX_BUFFER_VIEW *indices, UINT64 *tag)
{
	UINT64 mixed;

	asked->va = va;
	asked->bytes = bytes;
	asked->kind = kind;
	asked->stride = stride;
	if (indices) {
		asked->ib_va = indices->BufferLocation;
		asked->ib_bytes = indices->SizeInBytes;
		asked->ib_format = indices->Format;
	}
	mixed = (va ^ (asked->ib_va * 0x9e3779b97f4a7c15ull) ^ bytes ^ ((UINT64)stride << 40) ^ ((UINT64)kind << 60))
		* 0xff51afd7ed558ccdull;
	*tag = mixed | 1;
	return &buffer_cache[(mixed >> 49) & ~(UINT64)(BUFFER_CACHE_WAYS - 1)];
}

static bool same_buffer_asked(const BufferCacheEntry *a, const BufferCacheEntry *b)
{
	return a->va == b->va && a->bytes == b->bytes && a->ib_va == b->ib_va && a->kind == b->kind
		&& a->stride == b->stride && a->ib_bytes == b->ib_bytes && a->ib_format == b->ib_format;
}

// The slot of the bucket that is for what is asked, or to put it in
static BufferCacheSlot* buffer_cache_way(BufferCacheSlot *bucket, const BufferCacheEntry *asked, UINT64 tag, bool store)
{
	const volatile UINT64 *tags = &buffer_cache_tags[bucket - buffer_cache];
	static UINT next;
	UINT way;

	for (way = 0; way < BUFFER_CACHE_WAYS; way++) {
		if (tags[way] == tag && same_buffer_asked(&bucket[way].data, asked))
			return &bucket[way];
	}
	if (!store)
		return NULL;
	for (way = 0; way < BUFFER_CACHE_WAYS; way++) {
		if (!bucket[way].data.generation)
			return &bucket[way];
	}
	return &bucket[next++ % BUFFER_CACHE_WAYS];
}

static bool buffer_cache_stamp(const BufferCacheEntry *cached, UINT64 *stamp)
{
	UINT64 part;

	*stamp = Dx12FindBufferStamp(cached->va);
	if (!Dx12BufferHashStamp(cached->resource, cached->offset, cached->span, &part))
		return false;
	*stamp += part;
	if (!cached->ib_asked)
		return true;

	*stamp += Dx12FindBufferStamp(cached->ib_va);
	if (!cached->ib_found)
		return true;
	if (!Dx12BufferHashStamp(cached->ib, cached->ib_offset, cached->ib_span, &part))
		return false;
	*stamp += part;
	return true;
}

static bool same_buffer_hash(bool found, const Dx12BufferHash *hash, ID3D12Resource *resource, UINT64 offset)
{
	Dx12BufferHash now = {};

	if (Dx12FindBufferHash(resource, offset, &now) != found)
		return false;
	return !found || (now.bytes == hash->bytes && now.data_hash == hash->data_hash && now.frame == hash->frame);
}

// A buffer whose contents were never hashed (written by the GPU, or too often
// to keep up with) can only match a TextureOverride section by its size and
// kind. The game makes such buffers anew every frame at addresses the cache
// above has never seen, so the sizes and kinds that matched nothing are kept
// as well: kind, size and configuration in a word.
static volatile LONG64 boring_unhashed[4096];

static volatile LONG64* boring_unhashed_slot(UINT kind, UINT64 bytes, LONG64 *word)
{
	UINT64 shape = (bytes & 0xffffffffffull) | (UINT64)kind << 40;

	*word = (LONG64)(shape | (UINT64)(config_generation & 0xfffff) << 44);
	return &boring_unhashed[(shape * 0x9e3779b97f4a7c15ull) >> 52];
}

// Without the lock
static bool unhashed_buffer_is_boring(UINT64 va, UINT64 bytes, UINT kind)
{
	ID3D12Resource *resource;
	Dx12BufferHash hash;
	UINT64 offset;
	LONG64 word;

	if (*boring_unhashed_slot(kind, bytes, &word) != word)
		return false;
	return Dx12FindBuffer(va, &resource, &offset) && !Dx12FindBufferHash(resource, offset, &hash);
}

// Whether the lookups an entry was made from come to the same now. Without
// the lock, and without following its pointers.
static bool buffer_as_remembered(const BufferCacheEntry *cached)
{
	ID3D12Resource *resource;
	UINT64 offset;

	if (!Dx12FindBuffer(cached->va, &resource, &offset) || resource != cached->resource || offset != cached->offset)
		return false;
	if (!same_buffer_hash(cached->has_hash, &cached->data, cached->resource, cached->offset))
		return false;
	if (cached->ib_asked) {
		if (Dx12FindBuffer(cached->ib_va, &resource, &offset) != cached->ib_found)
			return false;
		if (cached->ib_found) {
			if (resource != cached->ib || offset != cached->ib_offset)
				return false;
			if (!same_buffer_hash(cached->ib_hashed, &cached->ib_data, cached->ib, cached->ib_offset))
				return false;
		}
	}
	return true;
}

// The stamp is only good for lookups made after it was taken, so they are
// made once more
static void remember_buffer(BufferCacheEntry *cached, bool has_hash, const Dx12BufferHash *data, const ReachLookup *lookup)
{
	cached->generation = 0;
	if (!buffer_cache_stamp(cached, &cached->stamp))
		return;

	cached->has_hash = has_hash;
	cached->data = has_hash ? *data : Dx12BufferHash();
	cached->ib_hashed = lookup->ib_hashed;
	cached->ib_data = lookup->ib_hashed ? lookup->ib_upload : Dx12BufferHash();
	if (!buffer_as_remembered(cached))
		return;

	cached->generation = buffer_cache_generation;
}

static Published* look_up_buffer(UINT64 va, UINT64 bytes, UINT kind, UINT stride, const D3D12_INDEX_BUFFER_VIEW *indices);

// stride and indices are for a vertex buffer
static Published* publish_buffer(UINT64 va, UINT64 bytes, UINT kind, UINT stride, const D3D12_INDEX_BUFFER_VIEW *indices)
{
	// Only what is asked is filled in, the rest of it is not looked at
	// here: a mod's draw comes by for each of its buffers
	BufferCacheEntry *cached, asked;
	BufferCacheSlot *slot;
	UINT64 stamp, tag;

	asked.ib_va = 0;
	asked.ib_bytes = 0;
	asked.ib_format = 0;
	slot = buffer_cache_way(buffer_cache_slot(&asked, va, bytes, kind, stride, indices, &tag), &asked, tag, false);
	cached = slot ? &slot->data : NULL;

	// In this order: the pointers are only good with the generation
	if (cached && cached->generation == buffer_cache_generation && cached->config == config_generation
	    && cached->entry->version == cached->entry_version
	    && buffer_cache_stamp(cached, &stamp) && stamp == cached->stamp) {
		stats.buffers_remembered++;
		cached->entry->frame = dx12_frame;
		if (cached->reach)
			cached->reach->frame = dx12_frame;
		return cached->entry;
	}
	return look_up_buffer(va, bytes, kind, stride, indices);
}

static Published* look_up_buffer(UINT64 va, UINT64 bytes, UINT kind, UINT stride, const D3D12_INDEX_BUFFER_VIEW *indices)
{
	D3D12_RESOURCE_DESC desc = {};
	const Dx12BufferHash *reach = NULL;
	Dx12BufferHash data = {};
	ReachLookup lookup = {};
	BufferCacheEntry asked = {};
	BufferCacheSlot *bucket, *slot;
	ID3D12Resource *resource;
	Published *entry;
	UINT64 offset, tag;
	uint32_t hash;
	bool has_hash;
	LONG64 word;

	bucket = buffer_cache_slot(&asked, va, bytes, kind, stride, indices, &tag);
	stats.buffers_looked_up++;

	if (!Dx12FindBuffer(va, &resource, &offset)) {
		stats.lookups_missing++;
		return NULL;
	}
	has_hash = Dx12FindBufferHash(resource, offset, &data);
	lookup.settled = true;
	if (has_hash && indices)
		reach = vertex_reach(resource, offset, bytes, stride, &data, indices, &lookup);

	entry = &published[{ resource, offset, kind }];
	entry->frame = dx12_frame;

	if (lookup.settled) {
		asked.resource = resource;
		asked.offset = offset;
		asked.span = has_hash ? data.bytes : 1;
		asked.ib_asked = lookup.ib_asked;
		asked.ib_found = lookup.ib_found;
		asked.ib = lookup.ib;
		asked.ib_offset = lookup.ib_offset;
		asked.ib_span = lookup.ib_hashed ? lookup.ib_upload.bytes : 1;
		asked.reach = lookup.reach;
		asked.entry = entry;
		asked.config = config_generation;
	}

	if (entry->resource && entry->bytes == bytes) {
		if (entry->has_data_hash == has_hash && entry->data_hash == data.data_hash && entry->config == config_generation
		    && entry->reach_bytes == (reach ? reach->bytes : 0) && entry->reach_hash == (reach ? reach->data_hash : 0)) {
			if (lookup.settled) {
				asked.entry_version = entry->version;
				remember_buffer(&asked, has_hash, &data, &lookup);
				if (asked.generation) {
					asked.boring = TextureOverrideCandidatesEmpty(entry->resource);
					if (asked.boring && !has_hash)
						InterlockedExchange64(boring_unhashed_slot(kind, bytes, &word), word);
				} else
					stats.lookups_unstamped++;
				// What could not be remembered still replaces
				// what was, which no longer goes
				slot = buffer_cache_way(bucket, &asked, tag, asked.generation != 0);
				if (slot) {
					LONG seq;

					// A thread recording may be putting a newer
					// stamp to what the slot had
					for (;;) {
						seq = slot->seq;
						if (!(seq & 1) && InterlockedCompareExchange(&slot->seq, seq + 1, seq) == seq)
							break;
						YieldProcessor();
					}
					InterlockedExchange64((volatile LONG64*)&buffer_cache_tags[slot - buffer_cache], (LONG64)tag);
					slot->data = asked;
					InterlockedExchange(&slot->seq, seq + 2);
				}
			} else {
				stats.lookups_unsettled++;
			}
			return entry;
		}
	} else {
		release_published(entry);
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		entry->resource = new_game_buffer(&desc, bytes,
			kind == PUBLISHED_INDEX_BUFFER ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER);
		ShimResourceOf(entry->resource)->game_va = va;
		entry->bytes = bytes;
	}

	entry->has_data_hash = has_hash;
	entry->data_hash = data.data_hash;
	entry->reach_bytes = reach ? reach->bytes : 0;
	entry->reach_hash = reach ? reach->data_hash : 0;
	entry->config = config_generation;
	entry->version++;
	stats.lookups_new++;

	// Without a hash for the data there is nothing a mod could have
	// matched under D3D11 either
	hash = has_hash ? pick_buffer_hash(&data, reach, kind) : 0;
	set_resource_hash(entry->resource, D3D11_RESOURCE_DIMENSION_BUFFER, hash, data.data_hash, NULL);
	if (kind != PUBLISHED_INDEX_BUFFER && G->mTextureOverrideMap.count(hash)) {
		stats.vertex_buffers_matched++;
		Dx12DumpNote("Vertex buffer %p+%llu matches the TextureOverride sections of hash %08x\n", resource, offset, hash);
	}
	return entry;
}

// The textures no TextureOverride section can match, for the threads recording
// to find without the lock: a word per texture made of everything the answer
// came of, which is the resource at the address, its contents and the
// configuration. In pairs, for the textures that would share a slot.
static volatile LONG64 boring_textures[8192];

static volatile LONG64* boring_texture_slot(ID3D12Resource *resource, const Dx12ResourceInfo *info, LONG64 *word)
{
	struct {
		ID3D12Resource *resource;
		UINT config;
		UINT cube;
	} key = { resource, config_generation, info->cube };
	uint32_t shape = crc32c_hw(crc32c_hw(0, &info->desc, sizeof(info->desc)), &key, sizeof(key));

	// Never what an empty slot has
	*word = (LONG64)((UINT64)shape << 32 | (info->has_data_hash ? info->data_hash : 0) | 1ull << 63);
	if (!info->has_data_hash)
		*word ^= 1ll << 62;
	return &boring_textures[(((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 51) & ~1ull];
}

// Nearly every draw asks about a texture, and what is known of one is behind a
// lock: so what texture_is_boring came to is kept by the address alone, in a
// word made of the address, the configuration and the stamp of what is known
// of the resource there, which is not the same word once any of them changed.
static volatile LONG64 boring_texture_addresses[8192];

// Without the lock
static bool texture_is_boring(ID3D12Resource *resource)
{
	UINT64 stamp = Dx12TextureStamp(resource);
	UINT64 mixed = ((UINT64)(uintptr_t)resource ^ (stamp * 0x9e3779b97f4a7c15ull) ^ ((UINT64)config_generation << 44))
		* 0xff51afd7ed558ccdull;
	// Never what an empty slot has
	LONG64 remembered = (LONG64)((mixed ^ (mixed >> 29)) | 1);
	volatile LONG64 *address = &boring_texture_addresses[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 51];
	volatile LONG64 *slot;
	Dx12ResourceInfo info;
	LONG64 word;

	if (*address == remembered)
		return true;

	// The engine is not shown what is not known, see publish_resource
	if (Dx12GetResourceInfo(resource, &info)) {
		slot = boring_texture_slot(resource, &info, &word);
		if (slot[0] != word && slot[1] != word)
			return false;
	}
	InterlockedExchange64(address, remembered);
	return true;
}

// entry is what publish_resource returned for the resource
static void remember_texture(ID3D12Resource *resource, const Published *entry)
{
	static UINT next;
	PublishedMemo *memo = published_memo(resource);
	bool known = memo->entry == entry && published_memo_holds(memo, resource, Dx12TextureStamp(resource));
	volatile LONG64 *slot;
	Dx12ResourceInfo info;
	LONG64 word;

	// The word may have made way for another texture's since
	if (known && memo->remembered && (!memo->boring || memo->boring[0] == memo->boring_word || memo->boring[1] == memo->boring_word))
		return;

	// Only if the entry is of the resource as it is by now
	if (!Dx12GetResourceInfo(resource, &info) || memcmp(&entry->desc, &info.desc, sizeof(info.desc))
	    || entry->has_data_hash != info.has_data_hash || entry->data_hash != info.data_hash || entry->config != config_generation)
		return;

	slot = boring_texture_slot(resource, &info, &word);
	if (known) {
		memo->remembered = true;
		memo->boring = slot;
		memo->boring_word = word;
	}
	if (slot[0] == word || slot[1] == word)
		return;
	if (TextureOverrideCandidatesEmpty(entry->resource))
		InterlockedExchange64(&slot[next++ & 1], word);
	else if (known)
		memo->boring = NULL;
}

// A constant buffer is bound by address and the game hands out a new address
// every time it fills one in, so these are not kept per resource: there is
// one object per stage, register and size, which is moved to where the buffer
// of the draw in the scope is.
//
// Under D3D11 such a buffer is created without contents, which leaves the
// description to be hashed. D3D12 knows nothing of that, but a renderer has
// few ways of creating its constant buffers.
struct PublishedConstants {
	ShimBuffer *buffer;
	UINT config;
	bool matched;
	// No TextureOverride section can match it
	bool boring;
};
static std::unordered_map<UINT64, PublishedConstants> published_constants;

// The ones that are boring, for the threads recording to find without the
// lock: stage, register, size and the configuration that goes for in a word
static volatile LONG64 boring_constants[256];

static volatile LONG64* boring_constants_slot(Dx12ShaderStage stage, UINT slot, UINT bytes, LONG64 *word)
{
	*word = (LONG64)((UINT64)bytes | (UINT64)slot << 32 | (UINT64)stage << 40 | (UINT64)(config_generation & 0xfffff) << 44);
	return &boring_constants[(((UINT64)bytes | (UINT64)slot << 32 | (UINT64)stage << 40) * 0x9e3779b97f4a7c15ull) >> 56];
}

static uint32_t constant_buffer_hash(UINT bytes)
{
	D3D11_BUFFER_DESC desc = {};

	desc.ByteWidth = bytes;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	return crc32c_hw(0, &desc, sizeof(desc));
}

static uint32_t pick_constant_buffer_hash(UINT bytes);

static PublishedConstants* publish_constants(Dx12ShaderStage stage, UINT slot, UINT bytes)
{
	PublishedConstants *entry = &published_constants[(UINT64)stage << 40 | (UINT64)slot << 32 | bytes];
	D3D12_RESOURCE_DESC desc = {};
	LONG64 word;

	if (!entry->buffer) {
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		entry->buffer = static_cast<ShimBuffer*>(new_game_buffer(&desc, (bytes + 255) & ~255u, D3D11_BIND_CONSTANT_BUFFER));
	}

	if (entry->config != config_generation) {
		uint32_t hash = pick_constant_buffer_hash(bytes);

		entry->config = config_generation;
		entry->matched = !!G->mTextureOverrideMap.count(hash);
		set_resource_hash(entry->buffer, D3D11_RESOURCE_DIMENSION_BUFFER, hash, 0, NULL);
		entry->boring = TextureOverrideCandidatesEmpty(entry->buffer);
	}
	if (entry->boring)
		InterlockedExchange64(boring_constants_slot(stage, slot, bytes, &word), word);
	return entry;
}

static uint32_t pick_constant_buffer_hash(UINT bytes)
{
	uint32_t hash, pooled;
	UINT bucket = 16;

	// Unreal Engine takes its uniform buffers from pools of sizes that
	// are powers of two
	while (bucket < bytes)
		bucket *= 2;
	pooled = constant_buffer_hash(bucket);
	if (G->mTextureOverrideMap.count(pooled))
		return pooled;

	hash = constant_buffer_hash((bytes + 15) & ~15u);
	return G->mTextureOverrideMap.count(hash) ? hash : pooled;
}

template <class View>
static View* new_game_view(Published *entry)
{
	View *view = new View();

	// No descriptor: there is nothing of ours to describe
	view->v.heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	view->v.resource11 = entry->resource;
	view->v.resource11->AddRef();
	view->v.resource = ShimResourceOf(entry->resource);
	view->mDesc = {};
	return view;
}

static void forget_unused_published()
{
	buffer_cache_generation++;
	for (auto i = published.begin(); i != published.end();) {
		if (dx12_frame - i->second.frame > 600) {
			release_published(&i->second);
			i = published.erase(i);
		} else {
			++i;
		}
	}
}

// -----------------------------------------------------------------------------
// Scope

struct ChangedParameter {
	UINT parameter;
	D3D12_ROOT_PARAMETER_TYPE type;
	// Tables: where the copy with the mod's descriptors is
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};

// A dispatch of the game that skins a mesh, which the engine is shown as the
// draw a D3D11 renderer would have skinned it with. See "Skinning done by a
// compute shader".
struct SkinDispatch {
	// Where the views of the vertices, their bone weights and the skinned
	// vertices start
	UINT64 source_va, weights_va, output_va;
	ID3D12Resource *output;
	UINT count, stride, weight_stride;
	// Of the vertex count, if that is a root constant. -1 otherwise.
	int constants_parameter;
};

// Where a D3D11 renderer has the bone weights bound for the draw that skins
const UINT SKIN_BLEND_SLOT = 2;

struct Dx12DrawScope {
	Dx12GraphicsCommandList *list;
	bool compute;
	// Set for a dispatch that skins: compute as far as the list goes, a
	// draw as far as the engine does
	const SkinDispatch *skin;
	// What a mod last skinned its vertices for it with, to the buffer of
	// ours: its vertex buffers in slots 0 and SKIN_BLEND_SLOT
	bool mod_skinned;
	ID3D11Buffer *skinned_vbs[2];
	UINT skinned_offsets[2];
	// Of the list, which do not change while the scope is open. The root
	// signature is NULL if the game's is not known.
	const Dx12PipelineInfo *pipeline;
	const Dx12RootSignatureInfo *signature;
	// The shaders the next draw runs with: the game's, but for the stages
	// a mod has given its own for a draw of its own, see apply_pipeline.
	// Those are made up of the mod's ShimShader for the draw.
	const Dx12ShaderInfo *shaders[DX12_STAGE_COUNT];
	Dx12ShaderInfo mod_shaders[DX12_STAGE_COUNT];
	// Bit per stage of those that is the mod's
	UINT custom_stages;
	// A mod's shaders could not be given to the draw being applied
	bool custom_failed;

	// What has been changed on the list and is to be put back
	UINT vbs_changed;
	bool ib_changed;
	bool topology_changed;
	// The game's pipeline made again with the input layout or the shaders
	// of a mod, if that is what the list has set, see apply_pipeline
	ID3D12PipelineState *variant;
	// What the engine was given for the input layout of the game's
	// pipeline, once it asked (Dx12PublishInputLayout). Not referenced.
	ID3D11InputLayout *game_layout;
	std::vector<ChangedParameter> parameters;
	// A heap of ours is set on the list in place of the game's view heap
	// (see side_heap), with a copy of every table the game had set. The
	// copies are kept while the scope is open: a table a mod changes for a
	// draw is set back to its copy for the next, and the game's heap comes
	// back when the scope closes or the game's own draw is made.
	bool heaps_changed;
	// Set once trying to switch to the side heap failed in this scope
	bool side_impossible;
	D3D12_GPU_DESCRIPTOR_HANDLE side_base[DX12_MAX_ROOT_ARGUMENTS];
	// How many descriptors each of those copies has: as many as the shaders
	// of the draw it was made for read, see grow_side_tables
	UINT side_size[DX12_MAX_ROOT_ARGUMENTS];
};

static Dx12DrawScope *scope;

static void log_once(bool *reported, const char *message)
{
	if (*reported)
		return;
	*reported = true;
	Dx12DumpNote("*** %s\n", message);
}

// What follows also answers for a draw of a Vulkan game (vk/VkDrawPath),
// which has no scope here: the engine and the shim context do not know the
// difference.
bool Dx12InDrawScope()
{
	return scope || VkInDrawScope();
}

bool Dx12ScopeHasShader(Dx12ShaderStage stage)
{
	if (!scope)
		return VkScopeHasShader(stage);
	return !!scope->pipeline->shaders[stage];
}

void Dx12ScopeUnsupported(const char *what)
{
	static std::unordered_set<std::string> reported;

	if (reported.insert(what).second)
		Dx12DumpNote("*** A mod tried %s for a draw or dispatch of the game, which is not possible on D3D12 yet. Ignored.\n", what);
}

// The buffers Dx12ScopeWrittenBuffer gave out for the dispatch being prepared
static std::vector<Published*> standins_pending;
// Counts the scopes opened, to tell whether a mod wrote in the current one
static UINT64 scope_serial;

void Dx12ScopeDispatched(bool recorded)
{
	for (Published *entry : standins_pending) {
		if (!recorded)
			continue;
		entry->standin_valid = true;
		entry->standin_drawn = false;
		entry->standin_frame = dx12_frame;
		entry->standin_scope = scope_serial;
	}
	standins_pending.clear();
	stats.mod_dispatches += recorded;
}

static bool skin_output_at(UINT64 va);
static UINT64 skin_output_bytes(UINT64 va);
static void alias_skin_vertices(Published *vertices, const SkinDispatch *skin);

ID3D11Buffer* Dx12ScopeVertexBuffer(UINT slot, UINT *stride)
{
	const D3D12_VERTEX_BUFFER_VIEW *view;
	Published *entry;
	UINT64 start;

	*stride = 0;
	if (!scope)
		return NULL;
	if (scope->skin) {
		const SkinDispatch *skin = scope->skin;

		if (slot == 0) {
			entry = publish_buffer(skin->source_va, (UINT64)skin->count * skin->stride, PUBLISHED_SKIN_BUFFER, skin->stride, NULL);
			*stride = skin->stride;
			if (entry)
				alias_skin_vertices(entry, skin);
		} else if (slot == SKIN_BLEND_SLOT) {
			entry = publish_buffer(skin->weights_va, (UINT64)skin->count * skin->weight_stride, PUBLISHED_SKIN_BUFFER,
				skin->weight_stride, NULL);
			*stride = skin->weight_stride;
		} else {
			return NULL;
		}
		if (!entry)
			*stride = 0;
		return entry ? static_cast<ShimBuffer*>(entry->resource) : NULL;
	}
	if (scope->compute || slot >= ARRAYSIZE(scope->list->mState.vertex_buffers))
		return NULL;
	// A D3D12 renderer sets the buffers a draw reads and leaves the rest
	// as they were, where its D3D11 counterpart unbinds them. What an
	// earlier draw left in a slot is no part of this one: a mod going by
	// it would take a light volume for the mesh drawn before it.
	if (!(scope->pipeline->vb_slots & 1u << slot))
		return NULL;

	view = &scope->list->mState.vertex_buffers[slot];
	start = G->dx12_dump_hashes ? ticks() : 0;
	entry = publish_buffer(view->BufferLocation, view->SizeInBytes, PUBLISHED_VERTEX_BUFFER, view->StrideInBytes,
		&scope->list->mState.index_buffer);
	if (start)
		stats.buffer_ticks += ticks() - start;
	if (!entry)
		return NULL;

	*stride = view->StrideInBytes;
	// For a mod that keeps what it is given to bind it to another draw
	ShimResourceOf(entry->resource)->skin_va = skin_output_at(view->BufferLocation) ? view->BufferLocation : 0;
	return static_cast<ShimBuffer*>(entry->resource);
}

// The buffer a D3D11 renderer would have streamed the skinned vertices out to.
// A mod copies it after its own draw to bind what that skinned to a draw of
// another mesh, which the game's buffer is not bound for.
ID3D11Buffer* Dx12ScopeStreamOutput()
{
	const SkinDispatch *skin = scope ? scope->skin : NULL;
	Published *entry;
	UINT64 skinned;

	if (!skin)
		return NULL;
	// As large as what the mod had skinned, which is what a draw that goes
	// by the size of its vertex buffer is to count
	skinned = skin_output_bytes(skin->output_va);
	entry = publish_buffer(skin->output_va, skinned ? skinned : (UINT64)skin->count * skin->stride, PUBLISHED_SKIN_BUFFER,
		skin->stride, NULL);
	if (!entry)
		return NULL;

	ShimResourceOf(entry->resource)->skin_va = skinned ? skin->output_va : 0;
	return static_cast<ShimBuffer*>(entry->resource);
}

ID3D11Buffer* Dx12ScopeIndexBuffer(DXGI_FORMAT *format)
{
	const D3D12_INDEX_BUFFER_VIEW *view;
	Published *entry;

	*format = DXGI_FORMAT_UNKNOWN;
	if (!scope)
		return VkScopeIndexBuffer(format);
	if (scope->compute)
		return NULL;
	view = &scope->list->mState.index_buffer;

	entry = publish_buffer(view->BufferLocation, view->SizeInBytes, PUBLISHED_INDEX_BUFFER, 0, NULL);
	if (!entry)
		return NULL;

	*format = view->Format;
	return static_cast<ShimBuffer*>(entry->resource);
}

ID3D11Buffer* Dx12ScopeConstantBuffer(Dx12ShaderStage stage, UINT slot)
{
	const Dx12ShaderInfo *shader;
	Dx12ResourceInfo info;
	ID3D12Resource *resource;
	Dx12DescriptorSlot view;
	ShimBuffer *buffer;
	UINT64 offset;
	UINT bytes;

	if (!scope)
		return VkScopeConstantBuffer(stage, slot);
	if (scope->compute != (stage == DX12_STAGE_CS))
		return NULL;
	shader = scope->pipeline->shaders[stage];
	// The shader is what says how much of what is at the address is the
	// buffer
	if (!shader || slot >= DX12_CB_SIZES || !shader->cb_bytes[slot])
		return NULL;
	if (!scope->signature || !Dx12ResolveRegister(&scope->list->mState, scope->signature, stage, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, slot, &view) || !view.offset)
		return NULL;
	bytes = shader->cb_bytes[slot];

	PublishedConstants *entry = publish_constants(stage, slot, bytes);
	buffer = entry->buffer;
	stats.constant_buffers_matched += entry->matched;

	// Upload memory is all a list of ours can count on being able to
	// read, and on having been written already
	buffer->r.foreign_va = 0;
	if (Dx12FindBuffer(view.offset, &resource, &offset) && Dx12GetResourceInfo(resource, &info)
	    && info.heap_type == D3D12_HEAP_TYPE_UPLOAD && info.desc.Width - offset >= buffer->mDesc.ByteWidth)
		buffer->r.foreign_va = view.offset;
	return buffer;
}

static Published* publish_register(Dx12ShaderStage stage, D3D12_DESCRIPTOR_RANGE_TYPE type, UINT slot, Dx12DescriptorSlot *view)
{
	if (!scope || scope->compute != (stage == DX12_STAGE_CS))
		return NULL;
	// A root descriptor has an address for a resource, which says too
	// little about it to be of use to a mod
	if (!scope->signature || !Dx12ResolveRegister(&scope->list->mState, scope->signature, stage, type, slot, view) || !view->resource)
		return NULL;
	return publish_resource(view->resource);
}

ID3D11ShaderResourceView* Dx12ScopeShaderResource(Dx12ShaderStage stage, UINT slot)
{
	const Dx12ShaderInfo *shader = scope ? scope->pipeline->shaders[stage] : NULL;
	Dx12DescriptorSlot view;
	Published *entry;
	ShimShaderResourceView *srv;

	if (!scope)
		return VkScopeShaderResource(stage, slot);

	// A table reaches as far as the root signature says, which goes for
	// every shader it is used with. Past what this one declares the game
	// need not have written anything, and what is there is left from
	// other draws.
	if (shader && shader->reflected && !shader->unbounded_srv && slot < 128 && !(shader->srv[slot / 64] & (1ull << (slot % 64))))
		return NULL;

	entry = publish_register(stage, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, slot, &view);
	if (!entry)
		return NULL;
	remember_texture(view.resource, entry);

	// The entry outlives the game's resource where its address passes on to
	// another of the same description, a normal map to a colour texture
	// for one, which differ in nothing but the format of their views. A
	// mod that picks its textures by that format has to see this view's.
	srv = static_cast<ShimShaderResourceView*>(entry->srv);
	if (srv && (srv->mDesc.Format != (DXGI_FORMAT)view.format || srv->mDesc.ViewDimension != (D3D11_SRV_DIMENSION)view.dimension)) {
		entry->srv->Release();
		entry->srv = NULL;
	}

	if (!entry->srv) {
		srv = new_game_view<ShimShaderResourceView>(entry);
		srv->mDesc.Format = (DXGI_FORMAT)view.format;
		// D3D12_SRV_DIMENSION has the values of D3D11_SRV_DIMENSION
		srv->mDesc.ViewDimension = (D3D11_SRV_DIMENSION)view.dimension;
		srv->v.format = (DXGI_FORMAT)view.format;
		entry->srv = srv;
	}
	return entry->srv;
}

ID3D11UnorderedAccessView* Dx12ScopeUnorderedAccess(UINT slot)
{
	Dx12DescriptorSlot view;
	Published *entry = publish_register(DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, slot, &view);
	ShimUnorderedAccessView *uav;

	if (!entry)
		return NULL;

	uav = static_cast<ShimUnorderedAccessView*>(entry->uav);
	if (uav && (uav->mDesc.Format != (DXGI_FORMAT)view.format || uav->mDesc.ViewDimension != (D3D11_UAV_DIMENSION)view.dimension)) {
		entry->uav->Release();
		entry->uav = NULL;
	}

	if (!entry->uav) {
		uav = new_game_view<ShimUnorderedAccessView>(entry);
		uav->mDesc.Format = (DXGI_FORMAT)view.format;
		uav->mDesc.ViewDimension = (D3D11_UAV_DIMENSION)view.dimension;
		uav->v.format = (DXGI_FORMAT)view.format;
		entry->uav = uav;
	}
	return entry->uav;
}

// -----------------------------------------------------------------------------
// Buffers of the game that mods write
//
// A mod's compute shader run in place of a dispatch of the game finds the
// game's unordered access views bound under D3D11, and writes the game's
// buffer through them: a mesh's shape key offsets, for one, which the game
// then draws with as a vertex buffer. Where the mod's mesh has more vertices
// than the game's, a TextureOverride section has the buffer created larger
// (override_vertex_count).
//
// Neither works on D3D12: the mod's shader runs on our list, which knows
// nothing of the state the game has its buffer in, and the buffer is the size
// the game made it. So the shader is given a buffer of ours instead, of the
// size the mods ask for, and the draws of mods get that in place of the game's
// wherever the game's is bound as a vertex buffer.
//
// The game clears such a buffer before the dispatches that add up in it, with
// a dispatch the mods know nothing of. Ours is cleared the first time a frame
// has it written, and when it was drawn with since.
//
// Ours only stands for the game's while the mods wrote it for what the game
// has in its own. A dispatch of the game that writes its buffer with no mod
// writing ours in its place starts the buffer over as far as we can tell: the
// clear, or the mesh of a mod that is switched off, or another mesh that got
// the buffer. What ours holds is not drawn with from then until a mod writes
// it again, cleared first. That goes for every dispatch of the game, also the
// ones no mod has a section for (Dx12GameDispatched). Writes of the game that
// are not dispatches are not seen: a copy, a clear of the view, an indirect
// dispatch.

static UINT standin_bytes(const Published *entry)
{
	TextureOverrideMatches matches;
	UINT bytes = (UINT)entry->desc.Width;

	find_texture_overrides_for_resource(entry->resource, &matches, NULL);
	for (TextureOverride *texture_override : matches) {
		if (texture_override->override_byte_width > 0)
			bytes = max(bytes, (UINT)texture_override->override_byte_width);
	}
	return bytes;
}

ID3D11UnorderedAccessView* Dx12ScopeWrittenBuffer(UINT slot)
{
	static const UINT clear[4] = {};
	static bool failed;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
	D3D11_BUFFER_DESC desc = {};
	Dx12DescriptorSlot view;
	Dx12ResourceInfo info;
	Published *entry;
	UINT bytes, element;

	if (!scope || !scope->compute || scope->skin)
		return NULL;
	entry = publish_register(DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, slot, &view);
	if (!entry || entry->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || entry->desc.Width > UINT_MAX)
		return NULL;
	if (!Dx12GetResourceInfo(view.resource, &info))
		return NULL;
	// Written for a buffer that is gone, with this one at its address
	if (entry->standin_serial != info.serial)
		entry->standin_valid = false;
	entry->standin_serial = info.serial;
	// Typed views of the whole buffer are what can be made again on a
	// buffer of another size
	element = dxgi_format_size((DXGI_FORMAT)view.format);
	if (view.dimension != D3D12_UAV_DIMENSION_BUFFER || !element || view.offset || (UINT64)view.size * element != entry->desc.Width) {
		Dx12ScopeUnsupported("to write a buffer of the game through a view that is not a typed one of all of it");
		return NULL;
	}

	bytes = standin_bytes(entry);
	desc.ByteWidth = bytes;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	if (entry->standin) {
		D3D11_BUFFER_DESC existing;

		entry->standin->GetDesc(&existing);
		if (existing.ByteWidth != bytes)
			release_standin(entry);
	}
	if (entry->standin_uav && static_cast<ShimUnorderedAccessView*>(entry->standin_uav)->mDesc.Format != (DXGI_FORMAT)view.format) {
		entry->standin_uav->Release();
		entry->standin_uav = NULL;
	}

	if (!entry->standin) {
		if (FAILED(dx12_shim->shim_device->CreateBuffer(&desc, NULL, &entry->standin))) {
			log_once(&failed, "Unable to create a buffer for a mod to write in place of one of the game. Not written.");
			return NULL;
		}
		dx12_standins_live++;
		entry->standin_drawn = true;
		Dx12DumpNote("Buffer %p of %llu bytes that the game's compute shaders write is %u bytes for the mods, which write a buffer of ours instead\n",
			view.resource, entry->desc.Width, bytes);
	}
	if (!entry->standin_uav) {
		uav_desc.Format = (DXGI_FORMAT)view.format;
		uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav_desc.Buffer.NumElements = bytes / element;
		if (FAILED(dx12_shim->shim_device->CreateUnorderedAccessView(entry->standin, &uav_desc, &entry->standin_uav))) {
			log_once(&failed, "Unable to create a buffer for a mod to write in place of one of the game. Not written.");
			return NULL;
		}
	}

	if (!entry->standin_valid || entry->standin_drawn || entry->standin_frame != dx12_frame) {
		dx12_shim->shim_context->ClearUnorderedAccessViewUint(entry->standin_uav, clear);
		// Nothing of a mod is in it until the dispatch is recorded,
		// see Dx12ScopeDispatched
		entry->standin_valid = false;
		entry->standin_drawn = false;
		entry->standin_frame = dx12_frame;
	}
	standins_pending.push_back(entry);
	return entry->standin_uav;
}

// After a dispatch of the game that is on its list: what it wrote that no mod
// wrote a buffer of ours for in the scope, if it had one, is the game's alone
// from here on. Where there is no telling what it wrote, that goes for every
// buffer.
static void game_wrote_buffers(Dx12GraphicsCommandList *list, UINT64 written_scope)
{
	const Dx12PipelineInfo *pipeline = Dx12ListPipeline(list);
	const Dx12ShaderInfo *shader = pipeline ? pipeline->shaders[DX12_STAGE_CS] : NULL;
	const Dx12RootSignatureInfo *signature = Dx12ListRootSignature(list, true);
	UINT64 mask = shader && shader->reflected && !shader->unmasked ? shader->uav : ~0ull, offset;
	Dx12DescriptorSlot view;
	ID3D12Resource *resource;
	unsigned long slot;

	if (!signature) {
		for (auto &entry : published) {
			if (entry.second.standin_scope != written_scope)
				entry.second.standin_valid = false;
		}
		return;
	}
	for (; _BitScanForward64(&slot, mask); mask &= mask - 1) {
		if (!Dx12ResolveRegister(&list->mState, signature, DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, slot, &view))
			continue;
		resource = view.resource;
		if (!resource && (!view.offset || !Dx12FindBuffer(view.offset, &resource, &offset)))
			continue;
		auto found = published.find({ resource, 0, PUBLISHED_RESOURCE });
		if (found != published.end() && found->second.standin && found->second.standin_scope != written_scope)
			found->second.standin_valid = false;
	}
}

void Dx12GameDispatched(Dx12GraphicsCommandList *list)
{
	Dx12ShimLock();
	// No scope of this dispatch has a number: 0 is none
	if (dx12_standins_live)
		game_wrote_buffers(list, 0);
	Dx12ShimUnlock();
}

// What a mod's compute shaders wrote in place of the buffer the game has at
// the address, for a draw of the mod's. NULL if there is no such thing.
static Published* written_buffer_at(UINT64 va, UINT64 *offset)
{
	ID3D12Resource *resource;
	Dx12ResourceInfo info;

	if (!Dx12FindBuffer(va, &resource, offset))
		return NULL;
	auto found = published.find({ resource, 0, PUBLISHED_RESOURCE });
	if (found == published.end() || !found->second.standin || !found->second.standin_valid)
		return NULL;
	// The address may have passed on to another buffer
	if (!Dx12GetResourceInfo(resource, &info) || found->second.standin_serial != info.serial
	    || memcmp(&found->second.desc, &info.desc, sizeof(info.desc)))
		return NULL;
	return &found->second;
}

// -----------------------------------------------------------------------------
// Applying what a mod bound to the game's list, and undoing it

static void set_root_argument(ID3D12GraphicsCommandList *list, bool compute, D3D12_ROOT_PARAMETER_TYPE type, UINT parameter, UINT64 value)
{
	D3D12_GPU_DESCRIPTOR_HANDLE table = { value };

	switch (type) {
	case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
		if (compute)
			list->SetComputeRootDescriptorTable(parameter, table);
		else
			list->SetGraphicsRootDescriptorTable(parameter, table);
		break;
	case D3D12_ROOT_PARAMETER_TYPE_CBV:
		if (compute)
			list->SetComputeRootConstantBufferView(parameter, value);
		else
			list->SetGraphicsRootConstantBufferView(parameter, value);
		break;
	case D3D12_ROOT_PARAMETER_TYPE_SRV:
		if (compute)
			list->SetComputeRootShaderResourceView(parameter, value);
		else
			list->SetGraphicsRootShaderResourceView(parameter, value);
		break;
	case D3D12_ROOT_PARAMETER_TYPE_UAV:
		if (compute)
			list->SetComputeRootUnorderedAccessView(parameter, value);
		else
			list->SetGraphicsRootUnorderedAccessView(parameter, value);
		break;
	}
}

static bool is_sampler_table(const Dx12RootSignatureInfo *signature, size_t parameter);
static bool prepare_side_tables();

// Puts back what the last draw of a mod changed, for the next one: the side
// heap stays set, with a table the mod changed set back to the copy of the
// game's that is in it
static void restore_overrides()
{
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12RootArguments *arguments = scope->compute ? &game->compute : &game->graphics;
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	UINT slot;

	for (const ChangedParameter &changed : scope->parameters) {
		if (changed.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && scope->heaps_changed)
			set_root_argument(list, scope->compute, changed.type, changed.parameter, scope->side_base[changed.parameter].ptr);
		else
			set_root_argument(list, scope->compute, changed.type, changed.parameter, arguments->values[changed.parameter]);
	}
	scope->parameters.clear();

	for (slot = 0; scope->vbs_changed; slot++, scope->vbs_changed >>= 1) {
		if (scope->vbs_changed & 1)
			list->IASetVertexBuffers(slot, 1, &game->vertex_buffers[slot]);
	}
	if (scope->ib_changed)
		list->IASetIndexBuffer(game->index_buffer.BufferLocation ? &game->index_buffer : NULL);
	scope->ib_changed = false;
	if (scope->topology_changed)
		list->IASetPrimitiveTopology(game->topology);
	scope->topology_changed = false;
}

// Sets the game's heaps on the list again in place of the side heap. With
// them every table the game had set is set again, of the graphics and of the
// compute root signature, which covers the tables a mod changed.
static void restore_heaps()
{
	const Dx12CmdState *game = &scope->list->mState;
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	size_t i;
	int side;

	list->SetDescriptorHeaps(game->heap_count, game->heaps);
	for (side = 0; side < 2; side++) {
		const Dx12RootSignatureInfo *signature = Dx12ListRootSignature(scope->list, !!side);
		const Dx12RootArguments *set = side ? &game->compute : &game->graphics;

		for (i = 0; signature && i < signature->parameters.size() && i < DX12_MAX_ROOT_ARGUMENTS; i++) {
			const Dx12DescriptorHeap *heap;

			if (signature->parameters[i].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !(set->set & (1ull << i)))
				continue;
			// One set while the game had another heap set is one it
			// has no use for any more
			heap = is_sampler_table(signature, i) ? game->sampler_heap : game->view_heap;
			if (heap && (set->values[i] < heap->mGpuStart || set->values[i] - heap->mGpuStart >= (UINT64)heap->mCount * heap->mIncrement))
				continue;
			set_root_argument(list, !!side, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, (UINT)i, set->values[i]);
		}
	}
	scope->heaps_changed = false;
	for (i = scope->parameters.size(); i--;) {
		if (scope->parameters[i].type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
			scope->parameters.erase(scope->parameters.begin() + i);
	}
}

// Puts the list back as the game left it, for a draw of the game or the end
// of the scope
static void restore_list()
{
	const Dx12CmdState *game = &scope->list->mState;
	ID3D12GraphicsCommandList *list = scope->list->mOrig;

	if (scope->heaps_changed)
		restore_heaps();

	restore_overrides();

	if (scope->variant)
		list->SetPipelineState(game->pipeline);
	scope->variant = NULL;
}

static const Dx12ShaderStage graphics_stages[] = { DX12_STAGE_VS, DX12_STAGE_PS, DX12_STAGE_GS, DX12_STAGE_HS, DX12_STAGE_DS };
static const Dx12ShaderStage compute_stages[] = { DX12_STAGE_CS };

// The highest register of a range that any of the shaders declares, as a
// number of registers from the first of the range. 0 if none does.
static UINT declared_in_range(const Dx12RootRange *range, const Dx12ShaderInfo *const *shaders, size_t shader_count)
{
	UINT reg, bits = range->type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ? 128 : 64, declared = 0;
	UINT64 mask[2] = {};
	size_t i;

	for (i = 0; i < shader_count; i++) {
		switch (range->type) {
		case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: mask[0] = shaders[i]->srv[0]; mask[1] = shaders[i]->srv[1]; break;
		case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: mask[0] = shaders[i]->cbv; break;
		default: mask[0] = shaders[i]->uav; break;
		}
		for (reg = range->base; reg < bits && reg - range->base < range->count; reg++) {
			if (mask[reg / 64] & (1ull << (reg % 64)))
				declared = max(declared, reg - range->base + 1);
		}
	}
	return declared;
}

// How many descriptors of a table the draws of the scope can read, from where
// the game set it: the registers the shaders of the pipeline state declare, or
// where they do not tell, up to the end of the furthest range of the root
// signature. False if that has no end.
static bool table_size(UINT parameter, UINT *size)
{
	static const D3D12_SHADER_VISIBILITY visibilities[DX12_STAGE_COUNT] = {
		D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_GEOMETRY,
		D3D12_SHADER_VISIBILITY_HULL, D3D12_SHADER_VISIBILITY_DOMAIN, D3D12_SHADER_VISIBILITY_ALL,
	};
	const Dx12RootSignatureInfo *signature = scope->signature;
	const Dx12ShaderStage *stages = scope->compute ? compute_stages : graphics_stages;
	size_t stage_count = scope->compute ? ARRAYSIZE(compute_stages) : ARRAYSIZE(graphics_stages);
	const Dx12ShaderInfo *shaders[DX12_STAGE_COUNT];
	bool declared = scope->pipeline->shaders_known, bounded = true, visible = false;
	UINT whole = 0, read = 0;
	size_t i, shader_count = 0;

	*size = 0;
	if (!signature || parameter >= signature->parameters.size())
		return false;
	const Dx12RootParameter &table = signature->parameters[parameter];

	for (i = 0; i < stage_count; i++) {
		const Dx12ShaderInfo *shader = scope->shaders[stages[i]];

		if (table.visibility != D3D12_SHADER_VISIBILITY_ALL && table.visibility != visibilities[stages[i]])
			continue;
		visible = true;
		if (!shader)
			continue;
		if (!shader->reflected || shader->unbounded_srv || shader->unmasked)
			declared = false;
		shaders[shader_count++] = shader;
	}
	// For a stage that is none of ours, like a mesh shader
	declared = declared && visible;

	for (const Dx12RootRange &range : table.ranges) {
		if (range.count == UINT_MAX)
			bounded = false;
		else
			whole = max(whole, range.offset + range.count);

		if (range.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
			declared = false;
		// Nothing of another space is declared where nothing is unmasked
		if (!declared || range.space)
			continue;
		UINT registers = declared_in_range(&range, shaders, shader_count);
		if (registers)
			read = max(read, range.offset + registers);
	}

	if (!declared && !bounded)
		return false;
	*size = declared ? read : whole;
	return true;
}

// A game may have made its heap as large as a heap can be, leaving no room for
// a reserve of ours (see Dx12DescriptorHeap::mReserve). The copies of its
// tables are then made in a heap of our own that replaces the game's for the
// scope, which takes copying every table the game had set (once a scope, see
// prepare_side_tables). Handed out and pinned to the list as the reserve is.
static ID3D12DescriptorHeap *side_heap;
static Dx12DescriptorArena side_arena(DX12_HEAP_RESERVE);

static bool side_alloc(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE *cpu, D3D12_GPU_DESCRIPTOR_HANDLE *gpu)
{
	D3D12_DESCRIPTOR_HEAP_DESC desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DX12_HEAP_RESERVE, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
	UINT start, increment = scope->list->mState.view_heap->mIncrement;
	Dx12DescriptorSpan *span;

	if (!side_heap && FAILED(dx12_shim->device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&side_heap))))
		return false;
	if (!side_arena.Alloc(count, &start, &span))
		return false;
	Dx12ShimPin(scope->list, span);

	*cpu = side_heap->GetCPUDescriptorHandleForHeapStart();
	cpu->ptr += (SIZE_T)start * increment;
	gpu->ptr = side_heap->GetGPUDescriptorHandleForHeapStart().ptr + (UINT64)start * increment;
	return true;
}

// Where to write a descriptor for the register so that the next draw reads
// it: in a copy of the table the game set, made the first time the table is
// asked about. NULL if there is no such place.
static D3D12_CPU_DESCRIPTOR_HANDLE* table_descriptor(const Dx12RegisterLocation *location, D3D12_CPU_DESCRIPTOR_HANDLE *descriptor)
{
	static bool no_heap, unbounded, no_room, no_side_room;
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12RootArguments *arguments = scope->compute ? &game->compute : &game->graphics;
	Dx12DescriptorHeap *heap = game->view_heap;
	ChangedParameter changed = {};
	Dx12DescriptorSpan *span;
	UINT64 base, first;
	UINT size;

	for (const ChangedParameter &existing : scope->parameters) {
		if (existing.parameter == location->parameter) {
			descriptor->ptr = existing.cpu.ptr + (SIZE_T)location->offset * heap->mIncrement;
			return descriptor;
		}
	}

	base = arguments->values[location->parameter];
	if (!heap || !heap->mGpuStart || base < heap->mGpuStart) {
		log_once(&no_heap, "A mod replaces something the game binds through a descriptor heap that is not known. Ignored.");
		return NULL;
	}

	if (!table_size(location->parameter, &size)) {
		stats.table_unbounded++;
		log_once(&unbounded, "A mod replaces something the game binds through a descriptor table without an end. Ignored.");
		return NULL;
	}
	// The game need not have a whole table's worth of descriptors where
	// it pointed the table, if its shaders do not read that far
	first = (base - heap->mGpuStart) / heap->mIncrement;
	if (location->offset >= size || first + location->offset >= heap->mCount)
		return NULL;
	size = (UINT)min((UINT64)size, heap->mCount - first);

	if (!heap->mReserve && heap->mMirror) {
		if (!scope->heaps_changed && (scope->side_impossible || !prepare_side_tables()))
			return NULL;
		if (!side_alloc(size, &changed.cpu, &changed.gpu)) {
			stats.table_no_room++;
			log_once(&no_side_room, "Out of descriptors of our own for what mods replace on the game's draws. Some were ignored.");
			return NULL;
		}
		stats.side_copies++;
	} else if (!heap->AllocReserved(size, &changed.cpu, &changed.gpu, &span)) {
		stats.table_no_room++;
		if (!no_room) {
			no_room = true;
			Dx12DumpNote("*** Out of descriptors for what mods replace on the game's draws. Some were ignored. "
				"(A table of %u for %s parameter %u of a heap of %u with %u more to spare, of which %u are taken by lists not done with them)\n",
				size, scope->compute ? "compute" : "graphics", location->parameter, heap->mCount, heap->mReserve,
				heap->mArena.mLive);
		}
		return NULL;
	} else {
		Dx12ShimPin(scope->list, span);
	}
	if (heap->mReserve)
		stats.table_copies++;
	stats.table_descriptors += size;
	dx12_shim->device->CopyDescriptorsSimple(size, changed.cpu, heap->MirrorCpuHandle(base), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	changed.parameter = location->parameter;
	changed.type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	scope->parameters.push_back(changed);

	descriptor->ptr = changed.cpu.ptr + (SIZE_T)location->offset * heap->mIncrement;
	return descriptor;
}

static bool is_sampler_table(const Dx12RootSignatureInfo *signature, size_t parameter)
{
	const Dx12RootParameter &table = signature->parameters[parameter];

	return table.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE && !table.ranges.empty()
	    && table.ranges[0].type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
}

// Sets the side heap on the list in place of the game's view heap and has the
// list use copies of every table the game had set. Once a scope, before a mod
// changes a table. False if that cannot be done, which is settled before
// anything is taken from the side heap: the scope then replaces nothing the
// game binds through a table.
static bool prepare_side_tables()
{
	static bool no_switch;
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12RootArguments *arguments = scope->compute ? &game->compute : &game->graphics;
	const Dx12RootSignatureInfo *signature = scope->signature;
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	Dx12DescriptorHeap *heap = game->view_heap;
	ID3D12DescriptorHeap *heaps[ARRAYSIZE(game->heaps)];
	UINT sizes[DX12_MAX_ROOT_ARGUMENTS];
	UINT64 base, first;
	bool found = false;
	size_t i, count = 0;

	if (signature)
		count = min(signature->parameters.size(), (size_t)DX12_MAX_ROOT_ARGUMENTS);

	for (i = 0; i < count; i++) {
		sizes[i] = 0;
		if (signature->parameters[i].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !(arguments->set & (1ull << i))
		    || is_sampler_table(signature, i))
			continue;
		base = arguments->values[i];
		first = base >= heap->mGpuStart ? (base - heap->mGpuStart) / heap->mIncrement : heap->mCount;
		// A table none of the shaders read has nothing to copy
		if (!table_size((UINT)i, &sizes[i]) || (sizes[i] && first >= heap->mCount)) {
			if (!no_switch) {
				Dx12DumpNote("  (%s parameter %zu of %u descriptors is set to %llu descriptors into a heap of %u)\n",
					scope->compute ? "compute" : "graphics", i, sizes[i], first, heap->mCount);
			}
			stats.table_unbounded++;
			break;
		}
		sizes[i] = (UINT)min((UINT64)sizes[i], heap->mCount - first);
	}
	for (size_t j = 0; j < game->heap_count; j++)
		found |= game->heaps[j] == heap->mOrig;
	if (i < count || !found) {
		log_once(&no_switch, "The tables of a draw could not all be copied to a heap of ours for what a mod replaces. Ignored.");
		scope->side_impossible = true;
		return false;
	}

	for (i = 0; i < count; i++) {
		D3D12_CPU_DESCRIPTOR_HANDLE cpu;

		scope->side_size[i] = sizes[i];
		if (!sizes[i])
			continue;
		if (!side_alloc(sizes[i], &cpu, &scope->side_base[i])) {
			stats.table_no_room++;
			log_once(&no_switch, "Out of descriptors of our own for the tables of a draw to be copied to a heap of ours. Ignored.");
			scope->side_impossible = true;
			return false;
		}
		stats.side_base_copies++;
		stats.table_descriptors += sizes[i];
		dx12_shim->device->CopyDescriptorsSimple(sizes[i], cpu, heap->MirrorCpuHandle(arguments->values[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	}

	// After the allocations: side_alloc makes the side heap, which is not
	// there if no table had anything to copy
	if (!side_heap) {
		scope->side_impossible = true;
		return false;
	}
	for (i = 0; i < game->heap_count; i++)
		heaps[i] = game->heaps[i] == heap->mOrig ? side_heap : game->heaps[i];
	list->SetDescriptorHeaps(game->heap_count, heaps);
	scope->heaps_changed = true;
	stats.side_switches++;
	// Tables are to be set after the heaps, the sampler ones as they were
	for (i = 0; i < count; i++) {
		UINT64 value;

		if (signature->parameters[i].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !(arguments->set & (1ull << i)))
			continue;
		if (is_sampler_table(signature, i))
			value = arguments->values[i];
		else if (sizes[i])
			value = scope->side_base[i].ptr;
		else
			// Read by none of the shaders, so anywhere in the heap
			// that is set will do
			value = side_heap->GetGPUDescriptorHandleForHeapStart().ptr;
		set_root_argument(list, scope->compute, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, (UINT)i, value);
	}
	return true;
}

// The copies in the side heap are as large as the shaders of the draw they
// were made for read, and the draws after it in the scope may be made with
// others: a mod's in place of the game's or the other way around. A table
// those read further into is copied again, before a mod changes any. False
// if one cannot be: the draw is then not to be made with the side heap.
static bool grow_side_tables()
{
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12RootArguments *arguments = scope->compute ? &game->compute : &game->graphics;
	const Dx12RootSignatureInfo *signature = scope->signature;
	Dx12DescriptorHeap *heap = game->view_heap;
	D3D12_CPU_DESCRIPTOR_HANDLE cpu;
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	UINT64 first;
	UINT size;
	size_t i, count = signature ? min(signature->parameters.size(), (size_t)DX12_MAX_ROOT_ARGUMENTS) : 0;

	for (i = 0; i < count; i++) {
		if (signature->parameters[i].type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE || !(arguments->set & (1ull << i))
		    || is_sampler_table(signature, i))
			continue;
		if (!table_size((UINT)i, &size)) {
			stats.table_unbounded++;
			return false;
		}
		if (!size)
			continue;
		first = arguments->values[i] >= heap->mGpuStart ? (arguments->values[i] - heap->mGpuStart) / heap->mIncrement : heap->mCount;
		// Not in the heap that is known, so nothing to copy it from
		if (first >= heap->mCount)
			return false;
		size = (UINT)min((UINT64)size, heap->mCount - first);
		if (size <= scope->side_size[i])
			continue;
		if (!side_alloc(size, &cpu, &gpu)) {
			stats.table_no_room++;
			return false;
		}
		stats.side_base_copies++;
		stats.table_descriptors += size;
		dx12_shim->device->CopyDescriptorsSimple(size, cpu, heap->MirrorCpuHandle(arguments->values[i]), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		scope->side_base[i] = gpu;
		scope->side_size[i] = size;
		set_root_argument(scope->list->mOrig, scope->compute, D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, (UINT)i, gpu.ptr);
	}
	return true;
}

// Has the list use the copies of the tables with what the mod has bound
static void bind_changed_tables()
{
	ID3D12GraphicsCommandList *list = scope->list->mOrig;

	for (const ChangedParameter &table : scope->parameters) {
		if (table.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
			set_root_argument(list, scope->compute, table.type, table.parameter, table.gpu.ptr);
	}
}

static void root_descriptor(const Dx12RegisterLocation *location, D3D12_ROOT_PARAMETER_TYPE type, UINT64 va)
{
	ChangedParameter changed = {};

	set_root_argument(scope->list->mOrig, scope->compute, type, location->parameter, va);

	for (const ChangedParameter &existing : scope->parameters) {
		if (existing.parameter == location->parameter)
			return;
	}
	changed.parameter = location->parameter;
	changed.type = type;
	scope->parameters.push_back(changed);
}

static bool locate(Dx12ShaderStage stage, D3D12_DESCRIPTOR_RANGE_TYPE type, UINT slot, Dx12RegisterLocation *location)
{
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12RootArguments *arguments = scope->compute ? &game->compute : &game->graphics;
	const Dx12ShaderInfo *shader = scope->shaders[stage];

	// A root signature has room for the registers of every shader it is
	// used with. What the shader of this draw does not declare it does not
	// read, and giving it that would cost a copy of the table for nothing:
	// IniParams is bound for every draw the engine looks at. A shader of a
	// mod may read a register the game's does not, which the copy of the
	// table may then be too short for if it was made for the game's.
	if (!shader)
		return false;
	if (shader->reflected) {
		switch (type) {
		case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
			if (!shader->unbounded_srv && slot < 128 && !(shader->srv[slot / 64] & (1ull << (slot % 64))))
				return false;
			break;
		case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
			if (slot < 64 && !(shader->cbv & (1ull << slot)))
				return false;
			break;
		case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
			if (slot < 64 && !(shader->uav & (1ull << slot)))
				return false;
			break;
		}
	}

	// Not finding it means the game's shaders do not read the register,
	// so there is nothing to replace
	return scope->signature && Dx12LocateRegister(scope->signature, stage, type, slot, location)
		&& (arguments->set & (1ull << location->parameter));
}

static void apply_shader_resource(Dx12ShaderStage stage, UINT slot, ID3D11ShaderResourceView *srv)
{
	ShimView *view = ShimViewOf(srv);
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	Dx12RegisterLocation location;

	// The game's own, put back by a mod that saved it
	if (view->resource->foreign || !view->handle.ptr)
		return;
	if (!locate(stage, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, slot, &location))
		return;

	if (!location.table) {
		if (view->resource->dimension != D3D11_RESOURCE_DIMENSION_BUFFER) {
			Dx12ScopeUnsupported("to bind a texture where the game binds a buffer by address");
			return;
		}
		if (Dx12ShimUseOnGameList(view->resource, scope->list))
			root_descriptor(&location, D3D12_ROOT_PARAMETER_TYPE_SRV, view->resource->resource->GetGPUVirtualAddress());
		return;
	}

	if (!Dx12ShimUseOnGameList(view->resource, scope->list) || !table_descriptor(&location, &descriptor))
		return;
	dx12_shim->device->CopyDescriptorsSimple(1, descriptor, view->handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

static void apply_constant_buffer(Dx12ShaderStage stage, UINT slot, const ShimStageState *bound)
{
	D3D12_CONSTANT_BUFFER_VIEW_DESC desc;
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	Dx12RegisterLocation location;
	UINT size, first;

	if (ShimResourceOf(bound->cbs[slot])->foreign)
		return;
	if (!locate(stage, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, slot, &location))
		return;

	desc.BufferLocation = Dx12ShimGameBufferAddress(bound->cbs[slot], scope->list, &size);
	first = bound->cb_first[slot] * 16;
	if (!desc.BufferLocation || first >= size)
		return;
	desc.BufferLocation += first;
	// Buffers are padded to what a view needs when they are created
	desc.SizeInBytes = bound->cb_count[slot] ? min((bound->cb_count[slot] * 16 + 255) & ~255u, size - first) : size - first;
	desc.SizeInBytes &= ~255u;
	if (!desc.SizeInBytes)
		return;

	if (!location.table)
		root_descriptor(&location, D3D12_ROOT_PARAMETER_TYPE_CBV, desc.BufferLocation);
	else if (table_descriptor(&location, &descriptor))
		dx12_shim->device->CreateConstantBufferView(&desc, descriptor);
}

// -----------------------------------------------------------------------------
// Skinning done by a compute shader
//
// Unity skins a mesh on D3D11 with a draw that streams the skinned vertices out
// to a buffer, and that draw is where a mod has its own mesh skinned: it finds
// the bone weights bound as a vertex buffer, binds its own vertices and
// weights and draws as many vertices as it has. On D3D12 the same renderer
// skins with a dispatch that reads vertices (t0) and weights (t1) and writes
// the skinned vertices (u0), a number of them given in a constant buffer (b0).
//
// The engine is shown such a dispatch as that draw, with the compute shader
// for its vertex shader. What a mod draws from there is dispatched with the
// game's shader, the mod's buffers and a buffer of ours to write to that is
// large enough for the mod's vertices, which is then bound wherever a draw of
// the game has the buffer it skins to.

// What the shader takes a thread group to be
const UINT SKIN_GROUP_VERTICES = 64;

struct SkinOutput {
	// Referenced. The vertices as they were last skinned, which draws read.
	ID3D12Resource *buffer;
	UINT64 bytes;
	// Referenced. What they are skinned to the next frame: the game may
	// draw with those of the frame before and skin after that, on one list
	// or on several executed together, and what was read there is in no
	// state to be written.
	ID3D12Resource *spare;
	UINT64 spare_bytes;
	UINT written_frame;
	UINT stride, vertices;
	// When it was last written or bound
	UINT frame;
	// The game's buffer it stands in for, to notice when the address has
	// passed on to something else
	ID3D12Resource *game;
	UINT64 stamp;
	// The recording that last wrote it, and left it in a state to be read
	Dx12GraphicsCommandList *list;
	UINT list_resets;
};

// By the address of the game's buffer
static std::unordered_map<UINT64, SkinOutput> skin_outputs;
// For the threads recording to tell without the lock whether a vertex buffer
// may be one of those: a bit per address, which stays set until the outputs
// are gone through again
static volatile LONG64 skin_va_filter[64];
static volatile LONG skin_outputs_live;

static UINT skin_filter_bit(UINT64 va)
{
	return (UINT)((va * 0x9e3779b97f4a7c15ull) >> 52);
}

static bool skin_filter_has(UINT64 va)
{
	UINT bit = skin_filter_bit(va);

	return (skin_va_filter[bit >> 6] >> (bit & 63)) & 1;
}

static bool is_skinning_shader(const Dx12ShaderInfo *shader)
{
	return shader && shader->stage == DX12_STAGE_CS && shader->reflected && !shader->unbounded_srv
		&& shader->srv[0] == 7 && !shader->srv[1] && shader->uav == 1 && shader->cbv == 1 && shader->cb_bytes[0] == 4;
}

// A view of count elements that make up a whole buffer says how large they are
static bool skin_view(const Dx12DescriptorSlot *view, UINT count, UINT64 *va, UINT *stride, ID3D12Resource **resource)
{
	UINT64 elements = view->offset + view->size;
	Dx12ResourceInfo info;

	// D3D12_SRV_DIMENSION_BUFFER and D3D12_UAV_DIMENSION_BUFFER are both 1
	if (!view->resource || view->dimension != 1 || view->size != count)
		return false;
	if (!Dx12GetResourceInfo(view->resource, &info) || info.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !info.va)
		return false;
	if (info.desc.Width % elements || info.desc.Width / elements > 2048)
		return false;

	*stride = (UINT)(info.desc.Width / elements);
	*va = info.va + view->offset * *stride;
	if (resource)
		*resource = view->resource;
	return true;
}

// Whether a dispatch with a shader that looks the part is one
static bool resolve_skin(Dx12GraphicsCommandList *list, const Dx12RootSignatureInfo *signature, UINT x, UINT y, UINT z,
		SkinDispatch *skin)
{
	const Dx12CmdState *state = &list->mState;
	Dx12DescriptorSlot source, weights, output;
	Dx12RegisterLocation location;
	UINT stride;
	size_t i;

	if (!signature || y != 1 || z != 1)
		return false;
	if (!Dx12ResolveRegister(state, signature, DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, &source)
	    || !Dx12ResolveRegister(state, signature, DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, &weights)
	    || !Dx12ResolveRegister(state, signature, DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, &output))
		return false;

	skin->count = source.size;
	if (!skin->count || x != (skin->count + SKIN_GROUP_VERTICES - 1) / SKIN_GROUP_VERTICES)
		return false;
	if (!skin_view(&source, skin->count, &skin->source_va, &skin->stride, NULL)
	    || !skin_view(&weights, skin->count, &skin->weights_va, &skin->weight_stride, NULL)
	    || !skin_view(&output, skin->count, &skin->output_va, &stride, &skin->output)
	    || stride != skin->stride)
		return false;

	skin->constants_parameter = -1;
	if (Dx12LocateRegister(signature, DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0, &location))
		return true;
	for (i = 0; i < signature->parameters.size() && i < DX12_MAX_ROOT_ARGUMENTS; i++) {
		const Dx12RootParameter *parameter = &signature->parameters[i];

		if (parameter->type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && !parameter->space && !parameter->reg) {
			skin->constants_parameter = (int)i;
			return true;
		}
	}
	return false;
}

static void rebuild_skin_filter();

static void forget_skin_output(UINT64 va)
{
	auto found = skin_outputs.find(va);

	if (found == skin_outputs.end())
		return;
	// The lists that use it have it pinned
	if (found->second.buffer)
		found->second.buffer->Release();
	if (found->second.spare)
		found->second.spare->Release();
	skin_outputs.erase(found);
	rebuild_skin_filter();
}

// What stands in for the game's buffer at the address, if anything does
static SkinOutput* find_skin_output(UINT64 va)
{
	auto found = skin_outputs.find(va);
	ID3D12Resource *resource;
	UINT64 stamp, offset;

	if (found == skin_outputs.end())
		return NULL;

	// Taken before asking, see Dx12FindBufferStamp
	stamp = Dx12FindBufferStamp(va);
	if (stamp != found->second.stamp) {
		if (!Dx12FindBuffer(va, &resource, &offset) || resource != found->second.game) {
			forget_skin_output(va);
			return NULL;
		}
		found->second.stamp = stamp;
	}
	return &found->second;
}

// The vertices a D3D12 renderer skins need not be those of its D3D11
// counterpart to the bit: Unity has their normals and tangents made
// orthonormal for the compute shader, which nothing can be undone from, so the
// hash a mod has of them matches nothing. The weights are the same, and a mod
// that goes by the vertices has a section for the weights as well, named alike:
// ...Position and ...Blend. So the vertices are given the hash of the former
// where the weights have that of the latter.
//
// Older mods have no section for the weights, only ...Position and
// ...VertexLimitRaise. The latter has the hash of the buffer the D3D11
// renderer streamed the skinned vertices out to, which it creates without
// contents, so the hash is that of its description alone: as many bytes as the
// mesh has vertices, bound as a vertex buffer, a shader resource and a stream
// output. That only narrows a dispatch down to the meshes of as many vertices,
// so the weights of one that skins that many are noted with where it skins to.
// A draw of what was skinned there with the index buffer or texture
// coordinates of the mod's ...IB or ...Texcoord section says the mesh is the
// mod's: its weights go by the ...Position section from then on.
static std::unordered_map<uint32_t, uint32_t> skin_aliases;
static UINT skin_aliases_config;

struct SkinCandidate {
	uint32_t position;
	// 0 for none
	uint32_t ib, texcoord;
};
// By the hash of the buffer skinned to
static std::unordered_map<uint32_t, std::vector<SkinCandidate>> skin_candidates;

struct SkinNoted {
	uint32_t output_hash, weights_hash;
	UINT64 source_va, weights_va;
	UINT frame;
};
// By the address skinned to
static std::unordered_map<UINT64, SkinNoted> skin_noted;
// For draw_is_boring to tell without the lock which draws to look at for that:
// a bit per address. Such a draw may be one the mods have nothing for until
// the mesh is known to be theirs.
static volatile LONG64 skin_noted_filter[64];
static volatile LONG skin_noted_live;
// How long a noted mesh has to be drawn as a mod's, and those that were not:
// the draws of a mesh of the same size that is no mod's are not to be looked
// at for good
const UINT SKIN_CONFIRM_FRAMES = 60;
static std::set<std::pair<UINT64, uint32_t>> skin_rejected;

// The weights of the meshes that draws said are the mods', and the hash of the
// vertices they go with
static std::unordered_map<uint32_t, uint32_t> skin_learned;
// For skin_is_boring to tell without the lock which vertices those are: a bit
// per address of them. Their dispatch has to be looked at again however much
// it was found to be nothing to the mods before.
static volatile LONG64 skin_learned_filter[64];
static volatile LONG skin_learned_live;

static bool va_filter_has(const volatile LONG64 *filter, UINT64 va)
{
	UINT bit = skin_filter_bit(va);

	return (filter[bit >> 6] >> (bit & 63)) & 1;
}

static void va_filter_add(volatile LONG64 *filter, volatile LONG *live, UINT64 va)
{
	UINT bit = skin_filter_bit(va);

	InterlockedOr64(&filter[bit >> 6], 1ll << (bit & 63));
	InterlockedExchange(live, 1);
}

static void rebuild_noted_filter()
{
	LONG64 filter[ARRAYSIZE(skin_noted_filter)] = {};
	size_t i;

	for (const auto &noted : skin_noted) {
		UINT bit = skin_filter_bit(noted.first);

		filter[bit >> 6] |= 1ll << (bit & 63);
	}
	for (i = 0; i < ARRAYSIZE(filter); i++) {
		if (skin_noted_filter[i] != filter[i])
			InterlockedExchange64(&skin_noted_filter[i], filter[i]);
	}
	InterlockedExchange(&skin_noted_live, !skin_noted.empty());
}

static bool ends_with(const std::wstring &name, const wchar_t *end, std::wstring *stem)
{
	size_t length = wcslen(end);

	if (name.size() < length || _wcsicmp(name.c_str() + name.size() - length, end))
		return false;
	*stem = name.substr(0, name.size() - length);
	std::transform(stem->begin(), stem->end(), stem->begin(), towlower);
	return true;
}

static void collect_skin_aliases()
{
	std::unordered_map<std::wstring, uint32_t> weights, outputs, ibs, texcoords;
	std::wstring stem;
	size_t i;

	if (skin_aliases_config == config_generation)
		return;
	skin_aliases_config = config_generation;

	skin_aliases.clear();
	skin_candidates.clear();
	skin_noted.clear();
	rebuild_noted_filter();
	skin_rejected.clear();
	skin_learned.clear();
	for (i = 0; i < ARRAYSIZE(skin_learned_filter); i++)
		InterlockedExchange64(&skin_learned_filter[i], 0);
	InterlockedExchange(&skin_learned_live, 0);

	for (const auto &entry : G->mTextureOverrideMap) {
		for (const TextureOverride &texture_override : entry.second) {
			if (ends_with(texture_override.ini_section, L"Blend", &stem))
				weights[stem] = entry.first;
			else if (ends_with(texture_override.ini_section, L"VertexLimitRaise", &stem))
				outputs[stem] = entry.first;
			else if (ends_with(texture_override.ini_section, L"IB", &stem))
				ibs[stem] = entry.first;
			else if (ends_with(texture_override.ini_section, L"Texcoord", &stem))
				texcoords[stem] = entry.first;
		}
	}
	for (const auto &entry : G->mTextureOverrideMap) {
		for (const TextureOverride &texture_override : entry.second) {
			SkinCandidate candidate = { entry.first };

			if (!ends_with(texture_override.ini_section, L"Position", &stem))
				continue;
			auto found = weights.find(stem);
			if (found != weights.end()) {
				skin_aliases[found->second] = entry.first;
				continue;
			}
			found = outputs.find(stem);
			if (found == outputs.end())
				continue;
			auto ib = ibs.find(stem);
			auto texcoord = texcoords.find(stem);
			candidate.ib = ib != ibs.end() ? ib->second : 0;
			candidate.texcoord = texcoord != texcoords.end() ? texcoord->second : 0;
			// Nothing a draw could tell it by
			if (!candidate.ib && !candidate.texcoord)
				continue;
			skin_candidates[found->second].push_back(candidate);
		}
	}
}

static uint32_t skin_output_hash(const SkinDispatch *skin)
{
	D3D11_BUFFER_DESC desc = {};

	desc.ByteWidth = skin->count * skin->stride;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_STREAM_OUTPUT;
	return crc32c_hw(0, &desc, sizeof(desc));
}

static uint32_t published_hash(ID3D11Resource *resource)
{
	uint32_t hash = 0;

	EnterCriticalSectionPretty(&G->mResourcesLock);
	auto found = G->mResources.find(resource);
	if (found != G->mResources.end())
		hash = found->second.hash;
	LeaveCriticalSection(&G->mResourcesLock);
	return hash;
}

static void note_skin_candidate(const SkinDispatch *skin, uint32_t weights_hash)
{
	uint32_t output_hash = skin_output_hash(skin);

	auto noted = skin_noted.find(skin->output_va);
	if (noted != skin_noted.end()) {
		// The same mesh again: its time to be drawn runs from the first
		if (noted->second.output_hash == output_hash && noted->second.weights_hash == weights_hash
		    && noted->second.source_va == skin->source_va)
			return;
		// Another mesh is skinned there now, which the draws to come are
		// of
		skin_noted.erase(noted);
		rebuild_noted_filter();
	}
	if (!skin_candidates.count(output_hash) || skin_rejected.count({ skin->output_va, weights_hash }))
		return;
	// Not to grow without end on a game that skins to ever new places
	if (skin_noted.size() >= 4096)
		return;
	skin_noted[skin->output_va] = { output_hash, weights_hash, skin->source_va, skin->weights_va, dx12_frame };
	va_filter_add(skin_noted_filter, &skin_noted_live, skin->output_va);
}

// Before the engine looks at a dispatch that skins to where a mesh was noted,
// which need not get as far as note_skin_candidate: if it is of another mesh,
// mod's or not, the draws to come are of that one
static void forget_noted_other(const SkinDispatch *skin)
{
	auto noted = skin_noted.find(skin->output_va);

	if (noted == skin_noted.end())
		return;
	if (noted->second.source_va == skin->source_va && noted->second.weights_va == skin->weights_va
	    && noted->second.output_hash == skin_output_hash(skin))
		return;
	skin_noted.erase(noted);
	rebuild_noted_filter();
}

static void alias_skin_vertices(Published *vertices, const SkinDispatch *skin)
{
	Published *weights;
	uint32_t hash, weights_hash;
	bool learned = false;

	collect_skin_aliases();
	if (skin_aliases.empty() && skin_candidates.empty())
		return;

	hash = published_hash(vertices->resource);
	if (G->mTextureOverrideMap.count(hash))
		return;
	weights = publish_buffer(skin->weights_va, (UINT64)skin->count * skin->weight_stride, PUBLISHED_SKIN_BUFFER, skin->weight_stride, NULL);
	if (!weights)
		return;
	weights_hash = published_hash(weights->resource);
	auto found = skin_aliases.find(weights_hash);
	if (found == skin_aliases.end()) {
		// What has no hash of its contents goes by 0, which every other
		// such mesh would be taken for
		if (!weights->has_data_hash)
			return;
		found = skin_learned.find(weights_hash);
		if (found == skin_learned.end()) {
			note_skin_candidate(skin, weights_hash);
			return;
		}
		learned = true;
	}

	set_resource_hash(vertices->resource, D3D11_RESOURCE_DIMENSION_BUFFER, found->second, vertices->data_hash, NULL);
	stats.skin_aliased++;
	if (G->dx12_dump_hashes) {
		static std::unordered_set<UINT64> noted;

		if (noted.size() < 400 && noted.insert((UINT64)hash << 32 | found->second).second) {
			Dx12DumpNote("Skinning: the vertices that would have been hashed as %08x go by %08x, the hash a mod has for those skinned with "
				"the weights of hash %08x%s\n", hash, found->second, found->first,
				learned ? ", as a draw of what was skinned told" : "");
		}
	}
}

// With the scope of a draw the engine looked at open: whether it draws what a
// dispatch noted by note_skin_candidate skinned, with the index buffer or
// texture coordinates of a mod for meshes of that many vertices
static void confirm_skin_candidate(bool indexed)
{
	const Dx12CmdState *game = &scope->list->mState;
	uint32_t ib = 0, texcoord = 0, position = 0;
	bool ib_asked = false, texcoord_asked = false;
	DXGI_FORMAT format;
	UINT stride;

	collect_skin_aliases();
	auto noted = skin_noted.find(game->vertex_buffers[0].BufferLocation);
	if (noted == skin_noted.end())
		return;
	auto candidates = skin_candidates.find(noted->second.output_hash);
	if (candidates == skin_candidates.end())
		return;

	for (const SkinCandidate &candidate : candidates->second) {
		if (candidate.ib && indexed && !ib_asked) {
			ID3D11Buffer *buffer = Dx12ScopeIndexBuffer(&format);

			ib = buffer ? published_hash(buffer) : 0;
			ib_asked = true;
		}
		if (candidate.texcoord && !texcoord_asked) {
			ID3D11Buffer *buffer = Dx12ScopeVertexBuffer(1, &stride);

			texcoord = buffer ? published_hash(buffer) : 0;
			texcoord_asked = true;
		}
		if ((!candidate.ib || !ib || candidate.ib != ib) && (!candidate.texcoord || !texcoord || candidate.texcoord != texcoord))
			continue;
		// Mods for different meshes that this draw would do for alike
		if (position && position != candidate.position)
			return;
		position = candidate.position;
	}
	// Another draw of it may be the one, as of a pass that the mods have
	// left alone
	if (!position)
		return;

	skin_learned[noted->second.weights_hash] = position;
	va_filter_add(skin_learned_filter, &skin_learned_live, noted->second.source_va);
	if (G->dx12_dump_hashes) {
		Dx12DumpNote("Skinning: a draw of the vertices skinned with the weights of hash %08x had the index buffer %08x and texture "
			"coordinates %08x, which make them those of hash %08x\n", noted->second.weights_hash, ib, texcoord, position);
	}
	skin_noted.erase(noted);
	rebuild_noted_filter();
}

static void expire_skin_candidates()
{
	bool expired = false;

	for (auto i = skin_noted.begin(); i != skin_noted.end();) {
		if (dx12_frame - i->second.frame > SKIN_CONFIRM_FRAMES) {
			if (skin_rejected.size() < 4096)
				skin_rejected.insert({ i->first, i->second.weights_hash });
			i = skin_noted.erase(i);
			expired = true;
		} else {
			++i;
		}
	}
	if (expired)
		rebuild_noted_filter();
}

static bool skin_output_at(UINT64 va)
{
	SkinOutput *output = skin_outputs.empty() ? NULL : find_skin_output(va);

	return output && output->vertices;
}

// 0 if nothing stands in for the game's buffer at the address
static UINT64 skin_output_bytes(UINT64 va)
{
	SkinOutput *output = skin_outputs.empty() ? NULL : find_skin_output(va);

	return output ? (UINT64)output->vertices * output->stride : 0;
}

// After outputs were taken out, so that the threads recording do not stop for
// the addresses of those
static void rebuild_skin_filter()
{
	LONG64 filter[ARRAYSIZE(skin_va_filter)] = {};
	size_t i;

	for (const auto &output : skin_outputs) {
		UINT bit = skin_filter_bit(output.first);

		filter[bit >> 6] |= 1ll << (bit & 63);
	}

	// A word at a time: each has the bits of what is left before and after
	for (i = 0; i < ARRAYSIZE(filter); i++) {
		if (skin_va_filter[i] != filter[i])
			InterlockedExchange64(&skin_va_filter[i], filter[i]);
	}
	InterlockedExchange(&skin_outputs_live, (LONG)skin_outputs.size());
}

static void forget_unused_skin_outputs()
{
	for (auto j = skin_outputs.begin(); j != skin_outputs.end();) {
		if (dx12_frame - j->second.frame > 600) {
			if (j->second.buffer)
				j->second.buffer->Release();
			if (j->second.spare)
				j->second.spare->Release();
			j = skin_outputs.erase(j);
		} else {
			++j;
		}
	}
	rebuild_skin_filter();
}

// A constant buffer holding nothing but a number of vertices. There are as
// many of these as meshes of different sizes that mods have skinned, and they
// are kept: a game list may use one at any time.
static std::unordered_map<UINT, UINT64> skin_count_vas;
static BYTE *skin_count_page;
static UINT64 skin_count_page_va;
static UINT skin_count_page_used;
const UINT SKIN_COUNT_PAGE_SIZE = 65536;
const UINT SKIN_COUNT_SIZE = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;

static UINT64 skin_count_va(UINT vertices)
{
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_UPLOAD };
	D3D12_RESOURCE_DESC desc = {};
	D3D12_RANGE no_read = {};
	ID3D12Resource *page;
	BYTE *cpu;
	UINT64 va;

	auto found = skin_count_vas.find(vertices);
	if (found != skin_count_vas.end())
		return found->second;

	if (!skin_count_page || skin_count_page_used + SKIN_COUNT_SIZE > SKIN_COUNT_PAGE_SIZE) {
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = SKIN_COUNT_PAGE_SIZE;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&page))))
			return 0;
		if (FAILED(page->Map(0, &no_read, (void**)&cpu))) {
			page->Release();
			return 0;
		}
		skin_count_page = cpu;
		skin_count_page_va = page->GetGPUVirtualAddress();
		skin_count_page_used = 0;
	}

	cpu = skin_count_page + skin_count_page_used;
	memset(cpu, 0, SKIN_COUNT_SIZE);
	memcpy(cpu, &vertices, sizeof(vertices));
	va = skin_count_page_va + skin_count_page_used;
	skin_count_page_used += SKIN_COUNT_SIZE;
	skin_count_vas[vertices] = va;
	return va;
}

// Has the dispatch read or write elements of a buffer of ours through a t or
// u register
static bool skin_bind_buffer(UINT reg, bool uav, ID3D12Resource *resource, UINT64 first, UINT elements, UINT stride)
{
	D3D12_DESCRIPTOR_RANGE_TYPE type = uav ? D3D12_DESCRIPTOR_RANGE_TYPE_UAV : D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
	D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	Dx12RegisterLocation location;

	if (!locate(DX12_STAGE_CS, type, reg, &location))
		return false;

	if (!location.table) {
		root_descriptor(&location, uav ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_SRV,
			resource->GetGPUVirtualAddress() + first * stride);
		return true;
	}

	if (!table_descriptor(&location, &descriptor))
		return false;
	if (uav) {
		uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
		uav_desc.Buffer.FirstElement = first;
		uav_desc.Buffer.NumElements = elements;
		uav_desc.Buffer.StructureByteStride = stride;
		dx12_shim->device->CreateUnorderedAccessView(resource, NULL, &uav_desc, descriptor);
	} else {
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Buffer.FirstElement = first;
		srv_desc.Buffer.NumElements = elements;
		srv_desc.Buffer.StructureByteStride = stride;
		dx12_shim->device->CreateShaderResourceView(resource, &srv_desc, descriptor);
	}
	return true;
}

// The vertex buffer a mod bound to the slot in place of what the dispatch
// reads through the register. What a mod left alone stays the game's: true
// unless the mod's cannot be used.
static bool skin_bind_mod_buffer(UINT reg, UINT slot, UINT stride, UINT vertices)
{
	const ShimState *mod = dx12_shim->shim_context->mState;
	ID3D11Buffer *buffer = mod->vbs[slot];
	ID3D12Resource *resource;
	UINT64 offset;
	UINT size, start = mod->vb_offsets[slot];

	if (!buffer || ShimResourceOf(buffer)->foreign)
		return true;
	if (!Dx12ShimGameBufferView(buffer, scope->list, stride, &resource, &offset, &size))
		return false;
	// The shader reads whole elements from the start of a view
	if (start >= size || (offset + start) % stride || size - start < stride)
		return false;

	return skin_bind_buffer(reg, false, resource, (offset + start) / stride, min(vertices, (size - start) / stride), stride);
}

static const UINT skin_input_slots[2] = { 0, SKIN_BLEND_SLOT };

static void note_skin_inputs()
{
	const ShimState *mod = dx12_shim->shim_context->mState;
	UINT i;

	scope->mod_skinned = true;
	for (i = 0; i < ARRAYSIZE(skin_input_slots); i++) {
		scope->skinned_vbs[i] = mod->vbs[skin_input_slots[i]];
		scope->skinned_offsets[i] = mod->vb_offsets[skin_input_slots[i]];
	}
}

// Whether the mod has what it last skinned with bound still
static bool mod_skin_inputs_bound()
{
	const ShimState *mod = dx12_shim->shim_context->mState;
	UINT i;

	if (!scope->mod_skinned)
		return false;
	for (i = 0; i < ARRAYSIZE(skin_input_slots); i++) {
		if (mod->vbs[skin_input_slots[i]] != scope->skinned_vbs[i]
		    || mod->vb_offsets[skin_input_slots[i]] != scope->skinned_offsets[i])
			return false;
	}
	return true;
}

static bool skin_bind_count(const SkinDispatch *skin, UINT vertices)
{
	D3D12_CONSTANT_BUFFER_VIEW_DESC desc;
	D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
	Dx12RegisterLocation location;

	if (skin->constants_parameter >= 0) {
		scope->list->mOrig->SetComputeRoot32BitConstant(skin->constants_parameter, vertices, 0);
		return true;
	}

	desc.BufferLocation = skin_count_va(vertices);
	desc.SizeInBytes = SKIN_COUNT_SIZE;
	if (!desc.BufferLocation || !locate(DX12_STAGE_CS, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 0, &location))
		return false;

	if (!location.table) {
		root_descriptor(&location, D3D12_ROOT_PARAMETER_TYPE_CBV, desc.BufferLocation);
		return true;
	}
	if (!table_descriptor(&location, &descriptor))
		return false;
	dx12_shim->device->CreateConstantBufferView(&desc, descriptor);
	return true;
}

// A draw of a mod in the scope of a dispatch that skins: skins that many
// vertices of what the mod has bound
static void skin_draw(UINT vertices)
{
	static bool no_buffer;
	static std::unordered_set<std::string> reported;
	const char *failed = NULL;
	const D3D12_RESOURCE_STATES read_state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	const SkinDispatch *skin = scope->skin;
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_BARRIER barrier = {};
	D3D12_RESOURCE_DESC desc = {};
	UINT64 bytes = (UINT64)vertices * skin->stride;
	SkinOutput *output;

	if (!vertices)
		return;

	output = &skin_outputs[skin->output_va];
	if (output->written_frame != dx12_frame) {
		std::swap(output->buffer, output->spare);
		std::swap(output->bytes, output->spare_bytes);
		output->list = NULL;
	}
	if (!output->buffer || output->bytes < bytes || output->stride != skin->stride) {
		// The lists that use it have it pinned
		if (output->buffer)
			output->buffer->Release();
		if (output->stride != skin->stride) {
			if (output->spare)
				output->spare->Release();
			output->spare = NULL;
			output->spare_bytes = 0;
			output->vertices = 0;
		}
		output->buffer = NULL;
		output->bytes = 0;
		output->list = NULL;

		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COMMON, NULL, IID_PPV_ARGS(&output->buffer)))) {
			log_once(&no_buffer, "Unable to create a buffer for the vertices of a mod to be skinned to. Not skinned.");
			forget_skin_output(skin->output_va);
			return;
		}
		output->bytes = bytes;
		output->stride = skin->stride;
	}

	restore_overrides();
	if (!skin_bind_mod_buffer(0, 0, skin->stride, vertices))
		failed = "the mod's vertices";
	else if (!skin_bind_mod_buffer(1, SKIN_BLEND_SLOT, skin->weight_stride, vertices))
		failed = "the mod's bone weights";
	else if (!skin_bind_buffer(0, true, output->buffer, 0, vertices, skin->stride))
		failed = "the buffer to skin to";
	else if (!skin_bind_count(skin, vertices))
		failed = "the number of vertices";
	if (failed) {
		const ShimState *mod = dx12_shim->shim_context->mState;

		stats.skin_failed++;
		if (reported.insert(failed).second) {
			Dx12DumpNote("*** A mod's mesh was not skinned, as the game's shader could not be given %s. (%u vertices where the game "
				"has %u of %u bytes with weights of %u; the mod's buffers have strides %u and %u at offsets %u and %u)\n",
				failed, vertices, skin->count, skin->stride, skin->weight_stride, mod->vb_strides[0],
				mod->vb_strides[SKIN_BLEND_SLOT], mod->vb_offsets[0], mod->vb_offsets[SKIN_BLEND_SLOT]);
		}
		restore_overrides();
		if (skin->constants_parameter >= 0)
			list->SetComputeRoot32BitConstant(skin->constants_parameter, skin->count, 0);
		forget_skin_output(skin->output_va);
		return;
	}
	bind_changed_tables();

	// A buffer goes back to the common state when the lists it was used by
	// have been executed, and from there to any other by being used. Only
	// what this recording did to it before takes a barrier to undo.
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = output->buffer;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	if (output->list == scope->list && output->list_resets == scope->list->mResets) {
		barrier.Transition.StateBefore = read_state;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		list->ResourceBarrier(1, &barrier);
	}
	list->Dispatch((vertices + SKIN_GROUP_VERTICES - 1) / SKIN_GROUP_VERTICES, 1, 1);
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	barrier.Transition.StateAfter = read_state;
	list->ResourceBarrier(1, &barrier);

	// Root constants are not among what restore_list puts back
	if (skin->constants_parameter >= 0)
		list->SetComputeRoot32BitConstant(skin->constants_parameter, skin->count, 0);

	Dx12ShimPin(scope->list, output->buffer);
	output->vertices = vertices;
	output->frame = dx12_frame;
	output->written_frame = dx12_frame;
	output->game = skin->output;
	output->stamp = Dx12FindBufferStamp(skin->output_va);
	output->list = scope->list;
	output->list_resets = scope->list->mResets;

	InterlockedOr64(&skin_va_filter[skin_filter_bit(skin->output_va) >> 6], 1ll << (skin_filter_bit(skin->output_va) & 63));
	InterlockedExchange(&skin_outputs_live, (LONG)skin_outputs.size());
	stats.mod_draws++;
	stats.skin_replaced++;
	note_skin_inputs();

	if (G->dx12_dump_hashes) {
		static std::unordered_set<UINT64> noted;
		const ShimState *mod = dx12_shim->shim_context->mState;

		if (noted.size() < 400 && noted.insert((UINT64)skin->count << 32 | vertices).second) {
			Dx12DumpNote("Skinning: a mod had %u vertices skinned where the game has %u, of %u bytes with weights of %u. "
				"It bound vertices of stride %u and weights of stride %u\n", vertices, skin->count, skin->stride,
				skin->weight_stride, mod->vbs[0] ? mod->vb_strides[0] : 0,
				mod->vbs[SKIN_BLEND_SLOT] ? mod->vb_strides[SKIN_BLEND_SLOT] : 0);
		}
	}
}

// For working out offline what the mods' hashes of the vertices a game skins
// are made of, where those of D3D12 do not match: what no mod took is written
// to a file, once
struct SkinDump {
	ID3D12Resource *readback;
	UINT64 bytes, fence;
	UINT stride, count;
	uint32_t hash;
};
static std::vector<SkinDump> skin_dumps;
static std::unordered_set<UINT64> skin_dumped;

static void dump_skin_source(const SkinDispatch *skin, UINT64 shader)
{
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_BARRIER barrier;
	D3D12_RESOURCE_DESC desc = {};
	ID3D12GraphicsCommandList *list;
	Dx12BufferHash hash = {}, weights_hash = {};
	ID3D12Resource *resource, *weights;
	UINT64 offset, weights_offset;
	SkinDump dump = {};
	bool needed, hashed, weights_hashed;

	if (skin_dumped.size() >= 600 || !skin_dumped.insert(skin->source_va ^ (UINT64)skin->count << 48).second)
		return;
	if (!Dx12FindBuffer(skin->source_va, &resource, &offset))
		return;
	hashed = Dx12FindBufferHash(resource, offset, &hash);
	weights_hashed = Dx12FindBuffer(skin->weights_va, &weights, &weights_offset) && Dx12FindBufferHash(weights, weights_offset, &weights_hash);

	Dx12DumpNote("Skinning: no mod for %u vertices of %u bytes with weights of %u, shader %016llx. "
		"D3D11 would have hashed the vertices as %08x or %08x (%llu bytes%s) and the weights as %08x or %08x (%llu bytes%s)\n",
		skin->count, skin->stride, skin->weight_stride, shader,
		Dx12BufferDescHash(&hash, D3D11_BIND_VERTEX_BUFFER), Dx12BufferDescHash(&hash, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE),
		hash.bytes, hashed ? "" : ", not uploaded as one",
		Dx12BufferDescHash(&weights_hash, D3D11_BIND_VERTEX_BUFFER), Dx12BufferDescHash(&weights_hash, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE),
		weights_hash.bytes, weights_hashed ? "" : ", not uploaded as one");

	dump.bytes = (UINT64)skin->count * skin->stride;
	if (skin_dumps.size() >= 16 || dump.bytes > (16 << 20) || !reach_barrier(resource, &barrier, &needed))
		return;

	desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	desc.Width = dump.bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&dump.readback))))
		return;

	list = Dx12ShimList();
	if (needed)
		list->ResourceBarrier(1, &barrier);
	list->CopyBufferRegion(dump.readback, 0, resource, offset, dump.bytes);
	if (needed) {
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		list->ResourceBarrier(1, &barrier);
	}
	// As in start_reach
	Dx12ShimDeferRelease(resource);

	dump.fence = Dx12ShimPendingFence();
	dump.stride = skin->stride;
	dump.count = skin->count;
	dump.hash = Dx12BufferDescHash(&hash, D3D11_BIND_VERTEX_BUFFER);
	skin_dumps.push_back(dump);
}

static void finish_skin_dumps()
{
	D3D12_RANGE nothing = {};
	wchar_t path[MAX_PATH], name[64];
	FILE *file;
	BYTE *data;
	size_t i;

	for (i = 0; i < skin_dumps.size();) {
		SkinDump *dump = &skin_dumps[i];
		D3D12_RANGE range = { 0, (SIZE_T)dump->bytes };

		if (!Dx12ShimFenceReached(dump->fence)) {
			i++;
			continue;
		}

		data = NULL;
		file = NULL;
		if (SUCCEEDED(dump->readback->Map(0, &range, (void**)&data)) && data) {
			if (GetModuleFileName(migoto_handle, path, MAX_PATH)) {
				wcsrchr(path, L'\\')[1] = 0;
				if (!wcscat_s(path, L"dx12_reach")) {
					CreateDirectoryW(path, NULL);
					swprintf_s(name, L"\\skin_%08x_%u_%u.bin", dump->hash, dump->stride, dump->count);
					if (!wcscat_s(path, name))
						_wfopen_s(&file, path, L"wb");
				}
			}
			if (file) {
				fwrite(data, 1, (size_t)dump->bytes, file);
				fclose(file);
			}
			dump->readback->Unmap(0, &nothing);
		}
		dump->readback->Release();
		skin_dumps.erase(skin_dumps.begin() + i);
	}
}

// A mod's vertex buffer is drawn with the game's pipeline state, whose input
// layout is that of the D3D12 renderer's own buffer. Where the D3D11 renderer
// the mod was made for lays its vertices out differently, the mod's come out
// wrong, and this says how the two differ.
static void note_stride_mismatch(UINT slot, UINT mod_stride)
{
	static std::unordered_set<std::string> reported;
	const D3D12_VERTEX_BUFFER_VIEW *view = &scope->list->mState.vertex_buffers[slot];
	char text[512], element[96];

	if (mod_stride) {
		_snprintf_s(text, _TRUNCATE, "*** A mod binds a vertex buffer of stride %u to slot %u, where the game's has stride %u and the layout",
			mod_stride, slot, view->StrideInBytes);
	} else {
		_snprintf_s(text, _TRUNCATE, "*** A mod binds an index buffer but no vertex buffer to slot %u, where the game's has stride %u and the layout",
			slot, view->StrideInBytes);
	}
	for (const Dx12InputElement &input : scope->pipeline->input_layout) {
		if (input.slot != slot)
			continue;
		_snprintf_s(element, _TRUNCATE, " %s%u:format %d@%u", input.semantic.c_str(), input.index, input.format, input.offset);
		strncat_s(text, element, _TRUNCATE);
	}
	if (reported.insert(text).second)
		Dx12DumpNote("%s\n", text);
}

// The other half of pack_positions: a vertex buffer of a mod made for such a
// D3D11 renderer has halves where the pipeline state of the draw reads floats.
// A copy with floats is made of it by a shader of ours and bound in its place.
// The copy is made again when the mod has written the buffer since, as one
// that animates its mesh with a compute shader does every frame.
static const char unpack_hlsl[] = R"(
Buffer<uint> packed : register(t0);
RWBuffer<uint> unpacked : register(u0);
cbuffer params : register(b0) { uint count; uint words; };

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint from = id.x * words, to = id.x * (words + 1), i;

	if (id.x >= count)
		return;
	uint xy = packed[from], zw = packed[from + 1];
	unpacked[to] = asuint(f16tof32(xy));
	unpacked[to + 1] = asuint(f16tof32(xy >> 16));
	unpacked[to + 2] = asuint(f16tof32(zw));
	for (i = 2; i < words; i++)
		unpacked[to + i + 1] = packed[from + i];
}
)";

static ID3D11ComputeShader *unpack_shader;
static bool unpack_failed;
struct UnpackedBuffer {
	// NULL where it could not be done
	ID3D11Buffer *copy;
	// ShimResource::version of the mod's buffer the copy was made of
	UINT64 version;
};
// By the mod's buffer, which is referenced so that no other takes its place
static std::unordered_map<ID3D11Buffer*, UnpackedBuffer> unpacked_buffers;

static void forget_unpacked_buffers()
{
	for (auto &entry : unpacked_buffers) {
		entry.first->Release();
		if (entry.second.copy)
			entry.second.copy->Release();
	}
	unpacked_buffers.clear();
}

// Whether the pipeline state reads positions of three floats from the start
// of the vertices in the slot and nothing else from those 12 bytes. What a
// renderer has no data for may be pointed at offset 0.
static bool reads_float_positions(UINT slot)
{
	bool found = false;

	for (const Dx12InputElement &input : scope->pipeline->input_layout) {
		if (input.slot != slot || input.per_instance)
			continue;
		if (input.semantic == "POSITION" && !input.index) {
			if (input.format != DXGI_FORMAT_R32G32B32_FLOAT || input.offset)
				return false;
			found = true;
		} else if (input.offset && input.offset < 12) {
			return false;
		}
	}
	return found;
}

// into is the copy made before, to be written again. The lists of the game
// that draw with it keep what it held, see ShimRenameResource. retry tells a
// failure that is worth trying again: a buffer was not ready (ShimNotReady).
static ID3D11Buffer* unpack_buffer(ID3D11Buffer *buffer, UINT stride, ID3D11Buffer *into, bool *retry)
{
	D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
	D3D11_BUFFER_DESC desc, out_desc = {}, params_desc = {};
	D3D11_SUBRESOURCE_DATA initial = {};
	ID3D11ShaderResourceView *srv = NULL;
	ID3D11UnorderedAccessView *uav = NULL;
	ID3D11Buffer *out = into, *params = NULL;
	ID3DBlob *blob = NULL, *errors = NULL;
	ShimResource *resource = ShimResourceOf(buffer);
	ShimState state = {};
	UINT values[4] = {};
	bool done = false;
	HRESULT hr;

	*retry = false;
	if (!unpack_shader && !unpack_failed) {
		hr = D3DCompile(unpack_hlsl, sizeof(unpack_hlsl) - 1, "UnpackPositions", NULL, NULL, "main", "cs_5_0",
			D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors);
		if (SUCCEEDED(hr))
			hr = dx12_shim->shim_device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), NULL, &unpack_shader);
		if (FAILED(hr)) {
			unpack_failed = true;
			Dx12DumpNote("*** Failed to build the shader that turns the positions of a mod's vertices into floats (0x%x): %s\n", hr,
				errors ? (const char*)errors->GetBufferPointer() : "");
		}
		if (blob)
			blob->Release();
		if (errors)
			errors->Release();
	}
	buffer->GetDesc(&desc);
	if (!unpack_shader || !resource->resource || resource->foreign || stride % 4 || desc.ByteWidth < stride)
		return NULL;

	values[0] = desc.ByteWidth / stride;
	values[1] = stride / 4;

	out_desc.ByteWidth = values[0] * (stride + 4);
	out_desc.Usage = D3D11_USAGE_DEFAULT;
	out_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_UNORDERED_ACCESS;
	params_desc.ByteWidth = sizeof(values);
	params_desc.Usage = D3D11_USAGE_DYNAMIC;
	params_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	params_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	initial.pSysMem = values;
	srv_desc.Format = DXGI_FORMAT_R32_UINT;
	srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srv_desc.Buffer.NumElements = values[0] * values[1];
	uav_desc.Format = DXGI_FORMAT_R32_UINT;
	uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav_desc.Buffer.NumElements = out_desc.ByteWidth / 4;

	if ((out || SUCCEEDED(dx12_shim->shim_device->CreateBuffer(&out_desc, NULL, &out)))
	    && SUCCEEDED(dx12_shim->shim_device->CreateBuffer(&params_desc, &initial, &params))
	    && SUCCEEDED(dx12_shim->shim_device->CreateShaderResourceView(buffer, &srv_desc, &srv))
	    && SUCCEEDED(dx12_shim->shim_device->CreateUnorderedAccessView(out, &uav_desc, &uav))) {
		state.stages[DX12_STAGE_CS].shader = unpack_shader;
		state.stages[DX12_STAGE_CS].cbs[0] = params;
		state.stages[DX12_STAGE_CS].cb_mask = 1;
		state.stages[DX12_STAGE_CS].srvs[0] = srv;
		state.stages[DX12_STAGE_CS].srv_mask[0] = 1;
		state.cs_uavs[0] = uav;
		if (Dx12ShimPrepareDispatch(&state)) {
			Dx12ShimList()->Dispatch((values[0] + 63) / 64, 1, 1);
			stats.buffers_unpacked++;
			done = true;
		}
	}
	if (!done) {
		*retry = ShimNotReady(resource) || (out && ShimNotReady(ShimResourceOf(out)));
		if (out && out != into)
			out->Release();
		out = NULL;
	}
	if (params)
		params->Release();
	if (srv)
		srv->Release();
	if (uav)
		uav->Release();
	return out;
}

// What to bind for the vertex buffer of a mod whose vertices are 4 bytes
// shorter than those the game has in the slot. NULL to bind it as it is.
static ID3D11Buffer* unpacked_buffer(ID3D11Buffer *buffer, UINT slot, UINT stride)
{
	static bool reported;
	bool retry;

	if (!reads_float_positions(slot))
		return NULL;

	UINT64 version = ShimResourceOf(buffer)->version;
	auto found = unpacked_buffers.find(buffer);
	if (found != unpacked_buffers.end()) {
		UnpackedBuffer *unpacked = &found->second;

		if (!unpacked->copy || unpacked->version == version)
			return unpacked->copy;
		if (!unpack_buffer(buffer, stride, unpacked->copy, &retry)) {
			// Left at the version before, to be done at a later draw
			if (retry)
				return unpacked->copy;
			unpacked->copy->Release();
			unpacked->copy = NULL;
		}
		unpacked->version = version;
		return unpacked->copy;
	}

	ID3D11Buffer *out = unpack_buffer(buffer, stride, NULL, &retry);
	if (retry)
		return NULL;
	buffer->AddRef();
	unpacked_buffers[buffer] = { out, version };
	if (out) {
		Dx12DumpNote("A mod's vertex buffer of stride %u was copied with its positions as floats for a draw that reads vertices of %u bytes\n",
			stride, stride + 4);
	} else {
		log_once(&reported, "A mod's vertices have halves for positions where the game reads floats, and could not be given those. They are drawn wrong.");
	}
	return out;
}

// The engine goes by the input layout the context has set to make one that
// reads the vertices of a mod as they are (ElementFormat and the like). The
// game's is part of its pipeline state, and is made up for the engine here.
void HackerContext::Dx12PublishInputLayout()
{
	std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
	ID3D11InputLayout *layout = NULL;
	UINT64 hash;

	if (!scope) {
		if (!VkInDrawScope() || VkScopeGameLayout())
			return;
		hash = VkScopeLayoutHash();
		if (hash)
			layout = mHackerDevice->FindCustomInputLayout(hash);
		if (layout) {
			IASetInputLayout(layout);
			VkScopeSetGameLayout(static_cast<HackerInputLayout*>(layout)->GetOrigInputLayout());
			layout->Release();
			return;
		}
		if (!VkScopeInputElements(&elements))
			return;
		VkScopeSetLayoutHash(CalculateInputLayoutHash(elements.data(), (UINT)elements.size(), NULL, 0));
	} else if (scope->compute || scope->skin || scope->game_layout) {
		return;
	}

	for (size_t i = 0; scope && i < scope->pipeline->input_layout.size(); i++) {
		const Dx12InputElement &input = scope->pipeline->input_layout[i];

		elements.push_back({ input.semantic.c_str(), input.index, input.format, input.slot, input.offset,
			input.per_instance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA, input.step_rate });
	}
	if (elements.empty() || FAILED(mHackerDevice->CreateCustomInputLayout(elements.data(), (UINT)elements.size(), NULL, 0, &layout)) || !layout) {
		static bool reported;

		log_once(&reported, "A mod changes the input layout for a draw whose own could not be made up for it. The game's layout is used.");
		return;
	}

	IASetInputLayout(layout);
	if (scope)
		scope->game_layout = static_cast<HackerInputLayout*>(layout)->GetOrigInputLayout();
	else
		VkScopeSetGameLayout(static_cast<HackerInputLayout*>(layout)->GetOrigInputLayout());
	layout->Release();
}

// What became of an input layout of a mod, once for each pipeline it met
static void note_input_layout(const ShimInputLayout *layout, ID3D12PipelineState *variant)
{
	static std::unordered_set<UINT64> noted;
	const Dx12CmdState *game = &scope->list->mState;
	char text[1024], part[96];

	if (noted.size() >= 64 || !noted.insert((UINT64)(uintptr_t)scope->pipeline ^ layout->mId * 0x9e3779b97f4a7c15ull).second)
		return;

	_snprintf_s(text, _TRUNCATE, "Input layout of a mod for vs %016llx: %s. The game's:",
		scope->pipeline->shaders[DX12_STAGE_VS] ? scope->pipeline->shaders[DX12_STAGE_VS]->hash : 0,
		!scope->game_layout ? "THE GAME'S WAS NOT PUBLISHED" : variant ? "pipeline made" : "NO PIPELINE");
	for (const Dx12InputElement &input : scope->pipeline->input_layout) {
		_snprintf_s(part, _TRUNCATE, " %s%u vb%u+%u fmt %d,", input.semantic.c_str(), input.index, input.slot, input.offset, input.format);
		strncat_s(text, part, _TRUNCATE);
	}
	strncat_s(text, " the mod's:", _TRUNCATE);
	for (const D3D12_INPUT_ELEMENT_DESC &element : layout->mElements) {
		_snprintf_s(part, _TRUNCATE, " %s%u vb%u+%u fmt %d,", element.SemanticName, element.SemanticIndex, element.InputSlot,
			element.AlignedByteOffset, element.Format);
		strncat_s(text, part, _TRUNCATE);
	}
	strncat_s(text, " strides of the game:", _TRUNCATE);
	for (UINT slot = 0; slot < 8 && slot < ARRAYSIZE(game->vertex_buffers); slot++) {
		_snprintf_s(part, _TRUNCATE, " %u", game->vertex_buffers[slot].StrideInBytes);
		strncat_s(text, part, _TRUNCATE);
	}
	Dx12DumpNote("%s\n", text);
}

// What locate and table_size go by for a shader of a mod
static void shader_info_of_mod(const ShimShader *shader, Dx12ShaderInfo *info)
{
	*info = {};
	info->hash = shader->hash;
	info->stage = shader->stage;
	info->size = (UINT32)shader->bytecode.size();
	info->dxil = shader->dxil;
	info->reflected = shader->reflected;
	info->srv[0] = shader->srv_used[0];
	info->srv[1] = shader->srv_used[1];
	info->cbv = shader->cbv_used;
	info->uav = shader->uav_used;
	info->targets = shader->targets;
	info->bytecode = shader->bytecode.data();
}

// Why D3D12 may have refused a pipeline with the shaders of a mod: the
// registers they read that the game's root signature has nothing for. Once
// for each pipeline and set of shaders.
static void note_custom_shader_failure(UINT stages, const char *why)
{
	static std::unordered_set<UINT64> noted;
	static const struct {
		D3D12_DESCRIPTOR_RANGE_TYPE type;
		char letter;
		UINT count;
	} kinds[] = {
		{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 't', 128 }, { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 'b', SHIM_CBV_SLOTS },
		{ D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 'u', 64 }, { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 's', SHIM_SAMPLER_SLOTS },
	};
	const ShimState *mod = dx12_shim->shim_context->mState;
	Dx12RegisterLocation location;
	char text[1024], part[32];
	UINT64 key = (UINT64)(uintptr_t)scope->pipeline;
	bool missing = false, samplers = false;
	UINT reg;

	for (Dx12ShaderStage stage : graphics_stages) {
		if (stages & 1u << stage)
			key = key * 0x9e3779b97f4a7c15ull ^ scope->mod_shaders[stage].hash;
	}
	if (noted.size() >= 64 || !noted.insert(key).second)
		return;

	_snprintf_s(text, _TRUNCATE, "*** A mod's draw was made with the game's shaders instead of its own: %s.", why);
	if (!scope->signature) {
		strncat_s(text, " The game's root signature is not known.", _TRUNCATE);
	} else {
		strncat_s(text, " Registers of the mod's shaders the root signature has nothing for:", _TRUNCATE);
		for (Dx12ShaderStage stage : graphics_stages) {
			const ShimShader *shader = stages & 1u << stage ? ShimShaderOf(mod->stages[stage].shader) : NULL;

			if (!shader)
				continue;
			for (const auto &kind : kinds) {
				for (reg = 0; reg < kind.count; reg++) {
					bool used;

					switch (kind.type) {
					case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: used = !!(shader->srv_used[reg / 64] & 1ull << reg % 64); break;
					case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: used = !!(shader->cbv_used & 1u << reg); break;
					case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: used = !!(shader->uav_used & 1ull << reg); break;
					default: used = !!(shader->sampler_used & 1u << reg); break;
					}
					if (!used || Dx12LocateRegister(scope->signature, stage, kind.type, reg, &location))
						continue;
					_snprintf_s(part, _TRUNCATE, " %s-%c%u", dx12_stage_names[stage], kind.letter, reg);
					strncat_s(text, part, _TRUNCATE);
					missing = true;
					samplers |= kind.type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
				}
			}
		}
		if (!missing)
			strncat_s(text, " none", _TRUNCATE);
		else if (samplers)
			strncat_s(text, " (the static samplers of a root signature are not read, so a sampler may be there after all)", _TRUNCATE);
	}
	Dx12DumpNote("%s\n", text);
}

// An input layout of a mod is what the engine set that is not the game's, and
// shaders of a mod are what it set for a draw of its own. On D3D12 either
// takes the game's pipeline state made again with them, which is set on the
// list until the next draw wants another or the scope closes. The game's own
// draw is never made with a mod's shaders.
static void apply_pipeline(const ShimState *mod, bool mod_draw)
{
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	ID3D12PipelineState *variant = NULL;
	Dx12PipelineVariantDesc wanted = {};
	const char *why = NULL;
	bool game_dxil = false;
	UINT stages = 0;

	if (scope->game_layout && mod->input_layout && mod->input_layout != scope->game_layout) {
		const ShimInputLayout *layout = static_cast<const ShimInputLayout*>(mod->input_layout);
		UINT64 hash = layout->mElements.size();

		for (const D3D12_INPUT_ELEMENT_DESC &element : layout->mElements) {
			hash = hash * 0x9e3779b97f4a7c15ull ^ fnv_64_buf(element.SemanticName, strlen(element.SemanticName));
			hash = hash * 0x9e3779b97f4a7c15ull
				^ fnv_64_buf(&element.SemanticIndex, sizeof(element) - offsetof(D3D12_INPUT_ELEMENT_DESC, SemanticIndex));
		}
		wanted.layout_hash = hash;
		wanted.elements = layout->mElements.data();
		wanted.element_count = (UINT)layout->mElements.size();
	}
	if (mod_draw && !scope->compute && !scope->skin) {
		// A pipeline state is all DXBC or all DXIL
		for (Dx12ShaderStage stage : graphics_stages) {
			if (scope->pipeline->shaders[stage]) {
				game_dxil = scope->pipeline->shaders[stage]->dxil;
				break;
			}
		}
		for (Dx12ShaderStage stage : graphics_stages) {
			const ShimShader *shader = ShimShaderOf(mod->stages[stage].shader);

			if (!shader) {
				if (mod->stages[stage].shader_unbound && scope->pipeline->shaders[stage])
					wanted.removed_stages |= 1u << stage;
				continue;
			}
			if (game_dxil && shader->dxil_form)
				shader = shader->dxil_form;
			shader_info_of_mod(shader, &scope->mod_shaders[stage]);
			wanted.shaders[stage] = &scope->mod_shaders[stage];
			stages |= 1u << stage;
		}
	}
	// A pixel shader of a mod reads what the stage before it wrote in the
	// D3D11 game, which the D3D12 game's may not all write
	if (stages & 1u << DX12_STAGE_PS) {
		static const Dx12ShaderStage upstream_stages[] = { DX12_STAGE_GS, DX12_STAGE_DS, DX12_STAGE_VS };
		const Dx12ShaderInfo *upstream = NULL, *fitted = NULL;
		const char *unfit = NULL;

		for (Dx12ShaderStage stage : upstream_stages) {
			if (wanted.removed_stages & 1u << stage)
				continue;
			upstream = wanted.shaders[stage] ? wanted.shaders[stage] : scope->pipeline->shaders[stage];
			if (upstream)
				break;
		}
		if (upstream && Dx12DxcFitPixelShader(wanted.shaders[DX12_STAGE_PS], upstream, &fitted, &unfit) < 0) {
			wanted.shaders[DX12_STAGE_PS] = NULL;
			stages &= ~(1u << DX12_STAGE_PS);
			stats.custom_failed++;
			scope->custom_failed = true;
			if (G->dx12_dump_hashes)
				note_custom_shader_failure(1u << DX12_STAGE_PS, unfit);
		} else if (fitted) {
			wanted.shaders[DX12_STAGE_PS] = fitted;
		}
	}

	if (wanted.elements || stages || wanted.removed_stages)
		variant = Dx12PipelineVariant(scope->pipeline, dx12_shim->device, &wanted, &why);

	for (Dx12ShaderStage stage : graphics_stages) {
		if (variant && (wanted.removed_stages & 1u << stage))
			scope->shaders[stage] = NULL;
		else
			scope->shaders[stage] = variant && wanted.shaders[stage] ? wanted.shaders[stage] : scope->pipeline->shaders[stage];
	}
	scope->custom_stages = variant ? stages : 0;
	if (stages && variant) {
		stats.custom_draws++;
	} else if (stages || wanted.removed_stages && !variant) {
		stats.custom_failed++;
		scope->custom_failed = true;
		if (G->dx12_dump_hashes)
			note_custom_shader_failure(stages, why);
	}

	if (G->dx12_dump_hashes && mod->input_layout && mod->input_layout != scope->game_layout)
		note_input_layout(static_cast<const ShimInputLayout*>(mod->input_layout), variant);

	if (variant == scope->variant)
		return;
	list->SetPipelineState(variant ? variant : scope->list->mState.pipeline);
	if (variant)
		Dx12ShimPin(scope->list, variant);
	scope->variant = variant;
}

// Makes the next draw or dispatch on the list use what the mod has bound in
// place of what the game has, as far as that can be done. Whatever an earlier
// call left on the list goes first: the mod may have unbound it since.
static void apply_overrides(bool mod_draw)
{
	const ShimState *mod = dx12_shim->shim_context->mState;
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12ShaderStage *stages = scope->compute ? compute_stages : graphics_stages;
	size_t stage_count = scope->compute ? ARRAYSIZE(compute_stages) : ARRAYSIZE(graphics_stages);
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	D3D12_VERTEX_BUFFER_VIEW vb;
	D3D12_INDEX_BUFFER_VIEW ib;
	SkinOutput *first_output = NULL;
	UINT slot, size, word, mask;
	unsigned long bit;
	size_t i;

	restore_overrides();
	// Before the bindings, which go by the shaders of the draw
	scope->custom_failed = false;
	apply_pipeline(mod, mod_draw);
	// With the game's heaps and tables the draw reads all it would have,
	// but for what a mod binds through a table
	if (scope->heaps_changed && !grow_side_tables()) {
		static bool no_growth;

		log_once(&no_growth, "The tables of a draw could not be copied again to a heap of ours for shaders that read further. What a mod replaces in them was ignored.");
		restore_heaps();
		scope->side_impossible = true;
	}

	for (i = 0; i < stage_count; i++) {
		const ShimStageState *bound = &mod->stages[stages[i]];

		for (word = 0; word < ARRAYSIZE(bound->srv_mask); word++) {
			for (mask = bound->srv_mask[word]; _BitScanForward(&bit, mask); mask &= mask - 1)
				apply_shader_resource(stages[i], word * 32 + bit, bound->srvs[word * 32 + bit]);
		}
		for (mask = bound->cb_mask; _BitScanForward(&bit, mask); mask &= mask - 1)
			apply_constant_buffer(stages[i], bit, bound);
	}
	bind_changed_tables();

	if (scope->compute)
		return;

	for (slot = 0; slot < ARRAYSIZE(mod->vbs) && slot < ARRAYSIZE(game->vertex_buffers); slot++) {
		if (!mod->vbs[slot])
			continue;
		if (ShimResourceOf(mod->vbs[slot])->skin_va) {
			// What the mod kept of another draw, see Dx12ScopeVertexBuffer
			SkinOutput *output = skin_outputs.empty() ? NULL : find_skin_output(ShimResourceOf(mod->vbs[slot])->skin_va);

			if (!output || !output->vertices)
				continue;
			vb.BufferLocation = output->buffer->GetGPUVirtualAddress();
			vb.SizeInBytes = output->vertices * output->stride;
			vb.StrideInBytes = output->stride;
			list->IASetVertexBuffers(slot, 1, &vb);
			scope->vbs_changed |= 1u << slot;
			Dx12ShimPin(scope->list, output->buffer);
			output->frame = dx12_frame;
			stats.skin_swapped++;
			continue;
		}
		if (ShimResourceOf(mod->vbs[slot])->foreign)
			continue;
		ID3D11Buffer *bound = mod->vbs[slot], *unpacked = NULL;

		vb.StrideInBytes = mod->vb_strides[slot];
		if (vb.StrideInBytes && game->vertex_buffers[slot].StrideInBytes == vb.StrideInBytes + 4 && !mod->vb_offsets[slot])
			unpacked = unpacked_buffer(bound, slot, vb.StrideInBytes);
		if (unpacked) {
			bound = unpacked;
			vb.StrideInBytes += 4;
		}
		vb.BufferLocation = Dx12ShimGameBufferAddress(bound, scope->list, &size);
		if (!vb.BufferLocation || mod->vb_offsets[slot] >= size)
			continue;
		vb.BufferLocation += mod->vb_offsets[slot];
		vb.SizeInBytes = size - mod->vb_offsets[slot];
		if (G->dx12_dump_hashes && game->vertex_buffers[slot].StrideInBytes
		    && game->vertex_buffers[slot].StrideInBytes != vb.StrideInBytes)
			note_stride_mismatch(slot, vb.StrideInBytes);
		list->IASetVertexBuffers(slot, 1, &vb);
		scope->vbs_changed |= 1u << slot;
	}

	// Where the game has a buffer bound that a mod had skinned to another
	// instead, see skin_draw
	for (mask = skin_outputs.empty() ? 0 : scope->pipeline->vb_slots & ~scope->vbs_changed; _BitScanForward(&bit, mask); mask &= mask - 1) {
		SkinOutput *output;

		if (bit >= ARRAYSIZE(game->vertex_buffers) || !game->vertex_buffers[bit].BufferLocation)
			continue;
		output = find_skin_output(game->vertex_buffers[bit].BufferLocation);
		// Another buffer of the mesh as it was skinned, which the game
		// keeps of an earlier frame to tell how far the vertices moved.
		// With no vertices of the mod in it, those of this frame do
		// better than the game's, which the mod's indices do not go with.
		if ((!output || !output->vertices) && first_output && bit
		    && game->vertex_buffers[bit].StrideInBytes == game->vertex_buffers[0].StrideInBytes
		    && game->vertex_buffers[bit].SizeInBytes == game->vertex_buffers[0].SizeInBytes) {
			output = first_output;
			stats.skin_stood_in++;
		}
		if (!output || !output->vertices)
			continue;
		if (!bit)
			first_output = output;
		vb.BufferLocation = output->buffer->GetGPUVirtualAddress();
		vb.SizeInBytes = output->vertices * output->stride;
		vb.StrideInBytes = game->vertex_buffers[bit].StrideInBytes;
		list->IASetVertexBuffers(bit, 1, &vb);
		scope->vbs_changed |= 1u << bit;
		Dx12ShimPin(scope->list, output->buffer);
		output->frame = dx12_frame;
		stats.skin_swapped++;
	}

	// Only for a mod's own draws: the game's own are of its mesh, which
	// goes with what the game wrote
	for (mask = mod_draw && dx12_standins_live ? scope->pipeline->vb_slots & ~scope->vbs_changed : 0; _BitScanForward(&bit, mask); mask &= mask - 1) {
		D3D11_BUFFER_DESC written_desc;
		Published *written;
		UINT64 offset;

		if (bit >= ARRAYSIZE(game->vertex_buffers) || !game->vertex_buffers[bit].BufferLocation)
			continue;
		written = written_buffer_at(game->vertex_buffers[bit].BufferLocation, &offset);
		if (!written)
			continue;
		written->standin->GetDesc(&written_desc);
		vb.BufferLocation = Dx12ShimGameBufferAddress(written->standin, scope->list, &size);
		if (!vb.BufferLocation || offset >= written_desc.ByteWidth)
			continue;
		vb.BufferLocation += offset;
		vb.SizeInBytes = written_desc.ByteWidth - (UINT)offset;
		vb.StrideInBytes = game->vertex_buffers[bit].StrideInBytes;
		list->IASetVertexBuffers(bit, 1, &vb);
		scope->vbs_changed |= 1u << bit;
		written->frame = dx12_frame;
		written->standin_drawn = true;
		stats.written_swapped++;
	}

	if (mod->ib && !ShimResourceOf(mod->ib)->foreign) {
		ib.BufferLocation = Dx12ShimGameBufferAddress(mod->ib, scope->list, &size);
		if (ib.BufferLocation && mod->ib_offset < size) {
			ib.BufferLocation += mod->ib_offset;
			ib.SizeInBytes = size - mod->ib_offset;
			ib.Format = mod->ib_format;
			list->IASetIndexBuffer(&ib);
			scope->ib_changed = true;
			// The hash of the vertex buffer the mod has for these
			// indices did not match, most likely
			if (G->dx12_dump_hashes && !(scope->vbs_changed & 1) && game->vertex_buffers[0].BufferLocation)
				note_stride_mismatch(0, 0);
		}
	}

	if (mod->topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED && (D3D12_PRIMITIVE_TOPOLOGY)mod->topology != game->topology) {
		list->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)mod->topology);
		scope->topology_changed = true;
	}
}

// What a draw of a mod came to, once for each kind of them
static void note_mod_draw(bool indexed, UINT count, UINT start)
{
	static std::unordered_set<UINT64> noted;
	const ShimState *mod = dx12_shim->shim_context->mState;
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12PipelineInfo *pipeline = scope->pipeline;
	const ShimStageState *ps = &mod->stages[DX12_STAGE_PS];
	char text[1024], part[96];
	unsigned long bit;
	UINT mask, word;
	UINT64 key;

	key = (pipeline->shaders[DX12_STAGE_VS] ? pipeline->shaders[DX12_STAGE_VS]->hash : 0) * 31
		+ game->index_buffer.BufferLocation * 7 + game->vertex_buffers[0].BufferLocation + count * 0x9e3779b97f4a7c15ull + start;
	// Once more for other formats of the game's textures or other textures
	// of the mod, which is what differs where a mod picks them by format,
	// and for shaders of the mod's
	key = key * 31 + ps->srv_mask[0];
	for (Dx12ShaderStage stage : graphics_stages)
		key = key * 31 + (scope->custom_stages & 1u << stage ? scope->shaders[stage]->hash : 0);
	for (word = 0; word < 8 && scope->signature; word++) {
		Dx12DescriptorSlot view = {};

		if (Dx12ResolveRegister(game, scope->signature, DX12_STAGE_PS, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, word, &view) && view.resource)
			key = key * 131 + view.format + 1;
		else
			key *= 131;
	}
	if (noted.size() >= 3000 || !noted.insert(key).second)
		return;

	_snprintf_s(text, _TRUNCATE, "Mod draw: %u from %u%s, vs %016llx%s ps %016llx%s, the game's index buffer %s%s. Vertex buffers:",
		count, start, indexed ? " indexed" : "",
		scope->shaders[DX12_STAGE_VS] ? scope->shaders[DX12_STAGE_VS]->hash : 0,
		scope->custom_stages & 1u << DX12_STAGE_VS ? " (the mod's)" : "",
		scope->shaders[DX12_STAGE_PS] ? scope->shaders[DX12_STAGE_PS]->hash : 0,
		scope->custom_stages & 1u << DX12_STAGE_PS ? " (the mod's)" : scope->custom_failed ? " (the mod's could not be)" : "",
		scope->ib_changed ? "replaced" : "kept", scope->heaps_changed ? ", tables in our heap" : "");
	for (mask = pipeline->vb_slots; _BitScanForward(&bit, mask); mask &= mask - 1) {
		const D3D12_VERTEX_BUFFER_VIEW *view;

		if (bit >= ARRAYSIZE(game->vertex_buffers))
			break;
		view = &game->vertex_buffers[bit];
		if (mod->vbs[bit] && ShimResourceOf(mod->vbs[bit])->skin_va && (scope->vbs_changed & 1u << bit))
			_snprintf_s(part, _TRUNCATE, " %lu=skinned for a mod and kept (the game's stride %u)", bit, view->StrideInBytes);
		else if (mod->vbs[bit] && (scope->vbs_changed & 1u << bit))
			_snprintf_s(part, _TRUNCATE, " %lu=the mod's of stride %u (the game's %u)", bit, mod->vb_strides[bit], view->StrideInBytes);
		else if (mod->vbs[bit])
			_snprintf_s(part, _TRUNCATE, " %lu=THE GAME'S, the mod's could not be bound (stride %u, %u bytes)", bit, view->StrideInBytes, view->SizeInBytes);
		else if (scope->vbs_changed & 1u << bit)
			_snprintf_s(part, _TRUNCATE, " %lu=skinned for a mod (stride %u)", bit, view->StrideInBytes);
		else
			_snprintf_s(part, _TRUNCATE, " %lu=the game's (stride %u, %u bytes)", bit, view->StrideInBytes, view->SizeInBytes);
		strncat_s(text, part, _TRUNCATE);
	}
	strncat_s(text, ". Textures of the mod at ps-t", _TRUNCATE);
	for (word = 0; word < ARRAYSIZE(ps->srv_mask); word++) {
		for (mask = ps->srv_mask[word]; _BitScanForward(&bit, mask); mask &= mask - 1) {
			_snprintf_s(part, _TRUNCATE, " %lu", word * 32 + bit);
			strncat_s(text, part, _TRUNCATE);
		}
	}
	Dx12DumpNote("%s\n", text);

	// What a mod that picks its textures by the format of the game's had
	// to go by, and what it bound: format of the view/of the resource
	_snprintf_s(text, _TRUNCATE, "  ps textures of %u from %u:", count, start);
	for (word = 0; word < 12; word++) {
		Dx12DescriptorSlot view = {};
		Dx12ResourceInfo info = {};
		ID3D11ShaderResourceView *bound = ps->srvs[word];

		if (!scope->signature || !Dx12ResolveRegister(game, scope->signature, DX12_STAGE_PS, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, word, &view)
		    || !view.resource)
			continue;
		Dx12GetResourceInfo(view.resource, &info);
		_snprintf_s(part, _TRUNCATE, " t%u game %u/%d %llux%u", word, view.format, info.desc.Format, info.desc.Width, info.desc.Height);
		strncat_s(text, part, _TRUNCATE);
		if (bound) {
			const ShimView *mod_view = ShimViewOf(bound);

			_snprintf_s(part, _TRUNCATE, " mod %d/%d %llux%u%s", static_cast<ShimShaderResourceView*>(bound)->mDesc.Format,
				mod_view->resource->desc.Format, mod_view->resource->desc.Width, mod_view->resource->desc.Height,
				mod_view->resource->foreign ? " (the game's)" : !mod_view->resource->resource ? " NO RESOURCE" : "");
			strncat_s(text, part, _TRUNCATE);
		}
		strncat_s(text, ",", _TRUNCATE);
	}
	Dx12DumpNote("%s\n", text);
}

void Dx12ScopeDraw(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
	const ShimState *mod = dx12_shim->shim_context->mState;
	ID3D12GraphicsCommandList *list;

	if (!scope) {
		VkScopeDraw(indexed, count, instances, start, base_vertex, start_instance);
		return;
	}
	list = scope->list->mOrig;

	if (scope->skin) {
		if (indexed)
			Dx12ScopeUnsupported("to draw with an index buffer where the game skins a mesh");
		else
			skin_draw(count);
		return;
	}
	if (scope->compute) {
		Dx12ScopeUnsupported("to draw");
		return;
	}

	// D3D11 reads every index as 0 without an index buffer, which draws
	// nothing. The game's is still bound to its list here and would draw
	// the game's mesh.
	if (indexed && mod->ib_unbound)
		return;

	apply_overrides(true);
	// A mod whose shaders could not be given to the game's pipeline is
	// drawn with the game's: seen without its effect rather than not at all
	static const bool draw_with_game_shaders_instead = true;
	if (scope->custom_failed && !draw_with_game_shaders_instead)
		return;
	stats.mod_draws++;
	if (G->dx12_dump_hashes)
		note_mod_draw(indexed, count, start);

	if (!indexed) {
		list->DrawInstanced(count, instances, start, start_instance);
	} else if (scope->ib_changed || scope->list->mState.index_buffer.BufferLocation) {
		list->DrawIndexedInstanced(count, instances, start, base_vertex, start_instance);
	}
}

// What the engine finds bound when it starts on a draw of the game: nothing
// but IniParams and the like, which it keeps bound on a D3D11 context whatever
// the game does. The context is only put back to that after a draw that
// changed a binding, which few of them do.
static ID3D11ShaderResourceView *scope_ini_params;
static int scope_ini_params_reg;
static bool scope_state_ready;

static void reset_scope_state()
{
	dx12_shim->shim_context->ClearState();
	dx12_shim->hacker_context->Bind3DMigotoResources();

	// All that Bind3DMigotoResources goes by
	scope_ini_params = dx12_shim->hacker_device->mIniResourceView;
	scope_ini_params_reg = G->IniParamsReg;
	scope_state_ready = true;
}

static void open_scope(Dx12DrawScope *opened, Dx12GraphicsCommandList *list, const Dx12PipelineInfo *pipeline, bool compute)
{
	opened->list = list;
	opened->compute = compute;
	opened->skin = NULL;
	opened->mod_skinned = false;
	opened->pipeline = pipeline;
	opened->signature = Dx12ListRootSignature(list, compute);
	memcpy(opened->shaders, pipeline->shaders, sizeof(opened->shaders));
	opened->custom_stages = 0;
	opened->custom_failed = false;
	opened->vbs_changed = 0;
	opened->ib_changed = false;
	opened->topology_changed = false;
	opened->variant = NULL;
	opened->game_layout = NULL;
	opened->parameters.clear();
	opened->side_impossible = false;
	opened->heaps_changed = false;

	Dx12OpenForeignScope();
	scope = opened;
}

void Dx12OpenForeignScope()
{
	ShimContext *context = dx12_shim->shim_context;

	// What the frame's own command lists have bound is theirs
	context->mState = &context->mScopeState;
	if (!scope_state_ready || scope_ini_params != dx12_shim->hacker_device->mIniResourceView
	    || scope_ini_params_reg != G->IniParamsReg)
		reset_scope_state();
	context->mChanged = false;

	scope_serial++;
}

static void refresh_guards();

void Dx12CloseForeignScope()
{
	ShimContext *context = dx12_shim->shim_context;

	if (context->mChanged)
		reset_scope_state();
	context->mState = &context->mFrameState;
	refresh_guards();
}

static void close_scope()
{
	restore_list();
	scope = NULL;
	Dx12CloseForeignScope();
}

// -----------------------------------------------------------------------------
// The draws not worth stopping for
//
// A mod that goes by meshes rather than shaders has a command list run for
// every draw that asks for the TextureOverride section of its vertex buffer,
// which few have. Rather than wait on each other for the engine to find that
// out, the threads recording find out themselves where it is certain: the
// command lists of the shaders can do nothing but ask about buffers
// (Dx12DrawFilter, per pipeline state), and each of those buffers was asked
// about before with nothing changed since (the buffer cache and
// boring_constants).

static volatile LONG fast_draws, fast_draws_pending;
// Dispatches that skin let through the same way, see skin_is_boring
static volatile LONG fast_skins;

void Dx12FlushFastDraws(Dx12GraphicsCommandList *list)
{
	if (!list->mFastDraws)
		return;
	InterlockedAdd(&fast_draws, list->mFastDraws);
	InterlockedAdd(&fast_draws_pending, list->mFastDraws);
	list->mFastDraws = 0;
}

// Why the others were, for the report
enum SlowReason {
	SLOW_SETTINGS,
	SLOW_FILTER,
	SLOW_LISTS,
	SLOW_GUARD,
	SLOW_INI_PARAMS,
	SLOW_VB_UNKNOWN,
	SLOW_VB_WANTED,
	SLOW_VB_CHANGED,
	SLOW_IB,
	SLOW_CB,
	SLOW_SKIN,
	SLOW_TEXTURE,
	SLOW_REASONS,
	SLOW_NOT = SLOW_REASONS,
};
static volatile LONG slow_draws[SLOW_REASONS];

static bool stage_of_shader_type(wchar_t shader_type, Dx12ShaderStage *stage)
{
	switch (shader_type) {
	case L'v': *stage = DX12_STAGE_VS; return true;
	case L'p': *stage = DX12_STAGE_PS; return true;
	case L'g': *stage = DX12_STAGE_GS; return true;
	case L'h': *stage = DX12_STAGE_HS; return true;
	case L'd': *stage = DX12_STAGE_DS; return true;
	case L'c': *stage = DX12_STAGE_CS; return true;
	}
	return false;
}

// A command list may do more than ask about buffers only if a condition of its
// own holds, which is rare for the lists that put back what a mod has set.
// Those conditions (guards, see CommandListCheckedSlots) are evaluated with
// the lock held whenever the engine has run, and kept as one bit each in a
// word for the threads recording, which cannot evaluate them: the expressions
// go away when the configuration is read again. Bit numbers are good while
// config_generation stays the same.

// A condition to be true or false, or an assignment to be of the value in place
struct Guard {
	CommandListExpression *expression;
	CommandListCheckedAssignment assignment;
};
static Guard guard_conditions[64];
static unsigned guard_count;
static volatile LONG64 guard_bits;

static bool guard_holds(unsigned guard)
{
	const CommandListCheckedAssignment *assignment = &guard_conditions[guard].assignment;
	float value;

	// As IfCommand::run decides
	if (guard_conditions[guard].expression)
		return !!guard_conditions[guard].expression->evaluate(NULL, dx12_shim->hacker_device);

	value = assignment->expression ? assignment->expression->evaluate(NULL, dx12_shim->hacker_device) : assignment->val;
	if (assignment->var)
		return assignment->var->fval == value;
	return G->iniParams[assignment->param_idx].*assignment->param_component == value;
}

// With the lock held
static void refresh_guards()
{
	LONG64 bits = 0;
	unsigned i;

	for (i = 0; i < guard_count; i++)
		bits |= (LONG64)guard_holds(i) << i;
	if (bits != guard_bits)
		InterlockedExchange64(&guard_bits, bits);
}

void Dx12DrawPathEngineRan()
{
	refresh_guards();
}

// assignment is for where there is no expression
static bool register_guard(CommandListExpression *expression, const CommandListCheckedAssignment *assignment, unsigned *guard)
{
	unsigned i;

	for (i = 0; i < guard_count; i++) {
		if (guard_conditions[i].expression == expression && (expression || guard_conditions[i].assignment == *assignment)) {
			*guard = i;
			return true;
		}
	}
	if (guard_count == ARRAYSIZE(guard_conditions))
		return false;

	guard_conditions[guard_count].expression = expression;
	if (!expression)
		guard_conditions[guard_count].assignment = *assignment;
	*guard = guard_count++;
	// No filter refers to the bit yet
	if (guard_holds(*guard))
		InterlockedOr64(&guard_bits, 1ll << *guard);
	return true;
}

// With the lock held
// The hashes are of the shaders the engine is told of, by stage and 0 for
// none
static void work_out_filter(Dx12DrawFilter *filter, const UINT64 *hashes)
{
	CommandListCheckedSlots slots = {};
	bool only_checks = true;
	Dx12ShaderStage stage;
	unsigned i, guard;

	InterlockedExchange(&filter->generation, 0);

	// The shaders HackerContext::BeforeDraw looks up
	for (Dx12ShaderStage graphics_stage : graphics_stages) {
		if (!hashes[graphics_stage] && graphics_stage != DX12_STAGE_VS && graphics_stage != DX12_STAGE_PS)
			continue;
		auto found = lookup_shaderoverride(hashes[graphics_stage]);
		if (found == G->mShaderOverrideMap.end())
			continue;
		if (found->second.depth_filter != DepthBufferFilter::NONE
		    || !CommandListOnlyChecksTextureOverrides(&found->second.command_list, &slots)
		    || !CommandListOnlyChecksTextureOverrides(&found->second.post_command_list, &slots)) {
			only_checks = false;
			break;
		}
	}

	for (i = 0; only_checks && i < slots.cb_count; i++) {
		if (!stage_of_shader_type(slots.cbs[i].shader_type, &stage) || slots.cbs[i].slot > 255) {
			only_checks = false;
			break;
		}
		filter->cbs[i].stage = (BYTE)stage;
		filter->cbs[i].slot = (BYTE)slots.cbs[i].slot;
	}

	for (i = 0; only_checks && i < slots.texture_count; i++) {
		if (!stage_of_shader_type(slots.textures[i].shader_type, &stage) || slots.textures[i].slot > 255) {
			only_checks = false;
			break;
		}
		filter->textures[i].stage = (BYTE)stage;
		filter->textures[i].slot = (BYTE)slots.textures[i].slot;
	}

	filter->guards_zero = filter->guards_nonzero = 0;
	for (i = 0; only_checks && i < slots.guard_count; i++) {
		if (!register_guard(slots.guards[i].expression, NULL, &guard)) {
			only_checks = false;
			break;
		}
		*(slots.guards[i].nonzero ? &filter->guards_nonzero : &filter->guards_zero) |= 1ull << guard;
	}
	for (i = 0; only_checks && i < slots.assignment_count; i++) {
		if (!register_guard(NULL, &slots.assignments[i], &guard)) {
			only_checks = false;
			break;
		}
		filter->guards_nonzero |= 1ull << guard;
	}

	filter->only_checks = only_checks;
	filter->vbs = slots.vbs;
	filter->ib = slots.ib;
	filter->cb_count = (BYTE)slots.cb_count;
	filter->texture_count = (BYTE)slots.texture_count;
	filter->override_generation = G->shader_override_generation;
	InterlockedExchange(&filter->generation, (LONG)config_generation);
}

// shaders are those the engine is told of, which for a dispatch that skins
// are not the pipeline's, see Dx12Dispatch
static void work_out_filter(const Dx12PipelineInfo *pipeline, const Dx12ShaderInfo *const *shaders)
{
	UINT64 hashes[DX12_STAGE_COUNT] = {};

	for (Dx12ShaderStage stage : graphics_stages)
		hashes[stage] = shaders[stage] ? shaders[stage]->hash : 0;
	work_out_filter(&pipeline->filter, hashes);
}

void Dx12WorkOutFilter(Dx12DrawFilter *filter, const UINT64 *shader_hashes)
{
	if (filter->generation != (LONG)config_generation || filter->override_generation != G->shader_override_generation)
		work_out_filter(filter, shader_hashes);
}

// IniParams is bound for every draw the engine looks at, which matters if a
// shader of the game can be given something for its register
static bool ini_params_reach_shaders(const Dx12RootSignatureInfo *signature)
{
	int reg = G->IniParamsReg;
	LONG state = signature->ini_params;
	Dx12RegisterLocation location;
	bool located = false;

	if (reg < 0)
		return false;
	if ((state >> 1) == reg + 1)
		return state & 1;

	for (Dx12ShaderStage stage : graphics_stages)
		located |= Dx12LocateRegister(signature, stage, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, reg, &location);
	InterlockedExchange(&signature->ini_params, (reg + 1) << 1 | (LONG)located);
	return located;
}

// What a draw found boring went by, as far as its buffers go: the counters
// that the stamps of their cache entries were the sums of, each with how many
// times it was in them, and the sum of those stamps. While the counters come
// to that sum the entries would say the same of the same buffers, see
// DrawMemo.
const UINT DRAW_MEMO_COUNTERS = 14;
struct DrawStamps {
	const volatile LONG64 *counters[DRAW_MEMO_COUNTERS];
	BYTE times[DRAW_MEMO_COUNTERS];
	UINT count;
	UINT64 total;
	// Not if a buffer was not answered for by its cache entry, or the
	// counters are more than there is room for
	bool whole;
};

static void add_draw_stamp_counters(DrawStamps *stamps, const volatile LONG64 *const *counters, UINT count)
{
	UINT i, j;

	for (i = 0; i < count; i++) {
		for (j = 0; j < stamps->count && stamps->counters[j] != counters[i]; j++)
			;
		if (j == stamps->count) {
			if (j == DRAW_MEMO_COUNTERS) {
				stamps->whole = false;
				return;
			}
			stamps->counters[j] = counters[i];
			stamps->times[j] = 0;
			stamps->count++;
		}
		if (stamps->times[j] == 255) {
			stamps->whole = false;
			return;
		}
		stamps->times[j]++;
	}
}

// The counters buffer_cache_stamp adds up for an entry, and what they came to
// when the entry was found to go by them
static void add_draw_stamps(DrawStamps *stamps, const BufferCacheEntry *cached, UINT64 stamp)
{
	const volatile LONG64 *counters[2 + 17];
	UINT count;

	if (!stamps->whole)
		return;

	Dx12FindBufferStampCounters(cached->va, counters);
	count = Dx12BufferHashStampCounters(cached->resource, cached->offset, cached->span, counters + 2, 17);
	if (!count) {
		stamps->whole = false;
		return;
	}
	add_draw_stamp_counters(stamps, counters, 2 + count);

	if (cached->ib_asked) {
		Dx12FindBufferStampCounters(cached->ib_va, counters);
		count = 0;
		if (cached->ib_found) {
			count = Dx12BufferHashStampCounters(cached->ib, cached->ib_offset, cached->ib_span, counters + 2, 17);
			if (!count) {
				stamps->whole = false;
				return;
			}
		}
		add_draw_stamp_counters(stamps, counters, 2 + count);
	}
	stamps->total += stamp;
}

static SlowReason buffer_is_boring(UINT64 va, UINT64 bytes, UINT kind, UINT stride, const D3D12_INDEX_BUFFER_VIEW *indices,
		DrawStamps *stamps = NULL)
{
	BufferCacheEntry asked = {}, cached;
	UINT64 stamp, tag;
	BufferCacheSlot *slot = buffer_cache_slot(&asked, va, bytes, kind, stride, indices, &tag);
	const volatile UINT64 *tags = &buffer_cache_tags[slot - buffer_cache];
	LONG seq;
	UINT way;

	for (way = 0; way < BUFFER_CACHE_WAYS; way++, slot++) {
		if (tags[way] != tag)
			continue;
		seq = slot->seq;
		if (seq & 1)
			continue;
		memcpy(&cached, &slot->data, offsetof(BufferCacheEntry, entry));
		if (slot->seq != seq || !same_buffer_asked(&cached, &asked))
			continue;

		// None of the pointers are followed, so the generation they
		// are good for does not matter. What they pointed to going
		// away does not change what the buffer is.
		if (!cached.generation || cached.config != config_generation)
			break;
		if (!cached.boring)
			return SLOW_VB_WANTED;
		if (!buffer_cache_stamp(&cached, &stamp))
			return SLOW_VB_CHANGED;
		if (stamp == cached.stamp) {
			if (stamps)
				add_draw_stamps(stamps, &cached, stamp);
			return SLOW_NOT;
		}
		// A stamp that moved says something was written near the
		// buffer, which is rarely the buffer. That takes the rest of
		// what the slot has.
		cached = slot->data;
		if (slot->seq != seq)
			return SLOW_VB_CHANGED;
		if (!buffer_as_remembered(&cached))
			return SLOW_VB_CHANGED;
		// The lookups came to the same after the stamp was taken, so it
		// goes for them: left as it was, every draw to come would make
		// them again
		if (InterlockedCompareExchange(&slot->seq, seq + 1, seq) == seq) {
			slot->data.stamp = stamp;
			InterlockedExchange(&slot->seq, seq + 2);
		}
		if (stamps)
			add_draw_stamps(stamps, &cached, stamp);
		return SLOW_NOT;
	}
	if (stamps)
		stamps->whole = false;
	return unhashed_buffer_is_boring(va, bytes, kind) ? SLOW_NOT : SLOW_VB_UNKNOWN;
}

// The buffers of a draw all at once: most draws of a frame are of meshes drawn
// the frame before, with the same vertex and index buffers, and asking the
// cache for each of those is most of what a draw not stopped for costs. So
// what the entries of all of them went by is kept together, by what was asked.
const UINT DRAW_MEMO_BUFFERS = 4;
struct DrawMemoKey {
	UINT64 vb_va[DRAW_MEMO_BUFFERS];
	// As bound, which the vertex buffers are asked with: 0 without one
	UINT64 ib_va;
	UINT vb_bytes[DRAW_MEMO_BUFFERS];
	UINT vb_stride[DRAW_MEMO_BUFFERS];
	// Bit per slot asked about, in the order of the above
	UINT vb_slots;
	UINT ib_bytes, ib_format;
	// Whether the index buffer is asked about as well
	UINT ib_asked;
};
struct alignas(64) DrawMemo {
	// Odd while being written
	volatile LONG seq;
	UINT config;
	UINT64 tag;
	UINT64 total;
	UINT count;
	// From draw_memos to the counters
	INT32 counters[DRAW_MEMO_COUNTERS];
	BYTE times[DRAW_MEMO_COUNTERS];
	DrawMemoKey key;
};
const UINT DRAW_MEMO_WAYS = 2;
static DrawMemo draw_memos[8192];

static DrawMemo* draw_memo_bucket(const DrawMemoKey *key, UINT64 *tag)
{
	UINT64 mixed = key->ib_va * 0x9e3779b97f4a7c15ull ^ key->vb_slots ^ ((UINT64)key->ib_asked << 40);
	UINT i;

	for (i = 0; i < DRAW_MEMO_BUFFERS; i++)
		mixed = (mixed ^ key->vb_va[i] ^ ((UINT64)key->vb_bytes[i] << 32) ^ key->vb_stride[i]) * 0xff51afd7ed558ccdull;
	mixed ^= mixed >> 32;
	*tag = mixed | 1;
	return &draw_memos[(mixed >> 20) & (ARRAYSIZE(draw_memos) - DRAW_MEMO_WAYS)];
}

// Without the lock: whether the buffers asked were found boring before with
// nothing they went by changed since
static bool draw_memo_holds(const DrawMemoKey *key)
{
	DrawMemo memo, *slot;
	UINT64 tag, total;
	UINT way, i;
	LONG seq;

	slot = draw_memo_bucket(key, &tag);
	for (way = 0; way < DRAW_MEMO_WAYS; way++, slot++) {
		if (slot->tag != tag)
			continue;
		seq = slot->seq;
		if (seq & 1)
			continue;
		memcpy(&memo.config, &slot->config, sizeof(memo) - offsetof(DrawMemo, config));
		if (slot->seq != seq || memo.tag != tag || memcmp(&memo.key, key, sizeof(*key)))
			continue;
		if (memo.config != config_generation || memo.count > DRAW_MEMO_COUNTERS)
			return false;

		total = 0;
		for (i = 0; i < memo.count; i++)
			total += (UINT64)*(const volatile LONG64*)((const BYTE*)draw_memos + memo.counters[i]) * memo.times[i];
		return total == memo.total;
	}
	return false;
}

static void remember_draw(const DrawMemoKey *key, const DrawStamps *stamps)
{
	DrawMemo *bucket, *slot = NULL;
	UINT64 tag;
	UINT way, i;
	LONG seq;

	bucket = draw_memo_bucket(key, &tag);
	for (way = 0; way < DRAW_MEMO_WAYS && !slot; way++) {
		if (bucket[way].tag == tag || !bucket[way].tag || bucket[way].config != config_generation)
			slot = &bucket[way];
	}
	if (!slot)
		slot = &bucket[(tag >> 1) % DRAW_MEMO_WAYS];

	seq = slot->seq;
	if ((seq & 1) || InterlockedCompareExchange(&slot->seq, seq + 1, seq) != seq)
		return;
	slot->tag = tag;
	slot->config = config_generation;
	slot->total = stamps->total;
	slot->count = stamps->count;
	for (i = 0; i < stamps->count; i++) {
		slot->counters[i] = (INT32)((const BYTE*)stamps->counters[i] - (const BYTE*)draw_memos);
		slot->times[i] = stamps->times[i];
	}
	slot->key = *key;
	InterlockedExchange(&slot->seq, seq + 2);
}

// A frame being analysed has the dumps of the command lists do something,
// which the filter takes them not to
static bool settings_slow()
{
	return G->hunting == HUNTING_MODE_ENABLED || !G->fix_enabled || G->track_region_hashes || G->track_implicit_index_buffers
	    || G->analyse_frame;
}

// Without the lock: whether the command lists of the pipeline would do nothing
// but ask about buffers, as of now. The filter as read is left in *worked_out.
static SlowReason filter_is_boring(const Dx12DrawFilter *filter, Dx12DrawFilter *worked_out)
{
	LONG generation;
	UINT64 guards;

	generation = filter->generation;
	if (!generation || generation != (LONG)config_generation)
		return SLOW_FILTER;
	memcpy(worked_out, filter, sizeof(*worked_out));
	if (filter->generation != generation || worked_out->override_generation != G->shader_override_generation)
		return SLOW_FILTER;
	if (!worked_out->only_checks)
		return SLOW_LISTS;

	guards = (UINT64)guard_bits;
	if ((guards & worked_out->guards_zero) || (guards & worked_out->guards_nonzero) != worked_out->guards_nonzero)
		return SLOW_GUARD;
	// The bits were those the filter means only if the configuration was
	// not read again before they were read, which clears them after
	// moving the generation on
	if ((LONG)config_generation != generation)
		return SLOW_FILTER;
	return SLOW_NOT;
}

bool Dx12FilterOnlyChecks(const Dx12DrawFilter *filter, Dx12DrawFilter *worked_out)
{
	return filter_is_boring(filter, worked_out) == SLOW_NOT;
}

// Without the lock. SLOW_NOT only if the engine would look at the draw and
// leave it at that.
static SlowReason draw_is_boring(Dx12GraphicsCommandList *list, const Dx12PipelineInfo *pipeline)
{
	SlowReason reason;
	const Dx12CmdState *game = &list->mState;
	const Dx12RootSignatureInfo *signature;
	Dx12DrawFilter worked_out;
	DrawMemoKey memo_key = {};
	DrawStamps stamps;
	UINT memo_buffers = 0;
	unsigned long slot;
	LONG64 word;
	UINT vbs, i;

	if (settings_slow())
		return SLOW_SETTINGS;

	// What a dispatch skinned that a mod may be for, see
	// confirm_skin_candidate
	if (skin_noted_live && (pipeline->vb_slots & 1) && game->vertex_buffers[0].BufferLocation
	    && va_filter_has(skin_noted_filter, game->vertex_buffers[0].BufferLocation))
		return SLOW_SKIN;

	// A vertex buffer to be swapped for what a mod had skinned
	for (vbs = skin_outputs_live ? pipeline->vb_slots : 0; _BitScanForward(&slot, vbs); vbs &= vbs - 1) {
		if (slot < ARRAYSIZE(game->vertex_buffers) && game->vertex_buffers[slot].BufferLocation
		    && skin_filter_has(game->vertex_buffers[slot].BufferLocation))
			return SLOW_SKIN;
	}

	reason = filter_is_boring(&pipeline->filter, &worked_out);
	if (reason != SLOW_NOT)
		return reason;

	stamps.count = 0;
	stamps.total = 0;
	stamps.whole = true;

	signature = Dx12ListRootSignature(list, false);
	if (signature && ini_params_reach_shaders(signature))
		return SLOW_INI_PARAMS;

	// What is not there is not looked up, see the Dx12Scope functions
	memo_key.ib_va = game->index_buffer.BufferLocation;
	memo_key.ib_bytes = game->index_buffer.SizeInBytes;
	memo_key.ib_format = game->index_buffer.Format;
	memo_key.ib_asked = worked_out.ib && game->index_buffer.BufferLocation;
	for (vbs = worked_out.vbs & pipeline->vb_slots; _BitScanForward(&slot, vbs); vbs &= vbs - 1) {
		if (slot >= ARRAYSIZE(game->vertex_buffers) || !game->vertex_buffers[slot].BufferLocation)
			continue;
		if (memo_buffers == DRAW_MEMO_BUFFERS) {
			stamps.whole = false;
			break;
		}
		memo_key.vb_va[memo_buffers] = game->vertex_buffers[slot].BufferLocation;
		memo_key.vb_bytes[memo_buffers] = game->vertex_buffers[slot].SizeInBytes;
		memo_key.vb_stride[memo_buffers] = game->vertex_buffers[slot].StrideInBytes;
		memo_key.vb_slots |= 1u << slot;
		memo_buffers++;
	}
	if (!stamps.whole || !draw_memo_holds(&memo_key)) {
		for (vbs = worked_out.vbs & pipeline->vb_slots; _BitScanForward(&slot, vbs); vbs &= vbs - 1) {
			if (slot >= ARRAYSIZE(game->vertex_buffers) || !game->vertex_buffers[slot].BufferLocation)
				continue;
			reason = buffer_is_boring(game->vertex_buffers[slot].BufferLocation, game->vertex_buffers[slot].SizeInBytes,
					PUBLISHED_VERTEX_BUFFER, game->vertex_buffers[slot].StrideInBytes, &game->index_buffer, &stamps);
			if (reason != SLOW_NOT)
				return reason;
		}
		if (worked_out.ib && game->index_buffer.BufferLocation
		    && buffer_is_boring(game->index_buffer.BufferLocation, game->index_buffer.SizeInBytes, PUBLISHED_INDEX_BUFFER, 0, NULL,
				&stamps) != SLOW_NOT)
			return SLOW_IB;
		if (stamps.whole)
			remember_draw(&memo_key, &stamps);
	}

	for (i = 0; i < worked_out.cb_count; i++) {
		Dx12ShaderStage stage = (Dx12ShaderStage)worked_out.cbs[i].stage;
		const Dx12ShaderInfo *shader = stage == DX12_STAGE_CS ? NULL : pipeline->shaders[stage];
		UINT cb = worked_out.cbs[i].slot;

		if (!shader || cb >= DX12_CB_SIZES || !shader->cb_bytes[cb])
			continue;
		if (*boring_constants_slot(stage, cb, shader->cb_bytes[cb], &word) != word)
			return SLOW_CB;
	}

	// What the engine is not shown is left out as it is there, see
	// Dx12ScopeShaderResource
	for (i = 0; i < worked_out.texture_count; i++) {
		Dx12ShaderStage stage = (Dx12ShaderStage)worked_out.textures[i].stage;
		const Dx12ShaderInfo *shader = pipeline->shaders[stage];
		UINT reg = worked_out.textures[i].slot;
		Dx12DescriptorSlot view;

		if (stage == DX12_STAGE_CS || !signature)
			continue;
		if (shader && shader->reflected && !shader->unbounded_srv && reg < 128 && !(shader->srv[reg / 64] & (1ull << (reg % 64))))
			continue;
		if (!Dx12ResolveRegister(game, signature, stage, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, reg, &view) || !view.resource)
			continue;
		if (!texture_is_boring(view.resource))
			return SLOW_TEXTURE;
	}
	return SLOW_NOT;
}

// Without the lock. SLOW_NOT only if the engine would look at the dispatch
// that skins and leave it at that, and nothing of ours stands in for what it
// skins to. The scope of such a dispatch answers for vertex buffer slots 0 and
// SKIN_BLEND_SLOT, no index buffer and the constant buffers of the compute
// shader, see the Dx12Scope functions.
static SlowReason skin_is_boring(const Dx12PipelineInfo *pipeline, const SkinDispatch *skin)
{
	Dx12DrawFilter worked_out;
	SlowReason reason;
	UINT i;

	if (settings_slow())
		return SLOW_SETTINGS;
	// What a mod had skinned to the buffer is to be let go of, see
	// Dx12Dispatch
	if (skin_outputs_live && skin_filter_has(skin->output_va))
		return SLOW_SKIN;
	// What draws said are a mod's vertices, see confirm_skin_candidate
	if (skin_learned_live && va_filter_has(skin_learned_filter, skin->source_va))
		return SLOW_SKIN;
	// What may be another mesh skinned to where one was noted, see
	// forget_noted_other
	if (skin_noted_live && va_filter_has(skin_noted_filter, skin->output_va))
		return SLOW_SKIN;

	reason = filter_is_boring(&pipeline->filter, &worked_out);
	if (reason != SLOW_NOT)
		return reason;

	if (worked_out.vbs & 1) {
		reason = buffer_is_boring(skin->source_va, (UINT64)skin->count * skin->stride, PUBLISHED_SKIN_BUFFER, skin->stride, NULL);
		if (reason != SLOW_NOT)
			return reason;
	}
	if (worked_out.vbs & 1u << SKIN_BLEND_SLOT) {
		reason = buffer_is_boring(skin->weights_va, (UINT64)skin->count * skin->weight_stride, PUBLISHED_SKIN_BUFFER,
				skin->weight_stride, NULL);
		if (reason != SLOW_NOT)
			return reason;
	}
	for (i = 0; i < worked_out.cb_count; i++) {
		if (worked_out.cbs[i].stage == DX12_STAGE_CS)
			return SLOW_CB;
	}
	// The scope of a dispatch has none for the other stages
	for (i = 0; i < worked_out.texture_count; i++) {
		if (worked_out.textures[i].stage == DX12_STAGE_CS)
			return SLOW_TEXTURE;
	}
	return SLOW_NOT;
}

// With the scope of a draw the engine looked at open: has everything the
// filter goes by asked about, as the command lists may not have got to all of
// it, for draw_is_boring to find the answers
static void prime_filter(const Dx12PipelineInfo *pipeline)
{
	const Dx12DrawFilter *filter = &pipeline->filter;
	unsigned long slot;
	DXGI_FORMAT format;
	UINT vbs, i, stride;

	if (!filter->only_checks)
		return;

	for (vbs = filter->vbs; _BitScanForward(&slot, vbs); vbs &= vbs - 1)
		Dx12ScopeVertexBuffer(slot, &stride);
	if (filter->ib)
		Dx12ScopeIndexBuffer(&format);
	for (i = 0; i < filter->cb_count; i++) {
		Dx12ShaderStage stage = (Dx12ShaderStage)filter->cbs[i].stage;
		const Dx12ShaderInfo *shader = stage == DX12_STAGE_CS ? NULL : pipeline->shaders[stage];
		UINT cb = filter->cbs[i].slot;

		if (shader && cb < DX12_CB_SIZES && shader->cb_bytes[cb])
			publish_constants(stage, cb, shader->cb_bytes[cb]);
	}
	for (i = 0; i < filter->texture_count; i++)
		Dx12ScopeShaderResource((Dx12ShaderStage)filter->textures[i].stage, filter->textures[i].slot);
}

// -----------------------------------------------------------------------------
// The calls of the game

void HackerContext::Dx12BeforeDraw(DrawContext &data, const UINT64 *shader_hashes)
{
	// The draws that were not stopped for count as well
	draw_number += InterlockedExchange(&fast_draws_pending, 0);

	mCurrentVertexShader = shader_hashes[DX12_STAGE_VS];
	mCurrentPixelShader = shader_hashes[DX12_STAGE_PS];
	mCurrentGeometryShader = shader_hashes[DX12_STAGE_GS];
	mCurrentHullShader = shader_hashes[DX12_STAGE_HS];
	mCurrentDomainShader = shader_hashes[DX12_STAGE_DS];

	// What the Set*Shader and IASet* methods do for hunting under D3D11
	if (G->hunting == HUNTING_MODE_ENABLED) {
		DXGI_FORMAT format;
		UINT slot, stride;

		// The hashes a TextureOverride section would match the buffers by
		mCurrentIndexBuffer = data.call_info.IndexCount ? GetResourceHash(Dx12ScopeIndexBuffer(&format)) : 0;
		for (slot = 0; slot < ARRAYSIZE(mCurrentVertexBuffers); slot++)
			mCurrentVertexBuffers[slot] = GetResourceHash(Dx12ScopeVertexBuffer(slot, &stride));

		EnterCriticalSectionPretty(&G->mCriticalSection);
		RegisterVisitedIndexBufferNoLock(mCurrentIndexBuffer);
		for (slot = 0; slot < ARRAYSIZE(mCurrentVertexBuffers); slot++)
			RegisterVisitedVertexBufferNoLock(mCurrentVertexBuffers[slot], slot);
		if (mCurrentVertexShader)
			G->mVisitedVertexShaders.insert(mCurrentVertexShader);
		if (mCurrentPixelShader)
			G->mVisitedPixelShaders.insert(mCurrentPixelShader);
		if (mCurrentGeometryShader)
			G->mVisitedGeometryShaders.insert(mCurrentGeometryShader);
		if (mCurrentHullShader)
			G->mVisitedHullShaders.insert(mCurrentHullShader);
		if (mCurrentDomainShader)
			G->mVisitedDomainShaders.insert(mCurrentDomainShader);
		LeaveCriticalSection(&G->mCriticalSection);
	}

	BeforeDraw(data);
}

void HackerContext::Dx12AfterDraw(DrawContext &data)
{
	AfterDraw(data);
}

UINT64 HackerContext::Dx12CurrentShader(wchar_t shader_type)
{
	bool compute = scope && scope->compute;

	if (!Dx12InDrawScope())
		return 0;

	switch (shader_type) {
	case L'v': return compute && !scope->skin ? 0 : mCurrentVertexShader;
	case L'h': return compute ? 0 : mCurrentHullShader;
	case L'd': return compute ? 0 : mCurrentDomainShader;
	case L'g': return compute ? 0 : mCurrentGeometryShader;
	case L'p': return compute ? 0 : mCurrentPixelShader;
	case L'c': return compute ? mCurrentComputeShader : 0;
	}
	return 0;
}

const ShaderBindings* HackerContext::Dx12CurrentShaderBindings(wchar_t shader_type)
{
	// Of a D3D12 shader the registers are known, but not what is declared
	// at them
	if (scope)
		return NULL;

	switch (shader_type) {
	case L'v': return VkScopeShaderBindings(DX12_STAGE_VS);
	case L'h': return VkScopeShaderBindings(DX12_STAGE_HS);
	case L'd': return VkScopeShaderBindings(DX12_STAGE_DS);
	case L'g': return VkScopeShaderBindings(DX12_STAGE_GS);
	case L'p': return VkScopeShaderBindings(DX12_STAGE_PS);
	}
	return NULL;
}

bool HackerContext::Dx12BeforeDispatch(DispatchContext *context, UINT64 shader_hash)
{
	mCurrentComputeShader = shader_hash;

	if (G->hunting == HUNTING_MODE_ENABLED && shader_hash) {
		EnterCriticalSectionPretty(&G->mCriticalSection);
		G->mVisitedComputeShaders.insert(shader_hash);
		LeaveCriticalSection(&G->mCriticalSection);
	}

	return BeforeDispatch(context);
}

void HackerContext::Dx12AfterDispatch(DispatchContext *context)
{
	AfterDispatch(context);
}

// Scopes are opened and closed often enough to keep the vector's memory
static Dx12DrawScope scope_storage;

bool Dx12Draw(Dx12GraphicsCommandList *list, bool indexed, UINT count, UINT instances, UINT start,
		INT base_vertex, UINT start_instance)
{
	const Dx12PipelineInfo *pipeline = Dx12ListPipeline(list);
	UINT64 hashes[DX12_STAGE_COUNT] = {};
	UINT64 times[5] = {};
	bool timed = G->dx12_dump_hashes;
	SlowReason reason;
	int stage;

	if (!pipeline)
		return false;
	reason = draw_is_boring(list, pipeline);
	if (reason == SLOW_NOT) {
		list->mFastDraws++;
		return false;
	}
	Dx12FlushFastDraws(list);
	if (timed)
		InterlockedIncrement(&slow_draws[reason]);

	for (stage = 0; stage < DX12_STAGE_COUNT; stage++)
		hashes[stage] = pipeline->shaders[stage] ? pipeline->shaders[stage]->hash : 0;

	if (timed)
		times[0] = ticks();
	Dx12ShimLock();
	if (!dx12_shim || scope) {
		Dx12ShimUnlock();
		return false;
	}
	if (timed)
		times[1] = ticks();

	if (pipeline->filter.generation != (LONG)config_generation
	    || pipeline->filter.override_generation != G->shader_override_generation)
		work_out_filter(pipeline, pipeline->shaders);

	if (reason == SLOW_SKIN) {
		// Whether the filter was right, to size it by
		unsigned long slot;
		UINT vbs;
		bool any = false;

		for (vbs = pipeline->vb_slots; _BitScanForward(&slot, vbs); vbs &= vbs - 1) {
			if (slot < ARRAYSIZE(list->mState.vertex_buffers) && list->mState.vertex_buffers[slot].BufferLocation
			    && skin_output_at(list->mState.vertex_buffers[slot].BufferLocation))
				any = true;
		}
		if (skin_noted.count(list->mState.vertex_buffers[0].BufferLocation))
			any = true;
		stats.skin_filter_misses += !any;
	}

	DrawContext c = indexed
		? DrawContext(DrawCall::DrawIndexedInstanced, 0, count, instances, base_vertex, start, start_instance, NULL, 0)
		: DrawContext(DrawCall::DrawInstanced, count, 0, instances, start, 0, start_instance, NULL, 0);

	open_scope(&scope_storage, list, pipeline, false);

	dx12_shim->hacker_context->Dx12BeforeDraw(c, hashes);
	if (G->analyse_frame)
		Dx12FrameDumpDraw(list, pipeline, indexed, count, instances, start, base_vertex, start_instance);
	if (timed)
		times[2] = ticks();
	stats.draws++;
	stats.skipped += c.call_info.skip;
	if (!c.call_info.skip) {
		apply_overrides(false);
		if (indexed)
			list->mOrig->DrawIndexedInstanced(count, instances, start, base_vertex, start_instance);
		else
			list->mOrig->DrawInstanced(count, instances, start, start_instance);
	}
	if (timed)
		times[3] = ticks();
	dx12_shim->hacker_context->Dx12AfterDraw(c);

	prime_filter(pipeline);
	if (!skin_noted.empty())
		confirm_skin_candidate(indexed);
	close_scope();
	if (timed) {
		times[4] = ticks();
		stats.wait_ticks += times[1] - times[0];
		stats.before_ticks += times[2] - times[1];
		stats.draw_ticks += times[3] - times[2];
		stats.after_ticks += times[4] - times[3];
	}
	Dx12ShimUnlock();
	return true;
}

bool Dx12Dispatch(Dx12GraphicsCommandList *list, UINT x, UINT y, UINT z)
{
	const Dx12PipelineInfo *pipeline = Dx12ListPipeline(list);
	DispatchContext c(x, y, z);
	SkinDispatch skin;

	bool skinning, unconfirmed = false, ran;
	SlowReason reason;

	if (!pipeline || !pipeline->shaders[DX12_STAGE_CS])
		return false;

	// Settled without the lock: the dispatch may be one the engine would
	// do nothing about, see skin_is_boring
	skinning = is_skinning_shader(pipeline->shaders[DX12_STAGE_CS]);
	if (skinning && !resolve_skin(list, Dx12ListRootSignature(list, true), x, y, z, &skin)) {
		skinning = false;
		unconfirmed = true;
	}
	if (skinning) {
		reason = skin_is_boring(pipeline, &skin);
		if (reason == SLOW_NOT) {
			if (G->dx12_dump_hashes)
				InterlockedIncrement(&fast_skins);
			list->mFastDraws++;
			return false;
		}
		if (G->dx12_dump_hashes)
			InterlockedIncrement(&slow_draws[reason]);
	}

	Dx12FlushFastDraws(list);

	Dx12ShimLock();
	if (!dx12_shim || scope) {
		Dx12ShimUnlock();
		return false;
	}
	stats.skin_unconfirmed += unconfirmed;

	if (skinning) {
		UINT64 hashes[DX12_STAGE_COUNT] = {};
		const Dx12ShaderInfo *shaders[DX12_STAGE_COUNT] = {};
		DrawContext draw(DrawCall::Draw, skin.count, 0, 0, 0, 0, 0, NULL, 0);

		if (!skin_noted.empty())
			forget_noted_other(&skin);

		// The shader that skins is what a mod's command lists for
		// vertex shaders are run for
		shaders[DX12_STAGE_VS] = pipeline->shaders[DX12_STAGE_CS];
		hashes[DX12_STAGE_VS] = shaders[DX12_STAGE_VS]->hash;
		if (pipeline->filter.generation != (LONG)config_generation
		    || pipeline->filter.override_generation != G->shader_override_generation)
			work_out_filter(pipeline, shaders);

		open_scope(&scope_storage, list, pipeline, true);
		scope_storage.skin = &skin;

		dx12_shim->hacker_context->Dx12BeforeDraw(draw, hashes);
		stats.skin_dispatches++;
		// Also where a mod skips it: the mod's vertices went to a
		// buffer of ours, and whatever else the game does with its
		// own is not to find it without any
		restore_list();
		list->mOrig->Dispatch(x, y, z);
		if (dx12_standins_live)
			game_wrote_buffers(list, scope_serial);
		// A mod that skinned its own vertices without skipping the
		// game's still has them drawn on D3D11 while it leaves them
		// bound: the game's draw that follows skins what is bound, to
		// the same buffer. One that bound something else has that
		// drawn, which nothing of ours holds.
		if (!draw.call_info.skip && !mod_skin_inputs_bound()) {
			forget_skin_output(skin.output_va);
			if (G->dx12_dump_hashes)
				dump_skin_source(&skin, hashes[DX12_STAGE_VS]);
		}
		dx12_shim->hacker_context->Dx12AfterDraw(draw);

		prime_filter(pipeline);
		close_scope();
		Dx12ShimUnlock();
		return true;
	}

	open_scope(&scope_storage, list, pipeline, true);

	ran = dx12_shim->hacker_context->Dx12BeforeDispatch(&c, pipeline->shaders[DX12_STAGE_CS]->hash);
	if (ran) {
		apply_overrides(false);
		list->mOrig->Dispatch(x, y, z);
	}
	dx12_shim->hacker_context->Dx12AfterDispatch(&c);
	if (ran && dx12_standins_live)
		game_wrote_buffers(list, scope_serial);

	close_scope();
	Dx12ShimUnlock();
	return true;
}

void Dx12ToggleMods()
{
	dx12_mods_disabled = !dx12_mods_disabled;
	Dx12LogInfo("disable_dx12_mods pressed - mods %s\n", dx12_mods_disabled ? "disabled" : "enabled");
}

void Dx12DumpMarkedShader(UINT64 hash, const char *type)
{
	const Dx12ShaderInfo *shader = Dx12FindShader(hash);
	wchar_t path[MAX_PATH];
	FILE *file = NULL;

	if (!shader || !shader->bytecode) {
		LogOverlay(LOG_WARNING, "> Shader %016llx was created with hunting off and has not been kept\n", hash);
		return;
	}

	if (!G->SHADER_PATH[0]) {
		LogOverlay(LOG_WARNING, "> No override_directory to write the marked shader to\n");
		return;
	}

	CreateDirectoryEnsuringAccess(G->SHADER_PATH);
	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%S.%ls", G->SHADER_PATH, hash, type, shader->dxil ? L"dxil" : L"dxbc");
	_wfopen_s(&file, path, L"wb");
	if (!file) {
		LogOverlay(LOG_WARNING, "> Unable to create %S\n", path);
		return;
	}
	fwrite(shader->bytecode, 1, shader->size, file);
	fclose(file);
	Dx12LogInfo("Marked shader written to %S\n", path);

	// The disassembler and the decompiler only read DXBC
	if (shader->dxil)
		LogOverlay(LOG_INFO, "> DXIL shader written as the game gave it: %S\n", path);
	else
		CopyBytecodeToFixes(hash, type, shader->bytecode, shader->size);
}

// The hashes in a form to copy from, which the overlay is not
static void log_selected_shaders()
{
	static const char *const names[] = { "vertex", "pixel", "geometry", "hull", "domain", "compute" };
	static UINT64 logged[ARRAYSIZE(names)];
	const UINT64 selected[ARRAYSIZE(names)] = {
		G->mSelectedVertexShader, G->mSelectedPixelShader, G->mSelectedGeometryShader,
		G->mSelectedHullShader, G->mSelectedDomainShader, G->mSelectedComputeShader,
	};
	size_t i;

	for (i = 0; i < ARRAYSIZE(names); i++) {
		if (selected[i] == logged[i])
			continue;
		logged[i] = selected[i];
		if (selected[i] != (UINT64)-1)
			Dx12LogInfo("Selected %s shader: hash_dx12 = %016llx\n", names[i], selected[i]);
	}
}

// A [ShaderRegex] section without patterns applies to every shader of the
// models it names, which is how a mod gets to look at every draw. Under D3D11
// that is settled as shaders are created. The game's shaders are not ours
// here, so the ones it has made pipelines of since the last frame are linked
// up now.
static size_t regex_linked_shaders;

void Dx12DrawPathConfigReloaded()
{
	regex_linked_shaders = 0;
	forget_unpacked_buffers();
	// What the mods wrote before is not what the ones there now would
	// write, if they write at all. The lists that use it have it pinned.
	for (auto &entry : published)
		release_standin(&entry.second);

	// Which part of a vertex buffer counts depended on what the mods
	// ask for, as does all that was worked out from their sections
	config_generation++;
	buffer_cache_generation++;
	guard_count = 0;
	InterlockedExchange64(&guard_bits, 0);
	for (auto i = reaches.begin(); i != reaches.end();) {
		if (i->second.state != REACH_PENDING)
			i = reaches.erase(i);
		else
			++i;
	}
}

static UINT reported_epoch;

static void note_memory()
{
	DXGI_QUERY_VIDEO_MEMORY_INFO process = {};
	UINT64 skinned = 0, standin_bytes = 0;
	unsigned standins = 0;
	LARGE_INTEGER frequency;

	for (const auto &output : skin_outputs)
		skinned += (output.second.buffer ? output.second.bytes : 0) + (output.second.spare ? output.second.spare_bytes : 0);
	for (const auto &entry : published) {
		D3D11_BUFFER_DESC desc;

		if (!entry.second.standin)
			continue;
		entry.second.standin->GetDesc(&desc);
		standin_bytes += desc.ByteWidth;
		standins++;
	}
	Dx12ShimProcessMemory(&process);
	QueryPerformanceFrequency(&frequency);
	Dx12DumpNote("  Memory of the GPU: mods have %ld textures of %lld MB and %ld buffers of %lld MB (%ld of them under 64 KB, "
		"which take %lld MB for %lld KB; %u stand in for buffers of the game, %llu MB), and %ld more for writes while the game's lists "
		"use %ld of them, %lld MB; %llu MB of vertices skinned for mods in %Iu places; the process uses %llu MB of the GPU's memory, "
		"which it was given %llu MB of\n",
		dx12_memory.textures, dx12_memory.texture_bytes >> 20, dx12_memory.buffers, dx12_memory.buffer_bytes >> 20,
		dx12_memory.small_buffers, ((LONG64)dx12_memory.small_buffers * D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) >> 20,
		dx12_memory.small_buffer_asked >> 10, standins, standin_bytes >> 20, dx12_memory.spares, dx12_memory.renamed,
		dx12_memory.spare_bytes >> 20, skinned >> 20, skin_outputs.size(), process.CurrentUsage >> 20, process.Budget >> 20);
	Dx12DumpNote("  Evicted mod resources: %ld of %lld MB now; since the last report %ld of %lld MB evicted, %ld of %lld MB made "
		"resident again in %.1f ms, %ld uses left out as that failed\n",
		dx12_memory.evicted, dx12_memory.evicted_bytes >> 20, dx12_memory.evictions, dx12_memory.eviction_bytes >> 20,
		dx12_memory.restores, dx12_memory.restore_bytes >> 20, dx12_memory.restore_ticks * 1000.0 / frequency.QuadPart,
		dx12_memory.restore_failures);
	dx12_memory.evictions = dx12_memory.restores = dx12_memory.restore_failures = 0;
	dx12_memory.eviction_bytes = dx12_memory.restore_bytes = dx12_memory.restore_ticks = 0;
}

static void link_shader_regex()
{
	std::vector<const Dx12ShaderInfo*> found;

	Dx12GetShaders(regex_linked_shaders, &found);
	regex_linked_shaders += found.size();
	if (shader_regex_groups.empty())
		return;

	for (const Dx12ShaderInfo *shader : found) {
		link_shader_regex_groups_dx12(shader);

		// The engine is shown a dispatch that skins as a draw with this
		// shader for its vertex shader, see Dx12Dispatch
		if (is_skinning_shader(shader)) {
			Dx12ShaderInfo as_vertex_shader = {};

			as_vertex_shader.hash = shader->hash;
			as_vertex_shader.stage = DX12_STAGE_VS;
			if (link_shader_regex_groups_dx12(&as_vertex_shader))
				Dx12DumpNote("Skinning: compute shader %016llx is taken to skin meshes, its dispatches are draws to the mods\n", shader->hash);
		}
	}
}

void Dx12DrawPathFramePresented()
{
	bool all = G->hunting == HUNTING_MODE_ENABLED && !dx12_mods_disabled;
	LARGE_INTEGER frequency;
	LONG reasons[SLOW_REASONS];
	double per_frame;
	HashSet *hashes;

	if (G->hunting == HUNTING_MODE_ENABLED)
		log_selected_shaders();

	link_shader_regex();
	update_reaches();
	if (!skin_dumps.empty())
		finish_skin_dumps();
	// Of the frame that ended, whose count the engine has reset
	InterlockedExchange(&fast_draws_pending, 0);

	if (G->dx12_dump_hashes && !(dx12_frame % 300)) {
		Dx12DumpNote("Frame %u, since the last of these lines: the engine saw %u draws and skipped %u, mods drew %u times and dispatched %u; "
			"vertex buffers newly matched %u, constant buffer lookups matched %u, index reach worked out %u, failed %u, pending %Iu; "
			"%Iu shaders linked to ShaderRegex, %Iu ShaderOverride hashes; %u submissions of our own list\n",
			dx12_frame, stats.draws, stats.skipped, stats.mod_draws, stats.mod_dispatches,
			stats.vertex_buffers_matched, stats.constant_buffers_matched, stats.reaches_known, stats.reaches_failed,
			pending_reaches.size(), regex_linked_shaders, G->mShaderOverrideMap.size(),
			dx12_shim_epoch - reported_epoch);
		QueryPerformanceFrequency(&frequency);
		per_frame = 1000.0 / frequency.QuadPart / 300;
		Dx12DumpNote("  Per frame the draws took %.2f ms waiting for the lock, %.2f ms before the draw (%.2f ms of that asking about "
			"vertex buffers), %.2f ms applying and drawing, %.2f ms after; %u buffers were answered as before, %u looked up; "
			"%u more draws and %u dispatches that skin were not stopped for\n",
			stats.wait_ticks * per_frame, stats.before_ticks * per_frame, stats.buffer_ticks * per_frame,
			stats.draw_ticks * per_frame, stats.after_ticks * per_frame, stats.buffers_remembered, stats.buffers_looked_up,
			(unsigned)InterlockedExchange(&fast_draws, 0), (unsigned)InterlockedExchange(&fast_skins, 0));
		for (LONG &count : reasons)
			count = InterlockedExchange(&slow_draws[&count - reasons], 0);
		Dx12DumpNote("  The others were stopped for because of: settings %d, a pipeline not looked at yet %d, command lists doing more "
			"than asking %d, a condition of a command list %d (%u watched), IniParams %d, a vertex buffer not known %d, wanted by a mod %d, "
			"written since %d, the index buffer %d, "
			"a constant buffer %d, a buffer a mod had skinned %d, a texture %d; of the buffers looked up %u were none known, %u not settled, "
			"%u too large to watch, %u new\n",
			reasons[SLOW_SETTINGS], reasons[SLOW_FILTER], reasons[SLOW_LISTS], reasons[SLOW_GUARD], guard_count,
			reasons[SLOW_INI_PARAMS], reasons[SLOW_VB_UNKNOWN],
			reasons[SLOW_VB_WANTED], reasons[SLOW_VB_CHANGED], reasons[SLOW_IB], reasons[SLOW_CB], reasons[SLOW_SKIN], reasons[SLOW_TEXTURE],
			stats.lookups_missing, stats.lookups_unsettled, stats.lookups_unstamped, stats.lookups_new);
		Dx12DumpNote("  Skinning: %u dispatches of the game were shown to the mods as draws and %u with such a shader were not, "
			"mods had their own vertices skinned %u times and failed to %u times, %u vertex buffers of draws were swapped for those, "
			"%Iu of them are kept, %u draws the filter stopped for one had none. Descriptor tables copied: %u in the game's reserve, "
			"%u in the side heap for mods and %u for the game's own, of %u descriptors in all, switching to that heap %u times; "
			"%u tables could not be for having no end or no known heap, %u for want of room\n",
			stats.skin_dispatches, stats.skin_unconfirmed, stats.skin_replaced, stats.skin_failed, stats.skin_swapped,
			skin_outputs.size(), stats.skin_filter_misses, stats.table_copies, stats.side_copies, stats.side_base_copies,
			stats.table_descriptors, stats.side_switches, stats.table_unbounded, stats.table_no_room);
		Dx12DumpNote("  Buffers of the game that mods write: %u have one of ours standing in, which %u vertex buffers of mod draws were swapped for\n",
			dx12_standins_live, stats.written_swapped);
		Dx12DumpNote("  Mod draws with shaders of their own: %u made with them, %u with the game's for want of a pipeline with them\n",
			stats.custom_draws, stats.custom_failed);
		Dx12DumpNote("  The fence was signalled %lld times for what game lists had pinned, which %lld times were executed on "
			"another queue than ours, which was made to wait for ours %lld times and %lld times ran them without, not being able to; "
			"%lld pages of upload memory were created and %lld released; %lld times a resource "
			"of a mod was given another D3D12 resource for a write while game lists use it\n",
			InterlockedExchange64(&dx12_stats.pin_signals, 0), InterlockedExchange64(&dx12_stats.pin_lates, 0),
			InterlockedExchange64(&dx12_stats.queue_waits, 0), InterlockedExchange64(&dx12_stats.queue_waits_skipped, 0),
			InterlockedExchange64(&dx12_stats.upload_pages_created, 0),
			InterlockedExchange64(&dx12_stats.upload_pages_released, 0),
			InterlockedExchange64(&dx12_stats.resources_renamed, 0));
		note_memory();
		stats = {};
		reported_epoch = dx12_shim_epoch;
	}

	if (seen_override_generation != G->shader_override_generation) {
		seen_override_generation = G->shader_override_generation;

		hashes = new HashSet();
		for (const auto &entry : G->mShaderOverrideMap)
			hashes->insert(entry.first);

		AcquireSRWLockExclusive(&wanted_lock);
		std::swap(wanted_hashes, hashes);
		ReleaseSRWLockExclusive(&wanted_lock);
		delete hashes;
		config_generation++;
		dx12_draw_generation++;
	}

	if (want_all != all) {
		want_all = all;
		dx12_draw_generation++;
	}

	// The key for comparing against the game as it is switches the
	// overrides off, but hunting carries on
	dx12_draw_path_active = all || (!dx12_mods_disabled && G->fix_enabled && wanted_hashes && !wanted_hashes->empty());

	if (!(dx12_frame & 255)) {
		forget_unused_published();
		forget_unused_skin_outputs();
	}
	if (!skin_noted.empty())
		expire_skin_candidates();

	refresh_guards();
}
