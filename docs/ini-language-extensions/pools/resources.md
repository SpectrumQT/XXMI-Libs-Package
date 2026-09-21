# Resources

## Template Resource

For resource-type elements, the pool uses an internal **template resource** to initialize each element. By default, the template uses the same option defaults as a `[Resource]` section with no options specified.

When a pool is parsed:

1. An internal **template resource** is created.
2. It stores all resource options and (optional) initial data.
3. Individual pool resources are instantiated from this template when first used.

To override template defaults, specify any `[Resource]` section options directly in the `[Pool]` section:

```ini
[PoolFoo]
pool_size = 4

type = Buffer
data = "Hello World!"
```

Every resource created within the pool inherits the template configuration and initial data.

## Pool Resource Access

A pool element resource is accessed using its element index in square brackets.

The element index is evaluated as an INI expression at runtime:

```ini
PoolFoo[0]
PoolFoo[$index]
PoolFoo[$index + 1]
```

### Reading an Unknown FIFO UID

When unassigned UID is requested for [FIFO-indexed](indexing.md/#fifo-indexing) pool, **template resource** is returned.

## Pool Resource Assignment

Pool element resources support assignment in the same way as resources declared in a `[Resource]` section.

Both copy types are supported:

```ini
PoolFoo[$index] = ref ResourceText  ; Reference Copy (cheap).
PoolFoo[$index] = copy ResourceText ; Full Copy (expensive).
```

> `ref` performs a reference copy by pointing to the underlying D3D resource, while `copy` creates a full copy of the underlying resource.

> Assignment to a pool resource resets the variable of the same element unless `pool_element_type_switch_reset = 0` is set. See [Element Types](declaration.md/#element-types).

## Pool Resource Ranges

### Slot Ranges

A range of pool elements can be bound to, or fetched from, a range of pipeline slots with a single operation:

```ini
ps-t[0:3] = ref PoolFoo[0:3]
PoolFoo[0:3] = ref ps-t[0:3]
```

Pool range bounds are always element indices that wrap like [Ring](indexing.md/#ring-indexing) indices, whatever the pool's index type. On a `fifo` or `spatial` pool a range addresses the physical elements directly and bypasses key lookup, like the [full range operations](variables.md/#full-range-operations) do.

See [Slot Indexing → Slot Ranges](../resources/slot-indexing.md/#slot-ranges) for details.

### Full Range Reset

`PoolFoo[*]` addresses the resources of **all** pool elements at once. It is only valid as the target of a `null` assignment:

```ini
PoolFoo[*] = null ; Set every pool resource to null.
```

Variables of the pool are left untouched, use `$PoolFoo[*] = null` to reset those. See [Variables → Full Range Operations](variables.md/#full-range-operations).

## Pool Resource Usage

Pool element resource can be used anywhere a custom resource is accepted.

```ini
PoolFoo[$index] = copy cs-t0
vs-t0 = ref PoolFoo[$index]
```

Creating or initializing a pool element does not immediately instantiate its underlying D3D resource.

Like resources declared in a [Resource] section, a pool resource is instantiated only when first used.

As a result, initialized pool elements do not consume VRAM until their underlying resources are required at runtime, such as when bound to a pipeline slot.
