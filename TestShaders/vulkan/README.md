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
