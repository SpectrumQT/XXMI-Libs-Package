# Stream Output

Stream output targets are addressed as `so0` to `so3`, the four slots D3D11 provides. They are the buffers the geometry (or vertex) shader stage writes its output into, which makes them the way to capture geometry the GPU has already transformed.

```ini
so0 = ref ResourceSkinned      ; bind a buffer as the stream output target
so0 = null                     ; unbind the slot
ResourceCopy = ref so0         ; read whatever the game currently has bound
```

## Bind Flags

A custom resource bound to `so0` needs `D3D11_BIND_STREAM_OUTPUT`. The flag is added automatically if the `so` assignment is the **first** use of that resource, because a custom resource picks up the bind flags of whatever it is first assigned to.

That is not enough for the usual case. Capturing geometry means writing it through `so0` and then reading it back somewhere, so the resource needs both flags, and whichever assignment happens to run first decides what it is created with. Declare them instead of relying on the order:

```ini
[ResourceSkinned]
type = Buffer
bind_flags = stream_output shader_resource
```

With frame analysis active, a reference copy whose source is missing a bind flag its destination requires is reported in the log.

## Regions

An offset selects where in the buffer the stage starts writing, which is what lets one draw call fill the part of a shared mesh belonging to one object:

```ini
so0 = ref ResourceSkinned->Region($start * $stride, $count * $stride)
```

Only the offset applies, as D3D11 takes no size for a stream output target. See [Resource Regions](resource-regions.md) for the specifier itself.

## Limitations

* **Offsets cannot be read back.** D3D11 has no call that reports the offset a stream output target was bound at. So `so0->Offset` reports unknown, `->Region` on an `so` *source* measures from the start of the buffer rather than from wherever the game bound it, and binding one slot leaves the other three at offset 0.
* **Ranges are not accepted.** `so[$i]` works, as it does for any pipeline slot (see [Slot Indexing](slot-indexing.md)), but there is no `so[$a:$b]` form.
* **The shader doing the writing cannot be overridden.** A geometry shader created together with its stream output declaration is not registered, so it has no hash: hunting does not list it, and `[ShaderOverride]` and shader replacement do not apply to it. The buffers reached through `so0` to `so3` are the only handle a fix has on a stream output pass.
