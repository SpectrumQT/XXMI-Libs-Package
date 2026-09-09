# Resource Options

## `override_color_space`

Controls the color space used when loading a texture resource.

By default, the texture's color space is determined by the texture format and/or metadata. 

`override_color_space` allows to explicitly override that behavior:
* `sRGB` — force the texture to be loaded as an sRGB texture.
* `Linear` — force the texture to be loaded as a linear texture.

```ini
[ResourceTexture]
filename = Texture.dds
override_color_space = sRGB
```
