#include "DrawPath.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Bridge.h"
#include "BufferHash.h"
#include "HashDump.h"
#include "ShimContext.h"
#include "ShimObjects.h"
#include "TextureHash.h"
#include "util.h"

#include "../globals.h"
#include "../HackerContext.h"
#include "../ResourceHash.h"

volatile bool dx12_draw_path_active;
volatile UINT dx12_draw_generation = 1;
bool dx12_mods_disabled;

// What the engine did with the game's draws since the last report
static struct {
	unsigned draws, skipped, mod_draws, mod_dispatches;
	unsigned vertex_buffers_matched, constant_buffers_matched;
	unsigned reaches_known, reaches_failed;
} stats;

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
	const Dx12PipelineInfo *pipeline;
	bool wanted = false;

	pipeline = state ? Dx12FindPipeline(state) : NULL;
	if (pipeline) {
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

	UINT frame;
};

static std::unordered_map<PublishedKey, Published, PublishedKeyHash> published;
// Counts configurations, as which of the hashes a resource could have had
// under D3D11 is the one to use depends on what the mods ask for
static UINT config_generation = 1;

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

static void release_published(Published *entry)
{
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
	desc.Width = key->ib_bytes + vb_bytes;
	desc.Height = 1;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
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

	reach->vb_bytes = vb_bytes;
	reach->fence = Dx12ShimPendingFence();
	return true;
}

// What is known about a vertex buffer drawn with the index buffer of the view,
// or NULL. Sets off finding out if it is not known.
static const Dx12BufferHash* vertex_reach(ID3D12Resource *vb, UINT64 vb_offset, UINT64 vb_bytes, UINT stride,
		const Dx12BufferHash *vb_upload, const D3D12_INDEX_BUFFER_VIEW *view)
{
	Dx12BufferHash ib_upload;
	ReachKey key = {};

	if (!stride || !view->BufferLocation || !view->SizeInBytes || vb_bytes < stride)
		return NULL;
	if (view->Format != DXGI_FORMAT_R16_UINT && view->Format != DXGI_FORMAT_R32_UINT)
		return NULL;
	// Only for what was uploaded once and left alone: anything else is
	// not a mesh a mod could have a hash for
	if (!Dx12FindBuffer(view->BufferLocation, &key.ib, &key.ib_offset) || !Dx12FindBufferHash(key.ib, key.ib_offset, &ib_upload))
		return NULL;

	key.vb = vb;
	key.vb_offset = vb_offset;
	key.ib_bytes = (UINT)min((UINT64)view->SizeInBytes, ib_upload.bytes);
	key.index_bytes = view->Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
	key.stride = stride;

	auto found = reaches.find(key);
	if (found != reaches.end()) {
		VertexReach *reach = &found->second;

		reach->frame = dx12_frame;
		if (reach->state == REACH_PENDING)
			return NULL;
		if (reach->vb_upload == vb_upload->data_hash && reach->ib_upload == ib_upload.data_hash)
			return reach->state == REACH_KNOWN ? &reach->hash : NULL;
		reaches.erase(found);
	}

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
	swprintf_s(name, L"\\%llu_%u_%u_%u.bin", key->vb_offset, key->ib_bytes, key->index_bytes, key->stride);
	if (wcscat_s(path, name))
		return;

	_wfopen_s(&file, path, L"wb");
	if (!file)
		return;
	fwrite(data, 1, (size_t)bytes, file);
	fclose(file);
}

static void finish_reach(const ReachKey *key, VertexReach *reach)
{
	D3D12_RANGE range = { 0, (SIZE_T)(key->ib_bytes + reach->vb_bytes) }, nothing = {};
	UINT64 last = 0, limit = reach->vb_bytes / key->stride;
	size_t next_end = 0;
	// Vertices used up to the end of each section, which never go down
	std::vector<UINT64> ends;
	Dx12BufferHash hash = {}, matched = {};
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
			if (!matched.bytes && key->ib_bytes + reach->vb_bytes >= 65536)
				dump_reach(key, data, key->ib_bytes + reach->vb_bytes);
		}
		reach->readback->Unmap(0, &nothing);
	}

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
	for (auto j = reaches.begin(); j != reaches.end();) {
		if (j->second.state != REACH_PENDING && dx12_frame - j->second.frame > 600)
			j = reaches.erase(j);
		else
			++j;
	}
}

