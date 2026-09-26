# Slot Scan

Suppose a shader binds its textures in a different order depending on the material, and we need the slot that holds the `R16G16B16A16_FLOAT` texture. A [dynamic slot index](../resources/slot-indexing.md/#dynamic-slot-index) makes it possible to inspect slots by number, and a recursive command list turns that into a loop over the slots.

```ini
[Constants]
global $slot = -1
global $i = 0

[TextureOverrideMaterial]
hash = 12345678
$slot = -1
$i = 0
run = CommandListFindSlot
if $slot >= 0
    ResourceNormals = ref ps-t[$slot]
endif

[CommandListFindSlot]
if ps-t[$i]->Format == DXGI_FORMAT_R16G16B16A16_FLOAT
    $slot = $i
else
    $i = $i + 1
    if $i < 16
        run = CommandListFindSlot
    endif
endif
```

* `ps-t[$i]->Format` queries the resource bound to slot `$i` and compares it with a [DXGI_FORMAT literal](../expressions/literals.md/#dxgi_format-enum-literals). An empty slot returns an [error value](../resources/resource-metadata.md/#error-values), which never matches.
* `run = CommandListFindSlot` inside its own body recurses until a match is found or `$i` reaches `16`. Each level counts against the [recursion limit](../command-lists/README.md/#recursion-limit) of 256, so a scan over all 128 `t` slots is still within bounds.
* `ResourceNormals = ref ps-t[$slot]` uses the found slot number directly in a resource copy.

The same pattern works with any per-slot metadata, for example `ps-t[$i]->Width` to find the largest texture, or `@ps-t[$i]` to find the slot a known resource is bound to:

```ini
if @ps-t[$i] == @ResourceFoo
    $slot = $i
endif
```

> Slot metadata requires querying the D3D11 resource, roughly 10-100 ns per slot. Cache the result in a variable rather than scanning on every draw when the layout is stable.
