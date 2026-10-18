# Lists the files a release uploads as assets: d3d11.dll, d3dx.ini,
# runtime-dependencies.json and every library that names, after checking it
# against the rules of docs/runtime-dependencies.md. Fails if a file is missing.
#
#   gh release create <tag> -R myparsleycat/XXMI-Libs-Package-Forked --latest @(./tools/release-assets.ps1)
param(
	[string]$Dll = 'x64/Release/d3d11.dll'
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$dependencies = Join-Path $root 'Dependencies'
$manifestPath = Join-Path $dependencies 'runtime-dependencies.json'

# As docs/runtime-dependencies.md has them
$maxFiles = 16
$maxManifestBytes = 64KB
$maxFileBytes = 64MB
$reserved = @('d3d11.dll', 'd3dcompiler_47.dll')
$apis = @('dx11', 'dx12', 'vulkan')

if ((Get-Item $manifestPath).Length -gt $maxManifestBytes) { throw "runtime-dependencies.json is over $maxManifestBytes bytes" }
$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
# The types as a launcher decodes them: -ne alone takes "1" for 1
if (-not ($manifest.schema -is [long] -or $manifest.schema -is [int]) -or $manifest.schema -ne 1) { throw "runtime-dependencies.json: schema must be the number 1" }
if ($manifest.files -isnot [array]) { throw "runtime-dependencies.json: files must be an array" }
$files = @($manifest.files)
if ($files.Count -gt $maxFiles) { throw "runtime-dependencies.json: more than $maxFiles files" }

$assets = @((Join-Path $root $Dll), (Join-Path $dependencies 'd3dx.ini'), $manifestPath)
$seen = @{}
foreach ($file in $files) {
	if ($file.name -isnot [string]) { throw "runtime-dependencies.json: a file has no name" }
	$name = $file.name
	if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\.dll$') { throw "runtime-dependencies.json: '$name' is not a plain .dll file name" }
	if ($reserved -contains $name.ToLowerInvariant()) { throw "runtime-dependencies.json: '$name' is reserved" }
	if ($seen.ContainsKey($name.ToLowerInvariant())) { throw "runtime-dependencies.json: '$name' is listed twice" }
	$seen[$name.ToLowerInvariant()] = $true
	foreach ($api in @($file.for)) {
		if ($apis -notcontains $api) { throw "runtime-dependencies.json: '$name' is for an unknown API '$api'" }
	}
	$path = Join-Path $dependencies $name
	if ((Get-Item $path).Length -gt $maxFileBytes) { throw "$name is over $maxFileBytes bytes" }
	$assets += $path
}

foreach ($asset in $assets) {
	if (-not (Test-Path $asset -PathType Leaf)) { throw "$asset is missing" }
	(Resolve-Path $asset).Path
}
