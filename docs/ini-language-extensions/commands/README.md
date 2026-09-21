# Commands

## `store`

Reads a 32-bit floating-point value from a GPU resource and stores it in an INI runtime variable.

```ini
store = $out, ResourceFoo, $offset
```

> ⚠️ **`store` is VERY expensive.** It stalls the CPU until the GPU has finished all queued work. Used on every draw call, or even every frame, it **will kill the frame rate**. Gate it so it runs rarely, see [GPU Readback](../examples/gpu-readback.md).

The command takes three arguments:

1. **Output variable** — the INI variable that receives the value.
2. **Resource target** — the pipeline slot or custom resource to read from.
3. **Offset expression** — a dynamically evaluated expression specifying which 32-bit value to read.

For example:

```ini
store = $out, ResourceFoo, 4
```

The offset is interpreted as a **32-bit value index**, so `4` reads the fifth 32-bit value from the resource.

### GPU Readback

`store` performs **GPU → CPU readback**. The requested data is copied from the GPU resource into a CPU-readable staging buffer and then read by the CPU.

Only the required data is read back whenever possible. For regular buffers, only the requested 4-byte value is copied. For structured buffers, the smallest range containing the required complete structures is copied.

Readback staging buffers are reused across `store` invocations rather than recreated for every call.

Despite these optimizations, GPU → CPU readback remains **very expensive**: the CPU has to wait for the GPU to finish everything it was given before the value can be read, which serializes the two and can cut the frame rate by a large factor when done frequently. `store` should therefore only be used when the value is actually needed by the CPU-side INI runtime, there is no other way around it, and the call is guarded so it does not execute on every draw or frame.

See [GPU Readback](../examples/gpu-readback.md) for practical usage patterns.
