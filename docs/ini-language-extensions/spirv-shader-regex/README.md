# SPIR-V Shader Regex

**XXMI DLL** can patch the SPIR-V vertex and pixel shaders of Vulkan games with ShaderRegex groups, as 3DMigoto patches D3D11 assembly.

A ShaderRegex group with at least one `SpirvPattern` subsection is a SPIR-V group. D3D11 and D3D12 never apply it, and the Vulkan path applies nothing else, apart from the D3D11 patches it [converts](#d3d11-patches-of-mods-on-vulkan). Put SPIR-V groups in their own file: a DLL without this feature disables the group with a warning and does nothing else.

The feature needs `SPIRV-Tools-shared.dll` next to `d3d11.dll`. Without it SPIR-V groups patch nothing.

## Where the patched shader is used

* Only mod draws use it, never the game's own draws, and only draws that do not set their own `vs` or `ps`.
* Each stage is decided on its own, by the groups for that stage (their `shader_model`):
  * A mod draw wants the patched shader of a stage when it has a texture bound for that stage (`vs-tN` or `ps-tN`) at a `t` register that a group for the stage declares in `InsertDeclarations`.
  * If a group for the stage declares no texture (only the IniParams or a sampler), every mod draw wants the patched shader of that stage. Such a group has to keep the game's behaviour unless an IniParams value that the mod sets for its draw says otherwise, as the [vertex shader example](#vertex-shader-example) does.
  * The draw then uses the patched shader if the game's shader of that stage was patched. A draw can use a patched vertex shader and a patched pixel shader together.
* Shaders are patched and their pipelines are made on a worker thread, when a draw first needs them. Until then the draw uses the game's shaders, so the patch can be missing for the first frames.
* A declared texture is taken from the stage of the patched shader that reads it, from the pixel shader's if both do. If the game's shader of that stage already uses that `t` register, the patched shaders are not used for that draw.
* `vk_report.txt` counts the mod draws that wanted a patched shader. Draws that want one only because a group declares no texture count only when the game's shader of that stage is patched or still being patched.

## D3D11 patches of mods on Vulkan

The Vulkan path makes SPIR-V groups of the D3D11 ShaderRegex patches it recognises, each time the configuration is loaded, and uses them as if they were in the mod's folder. Nothing is written to the mod. D3D11, D3D12 and the other importers never run this.

* It knows these patches:
  * the cutout mask of `CutoutMask.ini` (Zhuang Fangyi Ink Cheongsam, Liino Moonlit Violet and their copies), with any IniParams index, mod id, mask register, channel and threshold;
  * `[ShaderRegexSmoothNormalColor]` of Si FGO lancer Melusine and its copies, with any IniParams index and mod id. It patches the outline vertex shader to take the normal made of the vertex colour when that IniParams value is set. The declaration checks of the D3D11 pattern become the `Pattern` subsection of the Vulkan group.
* A patch is not converted when its ini has sections of its own for Vulkan (a section name with `Vulkan` in it, as RabbitFX has), or when its folder has a SPIR-V group of its own: that group is used instead. Groups that only classify shaders need no conversion.
* `d3d11_log.txt` says what became of each patch:

```
ShaderRegex: [ShaderRegex\Mods\...\CutoutMask.ini\ZhuangCut] is used for Vulkan as [ShaderRegexZhuangCutVulkanV2] (cutout mask)
ShaderRegex: the D3D11 patch [...] is not used for Vulkan: no conversion knows this patch
```

  The converted groups are named after the D3D11 group with `Vulkan` added (and for the cutout mask the width of the TEXCOORD it reads, `V2` or `V4`). That is the name the log gives when one of them patches a shader.

## Syntax

```ini
[ShaderRegexMyCutout]
shader_model = ps_5_0
temps = c245 pini ini on end

[ShaderRegexMyCutout.Pattern]
dcl_input_ps linear v0\.xy\n

[ShaderRegexMyCutout.InsertDeclarations]
dcl_resource_texture1d (float,float,float,float) t120
dcl_resource_texture2d (float,float,float,float) t116
dcl_sampler s0, mode_default

[ShaderRegexMyCutout.SpirvPatternA]
^%zz_in_loc0 = OpVariable %zz_ptr_Input_v2float Input$

[ShaderRegexMyCutout.SpirvPatternB]
^@zz_entry$

[ShaderRegexMyCutout.SpirvPatternB.Replace]
...
@zz_entry
```

* `shader_model`: only the stage is compared. Only vertex shaders (`vs_*`) and pixel shaders (`ps_*`) are patched.
* `temps`: each `${name}` in a replacement becomes an id of its own for the group, `%zz<n>_name`.
* `Pattern` (optional): must match the D3D11-style declarations the DLL derives from the shader's reflection, such as `dcl_input_ps linear v0.xy`. A dynamically indexed array counts in full, so constant buffer sizes can differ from D3D11. It cannot have a `Replace`.
* `InsertDeclarations`: the resources the patched shader can read. See [Declarations](#declarations).
* `SpirvPattern*`: regular expressions on the shader's text, applied in name order. With a `Replace` subsection the first match is replaced; without one the pattern only has to match. If any pattern of the group does not match, the whole group is undone for that shader. Groups are applied in name order.
* Commands and `filter_index` in a SPIR-V group are not run. Put them in a separate group without `SpirvPattern`.

## The shader's text

The shader is disassembled with SPIR-V assembly syntax (`spirv-dis`) and then changed so that patterns do not depend on the game.

### Names

| Name | What it is |
|---|---|
| `%zz_main` | the entry point function |
| `%zz_in_loc<N>`, `%zz_out_loc<N>` | Input and Output variables at Location N |
| `%zz_in_<BuiltIn>`, `%zz_out_<BuiltIn>` | BuiltIn variables, for example `%zz_in_FragCoord` |
| `%zz_glsl` | the `GLSL.std.450` import, added if the shader has none |
| `%zz_void`, `%zz_bool`, `%zz_int`, `%zz_uint`, `%zz_float` | scalar types (other widths add the width, e.g. `%zz_float16`) |
| `%zz_v<2-4><scalar>` | vectors, e.g. `%zz_v2float`, `%zz_v4int` |
| `%zz_image2d_float`, `%zz_sampler`, `%zz_sampled_image2d_float` | sampled image types |
| `%zz_ptr_<Storage>_<type>` | pointers, e.g. `%zz_ptr_Input_v2float`, `%zz_ptr_Uniform_v4float` |
| `%IniParams`, `%t<N>`, `%s<N>` | the declared resources |

All other ids keep their numbers (`%123`). The scalar and vector types above, `%zz_ptr_Uniform_v4float` and the types of the declared resources are added when the shader lacks them, so replacements can always use them.

### Markers

| Marker | Where |
|---|---|
| `@zz_globals` | before the first function: constants and types go here |
| `@zz_entry` | in a new first block of `%zz_main`, before it branches to the shader's own first block: code that runs before the shader goes here |

Keep the marker line in the replacement if a later pattern needs it. Marker lines are removed before the shader is assembled. Code at `@zz_entry` may branch with `OpSelectionMerge`/`OpBranchConditional`, but it has to end in the block that the marker is in, so that the branch to the shader's first block follows it. In a pixel shader, `OpKill` discards the pixel. A vertex shader can use neither `OpKill` nor implicit-LOD sampling; it can read textures with `OpImageFetch` or `OpImageSampleExplicitLod`.

## Declarations

| Line | Becomes |
|---|---|
| `dcl_resource_texture1d (...) t<ini_params>` | `%IniParams`: a uniform block of `v4float[count]`. Read element N with `OpAccessChain %zz_ptr_Uniform_v4float %IniParams <int 0> <int N>`. The values are those at the draw. |
| `dcl_resource_texture2d (...) tN` | `%tN`: a `%zz_image2d_float` of the texture the draw has at tN of the shader's stage (`vs-tN` or `ps-tN`). If none is bound, its size reads as 0 and texels as 0. |
| `dcl_sampler sN, mode_default` | `%sN`: a linear sampler that repeats |

The register of `%IniParams` is the one `ini_params` in `[Rendering]` sets. Other declarations disable the group with a warning.

## Validation

The patched shader is assembled and has to pass the SPIR-V validator with the block layout rules the original shader passes with. A shader that fails is drawn as it is, and `vk_report.txt` notes the validator's message.

## Debugging

With `vk_dump_hashes = 1` in `[Hunting]`, these files are written:

* `vk_report.txt`: which section patched which shader (`the vertex shader ... is patched by [...]`, `the pixel shader ...`), why a shader could not be patched, and counts of the draws that used the patch or the game's shader instead.
* `vk_patched\<hash>-<vs|ps>.before.spvasm`: the text the patterns are applied to.
* `vk_patched\<hash>-<vs|ps>.after.spvasm`: the text after the patterns are applied.

## Pixel shader example

A cutout for a mask at `ps-t116`, on when `x245 = 245997` is set for the draw: the pixel is discarded where the mask's alpha at TEXCOORD0 is below 1.

```ini
[ShaderRegexCutout]
shader_model = ps_5_0
temps = c0 c245 cguid fone pini ini inix on check img size sx sy sum bound fetch uv frc sizef scaled coord texel alpha cut kill end end2 end3

[ShaderRegexCutout.InsertDeclarations]
dcl_resource_texture1d (float,float,float,float) t120
dcl_resource_texture2d (float,float,float,float) t116

[ShaderRegexCutout.SpirvPatternA]
^%zz_in_loc0 = OpVariable %zz_ptr_Input_v2float Input$

[ShaderRegexCutout.SpirvPatternB]
^@zz_globals$

[ShaderRegexCutout.SpirvPatternB.Replace]
${c0} = OpConstant %zz_int 0\n
${c245} = OpConstant %zz_int 245\n
${cguid} = OpConstant %zz_float 245997\n
${fone} = OpConstant %zz_float 1\n
@zz_globals

[ShaderRegexCutout.SpirvPatternC]
^@zz_entry$

[ShaderRegexCutout.SpirvPatternC.Replace]
${pini} = OpAccessChain %zz_ptr_Uniform_v4float %IniParams ${c0} ${c245}\n
${ini} = OpLoad %zz_v4float ${pini}\n
${inix} = OpCompositeExtract %zz_float ${ini} 0\n
${on} = OpFOrdEqual %zz_bool ${inix} ${cguid}\n
OpSelectionMerge ${end} None\n
OpBranchConditional ${on} ${check} ${end}\n
${check} = OpLabel\n
${img} = OpLoad %zz_image2d_float %t116\n
${size} = OpImageQuerySizeLod %zz_v2int ${img} ${c0}\n
${sx} = OpCompositeExtract %zz_int ${size} 0\n
${sy} = OpCompositeExtract %zz_int ${size} 1\n
${sum} = OpIAdd %zz_int ${sx} ${sy}\n
${bound} = OpINotEqual %zz_bool ${sum} ${c0}\n
OpSelectionMerge ${end2} None\n
OpBranchConditional ${bound} ${fetch} ${end2}\n
${fetch} = OpLabel\n
${uv} = OpLoad %zz_v2float %zz_in_loc0\n
${frc} = OpExtInst %zz_v2float %zz_glsl Fract ${uv}\n
${sizef} = OpConvertSToF %zz_v2float ${size}\n
${scaled} = OpFMul %zz_v2float ${frc} ${sizef}\n
${coord} = OpConvertFToS %zz_v2int ${scaled}\n
${texel} = OpImageFetch %zz_v4float ${img} ${coord} Lod ${c0}\n
${alpha} = OpCompositeExtract %zz_float ${texel} 3\n
${cut} = OpFOrdLessThan %zz_bool ${alpha} ${fone}\n
OpSelectionMerge ${end3} None\n
OpBranchConditional ${cut} ${kill} ${end3}\n
${kill} = OpLabel\n
OpKill\n
${end3} = OpLabel\n
OpBranch ${end2}\n
${end2} = OpLabel\n
OpBranch ${end}\n
${end} = OpLabel\n
@zz_entry
```

## Vertex shader example

The outline of a mod's mesh takes the normal made of the vertex colour when `x197 = 314159` is set for the draw, as if the material's flag for that were on. This is what the Vulkan path makes of `[ShaderRegexSmoothNormalColor]` of Si FGO lancer Melusine, without the `Pattern` subsection that it copies from the D3D11 pattern. The group declares no texture, so every mod draw with a shader it patches uses the patched shader, and `x197` decides what it does.

The game's shader compares a float of a uniform block with 0.5 (`%flag`) and branches on that to a block that makes the normal of the colour's xy. The patch keeps that comparison as `${was}` and makes `%flag` the OR of it and the IniParams test.

```ini
[ShaderRegexSmoothNormal]
shader_model = vs_5_0
temps = c0 cindex cid was pini ini inix gate

[ShaderRegexSmoothNormal.InsertDeclarations]
dcl_resource_texture1d (float,float,float,float) t120

[ShaderRegexSmoothNormal.SpirvPatternA]
^@zz_globals$

[ShaderRegexSmoothNormal.SpirvPatternA.Replace]
${c0} = OpConstant %zz_int 0\n
${cindex} = OpConstant %zz_int 197\n
${cid} = OpConstant %zz_float 314159\n
@zz_globals

[ShaderRegexSmoothNormal.SpirvPatternB]
(?s)\A(?=.*?^(?<half>%\d+) = OpConstant %zz_float 0\.5$)
(?<prefix>.*?^(?<pw>%\d+) = OpAccessChain %zz_ptr_Uniform_float %\d+ %\d+\n(?<width>%\d+) = OpLoad %zz_float \k<pw>\n)
(?<flag>%\d+) = OpFOrdGreaterThan %zz_bool \k<width> \k<half>\n
(?=OpSelectionMerge (?<merge>%\d+) None\nOpBranchConditional \k<flag> (?<smooth>%\d+) \k<merge>\n\k<smooth> = OpLabel\n
(?<x>%\d+) = OpCompositeExtract %zz_float (?<color>%\d+) 0\n(?<y>%\d+) = OpCompositeExtract %zz_float \k<color> 1\n
(?<v>%\d+) = OpCompositeConstruct %zz_v3float \k<x> \k<y> %\d+\n(?<xy>%\d+) = OpVectorShuffle %zz_v2float \k<v> \k<v> 0 1\n
(?<dot>%\d+) = OpDot %zz_float \k<xy> \k<xy>\n(?<sat>%\d+) = OpExtInst %zz_float %zz_glsl FClamp \k<dot> %\d+ %\d+\n
(?<rest>%\d+) = OpFSub %zz_float %\d+ \k<sat>\n%\d+ = OpExtInst %zz_float %zz_glsl Sqrt \k<rest>\n)

[ShaderRegexSmoothNormal.SpirvPatternB.Replace]
${prefix}${was} = OpFOrdGreaterThan %zz_bool ${width} ${half}\n
${pini} = OpAccessChain %zz_ptr_Uniform_v4float %IniParams ${c0} ${cindex}\n
${ini} = OpLoad %zz_v4float ${pini}\n
${inix} = OpCompositeExtract %zz_float ${ini} 0\n
${gate} = OpFOrdEqual %zz_bool ${inix} ${cid}\n
${flag} = OpLogicalOr %zz_bool ${was} ${gate}\n
```

The lines of a pattern are joined without line breaks, so a long pattern can be split anywhere. Temps and named groups must have different names: temps are replaced first.
