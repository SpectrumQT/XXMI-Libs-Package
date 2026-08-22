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
