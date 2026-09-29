### Per-Object State

Suppose a game draws many instances of the same mesh, and we want to keep the previous frame's transform of every instance to compute motion vectors, without knowing in advance how many instances there are.

A `fifo` pool can track each instance by a key. Here the identity of the per-object constant buffer `vs-cb1` is used as the key, assuming the game keeps one buffer per instance. Each pool element stores the buffer from the previous frame as its resource, and the frame it was captured in as its variable:

```ini
[PoolObjects]
pool_size = 32
pool_index_type = fifo
pool_element_type_switch_reset = 0 ; Each element keeps both a resource and a variable.
pool_expiration_timeout_frames = 1 ; Forget instances that were not drawn for more than 1 frame.
pool_variable_default_value = -1   ; -1 marks an instance that has not been seen yet.

[TextureOverrideObject]
hash = 12345678
$object_id = @vs-cb1

if $PoolObjects[$object_id] == FRAME_NUMBER
    ; Second draw of this instance in the same frame, previous transform is already saved.
elif $PoolObjects[$object_id] == FRAME_NUMBER - 1
    ; Instance was drawn last frame, its previous transform is available.
    cs-cb1 = ref PoolObjects[$object_id]
    run = CommandListMotionVectors
endif

; Remember the current transform for the next frame.
PoolObjects[$object_id] = copy vs-cb1
$PoolObjects[$object_id] = FRAME_NUMBER

[KeyReset]
key = F9
run = CommandListReset

[CommandListReset]
PoolObjects[*] = null  ; Set all saved buffers to null.
$PoolObjects[*] = null ; Mark every element as unseen (-1).
```

How the pool options work together:

* `pool_index_type = fifo` maps each `$object_id` to its own element. Once all 32 elements are taken, a new instance evicts the oldest one.
* `pool_variable_default_value = -1` makes `$PoolObjects[$object_id]` return `-1` for an instance that is not tracked, so the `if` falls through and the instance is simply recorded.
* `pool_element_type_switch_reset = 0` allows `PoolObjects[$object_id]` and `$PoolObjects[$object_id]` to be assigned independently. With the default `1`, the `$PoolObjects[...]` assignment would set the resource that was just copied to `null`.
* `pool_expiration_timeout_frames = 1` expires elements that were not assigned for more than one frame. An instance that stops being drawn is forgotten: its key no longer resolves, its resource is set to `null` and its variable back to `-1`, so a stale transform is never paired with a new instance that happens to reuse the same key.

With this timeout, an element that is still tracked was always updated in the previous frame, so the `elif` condition is equivalent to checking that the instance is tracked at all. The explicit `FRAME_NUMBER - 1` comparison keeps the logic correct if the timeout is raised, for example to survive frames where the instance is culled.
