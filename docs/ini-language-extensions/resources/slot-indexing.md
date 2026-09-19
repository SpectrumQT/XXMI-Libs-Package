# Slot Indexing

Pipeline slot targets such as `ps-t0` or `vb1` use a fixed slot number. **XXMI DLL** allows the slot number to be an expression evaluated at runtime, and allows a range of slots to be bound or fetched by a single operation.

## Dynamic Slot Index

### `<slot>[$index]`

The slot number is given as an expression in square brackets and evaluated every time the line runs.

```ini
ps-t[$index] = ref ResourceFoo
ResourceBar = ref vs-cb[$index + 1]
```

Supported slot types:

| Syntax            | Slot Type                                  | Valid Index |
| ----------------- | ------------------------------------------ | ----------- |
| `<stage>-t[$i]`   | Shader resource                            | `0`–`127`   |
| `<stage>-u[$i]`   | Unordered access view (`ps` and `cs` only) | `0`–`63`    |
| `<stage>-cb[$i]`  | Constant buffer                            | `0`–`13`    |
| `vb[$i]`          | Vertex buffer                              | `0`–`31`    |
| `o[$i]`           | Render target                              | `0`–`7`     |
| `so[$i]`          | Stream output                              | `0`–`3`     |

`<stage>` is one of `vs`, `hs`, `ds`, `gs`, `ps` or `cs`.

The index is truncated to an integer. An index outside the valid range logs a warning and the operation is skipped.

A dynamic slot index is accepted anywhere a pipeline slot is, including `CheckTextureOverride`, `dump` and expressions:

```ini
CheckTextureOverride = ps-t[$index]
$resource_id = @ps-t[$index]
$size = ps-t[$index]->Size
```

> [Input layout](../input-layouts/README.md) overrides require a fixed slot, so `vb[$index]->ElementFormat(...)` is not supported.

## Slot Ranges

### `<slot>[$first:$last]`

Selects slots `$first` through `$last`, inclusive. Both bounds are expressions evaluated at runtime.

Ranges are supported for `t` and `cb` slots of every shader stage, and for `ps-u` and `cs-u` slots. The whole range is processed by a single D3D11 call, such as `PSSetShaderResources`, which is significantly cheaper than binding the slots one by one.

A slot range can only be used in a `ref` copy (the `ref` keyword may be omitted) with a pool, a custom resource or `null` on the other side:

```ini
ps-t[0:9] = ref PoolFoo[0:9] ; Bind pool elements 0–9 to ps-t0–ps-t9
PoolFoo[0:9] = ref ps-t[0:9] ; Fetch ps-t0–ps-t9 into pool elements 0–9
ps-t[0:3] = ref ResourceFoo  ; Bind the same resource to ps-t0–ps-t3
ps-t[0:3] = null             ; Unbind ps-t0–ps-t3
```

Slot bounds are validated at runtime: `$first` must not be negative, `$last` must not be smaller than `$first`, and `$last` must be a valid slot number. An invalid range logs a warning and the operation is skipped.

### Pool Ranges

`PoolFoo[$first:$last]` selects pool elements `$first` through `$last`, inclusive.

Pool bounds follow [Ring indexing](../pools/indexing.md/#ring-indexing) rules, so negative and overflowing indices wrap around the pool size. The range may not be larger than the pool.

A pool range is only valid on the other side of a slot range.

### Inheriting Bounds

When bounds are given on one side only, the other side uses the same bounds:

```ini
ps-t = ref PoolFoo[0:9]   ; Same as ps-t[0:9] = ref PoolFoo[0:9]
ps-t[0:9] = ref PoolFoo   ; Same as ps-t[0:9] = ref PoolFoo[0:9]
PoolFoo[0:9] = ref ps-t   ; Same as PoolFoo[0:9] = ref ps-t[0:9]
PoolFoo = ref ps-t[0:9]   ; Same as PoolFoo[0:9] = ref ps-t[0:9]
```

The bare `<stage>-t`, `<stage>-u` and `<stage>-cb` forms are only valid opposite a pool range.

When the slots inherit their bounds from a pool range, the wrapped pool indices are used as slot numbers, so the pool range must not wrap around the end of the pool. When the pool inherits its bounds from a slot range, the slot numbers are used as pool indices, so the range must not be larger than the pool.

When bounds are given on both sides, both ranges must have the same size.

### Copy Options

Only `unless_null` and `no_view_cache` are supported with slot ranges. Any other copy option, including `copy`, is rejected when the INI is loaded.

* `unless_null` — slots whose source is `null` keep their current binding, and pool elements whose source slot is empty are left untouched.
* `no_view_cache` — views created for the range are released after each run instead of being cached per slot.

When a pool element already holds a view of the slot's type, for example because it was fetched from a slot of the same type, that view is bound directly instead of creating a new one.

### Frame Analysis Dump

`dump` accepts a slot range with explicit bounds and dumps each slot in turn. The slot number is appended to the dump file name.

```ini
dump = ps-t[0:3]
```

Pool ranges and the bare `<stage>-t` form are not supported by `dump`. Other commands do not accept ranges.
