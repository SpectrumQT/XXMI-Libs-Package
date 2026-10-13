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
// The constant buffer a D3D11 shader of the stage would have at the register,
// with what the draw has there for its contents. Not referenced.
ID3D11Buffer* VkScopeConstantBuffer(int stage, UINT slot);
// What the shader of the stage declares, by the same registers. NULL if it
// has none.
struct ShaderBindings;
const ShaderBindings* VkScopeShaderBindings(int stage);
// A draw of a mod in place of or besides the game's, with what the mod has
// bound on the shim context
void VkScopeDraw(bool indexed, UINT count, UINT instances, UINT start, INT base_vertex, UINT start_instance);

// A resource of the shim that is marked as mirrored is going
struct ShimResource;
void VkScopeForgetResource(ShimResource *resource);
// That much of the shadow of a resource that is marked as shadow_pending is
// about to be read. The version of the resource changes if that had the
// shadow written.
void VkScopeFillShadow(ShimResource *resource, UINT64 offset, UINT64 size);

// After the configuration was read again
void VkModsConfigReloaded();

// For GetRegionHash. False if the buffer is not one of the above, and the
// hash is the engine's to work out.
bool VkScopeRegionHash(ID3D11Buffer *buffer, UINT offset, UINT size, uint32_t *hash);
