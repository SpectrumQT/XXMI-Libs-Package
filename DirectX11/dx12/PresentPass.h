#pragma once

#include <d3d11_1.h>

class Dx12SwapChain;

// What HackerSwapChain does around Present for D3D11: the engine's per frame
// work, with the [Present] command list before the frame is presented and the
// post one after. The commands those record are executed on the swap chain's
// queue right away, so they land on the finished frame and at the start of
// the next one like they do on D3D11.
void Dx12PresentBegin(Dx12SwapChain *swap_chain);
void Dx12PresentEnd(Dx12SwapChain *swap_chain);

// The game is about to release the buffers of the swap chain, for a resize or
// for good
void Dx12SwapChainBuffersReleased(Dx12SwapChain *swap_chain, bool destroyed);

// Draws the hunting overlay and the notices to the buffer that will be
// presented next. Needs the shim's lock, which the command lists run under.
// False if it was not drawn, as from the command list of a draw of the game.
bool Dx12DrawOverlay();

// What "bb" is in a command list: the buffer that will be presented next,
// with a reference. NULL if nothing is being presented through us.
ID3D11Resource* Dx12ShimBackBuffer();
