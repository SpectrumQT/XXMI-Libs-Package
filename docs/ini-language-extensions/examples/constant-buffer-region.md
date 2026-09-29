# Constant Buffer Region

Suppose the game packs the data of many objects into one large constant buffer and binds a window of it per draw with `VSSetConstantBuffers1`. Our compute shader needs only the bone matrices of the current object, which sit `256` bytes into that window and take `3072` bytes. [Resource regions](../resources/resource-regions.md) allow to reference exactly that part of the buffer.

```ini
[TextureOverrideCharacter]
hash = 12345678
run = CommandListSkinning

[CommandListSkinning]
; Absolute byte offset of the window the game bound for this draw.
$base = vs-cb1->Offset

; Reference the bone matrices only, no data is copied.
cs-cb0 = ref vs-cb1->Region($base + 256, 3072)

run = CustomShaderSkinning
cs-cb0 = null
```

* [`vs-cb1->Offset`](../resources/resource-metadata.md/#offset) returns the byte offset of the region currently visible to the vertex shader, derived from the `FirstConstant` the game passed.
* `->Region($byte_offset, $byte_size)` takes an absolute byte offset into the underlying buffer, so the visible offset is added to the local offset of the bone data.
* `ref` binds the same D3D11 buffer with a new region, and `cs-cb0` sees the data as if the region started at register `c0`. D3D11 requires constant buffer regions to start and end on 256-byte boundaries (multiples of 16 shader constants), which `256` and `3072` satisfy.

## Copying a Region

To keep the data past this draw, for example to compare bone matrices between frames, copy the region into a custom resource instead:

```ini
[ResourceBones]
type = Buffer

[CommandListSkinning]
$base = vs-cb1->Offset
ResourceBones = copy vs-cb1->Region($base + 256, 3072)
```

Only `3072` bytes are copied. `ResourceBones->Size` reports `3072` afterwards, and [`->Offset`](../resources/resource-metadata.md/#offset) of the copy is `0`.

## Checking the Window Size

Some games bind the whole buffer for small objects and a window for large ones. [`->Size`](../resources/resource-metadata.md/#size) of the slot reports the size of the visible region, so the script can tell them apart before picking offsets:

```ini
if vs-cb1->Size == 4096
    ; Whole per-object block is visible.
    cs-cb0 = ref vs-cb1
else
    cs-cb0 = ref vs-cb1->Region(vs-cb1->Offset + 256, 3072)
endif
```
