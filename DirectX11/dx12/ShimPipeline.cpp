#include "ShimPipeline.h"

#include <map>
#include <unordered_map>

#include "ShimDevice.h"
#include "util.h"

// The shaders of mods are D3D11 shaders: every stage has its own registers,
// all in space 0, and binds by slot. The root signature for a set of them
// mirrors that with up to three descriptor tables per stage, covering the
// registers b0, t0 and s0 up to the highest one that stage's shader uses, and
// one more table for the unordered access views, which D3D11 shares between
// the stages. Which tables exist only depends on those register counts, so
// that is what a root signature is looked up by.
//
// The order of the root parameters is the order of for_each_table.

const Dx12ShaderStage graphics_stages[] = { DX12_STAGE_VS, DX12_STAGE_PS, DX12_STAGE_GS, DX12_STAGE_HS, DX12_STAGE_DS };
const Dx12ShaderStage compute_stages[] = { DX12_STAGE_CS };

struct RootKey {
	BYTE cbvs[DX12_STAGE_COUNT];
	BYTE srvs[DX12_STAGE_COUNT];
	BYTE samplers[DX12_STAGE_COUNT];
	BYTE uavs;

	bool operator<(const RootKey &other) const
	{
		return memcmp(this, &other, sizeof(*this)) < 0;
	}
};

struct PipelineKey {
	// Of the shaders by stage, then the input layout. 0 for none.
	UINT64 ids[DX12_STAGE_COUNT + 1];
	D3D11_BLEND_DESC blend;
	D3D11_RASTERIZER_DESC rasterizer;
	D3D11_DEPTH_STENCIL_DESC depth_stencil;
	UINT sample_mask;
	DXGI_FORMAT rtv_formats[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	UINT rtv_count;
	DXGI_FORMAT dsv_format;
	DXGI_SAMPLE_DESC samples;
	D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
	D3D12_INDEX_BUFFER_STRIP_CUT_VALUE strip_cut;

	bool operator==(const PipelineKey &other) const
	{
		return !memcmp(this, &other, sizeof(*this));
	}
};

struct PipelineKeyHash {
	size_t operator()(const PipelineKey &key) const
	{
		return crc32c_hw(0, &key, sizeof(key));
	}
};

static std::map<RootKey, ID3D12RootSignature*> root_signatures;
// A NULL pipeline is one that could not be created, which is not tried again
static std::unordered_map<PipelineKey, ID3D12PipelineState*, PipelineKeyHash> pipelines;
// By the id of the compute shader
static std::map<UINT64, ID3D12PipelineState*> compute_pipelines;

static D3D12_CPU_DESCRIPTOR_HANDLE default_sampler;
static D3D12_CPU_DESCRIPTOR_HANDLE null_rtv;
static HRESULT (WINAPI *serialize_root_signature)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);

static void log_once(bool *reported, const char *message)
{
	if (*reported)
		return;
	*reported = true;
	Dx12LogInfo("*** %s\n", message);
}

