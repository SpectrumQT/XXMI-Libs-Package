# Resource Regions

Resource regions represent byte ranges within an underlying D3D11 buffer.

Regions can be defined implicitly by pipeline state or explicitly using the `->Region(...)` specifier.

## Pipeline Resource Regions

Pipeline slots may reference only a portion of an underlying buffer. For constant buffers, this region is defined by `FirstConstant` and `NumConstants`.

When a resource is copied or referenced from a pipeline slot, only the currently visible region is captured.

For example:

```ini
ResourceFoo = ref vs-cb0
ResourceBar = copy vs-cb0
```

Both operations use the region currently configured for `vs-cb0`, rather than the entire underlying constant buffer.

The `->Offset` attribute can be used to retrieve the absolute byte offset of the currently configured region.

See [Resource Metadata → `Offset`](resource-metadata.md/#offset) for details.

## Explicit Resource Regions

### `Region($byte_offset, $byte_size)`

The `->Region($byte_offset, $byte_size)` specifier selects an explicit region of the underlying buffer.

Both arguments are evaluated as byte values:

* `$byte_offset` specifies the absolute byte offset into the underlying buffer.
* `$byte_size` specifies the size of the region in bytes.

> **Experimental:** `->Region(...)` is experimental and its behavior may change. **Only CB regions** are really tested.

### Copying a Region

```ini
cs-cb0 = copy vs-cb0->Region($byte_offset, $byte_size)
```

Only the specified region of the underlying buffer is copied.

### Referencing a Region

```ini
cs-cb0 = ref vs-cb0->Region($byte_offset, $byte_size)
```

The same underlying buffer is referenced using the specified region without copying its contents.

See [Constant Buffer Region](../examples/constant-buffer-region.md) for a practical example.

### Binding a Region as a Stream Output Target

```ini
so0 = ref ResourceFoo->Region($start * $stride, $count * $stride)
```

Stream output is written from `$byte_offset` instead of from the start of the buffer, so a draw call can fill the part of a shared mesh that belongs to one object. Only the offset is used, as D3D11 takes no size for a stream output target.

Stream output slots the line does not name are bound at offset 0, since D3D11 does not report the offsets they were bound at.
