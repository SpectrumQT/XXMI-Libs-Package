# Commands

## `store`

Reads a 32-bit floating-point value from a GPU resource and stores it in an INI runtime variable.

```ini
store = $out, ResourceFoo, $offset
```

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

Despite these optimizations, GPU → CPU readback is **expensive** and `store` should therefore generally be used only when the value is actually needed by the CPU-side INI runtime, and there is no other way around it.
