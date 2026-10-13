#pragma once

#include <stdint.h>

#include <d3d11.h>

// What the engine is answered while it runs for a draw of a Vulkan game, see
// VkDrawPath.h. Asked through dx12/DrawPath, which the shim context goes to
// for whatever the mods have not bound themselves, with the lock of
// dx12/Bridge.h held.

bool VkInDrawScope();
// stage is a Dx12ShaderStage
bool VkScopeHasShader(int stage);
// The object stands for the index buffer as the draw has it bound: its
// contents start at the offset the game bound the buffer with. Not referenced.
ID3D11Buffer* VkScopeIndexBuffer(DXGI_FORMAT *format);
// A draw of a mod in place of or besides the game's
void VkScopeDraw();

// After the configuration was read again
void VkModsConfigReloaded();

// For GetRegionHash. False if the buffer is not one of the above, and the
// hash is the engine's to work out.
bool VkScopeRegionHash(ID3D11Buffer *buffer, UINT offset, UINT size, uint32_t *hash);
