[CmdletBinding()]
param(
    [string]$VersionFile = (Join-Path $PSScriptRoot "..\version.h"),
    [string]$OutputFile = (Join-Path $PSScriptRoot "..\version.generated.h"),
    [string]$Tag
)

$ErrorActionPreference = "Stop"

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    $result = & git @Arguments 2>&1

    if ($LASTEXITCODE -ne 0) {
        throw "Git command failed: git $($Arguments -join ' ')`n$result"
    }

    return ($result | Out-String).Trim()
}

function Parse-VersionTag {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Tag
    )

    if ($Tag -notmatch '^v?(\d+)\.(\d+)\.(\d+)(?:\.(\d+))?$') {
        throw "Invalid version tag '$Tag'. Expected vMAJOR.MINOR.PATCH or vMAJOR.MINOR.PATCH.BUILD"
    }

    return @{
        Major    = [int]$Matches[1]
        Minor    = [int]$Matches[2]
        Revision = [int]$Matches[3]
        Build    = if ($Matches[4]) { [int]$Matches[4] } else { 0 }
    }
}

$VersionFile = [System.IO.Path]::GetFullPath($VersionFile)
$OutputFile  = [System.IO.Path]::GetFullPath($OutputFile)

if (-not (Test-Path -LiteralPath $VersionFile)) {
    throw "Version file not found: $VersionFile"
}

# Verify that we are inside a Git repository.
& git rev-parse --is-inside-work-tree *> $null

if ($LASTEXITCODE -ne 0) {
    throw "Not inside a Git repository."
}

# ---------------------------------------------------------------------------
# Git repository
# ---------------------------------------------------------------------------

$remoteUrl = $null

try {
    $remoteUrl = Invoke-Git @(
        "config",
        "--get",
        "remote.origin.url"
    )
}
catch {
    $remoteUrl = $null
}

if ([string]::IsNullOrWhiteSpace($remoteUrl)) {
    throw "Git remote 'origin' is not configured."
}

$remoteUrl = $remoteUrl.Trim()

# Convert common GitHub remote formats to a clean HTTPS URL.
if ($remoteUrl -match '^git@github\.com:(.+?)(?:\.git)?$') {
    $repoUrl = "https://github.com/$($Matches[1])"
}
elseif ($remoteUrl -match '^ssh://git@github\.com/(.+?)(?:\.git)?$') {
    $repoUrl = "https://github.com/$($Matches[1])"
}
elseif ($remoteUrl -match '^https://github\.com/(.+?)(?:\.git)?$') {
    $repoUrl = "https://github.com/$($Matches[1])"
}
elseif ($remoteUrl -match '^http://github\.com/(.+?)(?:\.git)?$') {
    $repoUrl = "https://github.com/$($Matches[1])"
}
else {
    # Keep non-GitHub repositories usable as well.
    $repoUrl = $remoteUrl -replace '\.git$', ''
}

# ---------------------------------------------------------------------------
# Version tag
# ---------------------------------------------------------------------------

if ([string]::IsNullOrWhiteSpace($Tag)) {
    try {
        $Tag = Invoke-Git @(
            "describe",
            "--tags",
            "--match", "v[0-9]*",
            "--abbrev=0"
        )
    }
    catch {
        throw "No version tag was found. Create a tag such as v1.4.0 first."
    }
}

$Tag = $Tag.Trim()

$parsed = Parse-VersionTag $Tag

$major    = $parsed.Major
$minor    = $parsed.Minor
$revision = $parsed.Revision
$build    = $parsed.Build

# ---------------------------------------------------------------------------
# Git commit information
# ---------------------------------------------------------------------------

$headCommit = Invoke-Git @(
    "rev-parse",
    "HEAD"
)

$workingTreeStatus = Invoke-Git @(
    "status",
    "--porcelain"
)

$isDirty = -not [string]::IsNullOrWhiteSpace($workingTreeStatus)

$tagCommit = Invoke-Git @(
    "rev-list",
    "-n",
    "1",
    $Tag
)

$commitsSinceTag = [int](Invoke-Git @(
    "rev-list",
    "$Tag..HEAD",
    "--count"
))

$isRelease = ($headCommit -eq $tagCommit)

if (-not $isRelease) {
    $build += $commitsSinceTag
}

$shortCommit = Invoke-Git @(
    "rev-parse",
    "--short=8",
    "HEAD"
)

$commitDisplay = $shortCommit

if ($isDirty) {
    $commitDisplay += "-dirty"
}

# ---------------------------------------------------------------------------
# Builder identity
# ---------------------------------------------------------------------------

# GitHub Actions provides GITHUB_ACTOR automatically.
$builder = $env:GITHUB_ACTOR

# Local builds can optionally use:
#
#   git config --global github.username YourGitHubUsername
#
if ([string]::IsNullOrWhiteSpace($builder)) {
    try {
        $builder = Invoke-Git @(
            "config",
            "--get",
            "github.username"
        )
    }
    catch {
        $builder = $null
    }
}

# Fall back to the normal Git user name.
if ([string]::IsNullOrWhiteSpace($builder)) {
    try {
        $builder = Invoke-Git @(
            "config",
            "--get",
            "user.name"
        )
    }
    catch {
        $builder = $null
    }
}

if ([string]::IsNullOrWhiteSpace($builder)) {
    $builder = "Unknown"
}

$builder = $builder.Trim()

# ---------------------------------------------------------------------------
# Explorer Comments field
# ---------------------------------------------------------------------------

$companyName = "Built by $builder @ $commitDisplay"
$comments = $repoUrl

# Escape characters which could break the generated C/C++ string.
$companyName = $companyName.Replace('\', '\\').Replace('"', '\"')
$comments = $comments.Replace('\', '\\').Replace('"', '\"')


# ---------------------------------------------------------------------------
# Generate header
# ---------------------------------------------------------------------------

$content = @"
#define VERSION_MAJOR               $major
#define VERSION_MINOR               $minor
#define VERSION_REVISION            $revision
#define VERSION_BUILD               $build

#define VER_COMPANYNAME_STR         "$companyName"
#define VER_COMMENTS_STR            "$comments"

"@

$utf8 = New-Object System.Text.UTF8Encoding($false)

[System.IO.File]::WriteAllText(
    $OutputFile,
    $content,
    $utf8
)

$version = "$major.$minor.$revision.$build"

# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

Write-Host ""
Write-Host "========================================"
Write-Host " Generated version header"
Write-Host "========================================"
Write-Host "Version:            $version"
Write-Host "Source tag:         $Tag"
Write-Host "Commits since tag:  $commitsSinceTag"
Write-Host "Release build:      $isRelease"
Write-Host "Builder:            $builder"
Write-Host "Commit:             $commitDisplay"
Write-Host "Working tree dirty: $isDirty"
Write-Host "Repository:         $repoUrl"
Write-Host "Comments:           $comments"
Write-Host "Output:             $OutputFile"
Write-Host "========================================"
Write-Host ""