bool Dx12ShimPipelineInit()
{
	D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
	D3D12_SAMPLER_DESC sampler = {};
	HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");

	// Not linked against, or our DLL would bring d3d12.dll into every game
	*(FARPROC*)&serialize_root_signature = d3d12 ? GetProcAddress(d3d12, "D3D12SerializeRootSignature") : NULL;
	if (!serialize_root_signature) {
		Dx12LogInfo("*** No D3D12SerializeRootSignature, mods will not be applied\n");
		return false;
	}

	// What D3D11 samples with when no sampler is bound
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.MaxAnisotropy = 1;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = sampler.BorderColor[3] = 1.0f;
	sampler.MinLOD = -FLT_MAX;
	sampler.MaxLOD = FLT_MAX;
	default_sampler = Dx12ShimAllocDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

	rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
	null_rtv = Dx12ShimAllocDescriptor(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	if (!default_sampler.ptr || !null_rtv.ptr)
		return false;
	dx12_shim->device->CreateSampler(&sampler, default_sampler);
	dx12_shim->device->CreateRenderTargetView(NULL, &rtv, null_rtv);
	return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12ShimDefaultSampler()
{
	return default_sampler;
}

void Dx12ShimForgetPipelines(UINT64 id)
{
	bool uses;

	auto compute = compute_pipelines.find(id);
	if (compute != compute_pipelines.end()) {
		if (compute->second) {
			Dx12ShimDeferRelease(compute->second);
			compute->second->Release();
		}
		compute_pipelines.erase(compute);
	}

	for (auto i = pipelines.begin(); i != pipelines.end();) {
		uses = false;
		for (UINT64 other : i->first.ids)
			uses = uses || other == id;
		if (!uses) {
			i++;
			continue;
		}

		if (i->second) {
			Dx12ShimDeferRelease(i->second);
			i->second->Release();
		}
		i = pipelines.erase(i);
	}
}

// -----------------------------------------------------------------------------
// Root signatures

enum TableKind { TABLE_CBV, TABLE_SRV, TABLE_SAMPLER, TABLE_UAV };

static const D3D12_SHADER_VISIBILITY stage_visibility[DX12_STAGE_COUNT] = {
	D3D12_SHADER_VISIBILITY_VERTEX, D3D12_SHADER_VISIBILITY_PIXEL, D3D12_SHADER_VISIBILITY_GEOMETRY,
	D3D12_SHADER_VISIBILITY_HULL, D3D12_SHADER_VISIBILITY_DOMAIN, D3D12_SHADER_VISIBILITY_ALL,
};

// Calls visit(kind, stage, count) for every table of the root signature, in
// the order of its root parameters. stage is DX12_STAGE_COUNT for the table
// of unordered access views.
template <class Visitor>
static void for_each_table(const RootKey *key, Visitor visit)
{
	int stage;

	for (stage = 0; stage < DX12_STAGE_COUNT; stage++) {
		if (key->cbvs[stage])
			visit(TABLE_CBV, (Dx12ShaderStage)stage, key->cbvs[stage]);
		if (key->srvs[stage])
			visit(TABLE_SRV, (Dx12ShaderStage)stage, key->srvs[stage]);
		if (key->samplers[stage])
			visit(TABLE_SAMPLER, (Dx12ShaderStage)stage, key->samplers[stage]);
	}
	if (key->uavs)
		visit(TABLE_UAV, DX12_STAGE_COUNT, key->uavs);
}

static ID3D12RootSignature* get_root_signature(const RootKey *key, bool input_assembler)
{
	static const D3D12_DESCRIPTOR_RANGE_TYPE range_types[] = {
		D3D12_DESCRIPTOR_RANGE_TYPE_CBV, D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
		D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
	};
	D3D12_DESCRIPTOR_RANGE ranges[DX12_STAGE_COUNT * 3 + 1] = {};
	D3D12_ROOT_PARAMETER parameters[DX12_STAGE_COUNT * 3 + 1] = {};
	D3D12_ROOT_SIGNATURE_DESC desc = {};
	ID3D12RootSignature *signature = NULL;
	ID3DBlob *blob = NULL, *error = NULL;
	UINT count = 0;
	HRESULT hr;

	auto found = root_signatures.find(*key);
	if (found != root_signatures.end())
		return found->second;

	for_each_table(key, [&](TableKind kind, Dx12ShaderStage stage, UINT registers) {
		ranges[count].RangeType = range_types[kind];
		ranges[count].NumDescriptors = registers;
		parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		parameters[count].DescriptorTable.NumDescriptorRanges = 1;
		parameters[count].DescriptorTable.pDescriptorRanges = &ranges[count];
		parameters[count].ShaderVisibility = stage == DX12_STAGE_COUNT ? D3D12_SHADER_VISIBILITY_ALL : stage_visibility[stage];
		count++;
	});

	desc.NumParameters = count;
	desc.pParameters = parameters;
	if (input_assembler)
		desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

	hr = serialize_root_signature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &error);
	if (SUCCEEDED(hr)) {
		hr = dx12_shim->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
				IID_PPV_ARGS(&signature));
	}
	if (FAILED(hr)) {
		Dx12LogInfo("*** Failed to create a root signature with %u tables for a mod = 0x%x %s\n", count, hr,
			error ? (const char*)error->GetBufferPointer() : "");
	}
	if (blob)
		blob->Release();
	if (error)
		error->Release();

	root_signatures[*key] = signature;
	return signature;
}

// -----------------------------------------------------------------------------
// Descriptors

