# Pools

Pools provide indexed storage for INI scripting entities.

A pool manages a fixed number of elements that can be accessed using different indexing strategies. Every element can store a resource, an INI variable, or both.

## Topics

- [Declaration](declaration.md) — all `pool_` options: size, index type, initialization, expiration, element types and variable defaults
- [Indexing](indexing.md) — Ring, Static, FIFO and Spatial indexing
- [Pool Metadata](pool-metadata.md) — pool and element identity, indices, size, etc
- [Pool Operations](pool-operations.md) — pool references (proxies), descriptor copies and resets
- [Resources](resources.md) — pool resource templates, access, assignment, ranges and usage
- [Variables](variables.md) — pool variable access, assignment, full range operations and persistence

## Examples

- [Pool-Based Tracker](../examples/pool-based-tracker.md) — keep resource results from the last N frames using a Ring pool
- [Per-Object State](../examples/per-object-state.md) — track per-object variables and resources with a FIFO pool and expiration