#include "HashDump.h"

#include <share.h>
#include <string>
#include <unordered_set>

#include <d3d11.h>

#include "util.h"
#include "../globals.h"

#include "CmdState.h"
#include "Dx12CommandList.h"
#include "TextureHash.h"

volatile bool dx12_dump_draws;

// The format a texture hash is reported with where only one fits
const Dx12FormatRule DEFAULT_FORMAT_RULE = DX12_FORMAT_BC_TYPELESS;

// D3D12 does not record what a buffer is for, and the bind flags are part of
// the D3D11 hash. These are the ones a renderer gives its mesh buffers.
static const UINT buffer_bind_flags[] = {
	D3D11_BIND_VERTEX_BUFFER,
	D3D11_BIND_INDEX_BUFFER,
	D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE,
	D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE,
};
static const char *const buffer_bind_names[ARRAYSIZE(buffer_bind_flags)] = { "vb", "ib", "vb+srv", "ib+srv" };

enum {
	CSV_SHADERS,
	CSV_TEXTURES,
	CSV_BUFFERS,
	CSV_DRAWS,
	// Not a CSV: what the log is told, for when logging is disabled
	CSV_REPORT,
	CSV_COUNT
};
static const wchar_t *const csv_names[CSV_COUNT] = {
	L"dx12_shaders.csv", L"dx12_textures.csv", L"dx12_buffers.csv", L"dx12_draws.csv",
	L"dx12_report.txt",
};
static const char *const csv_headers[CSV_COUNT] = {
	"hash,stage,bytes,kind,reflected,srv_t64_t127,srv_t0_t63,cbv,uav\n",
	"frame,resource,source,width,height,mips,array,format,flags,cube,data_hash,hash_as_is,hash_bc_typeless,hash_unreal\n",
	"frame,resource,offset,bytes,data_hash,hash_vb,hash_ib,hash_vb_srv,hash_ib_srv\n",
	"frame,list,draw,type,count,instances,start,base_vertex,vs,ps,cs,ib,ib_offset,ib_hash,"
		"vb0,vb0_offset,vb0_stride,vb0_hash,vb0_hash_srv,render_targets,depth,bindings\n",
	"",
};

// Guards everything below
static SRWLOCK lock = SRWLOCK_INIT;
static FILE *csv_files[CSV_COUNT];
static bool csv_failed[CSV_COUNT];

// What was seen is only compared with the configuration when reporting, on
// the thread that presents: that one loads the configuration, and the mods may
// not be loaded yet when the game creates its first resources.
static std::unordered_set<uint32_t> texture_hashes[DX12_FORMAT_RULE_COUNT];
static std::unordered_set<uint32_t> buffer_hashes[ARRAYSIZE(buffer_bind_flags)];
static std::unordered_set<UINT64> shader_hashes;
static unsigned dxbc_shaders, dxil_shaders, unreflected_shaders;

struct DrawSample {
	UINT frame;
	unsigned draws;
	unsigned unknown_pipeline;
	unsigned indexed;
	unsigned ib_hashed;
	unsigned vb_hashed;
	// Textures a pixel shader reads
	unsigned textures_hashed;
	// Without a data hash: render targets and the like, where D3D11 has
	// none either, and ordinary textures, whose upload we missed
	unsigned textures_target;
	unsigned textures_plain;
	unsigned textures_untracked;
	unsigned textures_unresolved;
};
static DrawSample sample;
static bool sample_requested, sample_finished;

// With the lock held
static FILE* csv(int which)
{
	wchar_t path[MAX_PATH];

	if (!G->dx12_dump_hashes || csv_failed[which])
		return NULL;
	if (csv_files[which])
		return csv_files[which];

	csv_failed[which] = true;
	if (!GetModuleFileName(migoto_handle, path, MAX_PATH))
		return NULL;
	wcsrchr(path, L'\\')[1] = 0;
	if (wcscat_s(path, csv_names[which]))
		return NULL;

	csv_files[which] = _wfsopen(path, L"w", _SH_DENYNO);
	if (!csv_files[which]) {
		Dx12LogInfo("*** Unable to create %S\n", path);
		return NULL;
	}

	csv_failed[which] = false;
	fputs(csv_headers[which], csv_files[which]);
	return csv_files[which];
}

