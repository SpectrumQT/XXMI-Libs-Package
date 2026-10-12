#include "FrameDump.h"

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d11.h>

#include "util.h"
#include "../globals.h"
#include "../Overlay.h"
#include "../ResourceHash.h"

#include "Bridge.h"
#include "CmdState.h"
#include "DrawPath.h"
#include "Dx12CommandList.h"
#include "ResourceTracker.h"
#include "ShimObjects.h"

struct DumpedTexture {
	// Referenced, to be there to copy from when the frame is presented
	ID3D12Resource *resource;
	uint32_t hash;
	// Of the view the first draw read it through, which unlike the
	// resource's is never typeless
	DXGI_FORMAT view_format;
	// The files of the draws that read it
	std::vector<std::wstring> names;
};

struct TextureCopy {
	size_t texture;
	ID3D12Resource *readback;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT rows;
	UINT64 row_bytes;
	DXGI_FORMAT format;
};

// As much as is copied before waiting for the GPU and writing it out
const UINT64 BATCH_BYTES = 256ull << 20;

static bool active;
static FILE *log_file;
static UINT draw_number;
static std::vector<DumpedTexture> textures;
static std::unordered_map<ID3D12Resource*, size_t> texture_index;
static UINT textures_written, textures_failed;
static bool links_failed;

static void begin()
{
	wchar_t path[MAX_PATH];

	active = true;
	draw_number = 0;
	textures_written = textures_failed = 0;
	links_failed = false;

	swprintf_s(path, MAX_PATH, L"%ls\\log.txt", G->ANALYSIS_PATH);
	wfopen_ensuring_access(&log_file, path, L"w");
	if (log_file)
		fprintf(log_file, "D3D12: the draws in the order they were recorded in, which with several lists is not the order they are executed in\n\n");
}

static uint32_t hash_of(ID3D11Resource *resource)
{
	return resource ? GetResourceHash(resource) : 0;
}

// The shaders of the draw as D3D11 frame analysis has them in a file name
static std::wstring shader_suffix(const Dx12PipelineInfo *pipeline)
{
	static const Dx12ShaderStage order[] = { DX12_STAGE_VS, DX12_STAGE_HS, DX12_STAGE_DS, DX12_STAGE_GS, DX12_STAGE_PS };
	std::wstring suffix;
	wchar_t text[32];

	for (Dx12ShaderStage stage : order) {
		if (!pipeline->shaders[stage])
			continue;
		swprintf_s(text, L"-%S=%016llx", dx12_stage_names[stage], pipeline->shaders[stage]->hash);
		suffix += text;
	}
	return suffix;
}

static void remember(ID3D12Resource *resource, uint32_t hash, DXGI_FORMAT view_format, const wchar_t *name)
{
	auto found = texture_index.find(resource);

	if (found == texture_index.end()) {
		resource->AddRef();
		found = texture_index.emplace(resource, textures.size()).first;
		textures.push_back({ resource, hash, view_format });
	}
	textures[found->second].names.push_back(name);
}

