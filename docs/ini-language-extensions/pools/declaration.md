# Declaring Pools

Pools are declared using a dedicated `[Pool]`-type section. Each pool section must have a unique name, such as `[PoolFoo]`.

```ini
[PoolFoo]
pool_size = 4
pool_index_type = ring
```

Every element of a pool can hold a [resource](resources.md), a [variable](variables.md), or both. No separate declaration is needed: `PoolFoo[$index]` accesses the resource of an element, `$PoolFoo[$index]` accesses its variable.

## Pool Options

All pool options start with the `pool_` prefix. Any other option in a `[Pool]` section is treated as a `[Resource]` option of the pool's [template resource](resources.md/#template-resource).

| Option                             | Default | Description                                                                                                            |
| ---------------------------------- | ------- | ---------------------------------------------------------------------------------------------------------------------- |
| `pool_size`                        | `1`     | Number of elements in the pool. See [Pool Size](#pool-size).                                                           |
| `pool_index_type`                  | `ring`  | Element lookup strategy: `ring`, `static`, `fifo` or `spatial`. See [Pool Index Types](#pool-index-types).            |
| `pool_lazy_initialization`         | `1`     | Initialize elements on first access instead of at parse time. See [Element Initialization](#element-initialization).  |
| `pool_allocate_slot_on_missing`    | `0`     | `fifo` and `spatial` only: allocate a slot when an unknown key is **read**. See [Lookup Misses](#lookup-misses).      |
| `pool_spatial_radius`              | `1`     | `spatial` only: reuse an existing slot within this many grid cells. See [Spatial](#spatial).                          |
| `pool_expiration_timeout_frames`   | *off*   | Expire elements that were not updated for more than N frames. See [Element Expiration](#element-expiration).          |
| `pool_expiration_reset_elements`   | `1`     | Reset expired elements to their default values.                                                                        |
| `pool_expiration_refresh_on_read`  | `0`     | Reading an element also postpones its expiration.                                                                      |
| `pool_element_type_switch_reset`   | `1`     | Reset an element when it switches between resource and variable. See [Element Types](#element-types).                 |
| `pool_variable_default_value`      | `0`     | Initial and reset value of pool variables. See [Variables](#variables).                                                |
| `pool_persist_variables`           | `0`     | `ring` only: save pool variables to `d3dx_user.ini`. See [Variables](#variables).                                      |

## Pool Size

Specifies the maximum number of elements that the pool can contain. The default maximum pool size is `1`.

Use `pool_size = X`:
```ini
[PoolFoo]
pool_size = 4 ; Increase the maximum pool size to 4.
```

A pool with `pool_size` below `1` is not created, so any reference to it fails to parse.

## Element Initialization

By default, pool elements are initialized on first read/write access.

To force immediate initialization, set `pool_lazy_initialization = 0`:

```ini
[PoolFoo]
pool_lazy_initialization = 0 ; Initialize all elements at parse time.
```

Each initialized pool element costs around 500 bytes of RAM and slows down INI parsing a bit.

Initialization covers both the resource and the variable of an element. With lazy initialization, an element's resource is initialized on the first resource access (`PoolFoo[$index]`), and its variable on the first variable access (`$PoolFoo[$index]`).

## Pool Index Types

The index type controls how the value in square brackets is mapped to a pool element. `ring` and `static` treat it as an element index, `fifo` and `spatial` treat it as a key.

### Ring

Provides array-like access with support for negative indices and index wrapping.

Use `pool_index_type = ring`:

```ini
[PoolFoo]
pool_size = 4 ; Index -1 maps to index 3, while index 4 wraps to index 0.
pool_index_type = ring
```

> Pool **defaults** to `ring` index, so `pool_index_type = ring` can be omitted.

See [Ring Indexing](indexing.md/#ring-indexing) for more details.

### Static

Same as `ring`, but the index must be a numeric literal and is resolved once at parse time.

Use `pool_index_type = static`:

```ini
[PoolFoo]
pool_size = 4
pool_index_type = static

[CommandListFoo]
PoolFoo[2] = ref vs-cb0 ; OK, resolved at parse time.
$PoolFoo[-1] = 1        ; OK, wraps to index 3.
PoolFoo[$i] = ref vs-cb0 ; Syntax error: expression is not a numeric literal.
```

Accessing a static pool element has the same cost as accessing a regular `[Resource]` or global variable, since no index evaluation happens at runtime.

See [Static Indexing](indexing.md/#static-indexing) for more details.

### FIFO

Provides map-like access with floating-point keys and evicts the oldest element when the pool is full.

Use `pool_index_type = fifo`:

```ini
[PoolFoo]
pool_size = 4 ; When a fifth element is inserted, the oldest element is evicted.
pool_index_type = fifo
```

See [FIFO Indexing](indexing.md/#fifo-indexing) for more details.

### Spatial

Provides map-like access keyed by world position. Lookups resolve to the element whose position is closest to the requested one, as long as it is within `pool_spatial_radius` grid cells.

Use `pool_index_type = spatial`:

```ini
[PoolFoo]
pool_size = 16
pool_index_type = spatial
pool_spatial_radius = 2 ; Reuse an element within 2 cells of the requested position.
```

`pool_spatial_radius` is specified in grid cells at a reference frame rate of 120 FPS and is scaled with the frame time, so the allowed movement per frame stays roughly the same at any FPS. Values below `1` are clamped to `1`.

See [Spatial Indexing](indexing.md/#spatial-indexing) for more details.

## Lookup Misses

Applies to `fifo` and `spatial` pools.

By default, reading a key that is not assigned to any element does not modify the pool. The read returns the [template resource](resources.md/#template-resource) or the [default variable value](#variables) instead:

```ini
[PoolFooFIFO]
pool_size = 4
pool_index_type = fifo
pool_variable_default_value = -1

[CommandListFoo]
$x = $PoolFooFIFO[$unknown_uid] ; -1, nothing is allocated.
$PoolFooFIFO[$unknown_uid] = 5  ; Allocates an element for $unknown_uid.
```

Set `pool_allocate_slot_on_missing = 1` to allocate an element on read as well. When the pool is full, the oldest element is evicted, exactly as on assignment:

```ini
[PoolFooFIFO]
pool_size = 4
pool_index_type = fifo
pool_allocate_slot_on_missing = 1

[CommandListFoo]
$x = $PoolFooFIFO[$unknown_uid] ; 0, and $unknown_uid now owns an element.
```

## Element Expiration

Elements can be configured to expire when they are not updated for a number of frames.

Use `pool_expiration_timeout_frames = N`:

```ini
[PoolFoo]
pool_size = 8
pool_index_type = fifo
pool_expiration_timeout_frames = 2 ; Expire elements not updated for more than 2 frames.
```

Every element remembers the frame of its last update. An element expires once it has not been updated for more than `N` frames. Expired elements are processed on the first access to the pool in a frame, before the access itself is resolved.

Assignments count as updates:

```ini
PoolFoo[$id] = ref vs-cb0   ; Update
$PoolFoo[$id] = 1           ; Update
PoolFoo[0:3] = ref ps-t[0:3] ; Update of elements 0-3
```

Reads do not count as updates unless `pool_expiration_refresh_on_read = 1` is set:

```ini
vs-cb0 = ref PoolFoo[$id] ; Read
$x = $PoolFoo[$id]        ; Read
$index = PoolFoo[$id]->Index ; Read
```

When an element expires:

* For `fifo` and `spatial` pools, its key is forgotten and the element behaves like it was never assigned.
* With `pool_expiration_reset_elements = 1` (default), the element is reset: its resource becomes `null` and its variable is set to `pool_variable_default_value`.
* With `pool_expiration_reset_elements = 0`, the element keeps its current resource and variable until it is reused.

> `pool_expiration_timeout_frames = 0` expires an element on the first frame it is not updated in.

> `static` pools resolve their elements at parse time, so expiration does not apply to them.

## Element Types

Each pool element has a resource and a variable. By default, they are not meant to be used together: assigning one type resets the other.

With `pool_element_type_switch_reset = 1` (default):

```ini
PoolFoo[0] = ref ResourceFoo ; Element 0 holds a resource.
$PoolFoo[0] = 1              ; Element 0 switches to a variable, its resource is set to null.
PoolFoo[0] = ref ResourceBar ; Element 0 switches back to a resource, its variable is reset to pool_variable_default_value.
```

Set `pool_element_type_switch_reset = 0` to let the resource and the variable of an element coexist:

```ini
[PoolFoo]
pool_size = 4
pool_element_type_switch_reset = 0

[CommandListFoo]
PoolFoo[0] = ref ResourceFoo ; Element 0 holds a resource...
$PoolFoo[0] = 1              ; ...and a variable, both are kept.
```

Reads never switch the element type: reading `$PoolFoo[0]` while element 0 holds a resource returns `pool_variable_default_value` and leaves the resource in place.

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

### Variables

Variable-type pool elements are initialized to `pool_variable_default_value` (default `0`):

```ini
[PoolFoo]
pool_size = 4
pool_variable_default_value = -1 ; Every $PoolFoo[$index] starts at -1.
```

The same value is used whenever a variable is reset: by `$PoolFoo[*] = null`, by an [element type switch](#element-types), by [expiration](#element-expiration) or when a `fifo` / `spatial` [lookup misses](#lookup-misses).

Set `pool_persist_variables = 1` to save pool variables to `d3dx_user.ini` like `persist` global variables:

```ini
[PoolFoo]
pool_size = 4
pool_persist_variables = 1
```

Persistence is only supported for `ring` pools. Only elements that have been initialized are saved, so with lazy initialization a variable that was never accessed is not written to `d3dx_user.ini`.

See [Variables](variables.md) for more details.
