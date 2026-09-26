# Pool Metadata

Pool metadata can be accessed using two operators:

* `@` returns the identity of a pool or resolved pool resource.
* `->` returns attributes. Two of them are pool-specific: `->Index` of a pool element and `->Size` of a pool. All other [resource attributes](../resources/resource-metadata.md), such as `->Stride` or `->Format`, work on a pool element exactly as on a custom resource.

## Pool Resource Identity (`@`)

When used with a pool resource, `@` returns an identity value for the resolved resource.

```ini
$resource_id = @PoolFoo[$index]
```

The returned identity uniquely identifies the underlying resource buffer and can be compared with other resource identities.

For example:

```ini
if @PoolFoo[0] == @PoolFoo[1]
    ; Do something
endif
```

This condition is `true` when both pool resources reference the same underlying buffer.

See [Resource Metadata → Resource Identity (`@`)](../resources/resource-metadata.md/#resource-identity) for details about resource identities.

## Pool Identity (`@`)

When applied directly to a pool, `@` returns an identity value for the pool object itself.

```ini
$pool_id = @PoolFoo
```

A pool that is a [proxy](pool-operations.md/#pool-reference) of another pool returns the identity of the pool it resolves to.

## Pool Attributes (`->`)

`Index` applies to a pool element, `Size` applies to the pool itself. Any other `->` attribute on a pool element, e.g. `PoolFoo[$index]->Stride`, is the [resource attribute](../resources/resource-metadata.md) of the element's resource.

### `Index`

Returns the index of the pool element that the bracket value resolves to.

For Ring-indexed pools, this is the normalized index. For a pool with a size of `4`:

```ini
$index = PoolFoo[7]->Index ; Returns 3.
```

For FIFO-indexed pools, this is the element currently assigned to the specified UID:

```ini
$index = PoolFooFIFO[123.456]->Index
$index = PoolFooFIFO[$uid]->Index
```

This is useful when a script needs to determine which pool element was selected by FIFO assignment.

For a UID that is not assigned to any element, `Index` returns `-1` and does not allocate an element (see [Lookup Misses](declaration.md/#lookup-misses)):

```ini
if PoolFooFIFO[$uid]->Index == -1
    ; $uid is not tracked yet
endif
```

`Index` is also accepted on non-pool targets, where it identifies what kind of resource the target is:

| Value      | Meaning                                              |
| ---------- | ---------------------------------------------------- |
| `0` and up | Index of the pool element                            |
| `-1`       | Pool [template resource](resources.md/#template-resource), e.g. for an unassigned FIFO UID |
| `-2`       | Regular `[Resource]` (`ResourceFoo->Index`)          |
| `-3`       | Not a custom resource (`vs-t0->Index`)               |

See [Pool Index Types → FIFO](indexing.md/#fifo-indexing) for more details about FIFO indexing.

### `Size`

When applied directly to a pool, `Size` returns the configured pool capacity.

For `[PoolFoo]` with `pool_size = 4`:

```ini
$pool_size = PoolFoo->Size ; Returns 4.
```

For a [proxy](pool-operations.md/#pool-reference) pool, the size of the pool it resolves to is returned.

> `PoolFoo[$index]->Size` is the [resource size](../resources/resource-metadata.md/#size) of the element, not the pool size.