static void dump_textures(Dx12GraphicsCommandList *list, const Dx12PipelineInfo *pipeline, Dx12ShaderStage stage,
		const std::wstring &suffix)
{
	const Dx12RootSignatureInfo *signature = Dx12ListRootSignature(list, false);
	const Dx12ShaderInfo *shader = pipeline->shaders[stage];
	ID3D11ShaderResourceView *srv;
	Dx12DescriptorSlot view;
	Dx12ResourceInfo info;
	wchar_t name[MAX_PATH], hash_text[16];
	uint32_t hash;
	UINT reg;

	if (!shader || !shader->reflected || !signature)
		return;

	for (reg = 0; reg < 128; reg++) {
		if (!(shader->srv[reg / 64] & (1ull << (reg % 64))))
			continue;
		// Has the texture hashed the way the mods get to see it
		srv = Dx12ScopeShaderResource(stage, reg);
		if (!srv || !Dx12ResolveRegister(&list->mState, signature, stage, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, reg, &view)
		    || !view.resource || !Dx12GetResourceInfo(view.resource, &info))
			continue;
		hash = hash_of(ShimViewOf(srv)->resource11);

		if (log_file) {
			if (info.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
				fprintf(log_file, "       %s-t%u=%08x buffer of %llu bytes\n", dx12_stage_names[stage], reg, hash, info.desc.Width);
			} else {
				fprintf(log_file, "       %s-t%u=%08x %llux%u format=%u mips=%u%s\n", dx12_stage_names[stage], reg, hash,
					info.desc.Width, info.desc.Height, view.format ? view.format : info.desc.Format, info.desc.MipLevels,
					info.has_data_hash ? "" : " (hash of the description alone)");
			}
		}

		if (!(G->cur_analyse_options & FrameAnalysisOptions::DUMP_SRV))
			continue;
		if (info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || info.desc.SampleDesc.Count != 1)
			continue;

		hash_text[0] = 0;
		if (hash)
			swprintf_s(hash_text, L"=%08x", hash);
		swprintf_s(name, MAX_PATH, L"%06u-%S-t%u%ls%ls.dds", draw_number, dx12_stage_names[stage], reg, hash_text, suffix.c_str());
		remember(view.resource, hash, (DXGI_FORMAT)view.format, name);
	}
}

void Dx12FrameDumpDraw(Dx12GraphicsCommandList *list, const Dx12PipelineInfo *pipeline, bool indexed, UINT count,
		UINT instances, UINT start, INT base_vertex, UINT start_instance)
{
	std::wstring suffix;
	DXGI_FORMAT format;
	unsigned long slot;
	UINT stage, stride, vbs;

	if (!active)
		begin();
	draw_number++;
	suffix = shader_suffix(pipeline);

	if (log_file) {
		if (indexed) {
			fprintf(log_file, "%06u DrawIndexedInstanced(IndexCountPerInstance:%u, InstanceCount:%u, StartIndexLocation:%u, "
				"BaseVertexLocation:%d, StartInstanceLocation:%u) list=0x%p\n",
				draw_number, count, instances, start, base_vertex, start_instance, list);
		} else {
			fprintf(log_file, "%06u DrawInstanced(VertexCountPerInstance:%u, InstanceCount:%u, StartVertexLocation:%u, "
				"StartInstanceLocation:%u) list=0x%p\n",
				draw_number, count, instances, start, start_instance, list);
		}
		fprintf(log_file, "       %S\n", suffix.empty() ? L"" : suffix.c_str() + 1);
		if (indexed)
			fprintf(log_file, "       ib=%08x\n", hash_of(Dx12ScopeIndexBuffer(&format)));
		for (vbs = pipeline->vb_slots; _BitScanForward(&slot, vbs); vbs &= vbs - 1)
			fprintf(log_file, "       vb%lu=%08x\n", slot, hash_of(Dx12ScopeVertexBuffer(slot, &stride)));
	}

	for (stage = 0; stage < DX12_STAGE_CS; stage++)
		dump_textures(list, pipeline, (Dx12ShaderStage)stage, suffix);
}

// What a copy of one plane of a depth stencil texture comes as
static DXGI_FORMAT plane_format(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
	case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
	case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
	}
	return format;
}

static void note(const DumpedTexture *texture, const char *text)
{
	if (log_file)
		fprintf(log_file, "%08x at 0x%p not written: %s\n", texture->hash, texture->resource, text);
	textures_failed++;
}

