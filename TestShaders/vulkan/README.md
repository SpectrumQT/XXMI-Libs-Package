# Original-format regression

The test uses the Vulkan harness and its synthetic fixtures from
`local-patches/vk-harness/run`. Supply that fixture directory and the DLL under
test explicitly; all runs use temporary copies with their own `d3dx.ini` and
never read or modify game settings or installed mods.

```powershell
python TestShaders/vulkan/check_original_format.py `
  --harness-root local-patches/vk-harness/run `
  --dll x64/Release/d3d11.dll `
  --output build/vk-original-format-runtime
```

The native harness requires Windows with Vulkan/D3D12 interop. Missing fixtures,
runtime failures and parser warnings fail the test rather than skipping checks.

The fixture replaces an original RGBA texture with a BC7 DDS. The legacy
format-based slot detector must produce the failure pixels, while the original
format query must produce the success pixels. The same run checks that current
format queries still see each replacement, clearing restores the game's binding,
dynamic slots work, and unavailable slots, stages and draw scopes return `-1`.
It also verifies Vulkan auto-detection with `extended_rendering = 1` and with
the setting omitted, and checks that `extended_rendering = 0` produces the
unmodified game's pixels even with the mod installed.

## D3D11 detection and retained Vulkan objects

```powershell
./TestShaders/vulkan/check_retained_objects.ps1 `
  -VcVars 'C:/path/to/VC/Auxiliary/Build/vcvars64.bat' `
  -Output build/vk-retained-objects
```

This native test uses fake Vulkan handles and destroy callbacks, without a GPU
or Vulkan runtime. It checks that a D3D11 detection transition releases all four
retained object kinds exactly once, later destroys pass through, custom allocator
callbacks execute before their state is released, and device-scoped cleanup and concurrent destroys do
not leak or double-free objects. It requires the MSVC compiler and Windows SDK;
missing build dependencies fail the test.

Objects with custom allocators are destroyed synchronously. Pipelines depending
on destroyed shader modules, pipeline layouts or render passes fall back to the
game's draw path; destroyed descriptor set layouts cannot be used to allocate
new replacement sets. This avoids retaining application-owned allocator state.

## SPIR-V ShaderRegex

```powershell
python TestShaders/vulkan/check_spirv_patch.py `
  --harness-root local-patches/vk-harness/run `
  --dll x64/Release/d3d11.dll `
  --spirv-tools Dependencies/SPIRV-Tools-shared.dll `
  --output build/vk-spirv-patch
```

The mod `spirv_patch.ini` draws a triangle with a mask at `ps-t116` and
`x245 = 245997`; the group `spirv_patch_group.ini` patches the harness's pixel
shader to discard where the mask's alpha is below 1. A transparent mask must
discard the triangle, and the log must name the section that patched the shader.
An opaque mask, another `x245` value or no mask must draw it as without the
group. The same cut is checked with a declared sampler, and with `--draws 2` the
game's draw after the patched one must render as the game's.

The group `spirv_patch_vs_group.ini` patches the harness's vertex shader to move
the triangle off the screen where `x246 = 246001`. It declares no texture, so
the mod's draw uses it without one: with `x246` set the triangle must be gone,
without it drawn as before. With both groups, the pipeline that has both
shaders patched must cut and move as each group does alone, and the game's draw
after one with the patched vertex shader must render as the game's.

The D3D11 group `spirv_patch_d3d11.ini` is the same cut written as a mod's
`CutoutMask.ini` has it, with no SPIR-V version: the DLL must convert it, the
log must name the converted group that patched the shader, and the triangle
must be cut with a transparent mask and drawn with an opaque one.

The cut is also checked with `config_initialization_delay = -1`, where the
configuration read at load is never reloaded, and with
`ps_two_entries.spv` from the fixture directory in place of `ps.spv`: a copy of
its function is the module's first entry point, and the harness's pipeline uses
`main`, the second, which the patch must go into. The fixture is assembled
from `ps_two_entries.spvasm` next to it with
`spirv-as --target-env spv1.0`.

`vs_ps_main.spv` in the fixture directory is `vs.spv` and `ps.spv` linked into
one module (`spirv-link vs.spv ps.spv`), with a vertex and a fragment entry
point both named `main`. Used as both stages, each must be patched for itself:
the pixel shader group cuts and the vertex shader group moves the triangle.
