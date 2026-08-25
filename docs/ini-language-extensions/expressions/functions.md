# Functions

Expressions support a set of HLSL-style unary functions. All functions use explicit function-call syntax with required `()` to provide unambiguous parsing.

Function behavior follows the corresponding HLSL functions where an equivalent exists.

## Bit Operations

* [`countbits`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/countbits) — counts the number of set bits in the input value.

## Trigonometry

* [`sin`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-sin) — returns the sine of the input value.
* [`cos`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-cos) — returns the cosine of the input value.
* [`tan`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-tan) — returns the tangent of the input value.
* [`asin`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-asin) — returns the arcsine of the input value.
* [`acos`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-acos) — returns the arccosine of the input value.
* [`atan`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-atan) — returns the arctangent of the input value.

## Numeric Helpers

* [`abs`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-abs) — returns the absolute value of the input.
* [`sign`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-sign) — returns the sign of the input value.
* [`ceil`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-ceil) — rounds the input value up to the nearest integer.
* [`floor`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-floor) — rounds the input value down to the nearest integer.
* [`trunc`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-trunc) — truncates the fractional portion of the input value.
* [`round`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-round) — rounds the input value to the nearest integer.
* [`frac`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-frac) — returns the fractional portion of the input value.

## Roots

* [`sqrt`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-sqrt) — returns the square root of the input value.
* [`rsqrt`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-rsqrt) — returns the reciprocal square root of the input value.

## Exponential and Logarithmic

* [`exp`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-exp) — returns the base-e exponential of the input value.
* [`exp2`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-exp2) — returns the base-2 exponential of the input value.
* [`log`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-log) — returns the natural logarithm of the input value.
* [`log2`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-log2) — returns the base-2 logarithm of the input value.

## Clamping

* [`saturate`](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-saturate) — clamps the input value to the range `[0, 1]`.

## Procedural Functions

### `random`

`random($max)` generates a deterministic pseudo-random value. The argument is required and specifies the maximum magnitude of the generated result.

* For a positive `$max`, the result is in the range `[0, $max)`.
* For a negative `$max`, the result is in the range `($max, 0]`.
* The supplied maximum value is never returned.

The generated value is based on a hash of the current frame number, system tick count, and an incrementing per-call counter. This allows multiple `random()` calls within the same frame to produce distinct values while remaining deterministic for the same execution state.

Example:

```ini
$offset = random(10)
$offset = random(-10)
```
