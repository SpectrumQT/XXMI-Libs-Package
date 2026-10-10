#pragma once

#include <d3d11_1.h>
#include <INITGUID.h>

#include "DrawCallInfo.h"

#include "CommandList.h"

#include "HackerInputLayout.h"
#include "HackerDevice.h"
#include "Globals.h"

// {A3046B1E-336B-4D90-9FD6-234BC09B8687}
DEFINE_GUID(IID_HackerContext,
0xa3046b1e, 0x336b, 0x4d90, 0x9f, 0xd6, 0x23, 0x4b, 0xc0, 0x9b, 0x86, 0x87);


// Self forward reference for the factory interface.
class HackerContext;

HackerContext* HackerContextFactory(ID3D11Device1 *pDevice1, ID3D11DeviceContext1 *pContext1);

// Forward declaration to allow circular reference between HackerContext and HackerDevice. 
// We need this to allow each to reference the other as needed.

class HackerDevice;

enum class FrameAnalysisOptions;
struct ShaderOverride;


struct DrawContext
{
	ID3D11PixelShader *oldPixelShader;
	ID3D11VertexShader *oldVertexShader;
	CommandList *post_commands[5];
	DrawCallInfo call_info;

	DrawContext(DrawCall type,
			UINT VertexCount, UINT IndexCount, UINT InstanceCount,
			UINT FirstVertex, UINT FirstIndex, UINT FirstInstance,
			ID3D11Buffer **indirect_buffer, UINT args_offset) :
		oldVertexShader(NULL),
		oldPixelShader(NULL),
		call_info(type, VertexCount, IndexCount, InstanceCount, FirstVertex, FirstIndex, FirstInstance,
				indirect_buffer, args_offset)
	{
		memset(post_commands, 0, sizeof(post_commands));
	}
};

struct DispatchContext
{
	CommandList *post_commands;
	DrawCallInfo call_info;

	DispatchContext(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) :
		post_commands(NULL),
		call_info(DrawCall::Dispatch, 0, 0, 0, 0, 0, 0, NULL, 0, ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ)
	{}

	DispatchContext(ID3D11Buffer **indirect_buffer, UINT args_offset) :
		post_commands(NULL),
		call_info(DrawCall::DispatchIndirect, 0, 0, 0, 0, 0, 0, indirect_buffer, args_offset)
	{}
};



// These are per-context so we shouldn't need locks
struct MappedResourceInfo {
	D3D11_MAPPED_SUBRESOURCE map;
	bool mapped_writable;
	void *orig_pData;
	size_t size;
	UINT bind_flags;

	MappedResourceInfo() :
		orig_pData(NULL),
		size(0),
		mapped_writable(false),
		bind_flags(0)
	{}
};


// 1-6-18:  Current approach will be to only create one level of wrapping,
// specifically HackerDevice and HackerContext, based on the ID3D11Device1,
// and ID3D11DeviceContext1.  ID3D11Device1/ID3D11DeviceContext1 is supported
// on Win7+platform_update, and thus is a superset of what we need.  By
// using the highest level object supported, we can kill off a lot of conditional
// code that just complicates things. 
//
// The ID3D11DeviceContext1 will be supported on all OS except Win7 minus the 
// platform_update.  In that scenario, we will save a reference to the 
// ID3D11DeviceContext object instead, but store it and wrap it in HackerContext.
// 
// Specifically decided to not name everything *1, because frankly that is 
// was an awful choice on Microsoft's part to begin with.  Meaningless number
// completely unrelated to version/revision or functionality.  Bad.
// We will use the *1 notation for object names that are specific types,
// like the mOrigContext1 to avoid misleading types.
//
// Any HackerDevice will be the superset object ID3D11DeviceContext1 in all cases
// except for Win7 missing the evil platform_update.

// What a checktextureoverride command matched when it last ran, see
// HackerContext::mCheckTextureOverrideResults:
struct CheckTextureOverrideResult {
	static const unsigned MAX_MATCHES = 4;

	const CheckTextureOverrideCommand *command;
	unsigned count;
	TextureOverride *matches[MAX_MATCHES];
};

// Hierarchy:
//  HackerContext <- ID3D11DeviceContext1 <- ID3D11DeviceContext <- ID3D11DeviceChild <- IUnknown

class HackerContext : public ID3D11DeviceContext1
{
private:
	ID3D11Device1 *mOrigDevice1;
	ID3D11DeviceContext1 *mOrigContext1;
	ID3D11DeviceContext1 *mRealOrigContext1;
	HackerDevice *mHackerDevice;