static void null_srv(BYTE dimension, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};

	desc.ViewDimension = dimension ? (D3D12_SRV_DIMENSION)dimension : D3D12_SRV_DIMENSION_TEXTURE2D;
	desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	switch (desc.ViewDimension) {
	case D3D12_SRV_DIMENSION_BUFFER:
		desc.Format = DXGI_FORMAT_R32_UINT;
		break;
	case D3D12_SRV_DIMENSION_TEXTURE2DMS:
		break;
	case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
		desc.Texture2DMSArray.ArraySize = 1;
		break;
	case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
	case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
	case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
		// The array size or cube count is the fourth member of all
		// three
		desc.Texture2DArray.ArraySize = 1;
		// Fall through
	default:
		desc.Texture2D.MipLevels = 1;
		break;
	}
	dx12_shim->device->CreateShaderResourceView(NULL, &desc, handle);
}

static void null_uav(BYTE dimension, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};

	desc.ViewDimension = dimension ? (D3D12_UAV_DIMENSION)dimension : D3D12_UAV_DIMENSION_TEXTURE2D;
	desc.Format = desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
	if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE1DARRAY || desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE2DARRAY)
		desc.Texture2DArray.ArraySize = 1;
	else if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D)
		desc.Texture3D.WSize = 1;
	dx12_shim->device->CreateUnorderedAccessView(NULL, NULL, &desc, handle);
}

// The write_ functions are false if what is bound is not ready, see
// ShimNotReady
static bool write_cbv(const ShimStageState *stage, UINT slot, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	D3D12_CONSTANT_BUFFER_VIEW_DESC desc = {};
	ID3D11Buffer *buffer = stage->cbs[slot];
	ShimResourceDesc buffer_desc;
	UINT64 va, offset, size;

	if (buffer) {
		ShimGetResourceDesc(buffer, &buffer_desc);
		offset = (UINT64)stage->cb_first[slot] * 16;
		size = stage->cb_count[slot] ? (UINT64)stage->cb_count[slot] * 16 : buffer_desc.byte_width;
		// Before the address: this may have the contents uploaded again
		ShimReadShadow(ShimResourceOf(buffer), offset, size);
	}
	if (buffer && (va = Dx12ShimBufferAddress(buffer, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER))) {
		if (offset < buffer_desc.byte_width) {
			size = min(size, buffer_desc.byte_width - offset);
			desc.BufferLocation = va + offset;
			// Buffers are padded for this when they are created
			desc.SizeInBytes = (UINT)((size + 255) & ~255ull);
		}
	}

	dx12_shim->device->CreateConstantBufferView(desc.BufferLocation ? &desc : NULL, handle);
	return !buffer || !ShimNotReady(ShimResourceOf(buffer));
}

