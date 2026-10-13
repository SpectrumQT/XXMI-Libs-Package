#pragma once

#include <string>
#include <vector>

#include "Bridge.h"
#include "../vk/VkScope.h"
#include "PipelineRegistry.h"

// The D3D11 objects the engine holds on D3D12. They have to be real COM
// objects with the D3D11 vtables, as the engine calls GetDesc, GetType,
// GetResource and so on on them, but what backs them is D3D12.
//
// The engine passes them around as their D3D11 base interfaces, so finding
// our data behind an ID3D11Resource or ID3D11View must not depend on which
// derived class it is. Every class of a family therefore has the same layout
// up to and including that data: ShimChild<I>, which is the same for any I
// as all these interfaces are single inheritance chains, then the family's
// struct. See ShimResourceOf and friends.

class ShimPrivateData {
	struct Entry {
		GUID guid;
		IUnknown *unknown;
		std::vector<BYTE> data;
	};
	std::vector<Entry> mEntries;

	void Remove(REFGUID guid);
public:
	~ShimPrivateData();

	HRESULT Get(REFGUID guid, UINT *pDataSize, void *pData);
	HRESULT Set(REFGUID guid, UINT DataSize, const void *pData);
	HRESULT SetInterface(REFGUID guid, const IUnknown *pData);
};

void Dx12ShimGetDevice(ID3D11Device **ppDevice);
HRESULT ShimQueryInterface(IUnknown *object, const IID *const *iids, size_t count, REFIID riid, void **ppvObject);

#define SHIM_QUERY_INTERFACE(...) \
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override \
	{ \
		static const IID *const iids[] = { &__uuidof(IUnknown), &__uuidof(ID3D11DeviceChild), __VA_ARGS__ }; \
		return ShimQueryInterface(this, iids, ARRAYSIZE(iids), riid, ppvObject); \
	}

template <class Interface>
class ShimChild : public Interface
{
public:
	volatile LONG mRef;
	// The engine attaches an object to a resource to learn of its
	// destruction, so interfaces set here are released with us
	ShimPrivateData mPrivate;

	ShimChild() : mRef(1) {}
	virtual ~ShimChild() {}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return InterlockedIncrement(&mRef);
	}
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG ref = InterlockedDecrement(&mRef);

		if (!ref && !dx12_unloading)
			delete this;
		return ref;
	}
	void STDMETHODCALLTYPE GetDevice(ID3D11Device **ppDevice) override
	{
		Dx12ShimGetDevice(ppDevice);
	}
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override
	{
		return mPrivate.Get(guid, pDataSize, pData);
	}
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override
	{
		return mPrivate.Set(guid, DataSize, pData);
	}
	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *pData) override
	{
		return mPrivate.SetInterface(guid, pData);
	}
};

// -----------------------------------------------------------------------------
// Resources

struct ShimView;

struct ShimResource {
	D3D11_RESOURCE_DIMENSION dimension;

	// NULL for a buffer that only exists in shadow, and for a resource
	// of the game that is gone (see foreign)
	ID3D12Resource *resource;
	D3D12_RESOURCE_DESC desc;
	// Of the whole resource, as left by the commands recorded so far
	D3D12_RESOURCE_STATES state;
	// Every submission leaves the resources it used in the common state,
	// see Bridge.h. touched_epoch tells which those are.
	UINT touched_epoch;

	// Belongs to the game, like a back buffer. Not referenced: whoever
	// made the wrapper clears resource when the game lets go of it.
	bool foreign;
	// Where a constant buffer of the game is, if that is memory a list of
	// ours can read whatever the game is doing with it. Dx12ShimBufferAddress
	// returns it.
	UINT64 foreign_va;

