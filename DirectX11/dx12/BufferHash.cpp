#include "BufferHash.h"

#include <map>
#include <unordered_map>

#include <d3d11.h>

#include "util.h"

#include "HashDump.h"

Dx12BufferStats dx12_buffer_stats;

const UINT64 MAX_HASHED_BYTES = 256 << 20;
// A range written again within this many frames, this many times in a row,
// is a dynamic buffer
const UINT HOT_FRAMES = 30;
const UINT HOT_LIMIT = 4;

struct BufferRange {
	UINT64 bytes;
	uint32_t data_hash;
	bool valid;
	UINT frame;
	UINT hot;
};

static SRWLOCK lock = SRWLOCK_INIT;
// Ranges of one resource by their offset. They do not overlap.
static std::unordered_map<ID3D12Resource*, std::map<UINT64, BufferRange>> ranges;

// See Dx12BufferHashStamp. A counter per 64 KiB of a resource that is counted
// up by what is written there, shared with whatever else hashes to it, and
// one per resource for what covers too many of those to count them all up.
const UINT STAMP_SHIFT = 16;
const UINT64 STAMP_MAX_SPAN = 16;
static volatile LONG64 range_stamps[65536];
static volatile LONG64 resource_stamps[1024];

static volatile LONG64* range_stamp(ID3D12Resource *resource, UINT64 granule)
{
	UINT64 hash = ((UINT64)(uintptr_t)resource ^ (granule * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull;

	return &range_stamps[hash >> 48];
}

static volatile LONG64* resource_stamp(ID3D12Resource *resource)
{
	return &resource_stamps[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 54];
}

// How many of the resources that hash to a slot have ranges, counted with the
// lock held: most of the buffers a draw is asked about were never written by a
// copy, which Dx12FindBufferHash can then say without the lock
static volatile LONG resources_with_ranges[4096];

static volatile LONG* with_ranges(ID3D12Resource *resource)
{
	return &resources_with_ranges[((UINT64)(uintptr_t)resource * 0x9e3779b97f4a7c15ull) >> 52];
}

// With the lock held, so that whoever finds the stamp unchanged afterwards
// has seen all of the change or none of it
static void stamp_write(ID3D12Resource *resource, UINT64 offset, UINT64 bytes)
{
	UINT64 first = offset >> STAMP_SHIFT, last = (offset + (bytes ? bytes - 1 : 0)) >> STAMP_SHIFT;

	if (last - first >= STAMP_MAX_SPAN) {
		InterlockedIncrement64(resource_stamp(resource));
		return;
	}
	for (; first <= last; first++)
		InterlockedIncrement64(range_stamp(resource, first));
}

bool Dx12BufferHashStamp(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, UINT64 *stamp)
{
	UINT64 first = offset >> STAMP_SHIFT, last = (offset + (bytes ? bytes - 1 : 0)) >> STAMP_SHIFT;

	if (last - first >= STAMP_MAX_SPAN)
		return false;

	// The counters only go up, so their sum is the same only if each is
	*stamp = (UINT64)*resource_stamp(resource);
	for (; first <= last; first++)
		*stamp += (UINT64)*range_stamp(resource, first);
	return true;
}

UINT Dx12BufferHashStampCounters(ID3D12Resource *resource, UINT64 offset, UINT64 bytes, const volatile LONG64 **counters, UINT max)
{
	UINT64 first = offset >> STAMP_SHIFT, last = (offset + (bytes ? bytes - 1 : 0)) >> STAMP_SHIFT;
	UINT count = 0;

	if (last - first >= STAMP_MAX_SPAN || last - first + 2 > max)
		return 0;

	counters[count++] = resource_stamp(resource);
	for (; first <= last; first++)
		counters[count++] = range_stamp(resource, first);
	return count;
}

uint32_t Dx12BufferDescHash(const Dx12BufferHash *hash, UINT bind_flags)
{
	D3D11_BUFFER_DESC desc = {};

	desc.ByteWidth = (UINT)hash->bytes;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = bind_flags;

	return crc32c_hw(hash->data_hash, &desc, sizeof(desc));
}

// Returns false if the range turned out to be dynamic
static bool note_write(ID3D12Resource *resource, UINT64 offset, UINT64 bytes)
{
	UINT frame = dx12_frame;
	bool hash_it = true;

	AcquireSRWLockExclusive(&lock);

	auto inserted = ranges.try_emplace(resource);
	if (inserted.second)
		InterlockedIncrement(with_ranges(resource));
	std::map<UINT64, BufferRange> &map = inserted.first->second;

	// Whatever this write overlaps is no longer what was hashed
	auto i = map.lower_bound(offset);
	if (i != map.begin()) {
		auto previous = std::prev(i);
		if (previous->first + previous->second.bytes > offset)
			map.erase(previous);
	}
	while (i != map.end() && i->first < offset + bytes) {
		if (i->first == offset && i->second.bytes == bytes)
			++i;
		else
			i = map.erase(i);
	}

	BufferRange &range = map[offset];
	if (range.bytes == bytes && frame - range.frame < HOT_FRAMES) {
		if (range.hot < HOT_LIMIT)
			range.hot++;
	} else {
		range.hot = 0;
	}
	range.bytes = bytes;
	range.frame = frame;
	range.valid = false;
	hash_it = range.hot < HOT_LIMIT;

	// Whatever was erased overlaps the write, and so shares a counter
	// with it
	stamp_write(resource, offset, bytes);

	ReleaseSRWLockExclusive(&lock);

	return hash_it;
}

void Dx12CopyBufferRegion(ID3D12Resource *dst, UINT64 dst_offset, ID3D12Resource *src, UINT64 src_offset, UINT64 bytes)
{
	Dx12ResourceInfo dst_info, src_info;
	uint32_t hash;
	BYTE *data = NULL;

	if (!dst || !src || !bytes || bytes > MAX_HASHED_BYTES)
		return;

	if (!Dx12GetResourceInfo(src, &src_info) || !Dx12GetResourceInfo(dst, &dst_info)) {
		InterlockedIncrement64(&dx12_buffer_stats.untracked);
		return;
	}
	// Copies between upload buffers and into readback buffers are not
	// uploads of something a draw will use
	if (dst_info.cpu_accessible)
		return;
	if (!src_info.cpu_accessible || src_info.heap_type == D3D12_HEAP_TYPE_READBACK) {
		// Still replaces what was hashed there before
		note_write(dst, dst_offset, bytes);
		InterlockedIncrement64(&dx12_buffer_stats.not_mappable);
		return;
	}
	if (src_offset > src_info.desc.Width || bytes > src_info.desc.Width - src_offset) {
		InterlockedIncrement64(&dx12_buffer_stats.out_of_bounds);
		return;
	}

	if (!note_write(dst, dst_offset, bytes)) {
		InterlockedIncrement64(&dx12_buffer_stats.dynamic);
		return;
	}

	if (FAILED(src->Map(0, NULL, (void**)&data)) || !data) {
		InterlockedIncrement64(&dx12_buffer_stats.not_mappable);
		return;
	}
	hash = Dx12Crc32cWriteCombined(0, data + src_offset, (size_t)bytes);
	src->Unmap(0, NULL);

	AcquireSRWLockExclusive(&lock);
	auto resource = ranges.find(dst);
	if (resource != ranges.end()) {
		auto range = resource->second.find(dst_offset);
		if (range != resource->second.end() && range->second.bytes == bytes) {
			range->second.data_hash = hash;
			range->second.valid = true;
			stamp_write(dst, dst_offset, bytes);
		}
	}
	ReleaseSRWLockExclusive(&lock);

	InterlockedIncrement64(&dx12_buffer_stats.hashed);
	InterlockedAdd64(&dx12_buffer_stats.hashed_bytes, bytes);

	Dx12BufferHash result = { bytes, hash, dx12_frame };
	Dx12DumpBuffer(dst, dst_offset, &result);
}

bool Dx12FindBufferHash(ID3D12Resource *resource, UINT64 offset, Dx12BufferHash *hash)
{
	bool found = false;

	if (!*with_ranges(resource))
		return false;

	AcquireSRWLockShared(&lock);
	auto i = ranges.find(resource);
	if (i != ranges.end()) {
		auto range = i->second.find(offset);
		if (range != i->second.end() && range->second.valid) {
			hash->bytes = range->second.bytes;
			hash->data_hash = range->second.data_hash;
			hash->frame = range->second.frame;
			found = true;
		}
	}
	ReleaseSRWLockShared(&lock);

	return found;
}

void Dx12ForgetBufferHashes(ID3D12Resource *resource)
{
	AcquireSRWLockExclusive(&lock);
	if (ranges.erase(resource))
		InterlockedDecrement(with_ranges(resource));
	InterlockedIncrement64(resource_stamp(resource));
	ReleaseSRWLockExclusive(&lock);
}