static bool write_srv(const ShimShader *shader, ID3D11ShaderResourceView *srv, UINT slot, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	ShimView *view = ShimViewOf(srv);

	if (view && view->resource->resource)
		ShimUploadShadow(view->resource);
	if (!view || !Dx12ShimTransition(view->resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
		null_srv(shader->srv_dimension[slot], handle);
		return !view || !ShimNotReady(view->resource);
	}

	dx12_shim->device->CopyDescriptorsSimple(1, handle, view->handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return true;
}

static bool write_uav(ID3D11UnorderedAccessView *uav, BYTE dimension, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	ShimView *view = ShimViewOf(uav);

	if (!view || !Dx12ShimTransition(view->resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
		null_uav(dimension, handle);
		return !view || !ShimNotReady(view->resource);
	}

	dx12_shim->device->CopyDescriptorsSimple(1, handle, view->handle, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	return true;
}

static BYTE uav_dimension(const ShimShader *const *shaders, UINT slot)
{
	int stage;

	for (stage = 0; stage < DX12_STAGE_COUNT; stage++) {
		if (shaders[stage] && (shaders[stage]->uav_used & (1ull << slot)))
			return shaders[stage]->uav_dimension[slot];
	}
	return 0;
}

// Fills in the tables for the shaders and gives them to the root signature
// that has to be set on the list already. Records the transitions of what
// they refer to on the way. False if something bound is not ready
// (ShimNotReady): what the shaders make of a null view in its place is not
// to be drawn, nor kept as what a dispatch computed.
static bool bind_tables(const ShimState *state, const RootKey *key, const ShimShader *const *shaders,
		UINT64 uav_used, bool compute)
{
	ID3D12GraphicsCommandList *list = Dx12ShimList();
	ID3D11UnorderedAccessView *const *uavs = compute ? state->cs_uavs : state->om_uavs;
	D3D12_GPU_DESCRIPTOR_HANDLE tables[DX12_STAGE_COUNT * 3 + 1];
	D3D12_CPU_DESCRIPTOR_HANDLE cpu, samplers[SHIM_SAMPLER_SLOTS];
	D3D12_GPU_DESCRIPTOR_HANDLE gpu;
	UINT views = 0, sampler_count = 0, count = 0, i;
	bool resident = true;

	for_each_table(key, [&](TableKind kind, Dx12ShaderStage stage, UINT registers) {
		if (kind == TABLE_SAMPLER)
			sampler_count += registers;
		else
			views += registers;
	});

	cpu = Dx12ShimAllocViews(views, &gpu);
	if (views && !cpu.ptr)
		return false;
	Dx12ShimReserveSamplers(sampler_count);

	for_each_table(key, [&](TableKind kind, Dx12ShaderStage stage, UINT registers) {
		const ShimStageState *bound = stage == DX12_STAGE_COUNT ? NULL : &state->stages[stage];
		const ShimShader *shader = stage == DX12_STAGE_COUNT ? NULL : shaders[stage];
		UINT slot;

		if (kind == TABLE_SAMPLER) {
			for (slot = 0; slot < registers; slot++) {
				samplers[slot] = bound->samplers[slot]
					? static_cast<ShimSamplerState*>(bound->samplers[slot])->mHandle : default_sampler;
			}
			tables[count++] = Dx12ShimSamplerTable(samplers, registers);
			return;
		}

		tables[count++] = gpu;
		for (slot = 0; slot < registers; slot++) {
			// A register the shader does not use is never read,
			// so its descriptor can stay whatever it is
			switch (kind) {
			case TABLE_CBV:
				if ((shader->cbv_used & (1u << slot)) && !write_cbv(bound, slot, cpu))
					resident = false;
				break;
			case TABLE_SRV:
				if ((shader->srv_used[slot / 64] & (1ull << (slot % 64))) && !write_srv(shader, bound->srvs[slot], slot, cpu))
					resident = false;
				break;
			case TABLE_UAV:
				if ((uav_used & (1ull << slot)) && !write_uav(uavs[slot], uav_dimension(shaders, slot), cpu))
					resident = false;
				break;
			}
			cpu.ptr += dx12_shim_view_increment;
			gpu.ptr += dx12_shim_view_increment;
		}
	});
	if (!resident)
		return false;

	Dx12ShimBindHeaps();
	for (i = 0; i < count; i++) {
		if (compute)
			list->SetComputeRootDescriptorTable(i, tables[i]);
		else
			list->SetGraphicsRootDescriptorTable(i, tables[i]);
	}
	return true;
}

static void root_key(const ShimState *state, const Dx12ShaderStage *stages, size_t count, RootKey *key,
		const ShimShader **shaders, UINT64 *uav_used)
{
	size_t i;

	memset(key, 0, sizeof(*key));
	memset(shaders, 0, DX12_STAGE_COUNT * sizeof(*shaders));
	*uav_used = 0;

	for (i = 0; i < count; i++) {
		const ShimShader *shader = ShimShaderOf(state->stages[stages[i]].shader);

		shaders[stages[i]] = shader;
		if (!shader)
			continue;
		key->cbvs[stages[i]] = (BYTE)shader->cbv_count;
		key->srvs[stages[i]] = (BYTE)shader->srv_count;
		key->samplers[stages[i]] = (BYTE)shader->sampler_count;
		key->uavs = max(key->uavs, (BYTE)shader->uav_count);
		*uav_used |= shader->uav_used;
	}
}

// -----------------------------------------------------------------------------
// Graphics

static const D3D11_RENDER_TARGET_BLEND_DESC default_target_blend = {
	FALSE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD,
	D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL,
};
static const D3D11_RASTERIZER_DESC default_rasterizer = {
	D3D11_FILL_SOLID, D3D11_CULL_BACK, FALSE, 0, 0.0f, 0.0f, TRUE, FALSE, FALSE, FALSE,
};
static const D3D11_DEPTH_STENCILOP_DESC default_stencil_op = {
	D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS,
};
static const D3D11_DEPTH_STENCIL_DESC default_depth_stencil = {
	TRUE, D3D11_DEPTH_WRITE_MASK_ALL, D3D11_COMPARISON_LESS, FALSE,
	D3D11_DEFAULT_STENCIL_READ_MASK, D3D11_DEFAULT_STENCIL_WRITE_MASK, default_stencil_op, default_stencil_op,
};

// Pipeline keys are compared as bytes, and the padding of a description that
// came from the engine is not ours to rely on, so these copy member by member
// into a key that was zeroed.
static void copy_blend(D3D11_BLEND_DESC *dst, const D3D11_BLEND_DESC *src)
{
	UINT i;

	dst->AlphaToCoverageEnable = src ? src->AlphaToCoverageEnable : FALSE;
	dst->IndependentBlendEnable = src ? src->IndependentBlendEnable : FALSE;
	for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
		const D3D11_RENDER_TARGET_BLEND_DESC &from = src ? src->RenderTarget[i] : default_target_blend;
		D3D11_RENDER_TARGET_BLEND_DESC &to = dst->RenderTarget[i];

		to.BlendEnable = from.BlendEnable;
		to.SrcBlend = from.SrcBlend;
		to.DestBlend = from.DestBlend;
		to.BlendOp = from.BlendOp;
		to.SrcBlendAlpha = from.SrcBlendAlpha;
		to.DestBlendAlpha = from.DestBlendAlpha;
		to.BlendOpAlpha = from.BlendOpAlpha;
		to.RenderTargetWriteMask = from.RenderTargetWriteMask;
	}
}

static void copy_depth_stencil(D3D11_DEPTH_STENCIL_DESC *dst, const D3D11_DEPTH_STENCIL_DESC *src)
{
	dst->DepthEnable = src->DepthEnable;
	dst->DepthWriteMask = src->DepthWriteMask;
	dst->DepthFunc = src->DepthFunc;
	dst->StencilEnable = src->StencilEnable;
	dst->StencilReadMask = src->StencilReadMask;
	dst->StencilWriteMask = src->StencilWriteMask;
	dst->FrontFace = src->FrontFace;
	dst->BackFace = src->BackFace;
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE topology_type(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	switch (topology) {
	case D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
	case D3D11_PRIMITIVE_TOPOLOGY_POINTLIST:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
	case D3D11_PRIMITIVE_TOPOLOGY_LINELIST:
	case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP:
	case D3D11_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
	case D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
	case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
	case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
	case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
	case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
		return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	}
	return D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
}

static bool is_strip(D3D11_PRIMITIVE_TOPOLOGY topology)
{
	return topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP || topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
	    || topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ || topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ;
}

static ID3D12PipelineState* get_pipeline(const ShimState *state, const PipelineKey *key,
		const ShimShader *const *shaders, ID3D12RootSignature *signature)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	D3D12_SHADER_BYTECODE *const bytecode[DX12_STAGE_COUNT] = { &desc.VS, &desc.PS, &desc.GS, &desc.HS, &desc.DS, NULL };
	ShimInputLayout *layout = static_cast<ShimInputLayout*>(state->input_layout);
	ID3D12PipelineState *pipeline = NULL;
	HRESULT hr;
	UINT i;

	auto found = pipelines.find(*key);
	if (found != pipelines.end())
		return found->second;

	desc.pRootSignature = signature;
	for (Dx12ShaderStage stage : graphics_stages) {
		if (shaders[stage]) {
			bytecode[stage]->pShaderBytecode = shaders[stage]->bytecode.data();
			bytecode[stage]->BytecodeLength = shaders[stage]->bytecode.size();
		}
	}

	// The members of these descriptions that both APIs have are the same
	// types with the same values
	desc.BlendState.AlphaToCoverageEnable = key->blend.AlphaToCoverageEnable;
	desc.BlendState.IndependentBlendEnable = key->blend.IndependentBlendEnable;
	for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
		const D3D11_RENDER_TARGET_BLEND_DESC &src = key->blend.RenderTarget[i];
		D3D12_RENDER_TARGET_BLEND_DESC &dst = desc.BlendState.RenderTarget[i];

		dst.BlendEnable = src.BlendEnable;
		dst.SrcBlend = (D3D12_BLEND)src.SrcBlend;
		dst.DestBlend = (D3D12_BLEND)src.DestBlend;
		dst.BlendOp = (D3D12_BLEND_OP)src.BlendOp;
		dst.SrcBlendAlpha = (D3D12_BLEND)src.SrcBlendAlpha;
		dst.DestBlendAlpha = (D3D12_BLEND)src.DestBlendAlpha;
		dst.BlendOpAlpha = (D3D12_BLEND_OP)src.BlendOpAlpha;
		dst.LogicOp = D3D12_LOGIC_OP_NOOP;
		dst.RenderTargetWriteMask = src.RenderTargetWriteMask;
	}
	desc.SampleMask = key->sample_mask;

	desc.RasterizerState.FillMode = (D3D12_FILL_MODE)key->rasterizer.FillMode;
	desc.RasterizerState.CullMode = (D3D12_CULL_MODE)key->rasterizer.CullMode;
	desc.RasterizerState.FrontCounterClockwise = key->rasterizer.FrontCounterClockwise;
	desc.RasterizerState.DepthBias = key->rasterizer.DepthBias;
	desc.RasterizerState.DepthBiasClamp = key->rasterizer.DepthBiasClamp;
	desc.RasterizerState.SlopeScaledDepthBias = key->rasterizer.SlopeScaledDepthBias;
	desc.RasterizerState.DepthClipEnable = key->rasterizer.DepthClipEnable;
	desc.RasterizerState.MultisampleEnable = key->rasterizer.MultisampleEnable;
	desc.RasterizerState.AntialiasedLineEnable = key->rasterizer.AntialiasedLineEnable;

	desc.DepthStencilState.DepthEnable = key->depth_stencil.DepthEnable;
	desc.DepthStencilState.DepthWriteMask = (D3D12_DEPTH_WRITE_MASK)key->depth_stencil.DepthWriteMask;
	desc.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)key->depth_stencil.DepthFunc;
	desc.DepthStencilState.StencilEnable = key->depth_stencil.StencilEnable;
	desc.DepthStencilState.StencilReadMask = key->depth_stencil.StencilReadMask;
	desc.DepthStencilState.StencilWriteMask = key->depth_stencil.StencilWriteMask;
	// D3D12_DEPTH_STENCILOP_DESC is the same struct as the D3D11 one
	memcpy(&desc.DepthStencilState.FrontFace, &key->depth_stencil.FrontFace, sizeof(D3D12_DEPTH_STENCILOP_DESC));
	memcpy(&desc.DepthStencilState.BackFace, &key->depth_stencil.BackFace, sizeof(D3D12_DEPTH_STENCILOP_DESC));

	if (layout) {
		desc.InputLayout.pInputElementDescs = layout->mElements.data();
		desc.InputLayout.NumElements = (UINT)layout->mElements.size();
	}
	desc.IBStripCutValue = key->strip_cut;
	desc.PrimitiveTopologyType = key->topology;
	desc.NumRenderTargets = key->rtv_count;
	memcpy(desc.RTVFormats, key->rtv_formats, sizeof(desc.RTVFormats));
	desc.DSVFormat = key->dsv_format;
	desc.SampleDesc = key->samples;

	hr = dx12_shim->device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
	if (FAILED(hr)) {
		Dx12LogInfo("*** Failed to create a pipeline state for a mod = 0x%x: shaders vs=%llu ps=%llu gs=%llu hs=%llu ds=%llu, "
			"input layout %llu with %u elements, %u render targets, first format %d, depth format %d, topology type %d\n",
			hr, key->ids[DX12_STAGE_VS], key->ids[DX12_STAGE_PS], key->ids[DX12_STAGE_GS], key->ids[DX12_STAGE_HS],
			key->ids[DX12_STAGE_DS], key->ids[DX12_STAGE_COUNT], desc.InputLayout.NumElements, key->rtv_count,
			key->rtv_formats[0], key->dsv_format, key->topology);
	}

	pipelines[*key] = pipeline;
	return pipeline;
}

bool Dx12ShimPrepareDraw(const ShimState *state, bool indexed)
{
	static bool no_vs, no_topology, no_viewport, no_target, no_ib;
	static const D3D12_RECT no_scissor = { 0, 0, D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX };
	ID3D12GraphicsCommandList *list;
	const ShimShader *shaders[DX12_STAGE_COUNT];
	D3D12_CPU_DESCRIPTOR_HANDLE rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
	D3D12_VERTEX_BUFFER_VIEW vbs[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
	D3D12_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	D3D12_INDEX_BUFFER_VIEW ib = {};
	ID3D12RootSignature *signature;
	ID3D12PipelineState *pipeline;
	ShimView *dsv = ShimViewOf(state->dsv), *view;
	ShimResourceDesc buffer;
	PipelineKey key;
	RootKey root;
	UINT64 uav_used;
	UINT i, vb_count = 0;

	if (!state->stages[DX12_STAGE_VS].shader) {
		log_once(&no_vs, "A mod draws without a vertex shader of its own, which needs the draw of the game it belongs to. Dropped.");
		return false;
	}
	if (state->topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
		log_once(&no_topology, "A mod draws without a primitive topology. Dropped.");
		return false;
	}
	if (!state->viewport_count) {
		log_once(&no_viewport, "A mod draws without a viewport, which needs the draw of the game it belongs to. Dropped.");
		return false;
	}

	memset(&key, 0, sizeof(key));
	root_key(state, graphics_stages, ARRAYSIZE(graphics_stages), &root, shaders, &uav_used);
	for (Dx12ShaderStage stage : graphics_stages)
		key.ids[stage] = shaders[stage] ? shaders[stage]->id : 0;
	key.ids[DX12_STAGE_COUNT] = state->input_layout ? static_cast<ShimInputLayout*>(state->input_layout)->mId : 0;

	for (i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
		view = ShimViewOf(state->rtvs[i]);
		rtvs[i] = null_rtv;
		if (!view)
			continue;
		// A back buffer the game has released since
		if (!view->resource->resource)
			return false;
		rtvs[i] = view->handle;
		key.rtv_formats[i] = view->format;
		key.rtv_count = i + 1;
		key.samples = view->resource->desc.SampleDesc;
	}
	if (dsv) {
		if (!dsv->resource->resource)
			return false;
		key.dsv_format = dsv->format;
		key.samples = dsv->resource->desc.SampleDesc;
	}
	if (!key.rtv_count && !dsv) {
		// A pixel shader may write to a UAV alone
		if (!uav_used) {
			log_once(&no_target, "A mod draws without a render target of its own, which needs the draw of the game it belongs to. Dropped.");
			return false;
		}
		key.samples.Count = 1;
	}

	copy_blend(&key.blend, state->blend ? &static_cast<ShimBlendState*>(state->blend)->mDesc : NULL);
	key.sample_mask = state->sample_mask;
	key.rasterizer = state->rasterizer ? static_cast<ShimRasterizerState*>(state->rasterizer)->mDesc : default_rasterizer;
	copy_depth_stencil(&key.depth_stencil, state->depth_stencil
		? &static_cast<ShimDepthStencilState*>(state->depth_stencil)->mDesc : &default_depth_stencil);
	if (!dsv) {
		// D3D11 does no depth or stencil test without a depth buffer
		key.depth_stencil.DepthEnable = FALSE;
		key.depth_stencil.StencilEnable = FALSE;
	}
	// The members D3D12 does not have in its pipeline description
	key.rasterizer.ScissorEnable = FALSE;
	key.topology = topology_type(state->topology);
	if (is_strip(state->topology)) {
		key.strip_cut = state->ib_format == DXGI_FORMAT_R16_UINT
			? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
	}

	signature = get_root_signature(&root, true);
	if (!signature)
		return false;
	pipeline = get_pipeline(state, &key, shaders, signature);
	if (!pipeline)
		return false;

	// Everything up to here was able to bail out without leaving half a
	// draw on the list
	if (indexed) {
		if (!state->ib || !(ib.BufferLocation = Dx12ShimBufferAddress(state->ib, D3D12_RESOURCE_STATE_INDEX_BUFFER))) {
			log_once(&no_ib, "A mod draws indexed without an index buffer of its own, which needs the draw of the game it belongs to. Dropped.");
			return false;
		}
		ShimGetResourceDesc(state->ib, &buffer);
		if (state->ib_offset >= buffer.byte_width)
			return false;
		ib.BufferLocation += state->ib_offset;
		ib.SizeInBytes = buffer.byte_width - state->ib_offset;
		ib.Format = state->ib_format;
	}

	for (i = 0; i < D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++) {
		if (!state->vbs[i])
			continue;
		ShimGetResourceDesc(state->vbs[i], &buffer);
		if (state->vb_offsets[i] >= buffer.byte_width)
			continue;
		vbs[i].BufferLocation = Dx12ShimBufferAddress(state->vbs[i], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
		if (!vbs[i].BufferLocation && ShimNotReady(ShimResourceOf(state->vbs[i])))
			return false;
		if (!vbs[i].BufferLocation)
			continue;
		vbs[i].BufferLocation += state->vb_offsets[i];
		vbs[i].SizeInBytes = buffer.byte_width - state->vb_offsets[i];
		vbs[i].StrideInBytes = state->vb_strides[i];
		vb_count = i + 1;
	}

	for (i = 0; i < key.rtv_count; i++) {
		if (state->rtvs[i] && !Dx12ShimTransition(ShimViewOf(state->rtvs[i])->resource, D3D12_RESOURCE_STATE_RENDER_TARGET))
			return false;
	}
	if (dsv && !Dx12ShimTransition(dsv->resource, dsv->read_only ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE))
		return false;

	list = Dx12ShimList();
	list->SetGraphicsRootSignature(signature);
	if (!bind_tables(state, &root, shaders, uav_used, false))
		return false;
	list->SetPipelineState(pipeline);

	list->OMSetRenderTargets(key.rtv_count, rtvs, FALSE, dsv ? &dsv->handle : NULL);
	list->OMSetBlendFactor(state->blend_factor);
	list->OMSetStencilRef(state->stencil_ref);
	// D3D12_VIEWPORT is the same struct as D3D11_VIEWPORT
	list->RSSetViewports(state->viewport_count, (const D3D12_VIEWPORT*)state->viewports);
	if (state->rasterizer && static_cast<ShimRasterizerState*>(state->rasterizer)->mDesc.ScissorEnable) {
		// D3D11 clips everything away for a viewport without a
		// rectangle, D3D12 leaves it undefined
		for (i = 0; i < state->viewport_count; i++)
			scissors[i] = i < state->scissor_count ? state->scissors[i] : D3D12_RECT{};
	} else {
		for (i = 0; i < state->viewport_count; i++)
			scissors[i] = no_scissor;
	}
	list->RSSetScissorRects(state->viewport_count, scissors);

	// The values of D3D11_PRIMITIVE_TOPOLOGY are those of D3D12's
	list->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)state->topology);
	list->IASetVertexBuffers(0, vb_count, vbs);
	if (indexed)
		list->IASetIndexBuffer(&ib);
	return true;
}

// -----------------------------------------------------------------------------
// Compute

bool Dx12ShimPrepareDispatch(const ShimState *state)
{
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
	const ShimShader *shaders[DX12_STAGE_COUNT];
	ID3D12GraphicsCommandList *list;
	ID3D12RootSignature *signature;
	ID3D12PipelineState *pipeline;
	const ShimShader *shader;
	UINT64 uav_used;
	RootKey root;

	root_key(state, compute_stages, ARRAYSIZE(compute_stages), &root, shaders, &uav_used);
	shader = shaders[DX12_STAGE_CS];
	if (!shader)
		return false;

	signature = get_root_signature(&root, false);
	if (!signature)
		return false;

	auto found = compute_pipelines.find(shader->id);
	if (found != compute_pipelines.end()) {
		pipeline = found->second;
	} else {
		desc.pRootSignature = signature;
		desc.CS.pShaderBytecode = shader->bytecode.data();
		desc.CS.BytecodeLength = shader->bytecode.size();
		pipeline = NULL;
		if (FAILED(dx12_shim->device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline))))
			Dx12LogInfo("*** Failed to create a compute pipeline state for shader %llu of a mod\n", shader->id);
		compute_pipelines[shader->id] = pipeline;
	}
	if (!pipeline)
		return false;

	list = Dx12ShimList();
	list->SetComputeRootSignature(signature);
	if (!bind_tables(state, &root, shaders, uav_used, true))
		return false;
	list->SetPipelineState(pipeline);
	return true;
}
