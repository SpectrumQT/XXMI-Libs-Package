# Slot Indexing

Pipeline slot targets such as `ps-t0` or `vb1` use a fixed slot number. **XXMI DLL** allows the slot number to be an expression evaluated at runtime.

## Dynamic Slot Index

### `<slot>[$index]`

The slot number is given as an expression in square brackets and evaluated every time the line runs.

```ini
ps-t[$index] = ref ResourceFoo
ResourceBar = ref vs-cb[$index + 1]
```

Supported slot types:

| Syntax            | Slot Type                                  | Valid Index |
| ----------------- | ------------------------------------------ | ----------- |
| `<stage>-t[$i]`   | Shader resource                            | `0`–`127`   |
| `<stage>-u[$i]`   | Unordered access view (`ps` and `cs` only) | `0`–`63`    |
| `<stage>-cb[$i]`  | Constant buffer                            | `0`–`13`    |
| `vb[$i]`          | Vertex buffer                              | `0`–`31`    |
| `o[$i]`           | Render target                              | `0`–`7`     |
| `so[$i]`          | Stream output                              | `0`–`3`     |

`<stage>` is one of `vs`, `hs`, `ds`, `gs`, `ps` or `cs`.

The index is truncated to an integer. An index outside the valid range logs a warning and the operation is skipped.

A dynamic slot index is accepted anywhere a pipeline slot is, including `CheckTextureOverride`, `dump` and expressions:

```ini
CheckTextureOverride = ps-t[$index]
$resource_id = @ps-t[$index]
$size = ps-t[$index]->Size
```

> [Input layout](../input-layouts/README.md) overrides require a fixed slot, so `vb[$index]->ElementFormat(...)` is not supported.
