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

The right-hand side is evaluated as a whole before the operator is applied. The target can be an INI variable, a pool variable or an IniParam, and the `pre` and `post` keywords are accepted.

## Increment and Decrement Operators

`++` and `--` add or subtract `1` from an INI variable. As a statement on its own line, prefix and postfix forms are equivalent:

```ini
$x++  ; $x = ($x) + 1
--$x  ; $x = ($x) - 1
```

Inside an expression, the variable is updated when the expression is evaluated. The prefix form yields the new value, the postfix form the old one, as in C:

```ini
$a = ++$x       ; $x = $x + 1, then $a = $x
$b = $x++       ; $b = $x, then $x = $x + 1
$c = $PoolFoo[$i++]  ; pool index expressions count too
if $retries-- > 0
```

The operator must be written directly against the variable (`$x++`, not `$x ++`), and only INI variables can be incremented, not IniParams or pool variables. `$a--$b` is read as `$a-- $b`; write `$a - -$b` if that is what you mean.

Compound assignments, increments and decrements are parsed into the same expression as the equivalent plain assignment, so they have no runtime overhead. The frame analysis log shows the line as written.