	// These are per-context, moved from globals.h:
	uint32_t mCurrentVertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	uint32_t mCurrentIndexBuffer; // Only valid while hunting=1
	struct VertexBufferBinding {
		ID3D11Buffer* buffer;
		UINT offset;
		UINT stride;
	} mCurrentVertexBuffersBindings[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
	struct IndexBufferBinding {
		ID3D11Buffer* buffer;
		UINT offset;
		DXGI_FORMAT format;
		bool is_explicit;
	} mCurrentIndexBufferBinding;
	std::vector<ID3D11Resource *> mCurrentRenderTargets;
	ID3D11Resource *mCurrentDepthTarget;
	UINT mCurrentPSUAVStartSlot;
	UINT mCurrentPSNumUAVs;

	HackerInputLayout* mCurrentInputLayout;
	HackerInputLayout* mOriginalInputLayout;
	HackerInputLayout* mOverrideInputLayout;

	// Used for deny_cpu_read, track_texture_updates and constant buffer matching
	typedef std::unordered_map<ID3D11Resource*, MappedResourceInfo> MappedResources;
	MappedResources mMappedResources;

	unsigned draw_number;
	unsigned dispatch_number;

	// Whether the shader the game bound to each stage may still be waiting
	// for deferred ShaderRegex analysis. This is worked out from the lookup
	// SetShader() does anyway, so that draw and dispatch calls only take the
	// lock for the rare shader that still needs analysing instead of for
	// every bound shader on every call. When G->deferred_shader_generation
	// moves on all stages are rechecked, since shaders that are still bound
	// may have become candidates again:
	bool mVertexShaderDeferredPending;
	bool mHullShaderDeferredPending;
	bool mDomainShaderDeferredPending;
	bool mGeometryShaderDeferredPending;
	bool mPixelShaderDeferredPending;
	bool mComputeShaderDeferredPending;
	unsigned mDeferredShaderGeneration;

	// Direct mapped cache in front of G->mShaderOverrideMap for the lookups
	// every draw call does for each of its bound shaders. Entries (misses
	// included) are only valid while G->shader_override_generation matches:
	static const unsigned SHADER_OVERRIDE_CACHE_BITS = 10;
	struct ShaderOverrideCacheEntry {
		UINT64 hash;
		struct ShaderOverride *shader_override;
		unsigned generation;
	} mShaderOverrideCache[1 << SHADER_OVERRIDE_CACHE_BITS];
	struct ShaderOverride* LookupShaderOverride(UINT64 hash);

	// What the checktextureoverride commands that ran so far during the
	// draw call in progress matched (for most of them: nothing), so that
	// running the same commands again (typically from the post command
	// lists, having run from the pre command lists already) can skip
	// looking up bindings that cannot have changed. Only in use from the
	// start of BeforeDraw() to the end of AfterDraw(), for command lists
	// run with the DrawCallInfo of that draw call. Running a command that
	// might change the bindings forgets the results for the slots it may
	// have bound to, which is all of them unless the command is known to
	// keep to certain slots. Results that do not fit in here (too many of
	// them, or too many matches in one) are just looked up again:
	static const unsigned MAX_CHECK_TEXTURE_OVERRIDE_RESULTS = 32;
	CheckTextureOverrideResult mCheckTextureOverrideResults[MAX_CHECK_TEXTURE_OVERRIDE_RESULTS];
	unsigned mCheckTextureOverrideResultCount;
	const DrawCallInfo *mCheckTextureOverrideResultsDraw;

	// Views that command lists created to bind a resource by reference to a
	// t slot, so that binding the same resource again need not create
	// another. An operation remembers the view it used last by itself, but
	// that is no help to one that binds a different resource every time it
	// runs, such as those putting back the textures the game had bound
	// after each modded draw call: every run would destroy the view of the
	// previous one and create its own. A few entries per bucket, the bucket
	// chosen by resource. Each view keeps its resource alive, so those that
	// go unused for a whole frame are dropped:
	static const unsigned SHADER_RESOURCE_VIEW_CACHE_BUCKET_BITS = 8;
	static const unsigned SHADER_RESOURCE_VIEW_CACHE_WAYS = 4;
	struct ShaderResourceViewCacheEntry {
		ID3D11View *view;
		ID3D11Resource *resource;
		// What the view was created from besides the resource:
		UINT stride, offset, buf_size;
		DXGI_FORMAT format;
		ResourceCopyOptions options;
		unsigned frame_no;
	} mShaderResourceViewCache[SHADER_RESOURCE_VIEW_CACHE_WAYS << SHADER_RESOURCE_VIEW_CACHE_BUCKET_BITS];
	unsigned mShaderResourceViewCacheCount;
	unsigned mShaderResourceViewCacheFrame;
	ShaderResourceViewCacheEntry* ShaderResourceViewCacheBucket(ID3D11Resource *resource);
	void ExpireShaderResourceViews();

	FlatHashMap<UINT, ID3D11Buffer*> mReadbackBuffers = FlatHashMap<UINT, ID3D11Buffer*>(64);

	// These private methods are utility routines for HackerContext.
	void BeforeDraw(DrawContext &data);
	void AfterDraw(DrawContext &data);
	bool BeforeDispatch(DispatchContext *context);
	void AfterDispatch(DispatchContext *context);
	template <class ID3D11Shader,
		void (__stdcall ID3D11DeviceContext::*GetShaderVS2013BUGWORKAROUND)(ID3D11Shader**, ID3D11ClassInstance**, UINT*),
		void (__stdcall ID3D11DeviceContext::*SetShaderVS2013BUGWORKAROUND)(ID3D11Shader*, ID3D11ClassInstance*const*, UINT)
	>
	bool DeferredShaderReplacement(ID3D11DeviceChild *shader, UINT64 hash, wchar_t *shader_type);
	void UpdateDeferredShaderGeneration();
	void DeferredShaderReplacementBeforeDraw();
	void DeferredShaderReplacementBeforeDispatch();
	bool ExpandRegionCopy(ID3D11Resource *pDstResource, UINT DstX,
		UINT DstY, ID3D11Resource *pSrcResource, const D3D11_BOX *pSrcBox,
		UINT *replaceDstX, D3D11_BOX *replaceBox);
	bool MapDenyCPURead(ID3D11Resource *pResource, UINT Subresource,
			D3D11_MAP MapType, UINT MapFlags,
			D3D11_MAPPED_SUBRESOURCE *pMappedResource);
	bool MapTrackRegionHashes(ID3D11Resource* pResource, D3D11_MAP MapType, D3D11_RESOURCE_DIMENSION* dim);
	void TrackAndDivertMap(HRESULT map_hr, ID3D11Resource *pResource,
		UINT Subresource, D3D11_MAP MapType, UINT MapFlags,
		D3D11_MAPPED_SUBRESOURCE *pMappedResource);
	void TrackAndDivertUnmap(ID3D11Resource *pResource, UINT Subresource);
	void ProcessShaderOverride(ShaderOverride *shaderOverride, bool isPixelShader, DrawContext *data);
	ID3D11PixelShader* SwitchPSShader(ID3D11PixelShader *shader);
	ID3D11VertexShader* SwitchVSShader(ID3D11VertexShader *shader);
	void RecordDepthStencil(ID3D11DepthStencilView *target);
	template <void (__stdcall ID3D11DeviceContext::*GetShaderResources)(THIS_
		UINT StartSlot,
		UINT NumViews,
		ID3D11ShaderResourceView **ppShaderResourceViews)>
	void RecordShaderResourceUsage(std::map<UINT64, ShaderInfoData> &ShaderInfo, UINT64 currentShader);
	void _RecordShaderResourceUsage(ShaderInfoData *shader_info,
			ID3D11ShaderResourceView *views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT]);
	void RecordGraphicsShaderStats();
	void RecordComputeShaderStats();
	void RecordPeerShaders(std::set<UINT64> *PeerShaders, UINT64 this_shader_hash);
	void RecordRenderTargetInfo(ID3D11RenderTargetView *target, UINT view_num);
	ID3D11Resource* RecordResourceViewStats(ID3D11View *view, std::set<uint32_t> *resource_info);

