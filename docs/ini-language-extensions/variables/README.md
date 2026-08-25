# Variables

Variables store values that can be assigned and referenced by INI scripts.

## Variable Declaration

Global variables can be initialized using expressions. Expressions are evaluated when the declaration is parsed.

```ini
[Constants]
global $foo = 1
global $bar = $foo + 1

[CommandListFoo]
local $var = $bar + 1
```

The value of `$foo` is evaluated at parse time, and the resulting value is assigned to `$bar`.

Variables can also be used in most part of section options, since XXMI DLL introduce expressions support for them.

## Locked Variables

The `locked` keyword prevents a variable from being used as an assignment target.

For example:

```ini
global locked $var = 1.23
```

The variable can still be read as a value:

```ini
$x = $var + 1
```

but cannot be assigned a new value after its declaration, any attempt to do so will be ignored and trigger INI parser warning:

```ini
$var = 1 ; No-op
```

A locked variable is useful when a value should be initialized once and then treated as read-only.
