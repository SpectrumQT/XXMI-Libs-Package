#include "PresentPass.h"

#include <time.h>

#include "Bridge.h"
#include "DrawPath.h"
#include "Dx12CommandQueue.h"
#include "Dx12Device.h"
#include "Dx12SwapChain.h"
#include "FrameDump.h"
#include "ShimContext.h"
#include "ShimObjects.h"

#include "util.h"

#include "../CommandList.h"
#include "../globals.h"
#include "../HackerContext.h"
#include "../HackerDevice.h"
#include "../Hunting.h"
#include "../IniHandler.h"
#include "../input.h"
#include "../Overlay.h"
#include "../Override.h"
#include "../vk/VkScope.h"

// Frame actions are run for one swap chain at a time, the first one that
// presents. Another only gets its turn when that one is gone.
static Dx12SwapChain *presenting;
static bool resolution_known;
// Mods that draw to the screen from [Present] count on the viewport the game
// left behind, which with D3D11 covers the back buffer by then. Here the game
// leaves nothing behind, so every frame starts out with this.
static D3D11_VIEWPORT viewport;

// Wrappers of the swap chain's buffers by index. The engine may hold on to
// them and to views of them for as long as it likes, so they do not keep the
// buffers alive: the game could never resize its swap chain otherwise. When
// the buffers go, the wrappers are left without a resource.
static ShimTexture2D *back_buffers[DXGI_MAX_SWAP_CHAIN_BUFFERS];

ID3D11Resource* Dx12ShimBackBuffer()
{
	ID3D12Resource *buffer;
	UINT index;

	// Every swap chain that can present for D3D12 is an IDXGISwapChain3
	if (!presenting || presenting->mVersion < 3)
		return NULL;

	index = presenting->mOrig->GetCurrentBackBufferIndex();
	if (index >= ARRAYSIZE(back_buffers))
		return NULL;

	if (!back_buffers[index]) {
		if (FAILED(presenting->mOrig->GetBuffer(index, IID_PPV_ARGS(&buffer))))
			return NULL;
		// Between frames is the only time we record anything that
		// can use it, and then it is in the state to be presented in
		back_buffers[index] = ShimWrapTexture2D(buffer, D3D12_RESOURCE_STATE_PRESENT);
		buffer->Release();
	}

	back_buffers[index]->AddRef();
	return back_buffers[index];
}

static void submit()
{
	for (ShimTexture2D *buffer : back_buffers) {
		if (buffer)
			Dx12ShimTransition(&buffer->r, D3D12_RESOURCE_STATE_PRESENT);
	}
	Dx12ShimSubmit();
	Dx12LogDebugMessages();
}

void Dx12SwapChainBuffersReleased(Dx12SwapChain *swap_chain, bool destroyed)
{
	Dx12ShimLock();
	if (swap_chain != presenting) {
		Dx12ShimUnlock();
		return;
	}

	// Nothing that refers to the buffers may be left to execute
	submit();
	Dx12ShimSubmitAndWait();

	for (ShimTexture2D *&buffer : back_buffers) {
		if (!buffer)
			continue;
		buffer->r.resource = NULL;
		buffer->Release();
		buffer = NULL;
	}

	resolution_known = false;
	if (destroyed)
		presenting = NULL;
	Dx12ShimUnlock();
}

static bool begin(Dx12SwapChain *swap_chain)
{
	DXGI_SWAP_CHAIN_DESC desc;

	if (!dx12_shim && !Dx12ShimCreate(swap_chain->mQueue->mDevice, swap_chain->mQueue))
		return false;

	if (!presenting && swap_chain->mQueue->mDevice->mOrig == dx12_shim->device) {
		presenting = swap_chain;
		Dx12ShimSetQueue(swap_chain->mQueue);
	}
	if (swap_chain != presenting)
		return false;

	if (!resolution_known && SUCCEEDED(swap_chain->mOrig->GetDesc(&desc))) {
		resolution_known = true;
		viewport = { 0, 0, (float)desc.BufferDesc.Width, (float)desc.BufferDesc.Height, D3D11_MIN_DEPTH, D3D11_MAX_DEPTH };
		if (G->mResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN) {
			G->mResolutionInfo.width = desc.BufferDesc.Width;
			G->mResolutionInfo.height = desc.BufferDesc.Height;
			Dx12LogInfo("Got resolution from swap chain: %ix%i\n", G->mResolutionInfo.width, G->mResolutionInfo.height);
		}
	}
	return true;
}

