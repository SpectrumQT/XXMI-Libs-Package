#pragma once

#include <stdint.h>

#include <vector>

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
// The same for a buffer the shader reads as a whole, which is what it has at
// a t register. NULL for a texture. Not referenced.
ID3D11ShaderResourceView* VkScopeShaderResource(int stage, UINT slot);
// What the shader of the stage declares, by the same registers. NULL if it
// has none.
struct ShaderBindings;
const ShaderBindings* VkScopeShaderBindings(int stage);
// The vertex input of the draw's pipeline as an input layout would describe
// it, for the engine to make one of that reads the vertices of a mod. Vulkan
// has no semantics: an element of four unsigned integers is taken for
// BLENDINDICES, and the others are ATTRIBUTE with their location for index.
// False if the pipeline could not be made again with another.
bool VkScopeInputElements(std::vector<D3D11_INPUT_ELEMENT_DESC> *elements);
// The layout the engine made of those, which the context has set until a mod
// sets another. Not referenced.
void VkScopeSetGameLayout(ID3D11InputLayout *layout);
ID3D11InputLayout* VkScopeGameLayout();
// The hash the engine keeps that layout by, 0 until one was made for the
// pipeline of the draw: the elements are not put together for every draw of
// a mod to find the layout they make
UINT64 VkScopeLayoutHash();
void VkScopeSetLayoutHash(UINT64 hash);
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
// The resource of one that is marked as upload_pending is about to be read by
// a list of the shim, and is given what the game has there by now. False if
// the contents are not from here.
bool VkScopeUploadShadow(ShimResource *resource);

// After the configuration was read again
void VkModsConfigReloaded();
// At a present of the D3D12 device, with the lock of dx12/Bridge.h held: true
// if the frames of the engine are counted by what the game submits, not by
// the presents
bool VkModsCountFrames();

// For GetRegionHash. False if the buffer is not one of the above, and the
// hash is the engine's to work out.
bool VkScopeRegionHash(ID3D11Buffer *buffer, UINT offset, UINT size, uint32_t *hash);
