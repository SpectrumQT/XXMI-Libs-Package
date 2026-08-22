# Declaring Pools

Pools are declared using a dedicated `[Pool]`-type section. Each pool section must have a unique name, such as `[PoolFoo]`.

## Pool Size

Specifies the maximum number of elements that the pool can contain. The default maximum pool size is `1`.

Use `pool_size = X`:
```ini
[PoolFoo]
pool_size = 4 ; Increase the maximum pool size to 4.
```

## Element Initialization

By default, pool elements are initialized on first read/write access.

To force immideate initialization, set `pool_lazy_initialization = 0`:

```ini
[PoolFoo]
pool_lazy_initialization = 0 ; Initialize all elements at parse time.
```

## Pool Index Types

### Ring

Provides array-like access with support for negative indices and index wrapping.

Use `pool_index_type = ring`:

```ini
[PoolFoo]
pool_size = 4 ; Index -1 maps to index 3, while index 4 wraps to index 0.
pool_index_type = ring
```

See [Ring Indexing](indexing.md/#ring-indexing) for more details.

### FIFO

Provides map-like access with floating-point keys and evicts the oldest element when the pool is full.

Use `pool_index_type = fifo`:

```ini
[PoolFoo]
pool_size = 4 ; When a fifth element is inserted, the oldest element is evicted.
pool_index_type = fifo
```

See [FIFO Indexing](indexing.md/#fifo-indexing) for more details.

## Default Element Values

### Resources

Resource-type pool elements use an internal **template resource** for their configuration and initial data. Any `[Resource]` section options specified in the `[Pool]` section override the template defaults.

```ini
[PoolFoo]
pool_size = 4

type = Buffer
data = "Hello World!"
```

See [Template Resource](resources.md/#template-resource) for more details.
