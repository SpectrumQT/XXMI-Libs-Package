param(
    [Parameter(Mandatory = $true)][string]$VcVars,
    [Parameter(Mandatory = $true)][string]$Output
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$outDir = (Resolve-Path $Output).Path
$source = Join-Path $PSScriptRoot 'retained_objects.cpp'
$exe = Join-Path $outDir 'retained_objects.exe'
$obj = Join-Path $outDir 'retained_objects.obj'
$includes = @($root, "$root/Vulkan-Headers/include")
$includeFlags = ($includes | ForEach-Object { '/I"' + $_ + '"' }) -join ' '
$command = 'call "{0}" >nul && cl /nologo /std:c++14 /EHsc /O2 {1} "{2}" /Fo"{3}" /Fe"{4}" /link /INCREMENTAL:NO' -f $VcVars, $includeFlags, $source, $obj, $exe
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw 'Retained-object regression build failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Retained-object regression failed' }