// The engine's overlay, drawing through the shim to the buffer about to be
// presented. Only made once there is something to show.
static Overlay *overlay;

bool Dx12DrawOverlay()
{
	static ULONGLONG retry_at;

	// Within a draw of the game what the context draws goes to the game's
	// list with the game's pipeline, which the overlay's shaders are not for
	if (!dx12_shim || Dx12InDrawScope() || !OverlayHasAnythingToDraw())
		return false;

	if (!overlay) {
		// Not every frame: what failed may take a while to pass
		if (GetTickCount64() < retry_at)
			return false;
		try {
			overlay = new Overlay(dx12_shim->hacker_device, dx12_shim->hacker_context, NULL);
		} catch (...) {
			Dx12LogInfo("*** Failed to create the overlay, trying again in a while\n");
			retry_at = GetTickCount64() + 5000;
			return false;
		}
	}
	return overlay->DrawOverlay();
}

// HackerSwapChain::RunFrameActions without what needs a D3D11 swap chain:
// frame analysis
static void frame_actions()
{
	HackerDevice *device = dx12_shim->hacker_device;
	uint64_t ticks = GetSystemTicks();
	bool new_event;

	G->gFrameTime = (ticks - G->gSystemTickCount) / 1'000'000.0f;
	G->gTime = (ticks - G->ticks_at_launch) / 1'000'000.0f;
	G->gSystemTickCount = ticks;
	G->gFPSCounter.Update(ticks);

	if (!dx12_mods_disabled)
		RunCommandList(device, dx12_shim->hacker_context, &G->present_command_list, NULL, false);

	G->analyse_frame = false;

	if (G->hunting == HUNTING_MODE_ENABLED)
		UpdateHuntingBufferSelection(device);

	new_event = DispatchInputEvents(device);

	CurrentTransition.UpdatePresets(device);
	CurrentTransition.UpdateTransitions(device);

	if (G->gReloadConfigPending) {
		ReloadConfig(device);
		Dx12DrawPathConfigReloaded();
		VkModsConfigReloaded();
	}

	if (G->gConfigInitialized) {
		if (G->gTime - G->gSettingsSaveTime > G->gSettingsAutoSaveInterval)
			SavePersistentSettingsInBackground();
	} else if (G->gTime > G->gConfigInitializationDelay) {
		G->gConfigInitialized = true;
		ReloadConfig(device);
		Dx12DrawPathConfigReloaded();
		VkModsConfigReloaded();
	}

	if (!G->suppress_overlay)
		Dx12DrawOverlay();
	G->suppress_overlay = false;

	if (!VkModsCountFrames())
		G->frame_no++;
	dx12_shim->hacker_context->ResetCallCounters();

	if (G->hunting != HUNTING_MODE_ENABLED)
		return;

	if (new_event)
		G->huntTime = time(NULL);
	if (difftime(time(NULL), G->huntTime) > 60) {
		EnterCriticalSectionPretty(&G->mCriticalSection);
		TimeoutHuntingBuffers();
		LeaveCriticalSection(&G->mCriticalSection);
	}
}

void Dx12PresentBegin(Dx12SwapChain *swap_chain)
{
	Dx12ShimLock();
	if (begin(swap_chain)) {
		if (resolution_known)
			dx12_shim->shim_context->RSSetViewports(1, &viewport);
		frame_actions();
		Dx12ShimFramePresented();
		Dx12DrawPathFramePresented();
		submit();
		Dx12FrameDumpFramePresented();
	}
	Dx12ShimUnlock();
}

void Dx12PresentEnd(Dx12SwapChain *swap_chain)
{
	Dx12ShimLock();
	if (dx12_shim && swap_chain == presenting && !dx12_mods_disabled) {
		G->bb_is_upscaling_bb = !!G->SCREEN_UPSCALING && G->upscaling_command_list_using_explicit_bb_flip;
		RunCommandList(dx12_shim->hacker_device, dx12_shim->hacker_context, &G->post_present_command_list, NULL, true);
		Dx12DrawPathEngineRan();
		submit();
	}
	Dx12ShimUnlock();
}
