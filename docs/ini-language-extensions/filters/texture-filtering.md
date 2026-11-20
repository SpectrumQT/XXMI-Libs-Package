# Texture Filtering

Filter usage allows to replace textures in pixel-shader resource slots (`ps-t`) based on visual similarity. Instead of relying on a fixed slot index or resource hash, a **Filter** identifies a texture by its dHash, a compact visual fingerprint.

This is useful when the same texture appears in different `ps-t` slots, or when its CRC32 hash changes between mip levels or game resource quality tiers.

For the **[Filter]** section syntax and available properties, see the [Filter overview](README.md).

## Filter-Based Texture Slot Overriding

Assign a replacement Resource to a filtered pixel-shader texture slots assignment:

```ini
[CommandListExample]
ps-t[FilterTexture] = ref ResourceTexture
drawindexed = 1000, 0, 0

[FilterTexture]
match_format = BC7_UNORM
match_dhash = 8cb4796369a42c84

[ResourceTexture]
filename = Texture.dds
```

This searches the eligible `ps-t` slots declared by the currently bound pixel shader for textures matching `FilterTexture`, then binds `ResourceTexture` to the slot or slots with the best qualifying match. If `match_format` is specified, candidates with a different format are excluded before the dHash comparison.

> DHashes are included into texture names of extracted objects by **EFMI Tools** and **WWMI Tools** as `d=` and are sourced from `log.txt` of frame dumps made by **XXMI DLL v1.3.2+**.

> Filter-based slot matching currently supports pixel-shader resource slots (`ps-t`) only.

For the underlying dHash comparison, Hamming-distance calculation, and confidence-to-distance conversion, see [Texture Filtering - Advanced](texture-filtering-advanced.md).

### Texture Similarity and Confidence

Use `match_confidence` to control how closely a candidate texture must resemble the Filter's reference texture:

```ini
[FilterTexture]
match_format = BC7_UNORM
match_dhash = 8cb4796369a42c84
match_confidence = 0.90
```

When omitted, `match_confidence` defaults to `0.75` (75%). Higher values require a closer visual match; lower values allow more variation.

- Increase the confidence when visually similar texture states are being confused with one another.
- Lower the confidence only when the intended texture is not being matched.

For example, wet and dry versions of a material may look similar. A stricter threshold can help prevent one state from being mistaken for the other. If a stricter value stops the intended texture from matching, check the reference dHash and format before lowering the threshold.

### Conditional Texture Switching

Use ordinary INI flow control to choose between replacement Resources while keeping the same Filter:

```ini
if $toggle_a == 1
    ps-t[FilterTexture] = ref ResourceTexture_A
else
    ps-t[FilterTexture] = ref ResourceTexture_B
endif
```

* Both branches use `FilterTexture` to identify the original texture.
* The condition determines which replacement Resource is bound.

### Replacing Multiple Textures

A modded draw often needs several textures replaced, such as diffuse, normal, and light maps.

Assign a separate Filter to each replacement:

```ini
ps-t[FilterDiffuse] = ref ResourceDiffuse
ps-t[FilterNormalMap] = ref ResourceNormalMap
drawindexed = 1000, 0, 0

[ResourceDiffuse]
filename = Diffuse.dds

[FilterDiffuse]
match_format = BC7_UNORM
match_dhash = 8cb4796369a42c84

[ResourceNormalMap]
filename = NormalMap.dds

[FilterNormalMap]
match_format = BC5_UNORM
match_dhash = 56577a2d6a2759a8
```

Each Filter is matched against the initial texture snapshot, rather than against a texture installed by an earlier Filter-based assignment. This gives all assignments a consistent baseline.

If multiple Filter assignments target the same slot, a later assignment replaces the current match only when it finds a better match. A weaker match therefore does not overwrite a stronger one.

### Capturing the Initial Texture Snapshot

The **initial texture snapshot** is the set of `ps-t` bindings saved at a particular point of time. “Initial” is relative to that capture point; it does not necessarily mean the textures originally bound by the game.

If no snapshot has been captured explicitly, the toolkit captures one automatically immediately before processing the first Filter-based assignment (`ps-t[FilterTexture] =`). Any texture changes made before that first assignment are therefore included in the automatic snapshot.

Use `run = SnapshotInitialTextures` to choose exactly when the snapshot is captured:

```ini
; Save the current texture bindings before changing any slots.
run = SnapshotInitialTextures

; Override a slot directly.
ps-t0 = ref ResourceFallback

; Filter matching still uses the snapshot captured above.
ps-t[FilterDiffuse] = ref ResourceDiffuse
ps-t[FilterNormalMap] = ref ResourceNormalMap
drawindexed = 1000, 0, 0
```

A snapshot can be captured only once for a given draw call. Further `SnapshotInitialTextures` calls during that draw call are ignored.

Some model importers and mod INI templates manage snapshot capture automatically. Check their command lists before adding an explicit snapshot:

- **EFMI** captures a snapshot immediately before calling `[CommandList\EFMIv1\Callback_Component_DrawCustom]` for each instance of an instanced draw.
- **WWMI Tools**' latest `mod.ini` template captures a snapshot in `[CommandListTriggerResourceOverrides]`.

### Restoring the Initial Texture Snapshot

Use `run = RestoreInitialTextures` to restore the `ps-t` bindings saved in the snapshot. This is useful when a game reuses texture bindings between draws, as in Wuthering Waves, or uses instanced draws, as in Arknights: Endfield, and you want replacements to affect only selected draws.

```ini
run = SnapshotInitialTextures

ps-t[FilterDiffuse] = ref ResourceDiffuse
ps-t[FilterNormalMap] = ref ResourceNormalMap

; Draw using the replacement textures.
drawindexed = 1000, 0, 0

run = RestoreInitialTextures

; Draw again using the snapshotted textures.
drawindexed = 1000, 1000, 0
```

A captured snapshot can be restored multiple times. Restoring it also resets the per-slot best-match tracking, allowing subsequent Filter assignments to be evaluated against the restored bindings.

Some model importers and mod INI templates perform restoration automatically. Check their cleanup flow before adding an explicit restore:

- **EFMI** restores the snapshot after calling `[CommandList\EFMIv1\Callback_Component_DrawCustom]` for each instance of an instanced draw.
- **WWMI Tools**' latest `mod.ini` template restores the snapshot in `[CommandListCleanupSharedResources]`.
