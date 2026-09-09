# Binary Flags

This example uses a single INI variable to store and manipulate up to 24 independent binary flags using [bitwise and bitshift operators](../expressions/operators.md).

```ini
; ID of current flag, from 0 to 23
local $flag_id = 0

; Flags storage
local $flags = 0

; Bitwise mask for the calculations below
local $flag = 1 << $flag_id

; Check if $flag_id has `0` state in $flags
if ($flags & $flag) == 0
    ; Store `1` state for $flag_id in $flags
    $flags = $flags | $flag
endif

; Check if $flag_id has `1` state in $flags
if ($flags & $flag) != 0
    ; Store `0` state for $flag_id in $flags
    $flags = $flags & ~$flag
endif
```