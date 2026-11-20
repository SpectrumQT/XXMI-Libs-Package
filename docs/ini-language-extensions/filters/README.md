# Filters

The **XXMI DLL** provides the `Filter` section for defining reusable criteria that identify existing resources.

Filters are currently implemented as a minimal, texture-focused feature. Their primary use is identifying textures bound to pixel-shader resource slots (`ps-t`) by their dHash (difference hash), allowing texture overrides based on visual content rather than fixed shader slot indices or resource hashes.

This texture-filtering implementation is an initial step, not the intended limit of the feature. In future versions, Filters are planned to match against the full resource metadata, enabling broader resource-identification scenarios beyond texture dHash matching.

A `Filter` describes **what to match**; a [Resource section](../resources/README.md) describes **what to bind as a replacement**. Filters identify resources already used by the game, while Resources provide the replacements.

## Topics

- [Declaration](filter-declaration.md) — how to define a Filter and specify its matching criteria.
- [Metadata](filter-metadata.md) — metadata exposed by Filter sections.
- [Operations](filter-operations.md) — operations for applying and working with Filter matches.
- [Texture Filtering](texture-filtering.md) — practical guide to replacing textures using Filters.
- [Texture Filtering — Advanced](texture-filtering-advanced.md) — dHash similarity, Hamming distance, and confidence tuning.