// Records the copy of the texture to a buffer that can be read. 0 if there is
// nothing to wait for, otherwise the size of the buffer.
static UINT64 record_copy(size_t index, TextureCopy *copy)
{
	const DumpedTexture *texture = &textures[index];
	D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
	D3D12_HEAP_PROPERTIES heap = {};
	D3D12_RESOURCE_BARRIER barrier = {};
	D3D12_RESOURCE_DESC buffer = {};
	Dx12ResourceInfo info;
	UINT64 bytes = 0;

	if (!Dx12GetResourceInfo(texture->resource, &info)) {
		note(texture, "no longer known");
		return 0;
	}
	// A copy needs the state of what it reads, and a barrier the state
	// that is left: without it there is nothing to record
	if (info.top_state == DX12_STATE_UNKNOWN) {
		note(texture, "the state the game has it in is not known");
		return 0;
	}

	dx12_shim->device->GetCopyableFootprints(&info.desc, 0, 1, 0, &copy->footprint, &copy->rows, &copy->row_bytes, &bytes);
	if (!bytes || bytes == UINT64_MAX) {
		note(texture, "no layout to copy it in");
		return 0;
	}

	heap.Type = D3D12_HEAP_TYPE_READBACK;
	buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width = bytes;
	buffer.Height = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	if (FAILED(dx12_shim->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&copy->readback)))) {
		note(texture, "no memory to copy it to");
		return 0;
	}

	copy->texture = index;
	if (copy->footprint.Footprint.Format != info.desc.Format)
		copy->format = plane_format(copy->footprint.Footprint.Format);
	else
		copy->format = texture->view_format ? texture->view_format : info.desc.Format;

	// Of the subresource that is copied alone: the others may be in
	// states of their own
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = texture->resource;
	barrier.Transition.Subresource = 0;
	barrier.Transition.StateBefore = (D3D12_RESOURCE_STATES)info.top_state;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	if (barrier.Transition.StateBefore != barrier.Transition.StateAfter)
		Dx12ShimList()->ResourceBarrier(1, &barrier);

	dst.pResource = copy->readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = copy->footprint;
	src.pResource = texture->resource;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	Dx12ShimList()->CopyTextureRegion(&dst, 0, 0, 0, &src, NULL);

	if (barrier.Transition.StateBefore != barrier.Transition.StateAfter) {
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		Dx12ShimList()->ResourceBarrier(1, &barrier);
	}

	// Both outlive the copy, whatever becomes of waiting for it
	Dx12ShimDeferRelease(copy->readback);
	Dx12ShimDeferRelease(texture->resource);
	return bytes;
}

static bool write_dds(const wchar_t *path, const TextureCopy *copy)
{
	// DDS_HEADER and DDS_HEADER_DXT10, after the magic number
	struct {
		UINT32 magic, size, flags, height, width, linear_size, depth, mips, reserved[11];
		UINT32 pf_size, pf_flags, pf_four_cc, pf_bits[5];
		UINT32 caps[4], reserved2;
		UINT32 format, dimension, misc, array_size, misc2;
	} header = {};
	// The buffer ends with the last row, without the padding of the others
	D3D12_RANGE range = { (SIZE_T)copy->footprint.Offset, (SIZE_T)(copy->footprint.Offset
		+ (UINT64)copy->footprint.Footprint.RowPitch * (copy->rows - 1) + copy->row_bytes) }, nothing = {};
	FILE *file = NULL;
	void *data;
	UINT row;
	bool ok = true;

	if (FAILED(copy->readback->Map(0, &range, &data)))
		return false;

	header.magic = 0x20534444;
	header.size = 124;
	// CAPS, HEIGHT, WIDTH, PIXELFORMAT, LINEARSIZE
	header.flags = 0x81007;
	header.height = copy->footprint.Footprint.Height;
	header.width = copy->footprint.Footprint.Width;
	header.linear_size = (UINT32)(copy->row_bytes * copy->rows);
	header.mips = 1;
	header.pf_size = 32;
	// FOURCC "DX10"
	header.pf_flags = 4;
	header.pf_four_cc = 0x30315844;
	// TEXTURE
	header.caps[0] = 0x1000;
	header.format = copy->format;
	header.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
	header.array_size = 1;

	wfopen_ensuring_access(&file, path, L"wb");
	if (file) {
		ok = fwrite(&header, sizeof(header), 1, file) == 1;
		for (row = 0; ok && row < copy->rows; row++) {
			ok = fwrite((const BYTE*)data + copy->footprint.Offset + (UINT64)row * copy->footprint.Footprint.RowPitch,
				(size_t)copy->row_bytes, 1, file) == 1;
		}
		fclose(file);
	}
	copy->readback->Unmap(0, &nothing);
	return file && ok;
}

