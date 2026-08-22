# INI Runtime Parameters

INI expressions provide access to runtime parameters that are evaluated at the time the expression is executed.

## `FRAME_NUMBER`

Returns the number of the current frame.

The frame boundary is defined by the `Present` call.

## `DRAW_NUMBER`

Returns the number of the current draw call within the frame.

The counter starts at `1` for each new frame.

## `DISPATCH_NUMBER`

Returns the number of the current compute dispatch within the frame.

The counter starts at `1` for each new frame.
