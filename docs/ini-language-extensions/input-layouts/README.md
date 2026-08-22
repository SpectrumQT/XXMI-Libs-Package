# Input Layouts

**XXMI DLL** provides setters for overriding vertex input-layout elements.

An input-layout element is identified by its semantic name and semantic index. Its DXGI format and aligned byte offset can be overridden independently.

These overrides affect how the input assembler interprets vertex data. See [D3D11_INPUT_ELEMENT_DESC](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_input_element_desc) for the corresponding D3D11 input-layout structure.

## Element Format

### `ElementFormat(SEMANTIC_NAME, $index)`

Overrides the DXGI format of a vertex input element. Input slot is derived from pipeline slot token.

`SEMANTIC_NAME` identifies the HLSL semantic associated with the element, while `$index` specifies its semantic index. See [HLSL Semantics](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-semantics) for more information about semantics and semantic indices.

```ini
vb0->ElementFormat(BLENDINDICES, 0) = R16G16B16A16_UINT
```

This overrides the format of the `BLENDINDICES0` element with `R16G16B16A16_UINT`.

The format corresponds to the `Format` field of the D3D11 input-element description. See [DXGI_FORMAT](https://learn.microsoft.com/en-us/windows/win32/api/dxgiformat/ne-dxgiformat-dxgi_format).

## Element Offset

### `ElementOffset(SEMANTIC_NAME, $index)`

Overrides the aligned byte offset of a vertex input element.

The offset is specified in bytes and corresponds to the `AlignedByteOffset` field of the D3D11 input-element description.

```ini
vb0->ElementOffset(BLENDWEIGHTS, 0) = 8
```

This overrides the offset of the `BLENDWEIGHTS0` element to `8` bytes from the beginning of the vertex.

```
