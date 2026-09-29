# Examples

Practical scripts that combine several INI language extensions.

* [Binary Flags](binary-flags.md) — pack up to 24 flags into one variable with bitwise operators
* [Pool-Based Tracker](pool-based-tracker.md) — keep resource results from the last N frames using a Ring pool
* [Per-Object State](per-object-state.md) — track per-object variables and resources with a FIFO pool and expiration
* [Slot Range Capture](slot-range-capture.md) — save, rebind and restore a whole range of pipeline slots through a pool
* [Slot Scan](slot-scan.md) — loop over slots with a dynamic slot index and resource metadata
* [Command List Callbacks](command-list-callbacks.md) — expose hook points between namespaces with proxy command lists
* [GPU Readback](gpu-readback.md) — read a value from a GPU resource with `store` without stalling every frame
* [Constant Buffer Region](constant-buffer-region.md) — bind or copy part of a constant buffer with `->Region` and `->Offset`