	// Templates to reduce duplicated code:
	template <class ID3D11Shader,
		 void (__stdcall ID3D11DeviceContext::*OrigSetShader)(THIS_
				 ID3D11Shader *pShader,
				 ID3D11ClassInstance *const *ppClassInstances,
				 UINT NumClassInstances)
		 >
	STDMETHODIMP_(void) SetShader(THIS_
		/* [annotation] */
		__in_opt ID3D11Shader *pShader,
		/* [annotation] */
		__in_ecount_opt(NumClassInstances) ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances,
		std::set<UINT64> *visitedShaders,
		UINT64 selectedShader,
		UINT64 *currentShaderHash,
		ID3D11Shader **currentShaderHandle,
		bool *deferredPending);
	template <void (__stdcall ID3D11DeviceContext::*OrigSetShaderResources)(THIS_
			UINT StartSlot,
			UINT NumViews,
			ID3D11ShaderResourceView *const *ppShaderResourceViews)>
	void BindResources();
	template <void (__stdcall ID3D11DeviceContext::*OrigSetShaderResources)(THIS_
			UINT StartSlot,
			UINT NumViews,
			ID3D11ShaderResourceView *const *ppShaderResourceViews)>
	void SetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews);

protected:
	// Allow FrameAnalysisContext access to these as an interim measure
	// until it has been further decoupled from HackerContext. Be wary of
	// relying on these - they will be zero in release mode with no
	// ShaderOverrides / ShaderRegex:
	UINT64 mCurrentVertexShader;
	UINT64 mCurrentHullShader;
	UINT64 mCurrentDomainShader;
	UINT64 mCurrentGeometryShader;
	UINT64 mCurrentPixelShader;
	UINT64 mCurrentComputeShader;