static Published* publish_resource(ID3D12Resource *resource)
{
	Published *entry;
	Dx12ResourceInfo info;
	D3D11_TEXTURE2D_DESC desc2d;
	D3D11_RESOURCE_DIMENSION dimension;

	if (!Dx12GetResourceInfo(resource, &info))
		return NULL;

	entry = &published[{ resource, 0, PUBLISHED_RESOURCE }];
	entry->frame = dx12_frame;

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
	}
	return entry;
}

// stride and indices are for a vertex buffer
static Published* publish_buffer(UINT64 va, UINT64 bytes, UINT kind, UINT stride, const D3D12_INDEX_BUFFER_VIEW *indices)
{
	D3D12_RESOURCE_DESC desc = {};
	const Dx12BufferHash *reach = NULL;
	Dx12BufferHash data = {};
	ID3D12Resource *resource;
	Published *entry;
	UINT64 offset;
	uint32_t hash;
	bool has_hash;

	if (!Dx12FindBuffer(va, &resource, &offset))
		return NULL;
	has_hash = Dx12FindBufferHash(resource, offset, &data);
	if (has_hash && indices)
		reach = vertex_reach(resource, offset, bytes, stride, &data, indices);

	entry = &published[{ resource, offset, kind }];
	entry->frame = dx12_frame;

	if (entry->resource && entry->bytes == bytes) {
		if (entry->has_data_hash == has_hash && entry->data_hash == data.data_hash && entry->config == config_generation
		    && entry->reach_bytes == (reach ? reach->bytes : 0) && entry->reach_hash == (reach ? reach->data_hash : 0))
			return entry;
	} else {
		release_published(entry);
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		entry->resource = new_game_buffer(&desc, bytes,
			kind == PUBLISHED_INDEX_BUFFER ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_VERTEX_BUFFER);
		entry->bytes = bytes;
	}

	entry->has_data_hash = has_hash;
	entry->data_hash = data.data_hash;
	entry->reach_bytes = reach ? reach->bytes : 0;
	entry->reach_hash = reach ? reach->data_hash : 0;
	entry->config = config_generation;

	// Without a hash for the data there is nothing a mod could have
	// matched under D3D11 either
	hash = has_hash ? pick_buffer_hash(&data, reach, kind) : 0;
	set_resource_hash(entry->resource, D3D11_RESOURCE_DIMENSION_BUFFER, hash, data.data_hash, NULL);
	if (kind == PUBLISHED_VERTEX_BUFFER && G->mTextureOverrideMap.count(hash)) {
		stats.vertex_buffers_matched++;
		Dx12DumpNote("Vertex buffer %p+%llu matches the TextureOverride sections of hash %08x\n", resource, offset, hash);
	}
	return entry;
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
};
static std::unordered_map<UINT64, PublishedConstants> published_constants;

