# INI Runtime Parameters

INI expressions provide access to runtime parameters that are evaluated at the time the expression is executed.

## `FRAME_NUMBER`

Returns the number of the current frame.

```ini
$frame_id = FRAME_NUMBER
```

The frame boundary is defined by the `Present` call.

## `DRAW_NUMBER`

Returns the number of the current draw call within the frame.

```ini
$draw_id = DRAW_NUMBER
```

The counter starts at `1` for each new frame.

## `DISPATCH_NUMBER`

Returns the number of the current compute dispatch within the frame.

```ini
$dispatch_id = DISPATCH_NUMBER
```

The counter starts at `1` for each new frame.

## `TIME`

Returns a number of seconds passed since the game launch. Has microsecond precision (unlike 10-16 ms of original 3dmigoto).

```ini
$run_time = TIME
```

## `FRAME_TIME`

Returns a number of seconds passed between the previous and the current frame. Has microsecond precision.

```ini
$frame_time = FRAME_TIME
```

## `FPS`

Returns average FPS calculated by exponential moving average based counter.

```ini
$fps = FPS
```

## `DX12`

Returns `1` when the game renders with Direct3D 12 and `0` under Direct3D 11.

```ini
if ps == $shader_id && DX12
    ; The game's shaders could not be patched: bind to the registers they read
    ps-t0 = ref ResourceDiffuse
else if ps == $shader_id
    ps-t60 = ref ResourceDiffuse
endif
```

A `[ShaderRegex]` pattern cannot match or rewrite the DXIL shaders of a Direct3D 12 game. A group is linked to such a shader (its command list and `filter_index` apply) if it names a shader model of the shader's type and either has no pattern or states what the shader has to declare:

```ini
[ShaderRegexMain]
shader_model = ps_4_0 ps_5_0
filter_index = 1718.1
; Registers of space 0 the shader declares
dx12_match_registers = t0 t1 b2
; Render targets a pixel shader writes
dx12_match_render_targets = >=4
; Bytes of the constant buffer at b0 to b15
dx12_match_cb1_size = 64
```

The values of a game's shaders are listed in `dx12_shaders.csv`, written with `dx12_dump_hashes = 1` in `[Hunting]`.

## `DLL_SAMPLER_SLOTS`

Returns `1` when the DLL binds [sampler states](../resources/sampler-states.md) to `<stage>-s<slot>` slots for the running game, and `0` in Direct3D 12 and Vulkan games, where those lines are ignored.

```ini
[Present]
$sampler_slots = dll_sampler_slots
```

DLLs without sampler state support do not know this name and drop the whole line when loading the ini, so copy it into a variable declared with a default of `0` rather than naming it in an `if`. See [Detecting Support](../resources/sampler-states.md#detecting-support).
