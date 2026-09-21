# Variables

Every pool element holds a variable in addition to its [resource](resources.md). Pool variables are regular INI variables that are looked up by pool index instead of by name.

No declaration is required beyond the `[Pool]` section itself:

```ini
[PoolFoo]
pool_size = 4
pool_variable_default_value = 0 ; Optional, 0 is the default.
```

## Pool Variable Access

A pool variable is accessed with the `$` prefix and the element index in square brackets:

```ini
$PoolFoo[0]
$PoolFoo[$index]
$PoolFoo[$index + 1]
```

The index is evaluated as an INI expression at runtime and resolved using the pool's [index type](indexing.md). For `fifo` and `spatial` pools, the bracket value is a key rather than an index:

```ini
$PoolFooFIFO[$object_id]
$PoolFooSpatial[$spatial_hash]
```

A pool variable can be used anywhere a global variable is accepted, including expressions, conditions, section options and as an argument to other pool indices:

```ini
$x = $PoolFoo[$index] * 2

if $PoolFoo[$index] > 0
    ; Do something
endif

x = $PoolFoo[$index]

$PoolFoo[$PoolBar[$id] + 1] = $PoolBar[$id] + 1 ; Nested pool variables are allowed.
```

### Reading an Unknown FIFO UID

When an unassigned UID is requested for a [FIFO-indexed](indexing.md/#fifo-indexing) or [spatial](indexing.md/#spatial-indexing) pool, `pool_variable_default_value` is returned and no element is allocated:

```ini
[PoolFooFIFO]
pool_size = 4
pool_index_type = fifo
pool_variable_default_value = -1

[CommandListFoo]
if $PoolFooFIFO[$object_id] == -1
    ; $object_id is not tracked yet
endif
```

See [Lookup Misses](declaration.md/#lookup-misses) to change this behavior.

## Pool Variable Assignment

Pool variables are assigned like global variables. The right-hand side is an expression:

```ini
$PoolFoo[$index] = 1
$PoolFoo[$index] = $PoolFoo[$index] + 1
$PoolFoo[$index] = $PoolBar[$index] * $scale
```

For `fifo` and `spatial` pools, assigning a new key allocates an element, evicting the oldest one when the pool is full. See [FIFO Indexing → Assignment](indexing.md/#assignment).

> Assignment to a pool variable resets the resource of the same element unless `pool_element_type_switch_reset = 0` is set. See [Element Types](declaration.md/#element-types).

## Full Range Operations

`$PoolFoo[*]` addresses the variables of **all** pool elements at once. It is only valid as an assignment target.

### Set All Variables

```ini
$PoolFoo[*] = 0
$PoolFoo[*] = $var + 1
```

The expression is evaluated once, and the result is assigned to every element by its direct index, regardless of the pool's index type. For `fifo` and `spatial` pools, keys are not affected.

Like a single assignment, this switches every element to the variable type, so with the default `pool_element_type_switch_reset = 1` the resources of the pool are set to `null`.

### Reset All Variables

```ini
$PoolFoo[*] = null
```

Resets every pool variable to `pool_variable_default_value`. Resources of the pool are left untouched, use `PoolFoo[*] = null` to reset those. See [Pool Resource Ranges](resources.md/#pool-resource-ranges).

> `[$first:$last]` ranges are only supported for resources, opposite a [slot range](../resources/slot-indexing.md/#slot-ranges). Variables support the full range `[*]` only.

## Static Pools

For [static pools](indexing.md/#static-indexing), the index must be a numeric literal and the variable is resolved at parse time, so it is as cheap as a global variable:

```ini
[PoolFooStatic]
pool_size = 4
pool_index_type = static

[CommandListFoo]
$PoolFooStatic[0] = $PoolFooStatic[0] + 1
$PoolFooStatic[-1] = 0 ; Wraps to element 3.
```

## Persistence

Set `pool_persist_variables = 1` to save the pool's variables to `d3dx_user.ini`, like `persist` global variables. Persistence is only available for `ring` pools:

```ini
[PoolSettings]
pool_size = 8
pool_persist_variables = 1

[KeyToggle]
key = F5
run = CommandListToggle

[CommandListToggle]
$PoolSettings[$active] = 1 - $PoolSettings[$active] ; Saved across game restarts.
```

Only elements that have been initialized are saved. With the default `pool_lazy_initialization = 1`, a variable that has never been accessed is not written to the file. Set `pool_lazy_initialization = 0` to always save every element.

## Examples

### Per-Object Counter

Count how many times each object was drawn this frame, using a `fifo` pool keyed by the identity of the per-object constant buffer:

```ini
[PoolDrawCount]
pool_size = 64
pool_index_type = fifo
pool_expiration_timeout_frames = 0 ; Forget objects that were not drawn this frame.

[TextureOverrideObject]
hash = 12345678
$object_id = @vs-cb1 ; Identity of the per-object constant buffer.
$PoolDrawCount[$object_id] = $PoolDrawCount[$object_id] + 1
if $PoolDrawCount[$object_id] == 1
    ; First draw call of this object in the current frame
endif
```

### Pairing Variables With Resources

Store a resource and a related value in the same element, for example the frame a resource was captured in:

```ini
[PoolHistory]
pool_size = 2
pool_element_type_switch_reset = 0 ; Keep both the resource and the variable.

[TextureOverrideCSU0]
hash = 12345678
handling = skip
draw = from_caller ; Run the game's dispatch, then capture its output.
PoolHistory[FRAME_NUMBER] = copy cs-u0
$PoolHistory[FRAME_NUMBER] = FRAME_NUMBER

[CommandListUseHistory]
if $PoolHistory[FRAME_NUMBER - 1] == FRAME_NUMBER - 1
    cs-t0 = ref PoolHistory[FRAME_NUMBER - 1] ; Previous frame result is available.
endif
```

See [Per-Object State](../examples/per-object-state.md) for a complete example.
