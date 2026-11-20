# Custom Textures Filtering

The implementation below is unoptimized equivalent of built-in **XXMI DLL** dHash-based textures replacement strategy, described by [Textures Filtering](../filters/texture-filtering.md) page.

It uses a lot of the recently added XXMI DLL INI syntax, so should form a decent example.

> Fun fact: Actually it's the final form of related research and originally was planned to be bundled in model importer's core as INI API, but after some discussions and functionality formalization it was deemed sensible to be hardcoded to C++.

```ini
; ------------------------------------------------------------------------------
; - TEXTURE SLOTS ITERATION HELPER
; ------------------------------------------------------------------------------

[Constants]
global $slot_id = 0
global $last_slot_id = 0

[CommandListCallback_OnTexture]

[CommandListIterateTextureSlots]
$slot_id = ps->srv_first_slot
$last_slot_id = ps->srv_last_slot
if $slot_id >= 0
    run = CommandList_IterateTextureSlotsRecursive_Internal
endif

[CommandList_IterateTextureSlotsRecursive_Internal]
if ps->srv_type($slot_id) > 0
    run = CommandListCallback_OnTexture
endif
$slot_id = $slot_id + 1
if $slot_id <= $last_slot_id
    run = CommandList_IterateTextureSlotsRecursive_Internal
endif


; ------------------------------------------------------------------------------
; - INITIAL TEXTURES SNAPSHOTTING
; ------------------------------------------------------------------------------

; Stores the original texture reference for each pixel-shader texture slot.
;
; The INI var associated with each entry ($PoolInitialTextures) is also
; used to track the best Hamming distance found for that slot.
;
; A value of 65 means that no valid match has been recorded yet.
[PoolInitialTextures]
pool_size = 128
pool_element_type_switch_reset = 0


; Stores the Hamming distance calculated for each slot during the current matching pass.
; A value of 65 indicates that no valid distance was calculated.
[PoolHammingDistances]
pool_size = 128


; Save the current texture reference and reset the slot's best-match distance.
[CommandListSnapshotInitialTexture]
PoolInitialTextures[$slot_id] = ref ps-t[$slot_id]
$PoolInitialTextures[$slot_id] = 65

; Snapshot the original texture references for all texture slots used by the current pixel shader.
[CommandListSnapshotInitialTextures]
if $use_dll_strategy
    run = SnapshotInitialTextures
else
    CommandListCallback_OnTextureSlot = ref CommandListSnapshotInitialTexture
    run = CommandListIterateTextureSlots
endif


; Restore the texture reference saved by the most recent snapshot.
; Also reset the slot's best-match distance so a subsequent override pass can
; find a new match without being affected by previous results.
[CommandListRestoreInitialTexture]
ps-t[$slot_id] = ref PoolInitialTextures[$slot_id]
$PoolInitialTextures[$slot_id] = 65

; Restore the original textures in all texture slots used by the current pixel shader.
[CommandListRestoreInitialTextures]
if $use_dll_strategy
    run = RestoreInitialTextures
else
    CommandListCallback_OnTextureSlot = ref CommandListRestoreInitialTexture
    run = CommandListIterateTextureSlots
endif


; ------------------------------------------------------------------------------
; - REPLACEMENT STRATEGY IMPLEMENTATION
; ------------------------------------------------------------------------------

[Constants]
global $best_hamming_distance = 0
global $use_dll_strategy = 1


[ResourceTexture]
[FilterTexture]


; Overrides best matching texture with ResourceTexture across all ps-t slots declared by the current pixel shader.
;
; Matching is based on Hamming distance between dhash of texture in ps-t slot and `original_dhash` of ResourceTexture.
; Hamming distance can be read as "visual similarity", where Hamming distance of 0 is 100% similarity, and 64 is 0%.
; Default matching threshold is Hamming distance of 16 (75% similarity), and can be adjusted via $hamming_distance.
[CommandListOverrideBestMatchingTexture]
if $use_dll_strategy
    ps-t[FilterTexture] = ref ResourceTexture
else
    ; Maximum acceptable Hamming distance.
    ; A 64-bit hash has a possible distance of 0–64:
    ;   0  = identical hashes
    ;   16 = 75% bit agreement
    ;   64 = no matching bits
    ; The threshold is inclusive: a distance equal to this value is accepted.
    local $hamming_threshold = 64 - 64 * FilterTexture->Confidence

    ; PASS 1. Calculate Hamming distances between `original_dhash` of ResourceTexture and texture in each ps-t slot.
    ; The smallest Hamming distance will be stored in $best_hamming_distance.
    $best_hamming_distance = 65
    CommandListCallback_OnTextureSlot = ref CommandListCalculateHammingDistance
    run = CommandListIterateTextureSlots

    ; PASS 2. Replace textures in ps-t slots with ResourceTexture when their Hamming distance is equal to $best_hamming_distance.  
    if $best_hamming_distance <= $hamming_threshold
        CommandListCallback_OnTextureSlot = ref CommandListOverrideMatchingTexture
        run = CommandListIterateTextureSlots
    endif
endif


; Calculate the Hamming distance between the original texture in the current slot and ResourceTexture.
;
; Only textures with a matching format are compared.
; A negative distance indicates that the comparison failed or produced an invalid result.
;
; The calculated distance is saved per slot.
; The smallest valid distance across all slots is stored in $best_hamming_distance.
[CommandListCalculateHammingDistance]
local $hamming_distance = 65
if ps-t[$slot_id]->Format == FilterTexture->Format
    ; Compare the original texture in this slot with the candidate resource.
    $hamming_distance = PoolInitialTextures[$slot_id]->HammingDistance(FilterTexture)
    ; Treat an invalid comparison as an unavailable match.
    if $hamming_distance < 0
        $hamming_distance = 65
    ; Keep track of the best distance across all slots.
    elif $hamming_distance < $best_hamming_distance
        $best_hamming_distance = $hamming_distance
    endif
endif
; Store this slot's result for the second pass.
$PoolHammingDistances[$slot_id] = $hamming_distance


; Replace the texture in the current slot if:
;   1. Its distance equals the best distance found across all slots.
;   2. Its distance improves on the best distance previously recorded for this slot.
[CommandListOverrideMatchingTexture]
local $hamming_distance = $PoolHammingDistances[$slot_id]
if $hamming_distance == $best_hamming_distance
    ; Prevents less accurate match from replacing a better match already assigned to this slot.
    if $hamming_distance < $PoolInitialTextures[$slot_id]
        $PoolInitialTextures[$slot_id] = $hamming_distance
        ps-t[$slot_id] = ref ResourceTexture
    endif
endif

```
