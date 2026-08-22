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
