# Pool Indexing

Pool indexing strategy controls how pool elements are accessed and assigned.

| `pool_index_type` | Bracket value        | Lookup                                                          |
| ----------------- | -------------------- | --------------------------------------------------------------- |
| `ring`            | Element index        | Wraps around the pool size, evaluated at runtime                |
| `static`          | Element index        | Wraps around the pool size, resolved at parse time              |
| `fifo`            | Key (UID)            | Same key returns the same element, oldest is evicted when full  |
| `spatial`         | Key (spatial hash)   | Nearest element within `pool_spatial_radius`, FIFO when missing |

The same strategy is used for both [resources](resources.md) and [variables](variables.md) of a pool.

## Ring indexing

Ring indexing treats the pool as a circular array.

### Negative indices

Negative indices wrap from the end of the pool.

For a pool of size 4:

| Expression    | Pool Index |
| ------------- | ---------- |
| `PoolFoo[-1]` | `3`        |
| `PoolFoo[-2]` | `2`        |
| `PoolFoo[-3]` | `1`        |
| `PoolFoo[-4]` | `0`        |

### Overflow

Indices automatically wrap around the pool size.

For a pool of size 4:

| Expression   | Pool Index |
| ------------ | ---------- |
| `PoolFoo[0]` | `0`        |
| `PoolFoo[1]` | `1`        |
| `PoolFoo[2]` | `2`        |
| `PoolFoo[3]` | `3`        |
| `PoolFoo[4]` | `0`        |
| `PoolFoo[5]` | `1`        |
| `PoolFoo[8]` | `0`        |

This allows continuous cyclic access without manual modulo operations.

## Static indexing

Static indexing resolves elements exactly like Ring indexing, but at parse time instead of at runtime.

The value inside brackets must be a numeric literal:

```ini
PoolFooStatic[0]   ; OK
PoolFooStatic[-1]  ; OK, wraps to the last element
$PoolFooStatic[5]  ; OK, wraps around the pool size
PoolFooStatic[$i]  ; Syntax error
```

Since the element is known when the line is parsed, it is accessed directly with no per-run index evaluation. Use a static pool when the indices are fixed and the pool is accessed in hot command lists.

> [Full range operations](variables.md/#full-range-operations) and [pool ranges](../resources/slot-indexing.md/#pool-ranges) accept expressions on static pools, since they do not resolve a single element.

## FIFO indexing

Unlike Ring mode, the value inside brackets is treated as a user-defined key (UID), rather than a direct pool index.

Any floating point value or INI variable can be used as a UID:

```ini
PoolFooFIFO[123]
PoolFooFIFO[-987.654]
PoolFooFIFO[$object_id]
```

The same UID always resolves to the same pool element until its assignment slot is recycled.

### Read Access

When new UID is accessed:

1. If UID is found, returns previously assigned element.
2. If UID is not found, returns default element value (for resource, it's resource template, for variable, it's `pool_variable_default_value`).

A read of an unknown UID does not allocate an element unless `pool_allocate_slot_on_missing = 1` is set. See [Lookup Misses](declaration.md/#lookup-misses) for details.

### Assignment

When assignment happens for UID:

1. If UID already exists, assignment re-uses previously assigned element slot.
2. If UID is new, assignment uses the next non-assigned element slot.
3. When the pool is full, the oldest assigned UID is evicted and insertion writes emptied element slot.

### Recycling

For a pool of size 2, initial assignments:

```ini
PoolFooFIFO[1.0] = ref Resource10
PoolFooFIFO[2.0] = ref Resource20
```

produce:

| UID   | Resource   | Element Index |  
| ----- | ---------- | ------------- |
| `1.0` | Resource10 | `0`           |
| `2.0` | Resource20 | `1`           |

Subsequent lookups return the same assignments:

```ini
PoolFooFIFO[1.0] ; index 0 ref Resource10
PoolFooFIFO[2.0] ; index 1 ref Resource20
```

Assigining another UID:

```ini
PoolFooFIFO[3.0] = ref Resource30
```

overwrites the oldest slot assignment:

| UID   | Resource   | Element Index |  
| ----- | ---------- | ------------- |
| `3.0` | Resource30 | `0`           |
| `2.0` | Resource20 | `1`           |

UID `1.0` is no longer mapped.

## Spatial indexing

Spatial indexing is a FIFO map keyed by world position. It is meant for tracking objects that have no stable identifier but move only a little between frames.

The value inside brackets must be a **spatial hash**: a world position quantized into a 4096×256×4096 grid of cells (X, Y, Z) and packed into a single value. Spatial hashes are produced by the [`->SpatialHash(...)`](../resources/resource-metadata.md/#spatialhashx-y-z-cell_size) resource attribute:

```ini
$hash = vs-cb1->SpatialHash($x, $y, $z, $cell_size)
PoolFooSpatial[$hash] = ref vs-cb1
```

### Lookup

When a spatial hash is accessed:

1. If an element is assigned to the exact same cell, it is returned.
2. Otherwise, the assigned element closest to the requested cell is returned, if it is within `pool_spatial_radius` cells. The element is re-keyed to the requested cell, so it keeps following the object.
3. Otherwise, the lookup misses. On assignment (or with `pool_allocate_slot_on_missing = 1`), the first unassigned element is allocated, and when there is none, the oldest element is evicted like in FIFO mode. On read, the default element value is returned.

Distance between cells is the Chebyshev distance: the largest difference along any axis, so a diagonal neighbor is as close as an axis-aligned one.

### Radius Scaling

`pool_spatial_radius` is defined for a 120 FPS reference frame and is scaled by the frame time, so an object moving at constant speed is allowed the same displacement per frame regardless of FPS:

| FPS   | `pool_spatial_radius = 1` | `pool_spatial_radius = 2` |
| ----- | ------------------------- | ------------------------- |
| `240` | `1` cell                  | `1` cell                  |
| `120` | `1` cell                  | `2` cells                 |
| `60`  | `2` cells                 | `4` cells                 |
| `30`  | `4` cells                 | `8` cells                 |

Combine spatial indexing with [element expiration](declaration.md/#element-expiration) to free up elements of objects that left the scene.
