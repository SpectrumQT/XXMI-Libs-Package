# Resource Metadata

Resource metadata can be accessed using two operators:

- `@` returns a numeric identity for the underlying D3D11 resource.
- `->` retrieves resource attributes such as size and stride.

## Resource Identity (`@`)

Returns a numeric (floating point) identity value for the underlying D3D11 resource.
* Can be used with any `ResourceCopyTarget` entity, such as `@ResourceCustom` or `@vs-t0`.
* Returns `0` if no underlying D3D11 resource is available.

```ini
$resource_id_custom = @ResourceFoo
$resource_id_d3d11 = @vs-t0
```

The identity value is derived from the **C++ pointer** to the underlying D3D11 resource. It has **nothing** to do with the resource data itself, so it is not a replacement for data hashes.

> It's essentially a 30-bit hash of 48-bit virtual memory address (standard x86-64 processors typically don't use upper 16 bits). This allows the value to fit into INI variables, which use IEEE-754 floats, where upper 7 bits become the exponent, and lower 23 bits become the mantissa.

### Non-null Resource Check

Resource identity provides an efficient way to check for non-null resource:

```ini
if @ResourceFoo
    ; Do something
endif
```

> It's a lightweight alternative to `ResourceFoo !== null`, which is significantly more expensive.

## Resource Attributes (`->`)

Resource attributes can be accessed using the `->` syntax.

### `BindFlags`

Returns [D3D11_BIND_FLAGs](../expressions/literals.md/#d3d11_bind_flags-enum-literals) of a resource.

```ini
$bind_flags = ResourceFoo->BindFlags
```

### `Offset`

Returns the byte offset of the currently configured buffer region.

For constant buffers, index buffers, and vertex buffers, the offset is derived from the corresponding D3D11 region parameter:

* Constant buffers — `FirstConstant`
* Index buffers — `FirstIndex`
* Vertex buffers — `FirstVertex`

```ini
$offset = vs-cb0->Offset
$offset = ib0->Offset
$offset = vb0->Offset
```

### `Size`

Returns the resource size in bytes.

```ini
$size = ResourceFoo->Size
```

### `Stride`

Returns the resource stride in bytes.

```ini
$stride = ResourceFoo->Stride
```

### `Format`

Returns [DXGI_FORMAT](../expressions/literals.md/#dxgi_format-enum-literals) of resource (integer enum value).

```ini
$format = ResourceFoo->Format
```

### `Array`

Returns the array dimension of **texture** resource.

```ini
$array = ResourceTexture->Array
```

### `Mips`

Returns mipmap level count of **texture** resource.

```ini
$mips = ResourceTexture->Mips
```

### `Width`

Returns width of **texture** resource (in [texels](https://en.wikipedia.org/wiki/Texel_(graphics))).

```ini
$width = ResourceTexture->Width
```

### `Height`

Returns height of **texture** resource (in [texels](https://en.wikipedia.org/wiki/Texel_(graphics))).

```ini
$height = ResourceTexture->Height
```

### `SourceStride`

Returns the stride of the source resource used to populate the target **custom** resource.

```ini
[ResourceBar]
type = Buffer
stride = 42
filename = Bar.buf

[PoolFoo]
pool_size = 2
type = Buffer
format = R32_FLOAT

PoolFoo[0] = copy ResourceBar

$bar_stride = PoolFoo[0]->SourceStride ; 42
$foo_stride = PoolFoo[0]->Stride       ; 4
```

Pipeline slots, such as `vb0`, are not supported by `SourceStride`.

### `HashRegion($byte_offset, $byte_size)`

Returns a hash of the buffer contents in the specified region.

* `$byte_offset` — byte offset of the region, relative to the currently configured [buffer region](resource-regions.md) of the target.
* `$byte_size` — size of the region in bytes. A region past the end of the buffer is truncated.

```ini
$object_id = vs-cb1->HashRegion(0, 64) ; Hash of the first 64 bytes visible to the shader.
```

The value is a CRC32C of the data encoded as a 30-bit float, so it fits an INI variable and can be compared or used as a [FIFO pool](../pools/indexing.md/#fifo-indexing) key. It is derived from the data itself, unlike the [resource identity](#resource-identity), so equal data in different buffers produces the same hash.

Hashing requires a CPU-side copy of the buffer contents. Set `track_region_hashes = 1` in the `[Rendering]` section to capture the data as the game writes it through `Map` or `UpdateSubresource`; the hash of a region is then computed once per data change. A buffer that is written on the GPU, or any buffer when `track_region_hashes` is disabled, has to be read back the first time it is hashed, which stalls the pipeline.

```ini
[Rendering]
track_region_hashes = 1
```

For custom resources, the hash is only available with `track_region_hashes = 1` and when the data was captured from a pipeline slot (`ref` or `copy`). Other custom resources return an [error value](#error-values).

> **Experimental:** `->HashRegion(...)` is experimental and its behavior may change. **Only CB regions** are really tested.

### `SpatialHash($x, $y, $z, $cell_size)`

Returns a **spatial hash** of a world position stored in the buffer: the position is quantized into a 4096×256×4096 grid of cells (X, Y, Z) and the cell coordinates are packed into a single value.

* `$x`, `$y`, `$z` — offsets of the three position floats, in 32-bit elements, relative to the currently configured [buffer region](resource-regions.md) of the target.
* `$cell_size` — size of a grid cell in world units.

```ini
$hash = vs-cb1->SpatialHash(12, 13, 14, 0.5) ; Position at floats 12-14 of the visible region, 0.5 unit cells.
```

Positions within the same cell produce the same hash, and positions in neighboring cells produce hashes that a [spatial pool](../pools/indexing.md/#spatial-indexing) can resolve to the same element. The grid wraps around along each axis, so positions `4096` cells apart on X or Z (`256` on Y) collide.

The same data caching rules as for [`HashRegion`](#hashregionbyte_offset-byte_size) apply.

> **Experimental:** `->SpatialHash(...)` is experimental and its behavior may change. **Only CB regions** are really tested.

### Performance

Accessing `->` attributes from custom resources is extremely inexpensive, as the metadata is stored directly with the custom resource.

```ini
ResourceFoo->Size
```

Accessing attributes through pipeline slots is slower because the underlying D3D11 resource must be queried.

```ini
vb0->Size
```

Typical access times are approximately ~1 ns for custom resources and ~10–100 ns for slots, depending on the operation and environment.

### Error Values

Attribute getters return the following values when data retrieval fails:

| Value   | Meaning                                                   |
| ------- | --------------------------------------------------------- |
| `-1.0f` | `UNKNOWN` — all data sources returned `0`                 |
| `-2.0f` | `RESOURCE_NOT_FOUND` — no buffer was found for the target |
| `-3.0f` | `NOT_A_BUFFER` — the target is not a D3D11 buffer         |
| `-4.0f` | `NOT_A_TEXTURE` — the target is not a texture resource    |
