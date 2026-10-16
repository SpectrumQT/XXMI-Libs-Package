# Original Vulkan texture formats

`ps->srv_original_format(slot)` returns the DXGI format of the original texture
bound by the Vulkan game at the pixel shader's SRV slot. The slot uses the same
numbering as `ps-tN` and `ps->srv_sampled(index)` and can be an INI expression.

```ini
$slot = ps->srv_sampled(1)
if $slot >= 0
    $format = ps->srv_original_format($slot)
    if $format == 83
        ps-t[$slot] = ref ResourceNormalmap
    endif
endif
```

The query ignores textures assigned by command lists. For example, replacing a
game's BC5 normal map with a BC7 DDS leaves the original-format result at `83`
(`DXGI_FORMAT_BC5_UNORM`), while `ps-t[$slot]->format` returns `98`
(`DXGI_FORMAT_BC7_UNORM`) for the replacement. Material-slot detection can use
the original format without depending on which texture overrides ran first.

It returns `-1` outside a Vulkan game draw, for stages other than `ps`, and when
the original slot is missing, out of range, or has no known texture format.
It does not change bindings. Existing `->format` queries continue to describe
the resource currently bound by the game or a mod.

This query requires a DLL containing this extension; INI files using it must be
distributed with a compatible DLL.

## RabbitFX Vulkan integration

In the Vulkan material detector's `[CommandListCandidateVulkan]`, replace
`$vk_format = ps-t[$vk_candidate]->format` with:

```ini
$vk_format = ps->srv_original_format($vk_candidate)
```

The original game's format determines the material slot even if
`EFMIv1\OverrideTextures` has already assigned a mod texture of another format.
Individual mods can keep their existing RabbitFX calls.
