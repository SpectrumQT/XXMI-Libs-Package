# Filter Metadata

Filter metadata can be accessed using two operators:

* `@` returns the identity of a filter or resolved filter proxy chain root.
* `->` returns attributes.

## Filter Identity (`@`)

Returns a numeric identity value associated with the underlying Filter object.

```ini
$filter_id = @FilterTexture
```

- Returns `0` when the filter has no fields declared.
- Otherwise, returns a pointer-derived numeric identity.
- For proxy filters, the identity corresponds to the resolved root filter.

The identity represents the Filter object, not its attributes metadata.

## Filter Attributes (`->`)

### `Format`

Returns the numeric `DXGI_FORMAT` enum value specified by `match_format`.

```ini
$format = FilterTexture->Format
```

If `match_format` is not specified, the result is `DXGI_FORMAT_UNKNOWN`.

### `Confidence`

Returns the configured `match_confidence` value as a floating-point number.

```ini
$confidence = FilterTexture->Confidence
```

If `match_confidence` is omitted, the result is the default value, `0.75`.
