# Builds SPIRV-Tools-shared.dll, which the Vulkan path loads to take a game's
# SPIR-V shaders apart and put them back together for [ShaderRegex] sections
# with SpirvPattern subsections, and copies it with the C header and the
# license into the repository.
#
#   powershell -ExecutionPolicy Bypass -File tools/build-spirv-tools.ps1
#
# Needs git, CMake, Python 3 and the MSVC build tools. The work folder is
# build/spirv-tools (ignored by git).
param(
	[string]$Tag = 'vulkan-sdk-1.4.357.0',
	[string]$Python = ''
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$work = Join-Path $root 'build/spirv-tools'
$tools = Join-Path $work 'SPIRV-Tools'
$headers = Join-Path $tools 'external/spirv-headers'
$out = Join-Path $work 'out'

if (-not $Python) {
	$Python = (& py -3 -c 'import sys; print(sys.executable)').Trim()
}

New-Item -ItemType Directory -Force $work | Out-Null
if (-not (Test-Path $tools)) {
	git clone --depth 1 --branch $Tag https://github.com/KhronosGroup/SPIRV-Tools.git $tools
	if ($LASTEXITCODE) { throw 'cloning SPIRV-Tools failed' }
}
if (-not (Test-Path $headers)) {
	git clone --depth 1 --branch $Tag https://github.com/KhronosGroup/SPIRV-Headers.git $headers
	if ($LASTEXITCODE) { throw 'cloning SPIRV-Headers failed' }
}

# The static CRT keeps the DLL free of a vcruntime it would otherwise need
# next to it, like d3d11.dll itself
cmake -S $tools -B $out -G 'Visual Studio 17 2022' -A x64 `
	"-DPython3_EXECUTABLE=$Python" `
	-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
	-DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF `
	-DSPIRV_TOOLS_BUILD_STATIC=OFF
if ($LASTEXITCODE) { throw 'configuring failed' }
cmake --build $out --config Release --target SPIRV-Tools-shared -- /m
if ($LASTEXITCODE) { throw 'building failed' }

$dll = Get-ChildItem -Recurse -Path $out -Filter 'SPIRV-Tools-shared.dll' | Where-Object { $_.FullName -match 'Release' } | Select-Object -First 1
if (-not $dll) { throw 'SPIRV-Tools-shared.dll was not built' }

$include = Join-Path $root 'SPIRV-Tools/include/spirv-tools'
New-Item -ItemType Directory -Force $include | Out-Null
Copy-Item $dll.FullName (Join-Path $root 'Dependencies/SPIRV-Tools-shared.dll') -Force
Copy-Item (Join-Path $tools 'include/spirv-tools/libspirv.h') $include -Force
Copy-Item (Join-Path $tools 'LICENSE') (Join-Path $root 'SPIRV-Tools/LICENSE') -Force
Set-Content -Path (Join-Path $root 'SPIRV-Tools/VERSION') -Value "$Tag ($((git -C $tools rev-parse HEAD).Trim()))"
Write-Host "SPIRV-Tools $Tag built: $($dll.FullName)"
