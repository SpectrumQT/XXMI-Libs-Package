# Accessory Mesh

Suppose we want to attach an extra mesh, such as a bag or a piece of jewelry, to a character without replacing any of the character's own meshes. The accessory should be skinned by the game like the rest of the body, and it should be reusable: the same accessory files can be dropped into a mod for any character.

This example targets the common two-pass character pipeline used by games like ZZZ:

1. A **skinning pass** draws the raw vertex buffers (position, blend weights) through a vertex shader that writes the skinned result to an output vertex buffer via stream output.
2. A **render pass** draws the skinned output buffer with the character's index buffers and textures.

The accessory is pushed through the same two passes: its raw vertices are skinned right after the character's, so they land in the output buffer after the original vertices, and the render pass then draws them with the accessory's own index buffer. [`->Region`](../resources/resource-regions.md) is what makes the second step possible.

```ini
[Constants]
; Character body: 8831 vertices, 40 bytes each in the skinned output buffer.
global locked $body_vertices = 8831
; Accessory: 1416 vertices, drawn with 4608 indices.
global locked $accessory_vertices = 1416
global locked $accessory_indices = 4608

global locked $byte_offset = $body_vertices * 40
global locked $byte_size = $accessory_vertices * 40

; 1. Enlarge the skinned output buffer so it can hold both meshes.
[TextureOverride Body Draw]
hash = a28a907a
override_vertex_count = 10247 ; $body_vertices + $accessory_vertices
override_byte_stride = 40

; 2. Every draw that uses the body's blend buffer goes through here: the skinning
;    draw is handled inline, indexed render draws are forwarded to the IB sections.
[TextureOverride Body Blend]
hash = 7c85654d
handling = skip
if DRAW_TYPE == 2 || DRAW_TYPE == 4
    ; Render draw: let the matching IB section issue it.
    CheckTextureOverride = ib
elif DRAW_TYPE == 1
    ; Skinning draw: the body, then the accessory right after it.
    Draw = $body_vertices, 0

    vb0 = Resource.Accessory.Position
    vb2 = Resource.Accessory.Blend
    Draw = $accessory_vertices, 0
endif

; 3. Render pass: draw the body, then draw the accessory from the tail of the skinned buffers.
[TextureOverride BodyA]
hash = 046400d3
match_first_index = 0
match_index_count = 38595

Resource\ZZMI\Diffuse = ref Resource.Diffuse.Body
Resource\ZZMI\NormalMap = ref Resource.NormalMap.Body
Resource\ZZMI\LightMap = ref Resource.LightMap.Body
Resource\ZZMI\MaterialMap = ref Resource.MaterialMap.Body
run = CommandList\ZZMI\SetTextures

DrawIndexed = 38595, 0, 0

vb0 = vb0->Region($byte_offset, $byte_size) ; Skinned positions of the accessory.
vb1 = Resource.Accessory.Texcoord
vb2 = Resource.Accessory.Blend
vb3 = vb3->Region($byte_offset, $byte_size) ; Same for the second skinned buffer (previous frame).
ib = Resource.Accessory.IB

DrawIndexed = $accessory_indices, 0, 0

[Resource.Accessory.Position]
type = Buffer
stride = 40
filename = AccessoryPosition.buf
[Resource.Accessory.Blend]
type = Buffer
stride = 32
filename = AccessoryBlend.buf
[Resource.Accessory.Texcoord]
type = Buffer
stride = 20
filename = AccessoryTexcoord.buf
[Resource.Accessory.IB]
type = Buffer
format = DXGI_FORMAT_R32_UINT
filename = Accessory.ib

[Resource.Diffuse.Body]
filename = Diffuse.Body.dds
[Resource.Diffuse.Hair]
filename = Diffuse.Hair.dds
[Resource.NormalMap.Body]
filename = NormalMap.dds
[Resource.LightMap.Body]
filename = LightMap.dds
[Resource.MaterialMap.Body]
filename = MaterialMap.dds
```

## How It Works

**Enlarging the output buffer.** `override_vertex_count` and `override_byte_stride` make the skinned output buffer `10247 × 40` bytes instead of `8831 × 40`, so there is room for the accessory's vertices after the body's.

**Dispatching the body's draws.** The `Body Blend` section matches the body's blend vertex buffer, which is bound for the skinning draw *and* for every render draw of the body. With `handling = skip` none of them is issued by the game any more; the section decides per `DRAW_TYPE` what happens instead:

* `DRAW_TYPE == 1` is the non-indexed skinning draw, handled inline.
* `DRAW_TYPE == 2` or `4` is an indexed render draw. `CheckTextureOverride = ib` runs the `TextureOverride` section that matches the bound index buffer and the draw's index range (`match_first_index` / `match_index_count`), `BodyA` here. A `TextureOverride`'s command list never runs just because its resource got bound; it only runs when a `CheckTextureOverride` names its slot, which is what makes the Blend section the dispatcher. And since the dispatcher skipped the game's call, the IB section has to issue the draw itself.

**Skinning the accessory.** For the skinning draw the section issues the game's original `Draw` for the body, then binds the accessory's raw position and blend buffers to the same slots and draws its vertices. The game's vertex shader skins them exactly like the body, using the same bones. Stream output appends, so the accessory's skinned vertices are written right after the body's in the output buffer.

**Drawing the accessory.** `BodyA` sets the body's textures and issues the body draw with `DrawIndexed = 38595, 0, 0`, replacing the call the dispatcher skipped. Then:

* `vb0 = vb0->Region($byte_offset, $byte_size)` rebinds the skinned buffer starting at vertex `8831`, so the accessory's index buffer, whose indices start at `0`, addresses the accessory's vertices. No data is copied, it is the same buffer with a different start offset.
* `vb1`, `vb2` and `ib` are the accessory's own buffers, which are not shared with the body and therefore start at `0`.
* `vb3` is rebound the same way as `vb0`. Games that keep a second skinned buffer per character, such as the previous frame's positions for motion vectors, need every skinned buffer shifted by the same amount. Leaving `vb3` at its original binding pairs the accessory's current positions with the body's previous positions, which **will break temporal effects** such as TAA and motion blur: the accessory smears, ghosts or jitters even though it renders correctly in a still frame.

`DrawIndexed`'s third argument (`BaseVertexLocation`) could also shift vertex `0` to `8831`, but it applies to **every** vertex buffer of the draw, so the accessory's own texcoord and blend buffers would need `8831` dummy entries in front of their data. `->Region` shifts only the buffers that are actually shared.

## Reusing the Accessory

The accessory files and its three `[Resource]` sections are independent of the character. To attach the same accessory to another character, only the numbers at the top change: the character's vertex count, the stride of its skinned buffer, and the hashes of its skinning and render draws. Keeping them in `locked` globals makes that a three-line edit.

To render a component with another component's material instead of adding a mesh, see [Material Borrowing](material-borrowing.md).

> `->Region` is marked experimental in [Resource Regions](../resources/resource-regions.md). Vertex buffer regions with a whole-vertex offset, as used here, work as described; regions that do not start on a vertex boundary are not meaningful for `vb` slots.