public:
	// On D3D12 there are no Draw* and Dispatch methods of ours for the
	// game to call: dx12/DrawPath.cpp tells us of the game's calls with
	// these, along with the shaders the Set*Shader methods would have seen
	void Dx12BeforeDraw(DrawContext &data, const UINT64 *shader_hashes);
	void Dx12AfterDraw(DrawContext &data);
	bool Dx12BeforeDispatch(DispatchContext *context, UINT64 shader_hash);
	void Dx12AfterDispatch(DispatchContext *context);
	// The shader of the type ('v', 'p', ...) in the game's draw or dispatch
	// that is being handled, which has no D3D11 object to look up. 0 if
	// there is none or this is not D3D12.
	UINT64 Dx12CurrentShader(wchar_t shader_type);

	HackerContext(ID3D11Device1 *pDevice1, ID3D11DeviceContext1 *pContext1);
	~HackerContext();

	void SetHackerDevice(HackerDevice *pDevice);
	HackerDevice* GetHackerDevice();
	void Bind3DMigotoResources();
	void InitIniParams();
	ID3D11DeviceContext1* GetPossiblyHookedOrigContext1();
	ID3D11DeviceContext1* GetPassThroughOrigContext1();
	void HookContext();

	// public to allow CommandList access
	virtual void FrameAnalysisLog(char *fmt, ...) {};
	virtual void FrameAnalysisTrigger(FrameAnalysisOptions new_options) {};
	virtual void FrameAnalysisDump(ID3D11Resource *resource, FrameAnalysisOptions options,
		const wchar_t *target, DXGI_FORMAT format, UINT stride, UINT offset) {};

	unsigned GetDrawNumber() const { return draw_number; };
	unsigned GetDispatchNumber() const { return dispatch_number; };
	void ResetCallCounters() { draw_number = 0; dispatch_number = 0; };

	// public to allow CommandList access, see mCheckTextureOverrideResults:
	void NoteCheckTextureOverrideResult(const CheckTextureOverrideCommand *command, const DrawCallInfo *call_info, const TextureOverrideMatches &matches)
	{
		if (!call_info || call_info != mCheckTextureOverrideResultsDraw)
			return;
		if (mCheckTextureOverrideResultCount >= MAX_CHECK_TEXTURE_OVERRIDE_RESULTS
		 || matches.size() > CheckTextureOverrideResult::MAX_MATCHES)
			return;

		CheckTextureOverrideResult &result = mCheckTextureOverrideResults[mCheckTextureOverrideResultCount++];
		result.command = command;
		result.count = (unsigned)matches.size();
		for (unsigned i = 0; i < result.count; i++)
			result.matches[i] = matches[i];
	}
	// NULL unless the command has run before during this draw call and
	// nothing may have bound to its slot since. The result does not outlive
	// the next command that runs, copy the matches before running theirs:
	const CheckTextureOverrideResult* FindCheckTextureOverrideResult(const CheckTextureOverrideCommand *command, const DrawCallInfo *call_info) const
	{
		if (!call_info || call_info != mCheckTextureOverrideResultsDraw)
			return NULL;
		for (unsigned i = 0; i < mCheckTextureOverrideResultCount; i++) {
			if (mCheckTextureOverrideResults[i].command == command)
				return &mCheckTextureOverrideResults[i];
		}
		return NULL;
	}
	void ForgetCheckTextureOverrideResults() { mCheckTextureOverrideResultCount = 0; };
	// As above, for a command known to have bound to these slots only, see
	// CommandListCommand::binds_known_slots:
	void ForgetCheckTextureOverrideResults(ResourceCopyTargetType type, wchar_t shader_type, unsigned first_slot, unsigned slot_count)
	{
		for (unsigned i = 0; i < mCheckTextureOverrideResultCount; ) {
			const ResourceCopyTarget &target = mCheckTextureOverrideResults[i].command->target;
			bool bound = target.type == type;

			// There is only the one ib, and vb slots belong to no stage:
			if (bound && type != ResourceCopyTargetType::INDEX_BUFFER) {
				bound = target.slot >= first_slot && target.slot - first_slot < slot_count;
				if (type != ResourceCopyTargetType::VERTEX_BUFFER)
					bound = bound && target.shader_type == shader_type;
			}

			if (bound)
				mCheckTextureOverrideResults[i] = mCheckTextureOverrideResults[--mCheckTextureOverrideResultCount];
			else
				i++;
		}
	}

	// public to allow CommandList access, see mShaderResourceViewCache. A
	// view is only recalled for the exact parameters one was noted with, and
	// is the caller's to release. NULL if there is none:
	ID3D11View* RecallShaderResourceView(ID3D11Resource *resource, UINT stride, UINT offset,
			DXGI_FORMAT format, UINT buf_size, ResourceCopyOptions options);
	void NoteShaderResourceView(ID3D11View *view, ID3D11Resource *resource, UINT stride, UINT offset,
			DXGI_FORMAT format, UINT buf_size, ResourceCopyOptions options);

	ID3D11Buffer* GetReadbackBuffer(UINT size);

	void DeferInputLayoutOverride(HackerInputLayout* pInputLayout);
	void OverrideInputLayout();
	void RestoreInputLayout();

	// These are the shaders the game has set, which may be different from
	// the ones we have bound to the pipeline:
	ID3D11VertexShader *mCurrentVertexShaderHandle;
	ID3D11PixelShader *mCurrentPixelShaderHandle;
	ID3D11ComputeShader *mCurrentComputeShaderHandle;
	ID3D11GeometryShader *mCurrentGeometryShaderHandle;
	ID3D11DomainShader *mCurrentDomainShaderHandle;
	ID3D11HullShader *mCurrentHullShaderHandle;

	/*** IUnknown methods ***/

	HRESULT STDMETHODCALLTYPE QueryInterface(
		/* [in] */ REFIID riid,
		/* [iid_is][out] */ _COM_Outptr_ void __RPC_FAR *__RPC_FAR *ppvObject);

	ULONG STDMETHODCALLTYPE AddRef(void);

	ULONG STDMETHODCALLTYPE Release(void);


	/** ID3D11DeviceChild **/

	void STDMETHODCALLTYPE GetDevice(
		/* [annotation] */
		_Out_  ID3D11Device **ppDevice);

	HRESULT STDMETHODCALLTYPE GetPrivateData(
		/* [annotation] */
		_In_  REFGUID guid,
		/* [annotation] */
		_Inout_  UINT *pDataSize,
		/* [annotation] */
		_Out_writes_bytes_opt_(*pDataSize)  void *pData);

	HRESULT STDMETHODCALLTYPE SetPrivateData(
		/* [annotation] */
		_In_  REFGUID guid,
		/* [annotation] */
		_In_  UINT DataSize,
		/* [annotation] */
		_In_reads_bytes_opt_(DataSize)  const void *pData);

	HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(
		/* [annotation] */
		_In_  REFGUID guid,
		/* [annotation] */
		_In_opt_  const IUnknown *pData);


	/** ID3D11DeviceContext **/

	void STDMETHODCALLTYPE VSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE PSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE PSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11PixelShader *pPixelShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE PSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE VSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11VertexShader *pVertexShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE DrawIndexed(
		/* [annotation] */
		_In_  UINT IndexCount,
		/* [annotation] */
		_In_  UINT StartIndexLocation,
		/* [annotation] */
		_In_  INT BaseVertexLocation);

	void STDMETHODCALLTYPE Draw(
		/* [annotation] */
		_In_  UINT VertexCount,
		/* [annotation] */
		_In_  UINT StartVertexLocation);

	HRESULT STDMETHODCALLTYPE Map(
		/* [annotation] */
		_In_  ID3D11Resource *pResource,
		/* [annotation] */
		_In_  UINT Subresource,
		/* [annotation] */
		_In_  D3D11_MAP MapType,
		/* [annotation] */
		_In_  UINT MapFlags,
		/* [annotation] */
		_Out_  D3D11_MAPPED_SUBRESOURCE *pMappedResource);

	void STDMETHODCALLTYPE Unmap(
		/* [annotation] */
		_In_  ID3D11Resource *pResource,
		/* [annotation] */
		_In_  UINT Subresource);

	void STDMETHODCALLTYPE PSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE IASetInputLayout(
		/* [annotation] */
		_In_opt_  ID3D11InputLayout *pInputLayout);

	void STDMETHODCALLTYPE IASetVertexBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppVertexBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pStrides,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pOffsets);

	void STDMETHODCALLTYPE IASetIndexBuffer(
		/* [annotation] */
		_In_opt_  ID3D11Buffer *pIndexBuffer,
		/* [annotation] */
		_In_  DXGI_FORMAT Format,
		/* [annotation] */
		_In_  UINT Offset);

	void STDMETHODCALLTYPE DrawIndexedInstanced(
		/* [annotation] */
		_In_  UINT IndexCountPerInstance,
		/* [annotation] */
		_In_  UINT InstanceCount,
		/* [annotation] */
		_In_  UINT StartIndexLocation,
		/* [annotation] */
		_In_  INT BaseVertexLocation,
		/* [annotation] */
		_In_  UINT StartInstanceLocation);

	void STDMETHODCALLTYPE DrawInstanced(
		/* [annotation] */
		_In_  UINT VertexCountPerInstance,
		/* [annotation] */
		_In_  UINT InstanceCount,
		/* [annotation] */
		_In_  UINT StartVertexLocation,
		/* [annotation] */
		_In_  UINT StartInstanceLocation);

	void STDMETHODCALLTYPE GSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE GSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11GeometryShader *pShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE IASetPrimitiveTopology(
		/* [annotation] */
		_In_  D3D11_PRIMITIVE_TOPOLOGY Topology);

	void STDMETHODCALLTYPE VSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE VSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE Begin(
		/* [annotation] */
		_In_  ID3D11Asynchronous *pAsync);

	void STDMETHODCALLTYPE End(
		/* [annotation] */
		_In_  ID3D11Asynchronous *pAsync);

	HRESULT STDMETHODCALLTYPE GetData(
		/* [annotation] */
		_In_  ID3D11Asynchronous *pAsync,
		/* [annotation] */
		_Out_writes_bytes_opt_(DataSize)  void *pData,
		/* [annotation] */
		_In_  UINT DataSize,
		/* [annotation] */
		_In_  UINT GetDataFlags);

	void STDMETHODCALLTYPE SetPredication(
		/* [annotation] */
		_In_opt_  ID3D11Predicate *pPredicate,
		/* [annotation] */
		_In_  BOOL PredicateValue);

	void STDMETHODCALLTYPE GSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE GSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE OMSetRenderTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11RenderTargetView *const *ppRenderTargetViews,
		/* [annotation] */
		_In_opt_  ID3D11DepthStencilView *pDepthStencilView);

	void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(
		/* [annotation] */
		_In_  UINT NumRTVs,
		/* [annotation] */
		_In_reads_opt_(NumRTVs)  ID3D11RenderTargetView *const *ppRenderTargetViews,
		/* [annotation] */
		_In_opt_  ID3D11DepthStencilView *pDepthStencilView,
		/* [annotation] */
		_In_range_(0, D3D11_1_UAV_SLOT_COUNT - 1)  UINT UAVStartSlot,
		/* [annotation] */
		_In_  UINT NumUAVs,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  const UINT *pUAVInitialCounts);

	void STDMETHODCALLTYPE OMSetBlendState(
		/* [annotation] */
		_In_opt_  ID3D11BlendState *pBlendState,
		/* [annotation] */
		_In_opt_  const FLOAT BlendFactor[4],
		/* [annotation] */
		_In_  UINT SampleMask);

	void STDMETHODCALLTYPE OMSetDepthStencilState(
		/* [annotation] */
		_In_opt_  ID3D11DepthStencilState *pDepthStencilState,
		/* [annotation] */
		_In_  UINT StencilRef);

	void STDMETHODCALLTYPE SOSetTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppSOTargets,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pOffsets);

	void STDMETHODCALLTYPE DrawAuto(void);

	void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(
		/* [annotation] */
		_In_  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		_In_  UINT AlignedByteOffsetForArgs);

	void STDMETHODCALLTYPE DrawInstancedIndirect(
		/* [annotation] */
		_In_  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		_In_  UINT AlignedByteOffsetForArgs);

	void STDMETHODCALLTYPE Dispatch(
		/* [annotation] */
		_In_  UINT ThreadGroupCountX,
		/* [annotation] */
		_In_  UINT ThreadGroupCountY,
		/* [annotation] */
		_In_  UINT ThreadGroupCountZ);

	void STDMETHODCALLTYPE DispatchIndirect(
		/* [annotation] */
		_In_  ID3D11Buffer *pBufferForArgs,
		/* [annotation] */
		_In_  UINT AlignedByteOffsetForArgs);

	void STDMETHODCALLTYPE RSSetState(
		/* [annotation] */
		_In_opt_  ID3D11RasterizerState *pRasterizerState);

	void STDMETHODCALLTYPE RSSetViewports(
		/* [annotation] */
		_In_range_(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumViewports,
		/* [annotation] */
		_In_reads_opt_(NumViewports)  const D3D11_VIEWPORT *pViewports);

	void STDMETHODCALLTYPE RSSetScissorRects(
		/* [annotation] */
		_In_range_(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)  UINT NumRects,
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRects);

	void STDMETHODCALLTYPE CopySubresourceRegion(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_  UINT DstX,
		/* [annotation] */
		_In_  UINT DstY,
		/* [annotation] */
		_In_  UINT DstZ,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource,
		/* [annotation] */
		_In_  UINT SrcSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pSrcBox);

	void STDMETHODCALLTYPE CopyResource(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource);

	void STDMETHODCALLTYPE UpdateSubresource(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pDstBox,
		/* [annotation] */
		_In_  const void *pSrcData,
		/* [annotation] */
		_In_  UINT SrcRowPitch,
		/* [annotation] */
		_In_  UINT SrcDepthPitch);

	void STDMETHODCALLTYPE CopyStructureCount(
		/* [annotation] */
		_In_  ID3D11Buffer *pDstBuffer,
		/* [annotation] */
		_In_  UINT DstAlignedByteOffset,
		/* [annotation] */
		_In_  ID3D11UnorderedAccessView *pSrcView);

	void STDMETHODCALLTYPE ClearRenderTargetView(
		/* [annotation] */
		_In_  ID3D11RenderTargetView *pRenderTargetView,
		/* [annotation] */
		_In_  const FLOAT ColorRGBA[4]);

	void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
		/* [annotation] */
		_In_  ID3D11UnorderedAccessView *pUnorderedAccessView,
		/* [annotation] */
		_In_  const UINT Values[4]);

	void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
		/* [annotation] */
		_In_  ID3D11UnorderedAccessView *pUnorderedAccessView,
		/* [annotation] */
		_In_  const FLOAT Values[4]);

	void STDMETHODCALLTYPE ClearDepthStencilView(
		/* [annotation] */
		_In_  ID3D11DepthStencilView *pDepthStencilView,
		/* [annotation] */
		_In_  UINT ClearFlags,
		/* [annotation] */
		_In_  FLOAT Depth,
		/* [annotation] */
		_In_  UINT8 Stencil);

	void STDMETHODCALLTYPE GenerateMips(
		/* [annotation] */
		_In_  ID3D11ShaderResourceView *pShaderResourceView);

	void STDMETHODCALLTYPE SetResourceMinLOD(
		/* [annotation] */
		_In_  ID3D11Resource *pResource,
		FLOAT MinLOD);

	FLOAT STDMETHODCALLTYPE GetResourceMinLOD(
		/* [annotation] */
		_In_  ID3D11Resource *pResource);

	void STDMETHODCALLTYPE ResolveSubresource(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource,
		/* [annotation] */
		_In_  UINT SrcSubresource,
		/* [annotation] */
		_In_  DXGI_FORMAT Format);

	void STDMETHODCALLTYPE ExecuteCommandList(
		/* [annotation] */
		_In_  ID3D11CommandList *pCommandList,
		BOOL RestoreContextState);

	void STDMETHODCALLTYPE HSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE HSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11HullShader *pHullShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE HSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE HSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE DSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE DSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11DomainShader *pDomainShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE DSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE DSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE CSSetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_In_reads_opt_(NumViews)  ID3D11ShaderResourceView *const *ppShaderResourceViews);

	void STDMETHODCALLTYPE CSSetUnorderedAccessViews(
		/* [annotation] */
		_In_range_(0, D3D11_1_UAV_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_1_UAV_SLOT_COUNT - StartSlot)  UINT NumUAVs,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
		/* [annotation] */
		_In_reads_opt_(NumUAVs)  const UINT *pUAVInitialCounts);

	void STDMETHODCALLTYPE CSSetShader(
		/* [annotation] */
		_In_opt_  ID3D11ComputeShader *pComputeShader,
		/* [annotation] */
		_In_reads_opt_(NumClassInstances)  ID3D11ClassInstance *const *ppClassInstances,
		UINT NumClassInstances);

	void STDMETHODCALLTYPE CSSetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_In_reads_opt_(NumSamplers)  ID3D11SamplerState *const *ppSamplers);

	void STDMETHODCALLTYPE CSSetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers);

	void STDMETHODCALLTYPE VSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE PSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE PSGetShader(
		/* [annotation] */
		_Out_  ID3D11PixelShader **ppPixelShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE PSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE VSGetShader(
		/* [annotation] */
		_Out_  ID3D11VertexShader **ppVertexShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE PSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE IAGetInputLayout(
		/* [annotation] */
		_Out_  ID3D11InputLayout **ppInputLayout);

	void STDMETHODCALLTYPE IAGetVertexBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppVertexBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pStrides,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pOffsets);

	void STDMETHODCALLTYPE IAGetIndexBuffer(
		/* [annotation] */
		_Out_opt_  ID3D11Buffer **pIndexBuffer,
		/* [annotation] */
		_Out_opt_  DXGI_FORMAT *Format,
		/* [annotation] */
		_Out_opt_  UINT *Offset);

	void STDMETHODCALLTYPE GSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE GSGetShader(
		/* [annotation] */
		_Out_  ID3D11GeometryShader **ppGeometryShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE IAGetPrimitiveTopology(
		/* [annotation] */
		_Out_  D3D11_PRIMITIVE_TOPOLOGY *pTopology);

	void STDMETHODCALLTYPE VSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE VSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE GetPredication(
		/* [annotation] */
		_Out_opt_  ID3D11Predicate **ppPredicate,
		/* [annotation] */
		_Out_opt_  BOOL *pPredicateValue);

	void STDMETHODCALLTYPE GSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE GSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE OMGetRenderTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11RenderTargetView **ppRenderTargetViews,
		/* [annotation] */
		_Out_opt_  ID3D11DepthStencilView **ppDepthStencilView);

	void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(
		/* [annotation] */
		_In_range_(0, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT)  UINT NumRTVs,
		/* [annotation] */
		_Out_writes_opt_(NumRTVs)  ID3D11RenderTargetView **ppRenderTargetViews,
		/* [annotation] */
		_Out_opt_  ID3D11DepthStencilView **ppDepthStencilView,
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT UAVStartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - UAVStartSlot)  UINT NumUAVs,
		/* [annotation] */
		_Out_writes_opt_(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews);

	void STDMETHODCALLTYPE OMGetBlendState(
		/* [annotation] */
		_Out_opt_  ID3D11BlendState **ppBlendState,
		/* [annotation] */
		_Out_opt_  FLOAT BlendFactor[4],
		/* [annotation] */
		_Out_opt_  UINT *pSampleMask);

	void STDMETHODCALLTYPE OMGetDepthStencilState(
		/* [annotation] */
		_Out_opt_  ID3D11DepthStencilState **ppDepthStencilState,
		/* [annotation] */
		_Out_opt_  UINT *pStencilRef);

	void STDMETHODCALLTYPE SOGetTargets(
		/* [annotation] */
		_In_range_(0, D3D11_SO_BUFFER_SLOT_COUNT)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppSOTargets);

	void STDMETHODCALLTYPE RSGetState(
		/* [annotation] */
		_Out_  ID3D11RasterizerState **ppRasterizerState);

	void STDMETHODCALLTYPE RSGetViewports(
		/* [annotation] */
		_Inout_ /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumViewports,
		/* [annotation] */
		_Out_writes_opt_(*pNumViewports)  D3D11_VIEWPORT *pViewports);

	void STDMETHODCALLTYPE RSGetScissorRects(
		/* [annotation] */
		_Inout_ /*_range(0, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE )*/   UINT *pNumRects,
		/* [annotation] */
		_Out_writes_opt_(*pNumRects)  D3D11_RECT *pRects);

	void STDMETHODCALLTYPE HSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE HSGetShader(
		/* [annotation] */
		_Out_  ID3D11HullShader **ppHullShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE HSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE HSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE DSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE DSGetShader(
		/* [annotation] */
		_Out_  ID3D11DomainShader **ppDomainShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE DSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE DSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE CSGetShaderResources(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - StartSlot)  UINT NumViews,
		/* [annotation] */
		_Out_writes_opt_(NumViews)  ID3D11ShaderResourceView **ppShaderResourceViews);

	void STDMETHODCALLTYPE CSGetUnorderedAccessViews(
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_PS_CS_UAV_REGISTER_COUNT - StartSlot)  UINT NumUAVs,
		/* [annotation] */
		_Out_writes_opt_(NumUAVs)  ID3D11UnorderedAccessView **ppUnorderedAccessViews);

	void STDMETHODCALLTYPE CSGetShader(
		/* [annotation] */
		_Out_  ID3D11ComputeShader **ppComputeShader,
		/* [annotation] */
		_Out_writes_opt_(*pNumClassInstances)  ID3D11ClassInstance **ppClassInstances,
		/* [annotation] */
		_Inout_opt_  UINT *pNumClassInstances);

	void STDMETHODCALLTYPE CSGetSamplers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)  UINT NumSamplers,
		/* [annotation] */
		_Out_writes_opt_(NumSamplers)  ID3D11SamplerState **ppSamplers);

	void STDMETHODCALLTYPE CSGetConstantBuffers(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers);

	void STDMETHODCALLTYPE ClearState(void);

	void STDMETHODCALLTYPE Flush(void);

	D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType(void);

	UINT STDMETHODCALLTYPE GetContextFlags(void);

	HRESULT STDMETHODCALLTYPE FinishCommandList(
		BOOL RestoreDeferredContextState,
		/* [annotation] */
		_Out_opt_  ID3D11CommandList **ppCommandList);


	/** ID3D11DeviceContext1 **/

	void STDMETHODCALLTYPE CopySubresourceRegion1(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_  UINT DstX,
		/* [annotation] */
		_In_  UINT DstY,
		/* [annotation] */
		_In_  UINT DstZ,
		/* [annotation] */
		_In_  ID3D11Resource *pSrcResource,
		/* [annotation] */
		_In_  UINT SrcSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pSrcBox,
		/* [annotation] */
		_In_  UINT CopyFlags);

	void STDMETHODCALLTYPE UpdateSubresource1(
		/* [annotation] */
		_In_  ID3D11Resource *pDstResource,
		/* [annotation] */
		_In_  UINT DstSubresource,
		/* [annotation] */
		_In_opt_  const D3D11_BOX *pDstBox,
		/* [annotation] */
		_In_  const void *pSrcData,
		/* [annotation] */
		_In_  UINT SrcRowPitch,
		/* [annotation] */
		_In_  UINT SrcDepthPitch,
		/* [annotation] */
		_In_  UINT CopyFlags);

	void STDMETHODCALLTYPE DiscardResource(
		/* [annotation] */
		_In_  ID3D11Resource *pResource);

	void STDMETHODCALLTYPE DiscardView(
		/* [annotation] */
		_In_  ID3D11View *pResourceView);

	void STDMETHODCALLTYPE VSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE HSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE DSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE GSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE PSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE CSSetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  ID3D11Buffer *const *ppConstantBuffers,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pFirstConstant,
		/* [annotation] */
		_In_reads_opt_(NumBuffers)  const UINT *pNumConstants);

	void STDMETHODCALLTYPE VSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE HSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE DSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE GSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE PSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE CSGetConstantBuffers1(
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1)  UINT StartSlot,
		/* [annotation] */
		_In_range_(0, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - StartSlot)  UINT NumBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  ID3D11Buffer **ppConstantBuffers,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pFirstConstant,
		/* [annotation] */
		_Out_writes_opt_(NumBuffers)  UINT *pNumConstants);

	void STDMETHODCALLTYPE SwapDeviceContextState(
		/* [annotation] */
		_In_  ID3DDeviceContextState *pState,
		/* [annotation] */
		_Out_opt_  ID3DDeviceContextState **ppPreviousState);

	void STDMETHODCALLTYPE ClearView(
		/* [annotation] */
		_In_  ID3D11View *pView,
		/* [annotation] */
		_In_  const FLOAT Color[4],
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRect,
		UINT NumRects);

	void STDMETHODCALLTYPE DiscardView1(
		/* [annotation] */
		_In_  ID3D11View *pResourceView,
		/* [annotation] */
		_In_reads_opt_(NumRects)  const D3D11_RECT *pRects,
		UINT NumRects);
};
