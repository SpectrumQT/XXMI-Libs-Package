# GPU Readback

Suppose the distance from the camera to a character lives in a constant buffer, and the INI script needs it on the CPU side to decide which vertex buffer variant to bind. The [`store`](../commands/README.md/#store) command reads a single 32-bit value back from a GPU resource into an INI variable or a [pool variable](../pools/variables.md).

> ⚠️ **`store` is VERY expensive** unless the buffer is [cached in RAM](#reading-without-a-stall). A readback forces the CPU to wait until the GPU has finished all queued work and copied the value back, which throws away the CPU/GPU parallelism the game relies on. A `store` that runs on every draw call, or even once per frame for several objects, **will kill the frame rate**. Treat it as a last resort, and never let it run unconditionally in a `[TextureOverride]` or `[ShaderOverride]` section: always gate it so it executes rarely, as shown in [Reading Sparingly](#reading-sparingly).

The naive version below reads the value on every draw of the character. It works, but it is exactly the pattern to avoid:

```ini
[Constants]
global $distance = 0

[TextureOverrideCharacter]
hash = 12345678
; The value lives at 32-bit index 4 (byte 16) of the visible cb region.
store = $distance, vs-cb1, 4

if $distance < 10
    vb0 = ref ResourceBodyHighPoly
else
    vb0 = ref ResourceBodyLowPoly
endif
```

The third argument is an index of a 32-bit value, not a byte offset, so `4` reads the fifth 32-bit value of the buffer region. The value is interpreted as a `float`. An integer stored in the buffer comes back as its raw bit pattern rather than its numeric value, so `store` is meant for values the shader writes as floats.

## Reading Sparingly

Each `store` stalls the pipeline: the value is copied into a staging buffer and the CPU blocks until the GPU catches up. A single readback per frame is already noticeable, and a readback per draw call is disastrous. When the value changes rarely, read it rarely:

```ini
[TextureOverrideCharacter]
hash = 12345678
if FRAME_NUMBER % 30 == 0
    store = $distance, vs-cb1, 4
endif
```

Or read it once and re-read only when something that could change it happens, such as a key press or a resource identity change:

```ini
[Constants]
global $last_cb = 0

[TextureOverrideCharacter]
hash = 12345678
if @vs-cb1 != $last_cb
    $last_cb = @vs-cb1
    store = $distance, vs-cb1, 4
endif
```

Here [`@vs-cb1`](../resources/resource-metadata.md/#resource-identity) is a cheap identity of the bound buffer, so the readback only happens when the game switches to a different constant buffer.

## Reading Without a Stall

A buffer type listed in `cache_resource_data` is kept in RAM as the game writes it, and `store` then reads the value from that copy instead of the GPU:

```ini
[Rendering]
cache_resource_data = constant_buffer
```

This removes the stall entirely for constant buffers the game fills on the CPU, which is where most per-object values live, so the guards above are no longer needed for them. It does not help with buffers the GPU writes: those are never cached and always take the readback path.

## Storing Into a Pool Variable

The destination can be a pool variable, which is how a readback is kept per object rather than in a single global:

```ini
[TextureOverrideCharacter]
hash = 12345678
$object_id = @vs-cb1
store = $PoolObjects[$object_id], vs-cb1, 4
```

## Reading Computed Results

`store` also reads from custom resources, which makes it possible to fetch a result computed by a custom compute shader:

```ini
[ResourceResult]
type = RWBuffer
format = R32_FLOAT
array = 4

[CommandListMeasure]
cs-u0 = ref ResourceResult
run = CustomShaderMeasure
cs-u0 = null

store = $height, ResourceResult, 0
store = $width, ResourceResult, 1
```

Each `store` is a separate readback and a separate stall. When several values are needed, pack them into one structured buffer element so the smallest range containing the whole structure is copied once and the values are read from the staging copy.

## Alternatives

Before reaching for `store`, check whether the decision can be made without leaving the GPU or with data the INI runtime already has for free:

* [Resource metadata](../resources/resource-metadata.md) such as `->Size`, `->Stride`, `->Format` or `@` identity costs nanoseconds and often distinguishes the cases that a readback would.
* A custom shader can branch on the value itself, so the choice is made on the GPU instead of the CPU.
* [`->HashRegion`](../resources/resource-metadata.md/#hashregionbyte_offset-byte_size) tells whether data changed without stalling, for the buffer types listed in `cache_resource_data`.
