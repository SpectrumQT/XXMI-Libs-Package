# Runtime dependencies

`d3d11.dll` loads some libraries from its own folder when a feature first needs them. `Dependencies/runtime-dependencies.json` lists them, and every release uploads it as the asset `runtime-dependencies.json` together with each library it names. A launcher that installs `d3d11.dll` from a release reads it to know what else to install, so a new library needs no change to the launcher.

```json
{
  "schema": 1,
  "files": [
    { "name": "SPIRV-Tools-shared.dll", "for": ["vulkan"], "purpose": "..." }
  ]
}
```

* `schema`: 1.
* `files`: the libraries, at most 16.
  * `name`: the file name, which is also the name of its release asset: letters, digits, `.`, `_` and `-`, ending in `.dll`, with no path. `d3d11.dll` and `d3dcompiler_47.dll` cannot be listed.
  * `for`: the rendering APIs that use it, of `dx11`, `dx12` and `vulkan`. Only for people to read.
  * `purpose`: what it does. Only for people to read.

What the libraries do now:

| Library | Used for |
| --- | --- |
| `dxcompiler.dll`, `dxil.dll` | Shaders of mods on D3D12 games whose own shaders are DXIL |
| `SPIRV-Tools-shared.dll` | [SPIR-V ShaderRegex](ini-language-extensions/spirv-shader-regex/README.md) on Vulkan games |

Without one of them only its feature is missing, and the log says so.

## For a launcher

* Install every listed file next to `d3d11.dll` in every folder that `d3d11.dll` is installed in, whatever `for` says. The rendering API of a game is not known before it runs, and a library is only loaded when it is needed.
* Take each file only from the asset of that name in the same release, checked against the digest GitHub publishes for the asset, and at most 64 MiB. Leave out a release whose manifest names a file that it does not have as an asset.
* Refuse a manifest that breaks the rules above, or is over 64 KiB. Ignore fields it does not know.
* A release without `runtime-dependencies.json`, or with a `schema` above the one the launcher knows, is taken to need `dxcompiler.dll` and `dxil.dll`, as releases did before the manifest.
* A file a later release no longer lists is removed when that release is installed.

## Releasing

`tools/release-assets.ps1` checks the manifest against these rules and prints the paths of the assets to upload: `d3d11.dll`, `d3dx.ini`, `runtime-dependencies.json` and the listed libraries from `Dependencies`.

```powershell
gh release create <tag> -R myparsleycat/XXMI-Libs-Package-Forked --latest @(./tools/release-assets.ps1)
```

A new library goes into `Dependencies` and into the manifest in the same commit as the code that loads it.
