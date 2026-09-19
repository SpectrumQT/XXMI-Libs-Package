# Command Lists

## Recursion Limit

The command-list recursion limit is increased to **256**, from the original 3DMigoto limit of **64**.

With the additional INI runtime features implemented in the XXMI DLL, the original limit is likely to be reached by legitimate usage.

The limit applies to nested command-list execution and prevents excessively deep or accidental infinitely recursive command-lists.

## Proxy Command Lists

Command lists support **proxying by reference**, similar to copy-by-reference for resources.

A proxy command list acts as an entry point to another command list. When executed, it transparently forwards execution to the referenced command list without copying its commands.

This is especially useful for **running commands from other namespaces while maintaining scope isolation**. A command list can expose a small, stable interface to functionality implemented elsewhere, without requiring the caller to directly access or depend on the target namespace's internal command lists.

This makes proxy command lists useful for:

* **Scope isolation** — keep implementation details contained within another namespace.
* **Stable entry points** — callers can depend on a local command-list name even if the underlying implementation changes.
* **Callbacks** — expose a command-list entry point whose implementation can be redirected at runtime.
* **Composition** — connect command lists from different namespaces without duplicating their commands.
* **Chaining** — redirect through multiple proxy layers when necessary.

The proxy does not copy the target command list. It only redirects execution to it, so changes to the referenced command list are automatically reflected when the proxy is run.

### Set Proxy Reference

A command list reference can be assigned to another command list:

```ini
CommandListA = ref CommandListB
```

After assignment above, when `CommandListA` is run, it executes `CommandListB` instead:

```ini
run = CommandListA ; Runs CommandListB
```

If `CommandListA` already contains its own commands, those commands will not be executed as long as the proxy reference is active.

> Non-empty CommandList from different namespace cannot be overriden this way.

### Clear Proxy Reference

A proxy reference can be cleared by assigning `null`:

```ini
CommandListA = null
```

After the proxy reference is cleared, `CommandListA` behaves normally again:

```ini
run = CommandListA ; Runs CommandListA
```

If `CommandListA` contains its own commands, clearing the proxy reference makes those commands runnable again.

### Chaining

Proxy command lists can be chained:

```ini
CommandListA = ref CommandListB
CommandListB = ref CommandListC
```

Running `CommandListA` follows the chain and ultimately executes `CommandListC`:

```ini
run = CommandListA ; Runs CommandListC
```

This makes command-list indirection useful for building callback-based APIs, where a command list can serve as a stable entry point while its implementation is redirected or replaced elsewhere.

### Circular References

Daisy-chains are validated automatically at runtime.

If a proxy assignment would create a circular reference, the assignment is treated as a **no-op**. This prevents future `run` calls from following the chain indefinitely.

For example:

```ini
CommandListA = ref CommandListB
CommandListB = ref CommandListA ; Would create a circular reference
```

The second assignment effectively short-circuits, so the assignment is ignored:

```ini
CommandListB = ref CommandListA
; No-op
```

Therefore:

```ini
run = CommandListA
; Runs CommandListB
```

The proxy resolution stops safely rather than looping between `CommandListA` and `CommandListB`.

This also applies to longer chains. For example:

```ini
CommandListA = ref CommandListB
CommandListB = ref CommandListC
CommandListC = ref CommandListA ; No-op
```

The circular reference creation is avoided, leaving the already-established chain intact.

## Slot Operation Batching

The command list optimiser merges adjacent lines that bind or fetch shader resource slots (`t` slots) of the same shader stage into a single D3D11 call, such as `PSSetShaderResources` or `PSGetShaderResources`.

For example:

```ini
ps-t0 = ref ResourceFoo
ps-t1 = ref ResourceBar
ps-t2 = ref PoolFoo[$index]
ps-t3 = null
```

is executed as one `PSSetShaderResources` call covering `ps-t0`–`ps-t3`, and:

```ini
ResourceFoo = ref ps-t0
ResourceBar = ref ps-t1
```

is executed as one `PSGetShaderResources` call.

A line is batched when:

* For binds, the destination is a `t` slot and the source is a custom resource, a pool resource, or `null`.
* For fetches, the source is a `t` slot and the destination is a custom resource or a pool resource.

Both `ref` and `copy` lines are batched, with any copy options. Each line still performs its own copy and creates its own view, only the final bind or fetch call is shared.

A run of batched lines ends at any other line, at a line for a different shader stage, or when switching between binds and fetches. Slots do not need to be listed in order, but each batch covers a contiguous slot range, so a gap in the slot numbers splits the run into separate batches.

> Binding a pipeline slot to another pipeline slot (`ps-t1 = ref ps-t0`) is never batched, since reading all sources before binding any of them would change the meaning of a slot swap.

An `if`/`elif`/`else` chain is batched as well when every branch consists of exactly one such line for the same slot. The final `else` may be omitted, in which case the slot keeps its current binding when no condition matches:

```ini
ps-t0 = ref ResourceFoo
if $variant == 1
    ps-t1 = ref ResourceBarA
elif $variant == 2
    ps-t1 = ref ResourceBarB
else
    ps-t1 = ref ResourceBarC
endif
ps-t2 = ref ResourceBaz
```

Batching does not change the meaning of the lines. Sources are resolved in INI order, a slot bound twice within a batch takes the last value, and `unless_null` keeps the current binding for slots whose source is `null`. A batch using `unless_null` reads the current bindings first, so it may span gaps in the slot numbers while leaving the slots in between untouched.

The optimiser logs the number of lines saved by batching to `d3d11_log.txt`:

```
Merged 3 slot operations into batches in [CommandListFoo]
```
