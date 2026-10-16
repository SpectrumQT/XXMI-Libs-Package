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