	// Buffers whose contents come from the CPU and are only used where
	// D3D12 takes an address (vertex, index and constant buffers) have no
	// resource of their own. The contents live in shadow and are copied to
	// upload memory when a draw needs them, which keeps a constant buffer
	// rewritten between draws correct whatever order command lists are
	// executed in.
	bool cpu_only;
	// Contents as last written through Map or UpdateSubresource. Always
	// there for cpu_only buffers, otherwise only for what the engine may
	// map: subresource 0 with tightly packed rows.
	std::vector<BYTE> shadow;
	// Where the shadow of a cpu_only buffer was last uploaded, see
	// Dx12ShimBufferAddress
	UINT64 upload_va;
	UINT64 upload_version;
	UINT upload_epoch;
	// Counts the writes: of the shadow of a cpu_only buffer, and the times
	// a list of ours was to write a resource (Dx12ShimTransition)
	UINT64 version;
	// The version at which the shadow of a buffer with a resource was all
	// of its contents. At another, something wrote the resource that the
	// shadow knows nothing of.
	UINT64 shadow_version;
	// vk/VkDrawPath has a Vulkan buffer with the contents, and is told
	// when this goes
	bool mirrored;
	// The shadow of a constant buffer of a Vulkan game is yet to be read
	// from where the game has the contents, see ShimReadShadow
	bool shadow_pending;
	// And the resource is yet to be given what the shadow then holds, see
	// ShimUploadShadow
	bool upload_pending;
	// The same for use by the game's lists, where the upload has to
	// outlive our submissions. Referenced.
	ID3D12Resource *game_upload;
	UINT64 game_upload_va;
	UINT64 game_upload_version;
	// Whether Unmap has anything to do
	bool mapped_for_write;
	// Textures: the shadow has been written since it was last copied to
	// the resource. Left until something is about to use the resource
	// (ShimFlushWrite): IniParams is written for many draws that read none
	// of it, and each copy uploads all of it.
	bool write_pending;
	// What the device says of how subresource 0 lies in an upload, asked
	// once (ShimFootprint): it is wanted every time a texture is mapped
	// or written
	bool footprint_known = false;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
	UINT footprint_rows;
	UINT64 footprint_row_size, footprint_total;

	// Staging buffers: what was copied into them from the GPU lands in
	// this readback buffer, and reaches shadow when the engine maps them
	ID3D12Resource *readback;
	UINT64 readback_fence;
	bool readback_pending;

	// Not 0 for a vertex buffer that stands for vertices DrawPath had
	// skinned for a mod, which are in a buffer only it knows: the buffer
	// of the game they take the place of. A copy of such a buffer stands
	// for the same, see copy_buffer.
	UINT64 skin_va;

	// The views made of the resource, for ShimRenameResource to make them
	// again. Not referenced: a view goes before its resource.
	std::vector<ShimView*> views;
	// The D3D12 resources the game's lists were left with by
	// ShimRenameResource, kept for the next renaming once those lists
	// are done with them. Referenced.
	std::vector<ID3D12Resource*> spares;
};

void ShimDestroyResource(ShimResource *resource);

template <class Interface>
class ShimResourceObject : public ShimChild<Interface>
{
public:
	ShimResource r;

	ShimResourceObject() : r() {}
	~ShimResourceObject()
	{
		ShimDestroyResource(&r);
	}

	void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION *pResourceDimension) override
	{
		*pResourceDimension = r.dimension;
	}
	void STDMETHODCALLTYPE SetEvictionPriority(UINT EvictionPriority) override {}
	UINT STDMETHODCALLTYPE GetEvictionPriority() override
	{
		return 0;
	}
};

static inline ShimResource* ShimResourceOf(ID3D11Resource *resource)
{
	return resource ? &static_cast<ShimResourceObject<ID3D11Resource>*>(resource)->r : NULL;
}

// Before that much of the shadow of a buffer is read
inline void ShimReadShadow(ShimResource *resource, UINT64 offset = 0, UINT64 size = UINT64_MAX)
{
	if (resource->shadow_pending)
		VkScopeFillShadow(resource, offset, size);
}