void Dx12DumpShader(Dx12ShaderStage stage, const Dx12ShaderInfo *info)
{
	FILE *file;

	AcquireSRWLockExclusive(&lock);

	if (info->dxil)
		dxil_shaders++;
	else
		dxbc_shaders++;
	if (!info->reflected)
		unreflected_shaders++;
	shader_hashes.insert(info->hash);

	file = csv(CSV_SHADERS);
	if (file) {
		fprintf(file, "%016llx,%s,%u,%s,%d,%016llx,%016llx,%016llx,%016llx\n",
			info->hash, dx12_stage_names[stage], info->size, info->dxil ? "dxil" : "dxbc", info->reflected,
			info->srv[1], info->srv[0], info->cbv, info->uav);
	}

	ReleaseSRWLockExclusive(&lock);
}

void Dx12DumpTexture(ID3D12Resource *texture, const char *source)
{
	uint32_t hashes[DX12_FORMAT_RULE_COUNT];
	Dx12ResourceInfo info;
	FILE *file;
	int rule;

	if (!Dx12GetResourceInfo(texture, &info))
		return;

	for (rule = 0; rule < DX12_FORMAT_RULE_COUNT; rule++)
		hashes[rule] = Dx12TextureHash(&info, (Dx12FormatRule)rule);

	AcquireSRWLockExclusive(&lock);

	for (rule = 0; rule < DX12_FORMAT_RULE_COUNT; rule++)
		texture_hashes[rule].insert(hashes[rule]);

	file = csv(CSV_TEXTURES);
	if (file) {
		fprintf(file, "%u,%p,%s,%llu,%u,%u,%u,%d,0x%x,%d,%08x,%08x,%08x,%08x\n",
			dx12_frame, texture, source, info.desc.Width, info.desc.Height, info.desc.MipLevels,
			info.desc.DepthOrArraySize, info.desc.Format, info.desc.Flags, info.cube,
			info.data_hash, hashes[DX12_FORMAT_AS_IS], hashes[DX12_FORMAT_BC_TYPELESS],
			hashes[DX12_FORMAT_UNREAL]);
	}

	ReleaseSRWLockExclusive(&lock);
}

void Dx12DumpBuffer(ID3D12Resource *buffer, UINT64 offset, const Dx12BufferHash *hash)
{
	uint32_t hashes[ARRAYSIZE(buffer_bind_flags)];
	FILE *file;
	int i;

	for (i = 0; i < ARRAYSIZE(buffer_bind_flags); i++)
		hashes[i] = Dx12BufferDescHash(hash, buffer_bind_flags[i]);

	AcquireSRWLockExclusive(&lock);

	for (i = 0; i < ARRAYSIZE(buffer_bind_flags); i++)
		buffer_hashes[i].insert(hashes[i]);

	file = csv(CSV_BUFFERS);
	if (file) {
		fprintf(file, "%u,%p,%llu,%llu,%08x,%08x,%08x,%08x,%08x\n",
			dx12_frame, buffer, offset, hash->bytes, hash->data_hash,
			hashes[0], hashes[1], hashes[2], hashes[3]);
	}

	ReleaseSRWLockExclusive(&lock);
}

// -----------------------------------------------------------------------------

