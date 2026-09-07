# Pools

Pools provide indexed storage for INI scripting entities.

A pool manages a fixed number of elements that can be accessed using different indexing strategies. Pools can store resources and, soon, INI variables.

## Topics

- [Declaration](declaration.md) — pool configuration and element initialization
- [Indexing](indexing.md) — Ring and FIFO indexing
- [Pool Metadata](pool-metadata.md) — pool and element identity, indices, size, etc
- [Resources](resources.md) — pool resource templates, access, assignment, and usage

## Examples

- [Pool-Based Tracker](../examples/pool-based-tracker.md) — keep resource results from the last N frames using a Ring pool