static uint32_t constant_buffer_hash(UINT bytes)
{
	D3D11_BUFFER_DESC desc = {};

	desc.ByteWidth = bytes;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	return crc32c_hw(0, &desc, sizeof(desc));
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

struct Dx12DrawScope {
	Dx12GraphicsCommandList *list;
	bool compute;

	// What the shim context had bound before, which is the business of
	// the frame's own command lists
	ShimState saved;

	// What has been changed on the list and is to be put back
	UINT vbs_changed;
	bool ib_changed;
	bool topology_changed;
	std::vector<ChangedParameter> parameters;
};

static Dx12DrawScope *scope;

static void log_once(bool *reported, const char *message)
{
	if (*reported)
		return;
	*reported = true;
	Dx12DumpNote("*** %s\n", message);
}

bool Dx12InDrawScope()
{
	return !!scope;
}

void Dx12ScopeUnsupported(const char *what)
{
	static std::unordered_set<std::string> reported;

	if (reported.insert(what).second)
		Dx12DumpNote("*** A mod tried %s for a draw or dispatch of the game, which is not possible on D3D12 yet. Ignored.\n", what);
}

void Dx12ScopeDispatched()
{
	stats.mod_dispatches++;
}

ID3D11Buffer* Dx12ScopeVertexBuffer(UINT slot, UINT *stride)
{
	const D3D12_VERTEX_BUFFER_VIEW *view;
	Published *entry;

	*stride = 0;
	if (scope->compute || slot >= ARRAYSIZE(scope->list->mState.vertex_buffers))
		return NULL;

	view = &scope->list->mState.vertex_buffers[slot];
	entry = publish_buffer(view->BufferLocation, view->SizeInBytes, PUBLISHED_VERTEX_BUFFER, view->StrideInBytes,
		&scope->list->mState.index_buffer);
	if (!entry)
		return NULL;

	*stride = view->StrideInBytes;
	return static_cast<ShimBuffer*>(entry->resource);
}

ID3D11Buffer* Dx12ScopeIndexBuffer(DXGI_FORMAT *format)
{
	const D3D12_INDEX_BUFFER_VIEW *view = &scope->list->mState.index_buffer;
	Published *entry;

	*format = DXGI_FORMAT_UNKNOWN;
	if (scope->compute)
		return NULL;

	entry = publish_buffer(view->BufferLocation, view->SizeInBytes, PUBLISHED_INDEX_BUFFER, 0, NULL);
	if (!entry)
		return NULL;

	*format = view->Format;
	return static_cast<ShimBuffer*>(entry->resource);
}

ID3D11Buffer* Dx12ScopeConstantBuffer(Dx12ShaderStage stage, UINT slot)
{
	const Dx12PipelineInfo *pipeline = Dx12FindPipeline(scope->list->mState.pipeline);
	const Dx12ShaderInfo *shader = pipeline ? pipeline->shaders[stage] : NULL;
	D3D12_RESOURCE_DESC desc = {};
	Dx12ResourceInfo info;
	ID3D12Resource *resource;
	Dx12DescriptorSlot view;
	ShimBuffer *buffer;
	UINT64 offset;
	UINT bytes;

	if (!scope || scope->compute != (stage == DX12_STAGE_CS))
		return NULL;
	// The shader is what says how much of what is at the address is the
	// buffer
	if (!shader || slot >= DX12_CB_SIZES || !shader->cb_bytes[slot])
		return NULL;
	if (!Dx12ResolveRegister(&scope->list->mState, stage, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, slot, &view) || !view.offset)
		return NULL;
	bytes = shader->cb_bytes[slot];

	PublishedConstants *entry = &published_constants[(UINT64)stage << 40 | (UINT64)slot << 32 | bytes];
	if (!entry->buffer) {
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = bytes;
		entry->buffer = static_cast<ShimBuffer*>(new_game_buffer(&desc, (bytes + 255) & ~255u, D3D11_BIND_CONSTANT_BUFFER));
	}
	buffer = entry->buffer;

	if (entry->config != config_generation) {
		uint32_t hash = pick_constant_buffer_hash(bytes);

		entry->config = config_generation;
		entry->matched = !!G->mTextureOverrideMap.count(hash);
		set_resource_hash(buffer, D3D11_RESOURCE_DIMENSION_BUFFER, hash, 0, NULL);
	}
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
	if (scope->compute != (stage == DX12_STAGE_CS))
		return NULL;
	// A root descriptor has an address for a resource, which says too
	// little about it to be of use to a mod
	if (!Dx12ResolveRegister(&scope->list->mState, stage, type, slot, view) || !view->resource)
		return NULL;
	return publish_resource(view->resource);
}

ID3D11ShaderResourceView* Dx12ScopeShaderResource(Dx12ShaderStage stage, UINT slot)
{
	const Dx12PipelineInfo *pipeline = scope ? Dx12FindPipeline(scope->list->mState.pipeline) : NULL;
	const Dx12ShaderInfo *shader = pipeline ? pipeline->shaders[stage] : NULL;
	Dx12DescriptorSlot view;
	Published *entry;
	ShimShaderResourceView *srv;

	// A table reaches as far as the root signature says, which goes for
	// every shader it is used with. Past what this one declares the game
	// need not have written anything, and what is there is left from
	// other draws.
	if (shader && shader->reflected && !shader->unbounded_srv && slot < 128 && !(shader->srv[slot / 64] & (1ull << (slot % 64))))
		return NULL;

	entry = publish_register(stage, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, slot, &view);
	if (!entry)
		return NULL;

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
	}
}

