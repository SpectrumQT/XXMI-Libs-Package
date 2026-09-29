# Slot Range Capture

A mod shares its shaders with the rest of the game, so a texture slot it overrides for one draw call stays overridden for every later draw that reaches the same shader. The usual fix is to capture the game's bindings before the override and put them back after the draw, with one line per slot: [ZZMI's SlotFix](https://github.com/leotorrez/ZZMI-Package/tree/main/ZZMI/Core/ZZMI/Libraries/SlotFix) spends 38 lines capturing and restoring `ps-t0` to `ps-t18` that way. With [slot ranges](../resources/slot-indexing.md/#slot-ranges) each direction is one operation, whatever the slot count.

```ini
[Poolt]
pool_size = 3 ; ps-t3 to ps-t5

[TextureOverrideCharacter]
hash = 12345678

; Capture: one PSGetShaderResources for the slots this mod overrides.
Poolt[0:2] = ref ps-t[3:5]

ps-t3 = ref ResourceDiffuse
ps-t4 = ref ResourceNormalMap
ps-t5 = ref ResourceLightMap

; Release after the draw: one PSSetShaderResources.
post ps-t[3:5] = ref Poolt[0:2]
```

* `ref` is what makes the capture cheap: the pool elements only point at the game's textures, nothing is copied.
* The release creates no views either, because a pool element that was fetched from a slot already holds a view of the right type.
* Each side states its own bounds and the two only have to match in size, so the pool holds just the overridden slots instead of mirroring their slot numbers.
* The release must not use `unless_null`. A slot the game left empty is captured as an empty element, and restoring that element has to unbind the slot again — `unless_null` would leave the mod's texture bound instead.

## Capturing Once for Many Draws

When the capture and the release sit in different command lists, as they do when the mod framework restores the slots at the end of the frame, the capture needs a guard. Without it a second draw would capture the mod's own textures over the originals:

```ini
[Constants]
global $captured = 0

[CommandListCaptureSlots]
if !$captured
    Poolt[0:2] = ref ps-t[3:5]
    $captured = 1
endif

[CommandListReleaseSlots]
if $captured
    ps-t[3:5] = ref Poolt[0:2]
    Poolt[*] = null
    $captured = 0
endif
```

> `ref` is the fast path for ranges. Adding `copy` or other options such as `raw` makes every slot go through a regular single-slot copy, and only the final bind call is shared. See [Copy Options](../resources/slot-indexing.md/#copy-options).
