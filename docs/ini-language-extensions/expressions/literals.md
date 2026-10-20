# Literals

## Binary Literals

Binary integer literals use the `0b` prefix.

```ini
global $var = 0b01010111
```

The maximum binary literal length is **24 bits**.

This limitation is due to the use of IEEE-754 `float32` values as the underlying container for INI variables. The supported 24-bit range allows binary integer values to be represented without losing integer precision.

## D3D11_BIND_FLAGS Enum Literals

D3D11 bind flags literals use the `D3D11_BIND_` prefix and evaluate to the corresponding integer value from the [D3D11_BIND_FLAG Enumeration](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ne-d3d11-d3d11_bind_flag).

They are particularly useful with the [->BindFlags getter](../resources/resource-metadata.md/#bindflags):

```ini
$is_render_target = ps-t0->BindFlags & D3D11_BIND_RENDER_TARGET
```

## DXGI_FORMAT Enum Literals

DXGI format literals use the `DXGI_FORMAT_` prefix and evaluate to the corresponding integer value from the [DXGI_FORMAT Enumeration](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format).

They are particularly useful with the [->Format getter](../resources/resource-metadata.md/#format):

```ini
$format_matches = ps-t0->Format == DXGI_FORMAT_R32G32B32A32_FLOAT
```
