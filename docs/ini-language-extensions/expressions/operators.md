# Operators

## Precedence

For bitwise and bitshift operators, precedence is the same as in C++.

## Limitations

INI expression operands are represented as floating-point values. Before being processed by bitwise or bitshift operators, operands are implicitly converted to signed integers.

### Integer Conversion

Fractional values are truncated during conversion.

For example:

```
1.1 → 1
-1.9 → -1
```

### Integer Precision

Integer precision is limited by the underlying IEEE-754 single-precision (`float`) representation.

All integers up to `2^24` (`16,777,216`) are represented exactly. Above this value, not every integer can be represented exactly, so integer values may lose precision.

Consequently, bitwise and bitshift operations should use values that fit within the reliably representable integer range. In particular, up to **24 binary flags** can be stored in a single INI variable, using bits `0` through `23`.

See [Binary Flags](../examples/binary-flags.md) for a practical example.

## Bitwise Operators

The following bitwise operators are supported:

| Operator | Operation |
| --- | --- |
| `~` | NOT |
| `&` | AND |
| `^` | XOR |
| `\|` | OR |

## Bitshift Operators

| Operator | Operation   |
| -------- | ----------- |
| `<<`     | Left shift  |
| `>>`     | Right shift |
