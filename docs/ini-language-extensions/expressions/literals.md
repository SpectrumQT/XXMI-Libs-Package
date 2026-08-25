# Literals

## Binary Literals

Binary integer literals use the `0b` prefix.

```ini
global $var = 0b01010111
````

The maximum binary literal length is **24 bits**.

This limitation is due to the use of IEEE-754 `float32` values as the underlying container for INI variables. The supported 24-bit range allows binary integer values to be represented without losing integer precision.
