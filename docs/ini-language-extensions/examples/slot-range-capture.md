# Slot Range Capture

Suppose a custom pixel shader needs every texture the game bound for a character draw, plus one texture of our own, and the original bindings have to be restored afterwards. With [slot ranges](../resources/slot-indexing.md/#slot-ranges) the whole set of slots is saved, rebound and restored with one operation each.

```ini
[PoolTextures]
pool_size = 8 ; ps-t0 to ps-t7

[ResourceMask]
filename = Mask.dds

[TextureOverrideCharacter]
hash = 12345678
run = CommandListCharacter

[CommandListCharacter]
; Save the game's ps-t0-ps-t7 bindings by reference (cheap, no copy).
PoolTextures[0:7] = ref ps-t[0:7]

; Bind our own texture on top and draw with a custom shader.
ps-t8 = ref ResourceMask
run = CustomShaderCharacter

; Restore the original bindings and drop our texture.
ps-t[0:7] = ref PoolTextures[0:7]
ps-t8 = null

[CustomShaderCharacter]
ps = Character.hlsl
handling = skip
draw = from_caller
```

* `PoolTextures[0:7] = ref ps-t[0:7]` fetches eight slots with a single `PSGetShaderResources` call. Since the copy type is `ref`, the pool elements only point at the game's textures.
* `ps-t[0:7] = ref PoolTextures[0:7]` binds them back with a single `PSSetShaderResources` call. Pool elements that were fetched from a slot already hold a view of the right type, so no new views are created.

## Skipping Empty Slots

If the game leaves some of the slots empty, `unless_null` keeps the current binding for those instead of unbinding them, and leaves the corresponding pool elements untouched on fetch:

```ini
PoolTextures[0:7] = ref unless_null ps-t[0:7] ; Elements of empty slots are not modified.
ps-t[0:7] = ref unless_null PoolTextures[0:7] ; Empty pool elements do not unbind their slot.
```

## Inheriting Bounds

When one side has bounds, the other side may omit them. This is handy when the pool size defines the range:

```ini
ps-t = ref PoolTextures[0:7] ; Same as ps-t[0:7] = ref PoolTextures[0:7]
PoolTextures = ref ps-t[0:7] ; Same as PoolTextures[0:7] = ref ps-t[0:7]
```

The bounds can also be computed, for example from the number of textures the character actually uses:

```ini
$last = $texture_count - 1
PoolTextures[0:$last] = ref ps-t[0:$last]
```

An invalid range, such as `$last` below `0`, logs a warning and skips the operation instead of binding garbage.

## Constant Buffers

The same works for constant buffers of any stage. A common use is passing the vertex shader's constant buffers to a compute shader unchanged:

```ini
[PoolConstants]
pool_size = 4

[CommandListSkinning]
PoolConstants[0:3] = ref vs-cb[0:3]
cs-cb[0:3] = ref PoolConstants[0:3]
run = CustomShaderSkinning
cs-cb[0:3] = null
```

> `ref` is the fast path for ranges. Adding `copy` or other options such as `raw` makes every slot go through a regular single-slot copy, and only the final bind call is shared. See [Copy Options](../resources/slot-indexing.md/#copy-options).
