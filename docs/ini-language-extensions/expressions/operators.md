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

## Compound Assignment Operators

Any binary operator can be combined with `=` to apply it to the current value of the target:

```ini
$x += 1      ; $x = ($x) + (1)
$x *= 2 + 3  ; $x = ($x) * (2 + 3)
x0 |= 4      ; x0 = (x0) | (4)
```

Supported operators: `+=`, `-=`, `*=`, `/=`, `//=`, `%=`, `**=`, `<<=`, `>>=`, `&=`, `|=`, `^=`, `&&=` and `||=`.

The right-hand side is evaluated as a whole before the operator is applied. The target can be an INI variable or an IniParam, and the `pre` and `post` keywords are accepted.

## Increment and Decrement Operators

`++` and `--` add or subtract `1`. Prefix and postfix forms are equivalent, as both are statements rather than expressions:

```ini
$x++  ; $x = ($x) + 1
--$x  ; $x = ($x) - 1
```

Compound assignments, increments and decrements are rewritten into plain assignments when the INI is parsed, so they have no runtime overhead. The frame analysis log shows the line as written.