static void describe_binding(const Dx12DescriptorSlot *view, char prefix, UINT reg, bool count_it,
		DrawSample *counts, std::string *out)
{
	Dx12ResourceInfo info;
	char text[64];

	if (!view->resource) {
		// A root descriptor, always a buffer
		_snprintf_s(text, _TRUNCATE, "%c%u=buf ", prefix, reg);
	} else if (!Dx12GetResourceInfo(view->resource, &info)) {
		_snprintf_s(text, _TRUNCATE, "%c%u=untracked ", prefix, reg);
		counts->textures_untracked += count_it;
	} else if (info.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		_snprintf_s(text, _TRUNCATE, "%c%u=buf:%llu ", prefix, reg, info.desc.Width);
	} else if (info.desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
		_snprintf_s(text, _TRUNCATE, "%c%u=%s ", prefix, reg,
			info.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? "3d" : "1d");
	} else {
		// A ~ marks a hash of the description alone
		_snprintf_s(text, _TRUNCATE, "%c%u=%s%08x ", prefix, reg, info.has_data_hash ? "" : "~",
			Dx12TextureHash(&info, DEFAULT_FORMAT_RULE));
		if (info.has_data_hash)
			counts->textures_hashed += count_it;
		else if (info.desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
				| D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
			counts->textures_target += count_it;
		else
			counts->textures_plain += count_it;
	}

	out->append(text);
}

static void describe_registers(const Dx12CmdState *state, Dx12ShaderStage stage, const Dx12ShaderInfo *shader,
		DrawSample *counts, std::string *out)
{
	bool count_it = stage == DX12_STAGE_PS;
	Dx12DescriptorSlot view;
	char text[32];
	UINT reg;

	for (reg = 0; reg < 128; reg++) {
		if (!(shader->srv[reg / 64] & (1ull << (reg % 64))))
			continue;
		if (Dx12ResolveRegister(state, stage, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, reg, &view)) {
			describe_binding(&view, 't', reg, count_it, counts, out);
		} else {
			_snprintf_s(text, _TRUNCATE, "t%u=? ", reg);
			out->append(text);
			counts->textures_unresolved += count_it;
		}
	}

	if (stage != DX12_STAGE_CS)
		return;

	for (reg = 0; reg < 64; reg++) {
		if (!(shader->uav & (1ull << reg)))
			continue;
		if (Dx12ResolveRegister(state, stage, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, reg, &view)) {
			describe_binding(&view, 'u', reg, false, counts, out);
		} else {
			_snprintf_s(text, _TRUNCATE, "u%u=? ", reg);
			out->append(text);
		}
	}
}

static UINT64 shader_hash(const Dx12PipelineInfo *pipeline, Dx12ShaderStage stage)
{
	return pipeline && pipeline->shaders[stage] ? pipeline->shaders[stage]->hash : 0;
}

void Dx12DumpDraw(Dx12GraphicsCommandList *list, const char *type, UINT count, UINT instances, UINT start, INT base_vertex)
{
	const Dx12CmdState *state = &list->mState;
	const Dx12PipelineInfo *pipeline = state->pipeline ? Dx12FindPipeline(state->pipeline) : NULL;
	bool dispatch = !strcmp(type, "dispatch"), indexed = !strcmp(type, "indexed");
	Dx12ShaderStage stage = dispatch ? DX12_STAGE_CS : DX12_STAGE_PS;
	ID3D12Resource *ib = NULL, *vb = NULL;
	UINT64 ib_offset = 0, vb_offset = 0;
	Dx12BufferHash ib_hash = {}, vb_hash = {};
	bool ib_hashed = false, vb_hashed = false;
	DrawSample counts = {};
	std::string bindings;
	FILE *file;

	if (pipeline && pipeline->shaders[stage] && pipeline->shaders[stage]->reflected)
		describe_registers(state, stage, pipeline->shaders[stage], &counts, &bindings);

	if (!dispatch) {
		if (indexed && Dx12FindBuffer(state->index_buffer.BufferLocation, &ib, &ib_offset))
			ib_hashed = Dx12FindBufferHash(ib, ib_offset, &ib_hash);
		if (Dx12FindBuffer(state->vertex_buffers[0].BufferLocation, &vb, &vb_offset))
			vb_hashed = Dx12FindBufferHash(vb, vb_offset, &vb_hash);
	}

	AcquireSRWLockExclusive(&lock);

	sample.draws++;
	sample.unknown_pipeline += !pipeline;
	sample.indexed += indexed;
	sample.ib_hashed += ib_hashed;
	sample.vb_hashed += vb_hashed;
	sample.textures_hashed += counts.textures_hashed;
	sample.textures_target += counts.textures_target;
	sample.textures_plain += counts.textures_plain;
	sample.textures_untracked += counts.textures_untracked;
	sample.textures_unresolved += counts.textures_unresolved;

	file = csv(CSV_DRAWS);
	if (file) {
		fprintf(file, "%u,%p,%u,%s,%u,%u,%u,%d,%016llx,%016llx,%016llx,%p,%llu,%08x,%p,%llu,%u,%08x,%08x,%u,%d,%s\n",
			dx12_frame, list, list->mDraws + list->mDispatches, type, count, instances, start, base_vertex,
			shader_hash(pipeline, DX12_STAGE_VS), shader_hash(pipeline, DX12_STAGE_PS), shader_hash(pipeline, DX12_STAGE_CS),
			ib, ib_offset, ib_hashed ? Dx12BufferDescHash(&ib_hash, D3D11_BIND_INDEX_BUFFER) : 0,
			vb, vb_offset, state->vertex_buffers[0].StrideInBytes,
			vb_hashed ? Dx12BufferDescHash(&vb_hash, D3D11_BIND_VERTEX_BUFFER) : 0,
			vb_hashed ? Dx12BufferDescHash(&vb_hash, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE) : 0,
			state->render_target_count, state->has_depth_stencil, bindings.c_str());
	}

	ReleaseSRWLockExclusive(&lock);
}

void Dx12DumpFramePresented()
{
	if (!dx12_dump_draws && !sample_requested)
		return;

	AcquireSRWLockExclusive(&lock);
	if (dx12_dump_draws) {
		dx12_dump_draws = false;
		sample_finished = true;
	} else {
		sample_requested = false;
		sample = {};
		sample.frame = dx12_frame;
		dx12_dump_draws = true;
	}
	ReleaseSRWLockExclusive(&lock);
}

// -----------------------------------------------------------------------------

#define REPORT(fmt, ...) \
	do { \
		if (info) \
			Dx12LogInfo(fmt, __VA_ARGS__); \
		else \
			Dx12LogDebug(fmt, __VA_ARGS__); \
		if (report) \
			fprintf(report, fmt, __VA_ARGS__); \
	} while (0)

template <class Seen, class Map>
static size_t count_in_ini(const Seen &seen, const Map &map)
{
	size_t count = 0;

	for (auto hash : seen)
		count += map.count(hash) != 0;
	return count;
}

void Dx12HashReport(bool info)
{
	size_t pipeline_count, signature_count, shader_count;
	const Dx12TextureStats *t = &dx12_texture_stats;
	const Dx12BufferStats *b = &dx12_buffer_stats;
	FILE *report;
	int i;

	Dx12GetPipelineCounts(&pipeline_count, &signature_count, &shader_count);

	AcquireSRWLockExclusive(&lock);

	report = csv(CSV_REPORT);
	if (report)
		fprintf(report, "Frame %u\n", dx12_frame);

	REPORT("  %Iu pipelines, %Iu root signatures, %Iu shaders (%u DXBC, %u DXIL, %u without register information), ShaderOverride hashes seen: %Iu of %Iu\n",
		pipeline_count, signature_count, shader_count, dxbc_shaders, dxil_shaders, unreflected_shaders,
		count_in_ini(shader_hashes, G->mShaderOverrideMap), G->mShaderOverrideMap.size());

	REPORT("  Texture uploads: %lld hashed (%.1f MB). Not hashed: %lld lower mips or array slices, %lld partial, %lld updates, %lld not 2D, %lld unknown format, %lld untracked texture, %lld source not mappable, %lld out of bounds. Copies: %lld propagated a hash, %lld not of a whole top mip\n",
		t->hashed, t->hashed_bytes / 1048576.0, t->other_subresource, t->partial, t->update_ignored, t->not_2d,
		t->unknown_format, t->untracked, t->not_mappable, t->out_of_bounds, t->copy_propagated, t->copy_partial);

	REPORT("  Buffer uploads: %lld hashed (%.1f MB). Not hashed: %lld dynamic, %lld source not mappable, %lld untracked, %lld out of bounds\n",
		b->hashed, b->hashed_bytes / 1048576.0, b->dynamic, b->not_mappable, b->untracked, b->out_of_bounds);

	REPORT("  TextureOverride hashes: %Iu in the configuration, %Iu distinct texture and %Iu buffer hashes seen\n",
		G->mTextureOverrideMap.size(), texture_hashes[DEFAULT_FORMAT_RULE].size(), buffer_hashes[0].size());
	for (i = 0; i < DX12_FORMAT_RULE_COUNT; i++) {
		REPORT("    %Iu matched by textures with formats %s\n",
			count_in_ini(texture_hashes[i], G->mTextureOverrideMap), dx12_format_rule_names[i]);
	}
	for (i = 0; i < ARRAYSIZE(buffer_bind_flags); i++) {
		REPORT("    %Iu matched by buffers hashed as %s\n",
			count_in_ini(buffer_hashes[i], G->mTextureOverrideMap), buffer_bind_names[i]);
	}

	if (sample_finished) {
		sample_finished = false;
		REPORT("  Frame %u: %u draws and dispatches, %u with an unknown pipeline, %u indexed, index buffer hashed for %u, vb0 for %u\n",
			sample.frame, sample.draws, sample.unknown_pipeline, sample.indexed, sample.ib_hashed, sample.vb_hashed);
		REPORT("    Textures read by pixel shaders: %u hashed with data, %u render targets without, %u other textures without, %u untracked, %u registers not resolved\n",
			sample.textures_hashed, sample.textures_target, sample.textures_plain,
			sample.textures_untracked, sample.textures_unresolved);
	}

	for (i = 0; i < CSV_COUNT; i++) {
		if (csv_files[i])
			fflush(csv_files[i]);
	}

	if (G->dx12_dump_hashes)
		sample_requested = true;

	ReleaseSRWLockExclusive(&lock);
}
