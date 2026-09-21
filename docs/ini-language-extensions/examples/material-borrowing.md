# Material Borrowing

Suppose a character's body should be rendered with the material of another component, for example the hair shader with its own lighting model, while keeping the body's bones and weights. The game draws each component with its own render call, so the body has to be drawn again from inside the hair's render call, using the body's already skinned vertices.

This builds on the two-pass pipeline from [Accessory Mesh](accessory-mesh.md): a skinning pass writes the posed mesh through stream output, and a render pass draws it. Two pieces are needed:

* The posed body mesh, captured from the stream output slot `so0` of the body's skinning pass.
* The **previous** frame's posed mesh, since the render vertex shader takes it as a second vertex buffer (`vb3`) for motion vectors. A [Ring pool](../pools/indexing.md/#ring-indexing) indexed by `FRAME_NUMBER` keeps the last two captures.

```ini
; Posed body mesh of the current and the previous frame.
[PoolPosed]
pool_size = 2

; 1. Body dispatcher: skin the body and capture the posed mesh,
;    swallow the body's own render draws.
[TextureOverride Body Blend]
hash = 7c85654d
handling = skip
if DRAW_TYPE == 2 || DRAW_TYPE == 4
    ; Render draw: nothing to do, the body is drawn from the hair's section.
elif DRAW_TYPE == 1
    vb0 = Resource.Body.Position
    vb2 = Resource.Body.Blend
    Draw = 8831, 0
    PoolPosed[FRAME_NUMBER] = ref so0 ; Stream output target of this frame's skinning.
endif

[TextureOverride Body Draw]
hash = a28a907a
override_vertex_count = 8831
override_byte_stride = 40

; 3. Hair dispatcher, same structure as the body's.
[TextureOverride Hair Blend]
hash = 1b2c3d4e
handling = skip
if DRAW_TYPE == 2 || DRAW_TYPE == 4
    CheckTextureOverride = ib
elif DRAW_TYPE == 1
    Draw = 8050, 0
endif

[TextureOverride Hair Draw]
hash = 5bbaca72
override_vertex_count = 8050
override_byte_stride = 40

; 4. Hair render pass: draw the hair, then draw the posed body with the hair's shader.
[TextureOverride HairA]
hash = 468e532f
match_first_index = 0
match_index_count = 30948

DrawIndexed = 30948, 0, 0

Resource\ZZMI\Diffuse = ref Resource.Diffuse.Body
Resource\ZZMI\NormalMap = ref Resource.NormalMap.Body
Resource\ZZMI\LightMap = ref Resource.LightMap.Body
Resource\ZZMI\MaterialMap = ref Resource.MaterialMap.Body
run = CommandList\ZZMI\SetTextures

vb0 = ref PoolPosed[FRAME_NUMBER]     ; Posed body, this frame.
vb1 = Resource.Body.Texcoord
vb2 = Resource.Body.Blend
vb3 = ref PoolPosed[FRAME_NUMBER - 1] ; Posed body, previous frame.
ib = Resource.Body.IB

DrawIndexed = 38595, 0, 0

[TextureOverride HairB]
hash = 468e532f
match_first_index = 30948
match_index_count = 636
DrawIndexed = 636, 30948, 0

[Resource.Body.Position]
type = Buffer
stride = 40
filename = BodyPosition.buf
[Resource.Body.Blend]
type = Buffer
stride = 32
filename = BodyBlend.buf
[Resource.Body.Texcoord]
type = Buffer
stride = 20
filename = BodyTexcoord.buf
[Resource.Body.IB]
type = Buffer
format = DXGI_FORMAT_R32_UINT
filename = BodyA.ib

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

**Dispatching.** Both `Blend` sections match their component's blend vertex buffer, which is bound for the skinning draw *and* for every render draw of that component, and skip all of them with `handling = skip`. They then decide per `DRAW_TYPE` what to issue instead, as in [Accessory Mesh](accessory-mesh.md):

* `DRAW_TYPE == 1` is the skinning draw. The mod's own position and blend buffers are bound and the draw is issued with the mod's vertex count (`Draw = 8831, 0` for the body, `Draw = 8050, 0` for the hair). `Body Draw` and `Hair Draw` size the skinned output buffers accordingly.
* `DRAW_TYPE == 2` or `4` is an indexed render draw. The hair dispatcher forwards it with `CheckTextureOverride = ib` to the section matching the bound index buffer, `HairA` or `HairB`, which is responsible for drawing. The body dispatcher does **not** forward and issues nothing: since the game's call is already skipped, an empty branch means the body is not rendered with its own material at all.

**Capturing the posed mesh.** At the body's skinning draw the game has the buffer that receives the skinned vertices bound to stream output slot `so0`. `PoolPosed[FRAME_NUMBER] = ref so0` stores a reference to that buffer in the pool element of the current frame. With `pool_size = 2`, `PoolPosed[FRAME_NUMBER]` is always this frame's capture and `PoolPosed[FRAME_NUMBER - 1]` the previous frame's, as in [Pool-Based Tracker](pool-based-tracker.md).

**Redrawing under the hair material.** `HairA` is run by the hair dispatcher for the hair's first render draw. It issues the hair draw itself with `DrawIndexed = 30948, 0, 0`, replacing the call the dispatcher skipped, and then:

* The body's textures are set through the same `SetTextures` command list, so the hair shader samples the body's maps.
* `vb0` and `vb3` are the posed body meshes of this and the previous frame. `vb1`, `vb2` and `ib` are the body's static buffers.
* `DrawIndexed = 38595, 0, 0` issues the body draw with the hair's shaders, render state and constant buffers still bound.

`HairB` handles the hair's second index range and only issues its own draw.

> Everything happens in the sections' main (pre) command lists. `post` commands are not used on purpose: depending on how the draw is handled, a `TextureOverride`'s post list is not guaranteed to run, so ordering is expressed with `handling = skip` in the dispatchers and explicit draws in the IB sections instead.

## Reference or Copy

`ref` is enough when the game alternates between two stream output buffers, so that the buffer captured in the previous frame is not overwritten by the current skinning pass. Whether that is the case can be checked with resource identities:

```ini
if @PoolPosed[FRAME_NUMBER] == @PoolPosed[FRAME_NUMBER - 1]
    ; Same buffer both frames: the previous frame's data is already gone.
endif
```

If the game reuses a single buffer, capture with `copy` instead. It costs a full buffer copy per frame, but keeps the previous frame's positions intact:

```ini
PoolPosed[FRAME_NUMBER] = copy so0
```

## Caveats

This is not a general-purpose technique and should be verified in motion, not just in a still frame:

* **Draw order and scene state.** The body is now drawn at the hair's point in the frame, with whatever depth, stencil, blend and lighting state the hair draw runs under. Components that rely on an earlier or later position in the frame, for example for outlines or transparency sorting, may render incorrectly.
* **Armature pivot.** The posed vertices are in the body's object space. The hair's vertex shader applies the hair's object transform, so the two components must share the same armature pivot, otherwise the body ends up offset or rotated.
* **Vertex layout.** The hair's vertex shader must expect the same input layout as the body's, since the body's buffers are fed to it unchanged. [Input layout overrides](../input-layouts/README.md) can bridge small differences such as a format mismatch of one element.
* **Temporal effects.** As in [Accessory Mesh](accessory-mesh.md), `vb3` has to hold the previous frame's positions of the same mesh. Binding anything else there breaks TAA and motion blur for the body.
