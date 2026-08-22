# Pool Metadata

Pool metadata can be accessed using two operators:

* `@` returns the identity of a pool or resolved pool resource.
* `#` returns pool metadata, such as a resolved index, FIFO assignment index, or pool size.

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

See [Resource Metadata → Resource Identity (`@`)](../resource-metadata.md/#resource-identity) for details about resource identities.

## Pool Identity (`@`)

When applied directly to a pool, `@` returns an identity value for the pool object itself.

```ini
$pool_id = @PoolFoo
```

## Index Operator (`#`)

The `#` operator provides additional metadata about a pool. Its behavior depends on whether it is applied to a pool element or directly to the pool.

### Resolved Pool Index

For Ring-indexed pools, `#` returns the normalized index of the resolved pool entry.

For a pool with a size of `4`:

```ini
$index = #PoolFoo[7] ; Returns 3.
```

The returned value identifies the actual pool entry selected after index normalization.

### FIFO Assignment Index

For FIFO-indexed pools, `#` returns the pool slot currently assigned to the specified UID.

```ini
$index = #PoolFooFIFO[123.456]
$index = #PoolFooFIFO[$uid]
```

This is useful when a script needs to determine which pool resource was selected by FIFO assignment.

See [Pool Index Types → FIFO](indexing.md/#fifo-indexing) for more details about FIFO indexing.

### Pool Size

When applied directly to a pool, `#` returns the configured pool capacity.

For `[PoolFoo]` with `pool_size = 4`:

```ini
$pool_size = #PoolFoo
```
