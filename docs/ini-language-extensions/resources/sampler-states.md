# Sampler States

**XXMI DLL** lets a `[Resource]` section declare a sampler state, and binds it to the sampler slots (`s0`–`s15`) of any shader stage with `<stage>-s<slot>` lines. Custom shaders can then call `Sample()`, `SampleLevel()` and the like with the filtering and addressing they need.

Sampler bindings take effect in **Direct3D 11** games only. In Direct3D 12 and Vulkan games they are ignored, see [Direct3D 12 and Vulkan](#direct3d-12-and-vulkan).

## Declaration

### `type = SamplerState`

```ini
[ResourceLinearClamp]
type = SamplerState
filter = linear
address = clamp
```

| Key | Values | Default |
| --- | --- | --- |
| `filter` | `point`, `linear`, `anisotropic` | `linear` |
| `address` | `wrap`, `mirror`, `clamp`, `border`, `mirror_once` — for U, V and W | `clamp` |
| `address_u`, `address_v`, `address_w` | as `address`, for one coordinate | `address` |
| `max_anisotropy` | `1`–`16` | `16` with `filter = anisotropic`, else `1` |
| `mip_lod_bias` | number | `0` |
| `min_lod` | number | `0` |
| `max_lod` | number | `3.402823466e+38` (`D3D11_FLOAT32_MAX`, every mip level) |
| `comparison` | `never`, `less`, `equal`, `less_equal`, `greater`, `not_equal`, `greater_equal`, `always` | not set |
| `border_color` | four numbers, `r g b a` | `0 0 0 0` |

`filter` applies to minification, magnification and mip level selection alike (`point` is `D3D11_FILTER_MIN_MAG_MIP_POINT`, `linear` is `D3D11_FILTER_MIN_MAG_MIP_LINEAR`).

The sampler is a regular one unless `comparison` is given. With `comparison`, the filter becomes its `COMPARISON_` variant for `SampleCmp()` and `SampleCmpLevelZero()`, and the value is the comparison function.

A sampler state section takes no `filename`, `format`, size or `data` keys. It is only for sampler slots: assigning it to a texture slot or another resource (`ps-t0 = ResourceLinearClamp`, `ResourceCopy = ref ResourceLinearClamp`) is warned about when the ini is loaded and the line is ignored. `type = SamplerState` is not supported in [pool](../pools/declaration.md) (`[Pool...]`) sections.

The D3D11 sampler object is created the first time the resource is bound, for the device that binds it, and is made again after the ini is reloaded.

## Binding

### `<stage>-s<slot> = <resource>`

```ini
ps-s0 = ResourceLinearClamp
cs-s1 = ref ResourceLinearClamp
vs-s2 = ResourcePointWrap
ps-s0 = null
```

`<stage>` is one of `vs`, `hs`, `ds`, `gs`, `ps` or `cs`; `<slot>` goes from `0` to `15`. The value is a `type = SamplerState` resource, or `null` to unbind the slot. An optional `ref` is accepted; `copy` and the other resource copy options are not.

The lines can go anywhere a `ps-t0 = ...` line can: `[CustomShader]`, `[ShaderOverride]`, `[TextureOverride]`, `[CommandList]`, `[Present]` and so on, including `pre` / `post` and `if` blocks.

These lines are warned about when the ini is loaded, and ignored:

* a slot past `15` (`ps-s16 = ...`);
* a value that is not a `type = SamplerState` resource or `null`, such as a texture resource or a pool element;
* options other than `ref` (`ps-s0 = copy ResourceLinearClamp`);
* reading a sampler slot of the game (`ResourceFoo = ref ps-s0`). Sampler slots can only be bound over.

### In `[CustomShader]` sections

The sampler slots that the lines of a `[CustomShader]` section bind (its `post` lines and `if` blocks included) are put back the way the game had them once the section has run, like its shaders and blend, depth and rasterizer states. Only the slots the section names are saved and restored, so sections without sampler lines cost nothing extra.

Slots bound by a command list the section calls (`run = CommandListFoo`) are not tracked: bind the samplers in the custom shader section itself.

```ini
[CustomShaderRefraction]
ps = Refraction.hlsl
ps-t0 = ResourceSceneColor
ps-s0 = ResourceLinearClamp
ps-s1 = ResourcePointClamp
draw = 3, 0
; ps-s0 and ps-s1 have the game's samplers again from here on
```

Outside of `[CustomShader]` sections a sampler stays bound until something else binds that slot, like any other slot assignment.

### Legacy `sampler`

The `sampler = point_filter | linear_filter | anisotropic_filter` key of `[CustomShader]` sections still works as before: it binds a comparison sampler with `MaxLOD = 1` to `ps-s0` and restores `ps-s0`–`ps-s15` afterwards. If a section has both, a `ps-s0 = ...` line wins, as the command list runs after `sampler` was applied.

## Direct3D 12 and Vulkan

In Direct3D 12 and Vulkan games the `<stage>-s<slot>` lines are parsed and checked as above but do nothing when they run. The first such line run is noted once: in `d3d11_log.txt` with logging enabled (`log_level = info` or `debug`), and in `dx12_report.txt` with `dx12_dump_hashes = 1` in `[Hunting]`. The legacy `sampler` key behaves as before.

## Detecting Support

[`DLL_SAMPLER_SLOTS`](../parameters/README.md#dll_sampler_slots) is `1` when the DLL binds sampler states for the running game, and `0` in Direct3D 12 and Vulkan games.

DLLs without this feature do not know the identifier, so an expression that names it cannot be parsed: the line is dropped with an `Unrecognised entry` warning when the ini is loaded. Using it directly in an `if` would leave that block's `else` / `endif` without their `if`, so copy it into a variable instead, from a line that can be lost safely, and choose between two custom shaders with that:

```ini
[Constants]
global $sampler_slots = 0

[Present]
; Dropped by DLLs without sampler slots, which leaves $sampler_slots at 0
$sampler_slots = dll_sampler_slots

[CommandListFluid]
if $sampler_slots
    run = CustomShaderFluidSampled
else
    run = CustomShaderFluidLoad
endif

[CustomShaderFluidSampled]
ps = FluidSampled.hlsl
ps-s0 = ResourceLinearClamp
draw = 3, 0

[CustomShaderFluidLoad]
ps = FluidLoad.hlsl
draw = 3, 0
```

The variable is set in `[Present]` rather than `[Constants]` so that it is evaluated once the game's API is known.

Loading still logs a warning for every `type = SamplerState` and `<stage>-s<slot>` line on such DLLs (they are ignored there). A launcher that knows the DLL version can avoid the warnings by only installing the ini for versions with this feature.