class ShimBuffer final : public ShimResourceObject<ID3D11Buffer>
{
public:
	D3D11_BUFFER_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11Resource), &__uuidof(ID3D11Buffer))
	void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimTexture1D final : public ShimResourceObject<ID3D11Texture1D>
{
public:
	D3D11_TEXTURE1D_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11Resource), &__uuidof(ID3D11Texture1D))
	void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE1D_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimTexture2D final : public ShimResourceObject<ID3D11Texture2D>
{
public:
	D3D11_TEXTURE2D_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11Resource), &__uuidof(ID3D11Texture2D))
	void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimTexture3D final : public ShimResourceObject<ID3D11Texture3D>
{
public:
	D3D11_TEXTURE3D_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11Resource), &__uuidof(ID3D11Texture3D))
	void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE3D_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

// The D3D11 description of a resource, whichever kind it is
struct ShimResourceDesc {
	D3D11_USAGE usage;
	UINT bind_flags;
	UINT cpu_access;
	UINT misc_flags;
	// Buffers
	UINT byte_width;
	UINT stride;
	// Textures
	DXGI_FORMAT format;
	UINT mips;
	UINT array_size;
	UINT samples;
};
void ShimGetResourceDesc(ID3D11Resource *resource, ShimResourceDesc *desc);

// A texture of the game, for the engine to make views of. state is what the
// commands we record find it in and have to leave it in.
ShimTexture2D* ShimWrapTexture2D(ID3D12Resource *resource, D3D12_RESOURCE_STATES state);

// The address a draw can use for the buffer's current contents, uploading
// them first if the buffer has no resource. 0 if there is nothing to use.
UINT64 Dx12ShimBufferAddress(ID3D11Buffer *buffer, D3D12_RESOURCE_STATES state);

// -----------------------------------------------------------------------------
// Views

struct ShimView {
	D3D12_DESCRIPTOR_HEAP_TYPE heap_type;
	D3D12_CPU_DESCRIPTOR_HANDLE handle;
	// Referenced
	ID3D11Resource *resource11;
	ShimResource *resource;
	DXGI_FORMAT format;
	// Depth stencil views that do not write
	bool read_only;
	// What the descriptor was made with, to make it again on another
	// D3D12 resource (ShimRecreateView)
	enum Kind { SRV, UAV, RTV, DSV } kind;
	union {
		D3D12_SHADER_RESOURCE_VIEW_DESC srv;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
		D3D12_RENDER_TARGET_VIEW_DESC rtv;
		D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
	} desc12;
};

void ShimDestroyView(ShimView *view);
// Writes the descriptor again, for the D3D12 resource the view's resource
// has now
void ShimRecreateView(ShimView *view);

template <class Interface>
class ShimViewObject : public ShimChild<Interface>
{
public:
	ShimView v;

	ShimViewObject() : v() {}
	~ShimViewObject()
	{
		ShimDestroyView(&v);
	}

	void STDMETHODCALLTYPE GetResource(ID3D11Resource **ppResource) override
	{
		v.resource11->AddRef();
		*ppResource = v.resource11;
	}
};

static inline ShimView* ShimViewOf(ID3D11View *view)
{
	return view ? &static_cast<ShimViewObject<ID3D11View>*>(view)->v : NULL;
}

class ShimShaderResourceView final : public ShimViewObject<ID3D11ShaderResourceView>
{
public:
	D3D11_SHADER_RESOURCE_VIEW_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11View), &__uuidof(ID3D11ShaderResourceView))
	void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimUnorderedAccessView final : public ShimViewObject<ID3D11UnorderedAccessView>
{
public:
	D3D11_UNORDERED_ACCESS_VIEW_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11View), &__uuidof(ID3D11UnorderedAccessView))
	void STDMETHODCALLTYPE GetDesc(D3D11_UNORDERED_ACCESS_VIEW_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimRenderTargetView final : public ShimViewObject<ID3D11RenderTargetView>
{
public:
	D3D11_RENDER_TARGET_VIEW_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11View), &__uuidof(ID3D11RenderTargetView))
	void STDMETHODCALLTYPE GetDesc(D3D11_RENDER_TARGET_VIEW_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimDepthStencilView final : public ShimViewObject<ID3D11DepthStencilView>
{
public:
	D3D11_DEPTH_STENCIL_VIEW_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11View), &__uuidof(ID3D11DepthStencilView))
	void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