static void restore_list()
{
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12RootArguments *arguments = scope->compute ? &game->compute : &game->graphics;
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	UINT slot;

	for (const ChangedParameter &changed : scope->parameters)
		set_root_argument(list, scope->compute, changed.type, changed.parameter, arguments->values[changed.parameter]);
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

// The descriptors of a table as the game set it, from the root signature: up
// to the end of the range reaching furthest. 0 if that has no end.
static UINT table_size(const Dx12RootArguments *arguments, UINT parameter)
{
	const Dx12RootSignatureInfo *signature = Dx12FindRootSignature(arguments->signature);
	UINT size = 0;

	if (!signature || parameter >= signature->parameters.size())
		return 0;

	for (const Dx12RootRange &range : signature->parameters[parameter].ranges) {
		if (range.count == UINT_MAX)
			return 0;
		size = max(size, range.offset + range.count);
	}
	return size;
}

// Where to write a descriptor for the register so that the next draw reads
// it: in a copy of the table the game set, made the first time the table is
// asked about. NULL if there is no such place.
static D3D12_CPU_DESCRIPTOR_HANDLE* table_descriptor(const Dx12RegisterLocation *location, D3D12_CPU_DESCRIPTOR_HANDLE *descriptor)
{
	static bool no_heap, unbounded, no_room;
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12RootArguments *arguments = scope->compute ? &game->compute : &game->graphics;
	Dx12DescriptorHeap *heap = game->view_heap;
	ChangedParameter changed = {};
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

	size = table_size(arguments, location->parameter);
	if (!size) {
		log_once(&unbounded, "A mod replaces something the game binds through a descriptor table without an end. Ignored.");
		return NULL;
	}
	// The game need not have a whole table's worth of descriptors where
	// it pointed the table, if its shaders do not read that far
	first = (base - heap->mGpuStart) / heap->mIncrement;
	if (first + location->offset >= heap->mCount)
		return NULL;
	size = (UINT)min((UINT64)size, heap->mCount - first);

	if (!heap->AllocReserved(size, &changed.cpu, &changed.gpu)) {
		log_once(&no_room, "Out of descriptors for what mods replace on the game's draws. Some were ignored.");
		return NULL;
	}
	dx12_shim->device->CopyDescriptorsSimple(size, changed.cpu, heap->MirrorCpuHandle(base), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	changed.parameter = location->parameter;
	changed.type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	scope->parameters.push_back(changed);

	descriptor->ptr = changed.cpu.ptr + (SIZE_T)location->offset * heap->mIncrement;
	return descriptor;
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

	// Not finding it means the game's shaders do not read the register,
	// so there is nothing to replace
	return Dx12LocateRegister(game, stage, type, slot, location)
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

static const Dx12ShaderStage graphics_stages[] = { DX12_STAGE_VS, DX12_STAGE_PS, DX12_STAGE_GS, DX12_STAGE_HS, DX12_STAGE_DS };
static const Dx12ShaderStage compute_stages[] = { DX12_STAGE_CS };

// Makes the next draw or dispatch on the list use what the mod has bound in
// place of what the game has, as far as that can be done. Whatever an earlier
// call left on the list goes first: the mod may have unbound it since.
static void apply_overrides()
{
	const ShimState *mod = &dx12_shim->shim_context->mState;
	const Dx12CmdState *game = &scope->list->mState;
	const Dx12ShaderStage *stages = scope->compute ? compute_stages : graphics_stages;
	size_t stage_count = scope->compute ? ARRAYSIZE(compute_stages) : ARRAYSIZE(graphics_stages);
	ID3D12GraphicsCommandList *list = scope->list->mOrig;
	D3D12_VERTEX_BUFFER_VIEW vb;
	D3D12_INDEX_BUFFER_VIEW ib;
	UINT slot, size;
	size_t i;

	restore_list();

	for (i = 0; i < stage_count; i++) {
		const ShimStageState *bound = &mod->stages[stages[i]];

		for (slot = 0; slot < SHIM_SRV_SLOTS; slot++) {
			if (bound->srvs[slot])
				apply_shader_resource(stages[i], slot, bound->srvs[slot]);
		}
		for (slot = 0; slot < SHIM_CBV_SLOTS; slot++) {
			if (bound->cbs[slot])
				apply_constant_buffer(stages[i], slot, bound);
		}
	}
	for (const ChangedParameter &changed : scope->parameters) {
		if (changed.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
			set_root_argument(list, scope->compute, changed.type, changed.parameter, changed.gpu.ptr);
	}

	if (scope->compute)
		return;

	for (slot = 0; slot < ARRAYSIZE(mod->vbs) && slot < ARRAYSIZE(game->vertex_buffers); slot++) {
		if (!mod->vbs[slot] || ShimResourceOf(mod->vbs[slot])->foreign)
			continue;
		vb.BufferLocation = Dx12ShimGameBufferAddress(mod->vbs[slot], scope->list, &size);
		if (!vb.BufferLocation || mod->vb_offsets[slot] >= size)
			continue;
		vb.BufferLocation += mod->vb_offsets[slot];
		vb.SizeInBytes = size - mod->vb_offsets[slot];
		vb.StrideInBytes = mod->vb_strides[slot];
		list->IASetVertexBuffers(slot, 1, &vb);
		scope->vbs_changed |= 1u << slot;
	}

	if (mod->ib && !ShimResourceOf(mod->ib)->foreign) {
		ib.BufferLocation = Dx12ShimGameBufferAddress(mod->ib, scope->list, &size);
		if (ib.BufferLocation && mod->ib_offset < size) {
			ib.BufferLocation += mod->ib_offset;
			ib.SizeInBytes = size - mod->ib_offset;
			ib.Format = mod->ib_format;
			list->IASetIndexBuffer(&ib);
			scope->ib_changed = true;
		}
	}

	if (mod->topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED && (D3D12_PRIMITIVE_TOPOLOGY)mod->topology != game->topology) {
		list->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)mod->topology);
		scope->topology_changed = true;
	}
}

void Dx12ScopeDraw(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
	const ShimState *mod = &dx12_shim->shim_context->mState;
	ID3D12GraphicsCommandList *list = scope->list->mOrig;

	if (scope->compute) {
		Dx12ScopeUnsupported("to draw");
		return;
	}
	// Drawing with the game's shaders is what works. A custom shader
	// would need a pipeline state of ours on the game's list.
	for (Dx12ShaderStage stage : graphics_stages) {
		if (mod->stages[stage].shader) {
			Dx12ScopeUnsupported("to draw with a custom shader");
			return;
		}
	}

	apply_overrides();
	stats.mod_draws++;

	if (!indexed) {
		list->DrawInstanced(count, instances, start, start_instance);
	} else if (scope->ib_changed || scope->list->mState.index_buffer.BufferLocation) {
		list->DrawIndexedInstanced(count, instances, start, base_vertex, start_instance);
	}
}

static void open_scope(Dx12DrawScope *opened, Dx12GraphicsCommandList *list, bool compute)
{
	ShimContext *context = dx12_shim->shim_context;

	opened->list = list;
	opened->compute = compute;
	opened->vbs_changed = 0;
	opened->ib_changed = false;
	opened->topology_changed = false;
	opened->parameters.clear();

	// Not ClearState: the references stay with what is saved
	opened->saved = context->mState;
	memset(&context->mState, 0, sizeof(context->mState));
	context->ClearState();
	// IniParams and the like, which the engine keeps bound on a D3D11
	// context whatever the game does
	dx12_shim->hacker_context->Bind3DMigotoResources();

	scope = opened;
}

static void close_scope()
{
	ShimContext *context = dx12_shim->shim_context;

	restore_list();
	context->ClearState();
	context->mState = scope->saved;
	scope = NULL;

	// Whatever the mod's resources needed done before the game's list
	// can use them, see Bridge.h
	Dx12ShimSubmit();
}

// -----------------------------------------------------------------------------
// The calls of the game

void HackerContext::Dx12BeforeDraw(DrawContext &data, const UINT64 *shader_hashes)
{
	mCurrentVertexShader = shader_hashes[DX12_STAGE_VS];
	mCurrentPixelShader = shader_hashes[DX12_STAGE_PS];
	mCurrentGeometryShader = shader_hashes[DX12_STAGE_GS];
	mCurrentHullShader = shader_hashes[DX12_STAGE_HS];
	mCurrentDomainShader = shader_hashes[DX12_STAGE_DS];

	// What the Set*Shader methods do for hunting under D3D11
	if (G->hunting == HUNTING_MODE_ENABLED) {
		EnterCriticalSectionPretty(&G->mCriticalSection);
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
	if (!scope)
		return 0;

	switch (shader_type) {
	case L'v': return scope->compute ? 0 : mCurrentVertexShader;
	case L'h': return scope->compute ? 0 : mCurrentHullShader;
	case L'd': return scope->compute ? 0 : mCurrentDomainShader;
	case L'g': return scope->compute ? 0 : mCurrentGeometryShader;
	case L'p': return scope->compute ? 0 : mCurrentPixelShader;
	case L'c': return scope->compute ? mCurrentComputeShader : 0;
	}
	return 0;
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
	const Dx12PipelineInfo *pipeline = Dx12FindPipeline(list->mState.pipeline);
	UINT64 hashes[DX12_STAGE_COUNT] = {};
	int stage;

	if (!pipeline)
		return false;
	for (stage = 0; stage < DX12_STAGE_COUNT; stage++)
		hashes[stage] = pipeline->shaders[stage] ? pipeline->shaders[stage]->hash : 0;

	Dx12ShimLock();
	if (!dx12_shim || scope) {
		Dx12ShimUnlock();
		return false;
	}

	DrawContext c = indexed
		? DrawContext(DrawCall::DrawIndexedInstanced, 0, count, instances, base_vertex, start, start_instance, NULL, 0)
		: DrawContext(DrawCall::DrawInstanced, count, 0, instances, start, 0, start_instance, NULL, 0);

	open_scope(&scope_storage, list, false);

	dx12_shim->hacker_context->Dx12BeforeDraw(c, hashes);
	stats.draws++;
	stats.skipped += c.call_info.skip;
	if (!c.call_info.skip) {
		apply_overrides();
		if (indexed)
			list->mOrig->DrawIndexedInstanced(count, instances, start, base_vertex, start_instance);
		else
			list->mOrig->DrawInstanced(count, instances, start, start_instance);
	}
	dx12_shim->hacker_context->Dx12AfterDraw(c);

	close_scope();
	Dx12ShimUnlock();
	return true;
}

bool Dx12Dispatch(Dx12GraphicsCommandList *list, UINT x, UINT y, UINT z)
{
	const Dx12PipelineInfo *pipeline = Dx12FindPipeline(list->mState.pipeline);
	DispatchContext c(x, y, z);

	if (!pipeline || !pipeline->shaders[DX12_STAGE_CS])
		return false;

	Dx12ShimLock();
	if (!dx12_shim || scope) {
		Dx12ShimUnlock();
		return false;
	}

	open_scope(&scope_storage, list, true);

	if (dx12_shim->hacker_context->Dx12BeforeDispatch(&c, pipeline->shaders[DX12_STAGE_CS]->hash)) {
		apply_overrides();
		list->mOrig->Dispatch(x, y, z);
	}
	dx12_shim->hacker_context->Dx12AfterDispatch(&c);

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
		Dx12LogInfo("*** Shader %016llx was created with hunting off and has not been kept\n", hash);
		return;
	}

	if (!G->SHADER_PATH[0]) {
		Dx12LogInfo("*** No override_directory to write the marked shader to\n");
		return;
	}

	CreateDirectoryEnsuringAccess(G->SHADER_PATH);
	swprintf_s(path, MAX_PATH, L"%ls\\%016llx-%S.%ls", G->SHADER_PATH, hash, type, shader->dxil ? L"dxil" : L"dxbc");
	_wfopen_s(&file, path, L"wb");
	if (!file) {
		Dx12LogInfo("*** Unable to create %S\n", path);
		return;
	}
	fwrite(shader->bytecode, 1, shader->size, file);
	fclose(file);
	Dx12LogInfo("Marked shader written to %S\n", path);
}

// There is no overlay to show what the hunting keys moved to
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

	// Which part of a vertex buffer counts depended on what the mods
	// ask for
	for (auto i = reaches.begin(); i != reaches.end();) {
		if (i->second.state != REACH_PENDING)
			i = reaches.erase(i);
		else
			++i;
	}
}

static void link_shader_regex()
{
	std::vector<const Dx12ShaderInfo*> found;

	Dx12GetShaders(regex_linked_shaders, &found);
	regex_linked_shaders += found.size();
	if (shader_regex_groups.empty())
		return;

	for (const Dx12ShaderInfo *shader : found)
		link_shader_regex_groups_dx12(shader);
}

void Dx12DrawPathFramePresented()
{
	bool all = G->hunting == HUNTING_MODE_ENABLED && !dx12_mods_disabled;
	HashSet *hashes;

	if (G->hunting == HUNTING_MODE_ENABLED)
		log_selected_shaders();

	link_shader_regex();
	update_reaches();

	if (G->dx12_dump_hashes && !(dx12_frame % 300)) {
		Dx12DumpNote("Frame %u, since the last of these lines: the engine saw %u draws and skipped %u, mods drew %u times and dispatched %u; "
			"vertex buffers newly matched %u, constant buffer lookups matched %u, index reach worked out %u, failed %u, pending %Iu; "
			"%Iu shaders linked to ShaderRegex, %Iu ShaderOverride hashes\n",
			dx12_frame, stats.draws, stats.skipped, stats.mod_draws, stats.mod_dispatches,
			stats.vertex_buffers_matched, stats.constant_buffers_matched, stats.reaches_known, stats.reaches_failed,
			pending_reaches.size(), regex_linked_shaders, G->mShaderOverrideMap.size());
		stats = {};
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

	if (!(dx12_frame & 255))
		forget_unused_published();
}
