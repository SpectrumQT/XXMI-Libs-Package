# Pool Operations

A pool itself, without an element index, can be the target of a few operations that affect the pool as a whole.

| Operation                    | Effect                                                                 |
| ---------------------------- | ---------------------------------------------------------------------- |
| `PoolFoo = ref PoolBar`      | Turn `PoolFoo` into a proxy of `PoolBar`. See [Pool Reference](#pool-reference). |
| `PoolFoo = copy_desc PoolBar`| Copy the configuration of `PoolBar` into `PoolFoo`. See [Pool Descriptor Copy](#pool-descriptor-copy). |
| `PoolFoo = null`             | Drop the proxy, or clear the pool's index state. See [Pool Reset](#pool-reset). |

`copy` is not supported between pools: elements have to be copied one by one or via [ranges](resources.md/#pool-resource-ranges).

## Pool Reference

### `PoolFoo = ref PoolBar`

Turns `PoolFoo` into a proxy of `PoolBar`. From then on, every access to `PoolFoo` is redirected to `PoolBar`, including element resources, variables, ranges, [metadata](pool-metadata.md) and full range operations:

```ini
[PoolFoo]
pool_size = 4

[PoolBar]
pool_size = 8

[CommandListFoo]
PoolFoo = ref PoolBar

PoolFoo[0] = ref vs-cb0   ; Assigns PoolBar[0]
$x = $PoolFoo[3]          ; Reads $PoolBar[3]
$size = PoolFoo->Size     ; 8
```

`ref` is the default copy type, so `PoolFoo = PoolBar` is equivalent.

The proxy always resolves to the root of the chain: if `PoolBar` is itself a proxy of `PoolBaz`, `PoolFoo` is redirected to `PoolBaz` directly. A reference that would form a cycle (`PoolBar = ref PoolFoo` while `PoolFoo` is a proxy of `PoolBar`) is ignored with a warning.

The proxy is a runtime state and can be changed at any time. This allows a command list to be written against one pool name and switched between several backing pools:

```ini
if $mode == 1
    PoolActive = ref PoolLeft
else
    PoolActive = ref PoolRight
endif

; Same code for both pools:
ps-t[0:3] = ref PoolActive[0:3]
```

The bind flags of the `[PoolFoo]` template are propagated to `PoolBar` at parse time, so resources of `PoolBar` can be bound everywhere `PoolFoo` is bound.

`PoolFoo`'s own elements are not touched by the reference and become accessible again once the proxy is dropped with `PoolFoo = null`.

> `@PoolFoo` returns the identity of the pool it currently resolves to, so `@PoolFoo == @PoolBar` checks whether `PoolFoo` is a proxy of `PoolBar`.

## Pool Descriptor Copy

### `PoolFoo = copy_desc PoolBar`

Copies the configuration of `PoolBar` into `PoolFoo`, without copying any elements:

* `pool_size`, `pool_index_type` and `pool_lazy_initialization`
* `pool_spatial_radius`
* `pool_expiration_timeout_frames` and `pool_expiration_reset_elements`
* [Template resource](resources.md/#template-resource) options and initial data
* [Variable template](variables.md): `pool_variable_default_value` and `pool_persist_variables`

If `PoolFoo` was a proxy, the proxy is dropped first. The index state of `PoolFoo` is cleared as with `PoolFoo = null`, and the pool is re-initialized with the new size.

Elements that were already initialized keep their contents. With `pool_lazy_initialization = 0` in the copied configuration, every element is re-initialized from the new templates immediately.

```ini
[PoolTemplateA]
pool_size = 16
pool_index_type = fifo
pool_expiration_timeout_frames = 2

[PoolWork]
pool_size = 1

[CommandListSetup]
PoolWork = copy_desc PoolTemplateA ; PoolWork is now a 16 element FIFO pool with expiration.
```

## Pool Reset

### `PoolFoo = null`

If `PoolFoo` is a proxy, drops the proxy and does nothing else: the pool's own elements and index state are back as they were before `PoolFoo = ref ...`.

Otherwise, clears the pool's index state:

* `fifo` and `spatial` pools forget all key assignments, and the FIFO eviction order restarts from the first element.
* [Expiration](declaration.md/#element-expiration) records are cleared, so every element counts as expired. With `pool_expiration_reset_elements = 1` (default), all elements are reset on the next access to the pool.

Element resources and variables are not reset by the operation itself. Use the [full range operations](variables.md/#full-range-operations) for that:

```ini
PoolFoo = null     ; Forget keys and expiration.
PoolFoo[*] = null  ; Set every resource to null.
$PoolFoo[*] = null ; Set every variable to pool_variable_default_value.
```
