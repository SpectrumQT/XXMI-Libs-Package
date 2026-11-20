# Filter Operations

A filter can be the target of a few operations that affect the filter as a whole.

## Filter References

A Filter section can act as a proxy to another Filter section.

### `FilterFoo = ref FilterBar`

Turns `FilterFoo` into a proxy to `FilterBar`.

While proxy mode is active, operations using `FilterFoo` resolve to the referenced filter `FilterBar`.

```ini
[FilterFoo]

[FilterBar]
match_format = DXGI_FORMAT_BC7_UNORM

[CommandListExample]
$format = FilterFoo->Format ; Returns FilterFoo's default enum value `DXGI_FORMAT_UNKNOWN`
FilterFoo = ref FilterBar
$format = FilterFoo->Format ; Returns proxied FilterBar's enum value `DXGI_FORMAT_BC7_UNORM`
```

References can also be used to share filter definitions between command lists and avoid repeating matching criteria.

## Filter Proxy Reset

### `FilterFoo = null`

Disables proxy mode on `FilterFoo`.

After proxy mode is unset, `FilterFoo` uses its own declared filter fields again.

```ini
[CommandListExample]
$format = FilterFoo->Format ; Returns FilterFoo's default enum value `DXGI_FORMAT_UNKNOWN`
FilterFoo = ref FilterBar
$format = FilterFoo->Format ; Returns proxied FilterBar's enum value `DXGI_FORMAT_BC7_UNORM`
FilterFoo = null ;
$format = FilterFoo->Format ; Returns FilterFoo's default enum value `DXGI_FORMAT_UNKNOWN`
```