// -----------------------------------------------------------------------------
// Shaders

const UINT SHIM_CBV_SLOTS = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
const UINT SHIM_SRV_SLOTS = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
const UINT SHIM_SAMPLER_SLOTS = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
const UINT SHIM_UAV_SLOTS = D3D11_1_UAV_SLOT_COUNT;

struct ShimShader {
	Dx12ShaderStage stage;
	// Identifies the shader in pipeline keys. An address would not do,
	// as it can pass on to another shader.
	UINT64 id;
	// Of the bytecode, for the keys of the game's pipelines made again with
	// the shader (DrawPath), which outlive it: an id could be taken again
	UINT64 hash;
	std::vector<BYTE> bytecode;

	// What the shader reads, from its reflection: highest register + 1 of
	// each kind, a bit per register it really uses, and what kind of
	// resource it expects where, for binding nothing. The masks are all
	// zero where it could not be read (reflected).
	bool reflected;
	bool dxil;
	// The same shader compiled to DXIL (Dx12DxcCompile), for the pipelines
	// of the game whose own shaders are: a pipeline state does not take
	// DXBC and DXIL together. NULL if there is none. Owned.
	ShimShader *dxil_form;
	// How many render targets a pixel shader writes
	UINT targets;
	UINT cbv_count;
	UINT srv_count;
	UINT sampler_count;
	UINT uav_count;
	UINT cbv_used;
	UINT64 srv_used[2];
	UINT sampler_used;
	UINT64 uav_used;
	BYTE srv_dimension[SHIM_SRV_SLOTS];
	BYTE uav_dimension[SHIM_UAV_SLOTS];
};

void ShimDestroyShader(ShimShader *shader);

template <class Interface>
class ShimShaderObject : public ShimChild<Interface>
{
public:
	ShimShader s;

	ShimShaderObject() : s() {}
	~ShimShaderObject()
	{
		ShimDestroyShader(&s);
	}

	SHIM_QUERY_INTERFACE(&__uuidof(Interface))
};

static inline ShimShader* ShimShaderOf(ID3D11DeviceChild *shader)
{
	return shader ? &static_cast<ShimShaderObject<ID3D11DeviceChild>*>(shader)->s : NULL;
}

// -----------------------------------------------------------------------------
// State objects

class ShimBlendState final : public ShimChild<ID3D11BlendState>
{
public:
	D3D11_BLEND_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11BlendState))
	void STDMETHODCALLTYPE GetDesc(D3D11_BLEND_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimDepthStencilState final : public ShimChild<ID3D11DepthStencilState>
{
public:
	D3D11_DEPTH_STENCIL_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11DepthStencilState))
	void STDMETHODCALLTYPE GetDesc(D3D11_DEPTH_STENCIL_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimRasterizerState final : public ShimChild<ID3D11RasterizerState>
{
public:
	D3D11_RASTERIZER_DESC mDesc;

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11RasterizerState))
	void STDMETHODCALLTYPE GetDesc(D3D11_RASTERIZER_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimSamplerState final : public ShimChild<ID3D11SamplerState>
{
public:
	D3D11_SAMPLER_DESC mDesc;
	D3D12_CPU_DESCRIPTOR_HANDLE mHandle;

	~ShimSamplerState();

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11SamplerState))
	void STDMETHODCALLTYPE GetDesc(D3D11_SAMPLER_DESC *pDesc) override
	{
		*pDesc = mDesc;
	}
};

class ShimInputLayout final : public ShimChild<ID3D11InputLayout>
{
public:
	// As for ShimShader
	UINT64 mId;
	std::vector<D3D12_INPUT_ELEMENT_DESC> mElements;
	// What the SemanticName of the elements points at
	std::vector<std::string> mSemantics;

	~ShimInputLayout();

	SHIM_QUERY_INTERFACE(&__uuidof(ID3D11InputLayout))
};

UINT64 ShimNextId();