// One file per texture, and a name for it per draw that read it
static void link_names(const DumpedTexture *texture, const std::wstring &base)
{
	std::wstring target = base + L".dds", link, another;
	UINT copies = 0;

	for (const std::wstring &name : texture->names) {
		link = std::wstring(G->ANALYSIS_PATH) + L"\\" + name;
		if (CreateHardLinkW(link.c_str(), target.c_str(), NULL))
			continue;
		// A file only takes so many names
		if (GetLastError() == ERROR_TOO_MANY_LINKS) {
			another = base + L"." + std::to_wstring(++copies) + L".dds";
			if (CopyFileW(target.c_str(), another.c_str(), FALSE) && CreateHardLinkW(link.c_str(), another.c_str(), NULL)) {
				target = another;
				continue;
			}
		}
		if (!links_failed && log_file)
			fprintf(log_file, "Unable to link %S to %S (error %lu): the files are in the deduped folder alone\n",
				link.c_str(), target.c_str(), GetLastError());
		links_failed = true;
		return;
	}
}

static void write_batch(std::vector<TextureCopy> *batch, const wchar_t *directory)
{
	wchar_t base[MAX_PATH];
	UINT64 fence;
	bool done;

	if (batch->empty())
		return;
	fence = Dx12ShimPendingFence();
	Dx12ShimSubmitAndWait();
	// Which gives up after a while
	done = Dx12ShimFenceReached(fence);

	for (TextureCopy &copy : *batch) {
		const DumpedTexture *texture = &textures[copy.texture];

		if (!done) {
			note(texture, "the GPU did not get to copying it");
			copy.readback->Release();
			continue;
		}

		// The textures a hash stands for are not always one: the ones
		// the game fills in later are hashed by their description
		swprintf_s(base, MAX_PATH, L"%ls\\%08x-%04Iu", directory, texture->hash, copy.texture);
		if (write_dds((std::wstring(base) + L".dds").c_str(), &copy)) {
			textures_written++;
			if (log_file) {
				fprintf(log_file, "%08x at 0x%p: deduped\\%08x-%04Iu.dds, %ux%u format=%u, read by %Iu draws\n",
					texture->hash, texture->resource, texture->hash, copy.texture, copy.footprint.Footprint.Width,
					copy.footprint.Footprint.Height, copy.format, texture->names.size());
			}
			if (!links_failed)
				link_names(texture, base);
		} else {
			note(texture, "the file could not be written");
		}
		copy.readback->Release();
	}
	batch->clear();
}

void Dx12FrameDumpFramePresented()
{
	std::vector<TextureCopy> batch;
	wchar_t directory[MAX_PATH];
	TextureCopy copy;
	UINT64 bytes, batched = 0;
	size_t i;

	// The key was pressed during this present: the frame is yet to come
	if (!active || G->analyse_frame)
		return;

	if (!textures.empty()) {
		swprintf_s(directory, MAX_PATH, L"%ls\\deduped", G->ANALYSIS_PATH);
		CreateDirectoryEnsuringAccess(directory);
		if (log_file)
			fprintf(log_file, "\nTextures, as they were when the frame was presented:\n");
	}

	for (i = 0; i < textures.size(); i++) {
		copy = {};
		bytes = record_copy(i, &copy);
		if (!bytes)
			continue;
		batch.push_back(copy);
		batched += bytes;
		if (batched >= BATCH_BYTES) {
			write_batch(&batch, directory);
			batched = 0;
		}
	}
	write_batch(&batch, directory);

	for (DumpedTexture &texture : textures)
		texture.resource->Release();
	textures.clear();
	texture_index.clear();

	if (log_file)
		fclose(log_file);
	log_file = NULL;
	active = false;

	LogOverlayW(LOG_INFO, L"Frame analysis saved to %ls: %u draws, %u textures written, %u not\n",
		G->ANALYSIS_PATH, draw_number, textures_written, textures_failed);
}
