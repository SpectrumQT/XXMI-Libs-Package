# Pool Indexing

Pool indexing strategy controls how pool elements are accessed and assigned.

## Static and dynamic indices

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
2. If UID is not found, returns default element value (for resource, it's resource template).

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