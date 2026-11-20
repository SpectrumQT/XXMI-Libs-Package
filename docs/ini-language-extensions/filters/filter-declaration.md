# Filter Declaration

Filters are declared using a dedicated `[Filter]`-type section. Each filter section must have a unique name, such as `[FilterTexture]`.

```ini
[FilterTexture]
match_dhash = 1363895a02495a04
match_format = BC7_UNORM
match_confidence = 0.75
```

## Filter Options

| Option | Default | Description |
| ------ | -------- | ----------- |
| `match_dhash` | — | Reference texture dHash. |
| `match_format` | — | Restricts matching to slots whose texture SRV format matches the specified `DXGI_FORMAT`. |
| `match_confidence` | `0.75` | Minimum required dHash similarity, from `0.0` to `1.0`. |

## Difference Hash (DHash)

Specifies the 64-bit [dHash](texture-filtering-advanced.md/#1-what-is-dhash) used as the reference for texture matching.

The value can be obtained from the original texture's entry in the dumped `log.txt`.

Use `match_dhash`:
```ini
[FilterTexture]
match_dhash = 1363895a02495a04
```

A smaller [Hamming distance](texture-filtering-advanced.md/#2-what-is-hamming-distance) between the reference dHash and a candidate texture indicates a closer match.

## DXGI Format

Optionally restricts matching to textures with a particular DXGI format.

Use `match_format`:
```ini
[FilterTexture]
match_dhash = 1363895a02495a04
match_format = BC7_UNORM
```

When specified, slots with a different SRV format are excluded from matching.

## Confidence

Sets the minimum required [dHash similarity](texture-filtering-advanced.md#3-hamming-distance-as-a-similarity-score). The value ranges from `0.0` to `1.0`; the default is `0.75`.

Use `match_confidence`:
```ini
[FilterTexture]
match_dhash = 1363895a02495a04
match_confidence = 0.90
```

Higher values require a closer match. 

> See [Texture Filtering — Advanced](texture-filtering-advanced.md#4-how-match_confidence-becomes-a-distance-limit-and-how-to-tune-it) for practical tuning guidance and the confidence-to-Hamming-distance conversion.
