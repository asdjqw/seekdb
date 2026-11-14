<#
.SYNOPSIS
    Download reusable Windows dependencies and optionally build seekdb.

.EXAMPLE
    .\setup-windows-build.ps1

.EXAMPLE
    .\setup-windows-build.ps1 -Action Build -Jobs 12

.EXAMPLE
    .\setup-windows-build.ps1 -Action Reset
#>

[CmdletBinding()]
param(
    [ValidateSet("Check", "Download", "Build", "Reset")]
    [string]$Action = "Download",
    [ValidateRange(1, 256)]
    [int]$Jobs = 12,
    [string]$CacheDir = $env:SEEKDB_DEPS_CACHE
)

$ErrorActionPreference = "Stop"
$RepoRoot = $PSScriptRoot
$DepsDir = Join-Path $RepoRoot "deps\3rd"
$BuildDir = Join-Path $RepoRoot "build_release"
$DepScript = Join-Path $RepoRoot "deps\init\dep_create.ps1"
$Profile = Join-Path $RepoRoot "deps\init\oceanbase.windows.x86_64.deps"

if ([string]::IsNullOrWhiteSpace($CacheDir)) {
    $CacheDir = Join-Path $env:LOCALAPPDATA "seekdb\deps-cache\windows-x86_64"
}
$CacheDir = [System.IO.Path]::GetFullPath($CacheDir)
$ProfileMd5 = (Get-FileHash -LiteralPath $Profile -Algorithm MD5).Hash.ToLower()
$PackageCount = @(Get-Content -LiteralPath $Profile -Encoding UTF8 |
    Where-Object { $_.Trim() -match "^[^#].*\.tar\.gz(?:\s|$)" }).Count

function Write-Step {
    param([string]$Message)
    Write-Host "[setup-windows-build] $Message" -ForegroundColor Cyan
}

function Test-DependenciesReady {
    return (Test-Path -LiteralPath (Join-Path $DepsDir "DONE")) -and
           (Test-Path -LiteralPath (Join-Path $DepsDir $ProfileMd5))
}

function Show-Status {
    $ArchiveCount = if (Test-Path -LiteralPath $CacheDir) {
        @(Get-ChildItem -LiteralPath $CacheDir -Filter "*.tar.gz" -File).Count
    } else {
        0
    }
    $PartialCount = if (Test-Path -LiteralPath $CacheDir) {
        @(Get-ChildItem -LiteralPath $CacheDir -Filter "*.part" -File).Count
    } else {
        0
    }
    Write-Host "Repository : $RepoRoot"
    Write-Host "Dependencies ready : $(Test-DependenciesReady)"
    Write-Host "Package cache      : $CacheDir"
    Write-Host "Cached archives    : $ArchiveCount / $PackageCount"
    Write-Host "Resumable partials : $PartialCount"
    Write-Host "Build directory    : $BuildDir"
}

if ($Action -eq "Check") {
    Show-Status
    exit 0
}

if ($Action -eq "Reset") {
    Write-Step "Removing generated build and installed dependency directories"
    foreach ($Target in @($BuildDir, $DepsDir, (Join-Path $RepoRoot "deps\3rd.stage"),
                          (Join-Path $RepoRoot "deps\3rd.backup"))) {
        if (Test-Path -LiteralPath $Target) {
            Remove-Item -LiteralPath $Target -Recurse -Force
            Write-Host "Removed: $Target"
        }
    }
    Write-Host "The reusable archive cache was preserved: $CacheDir"
    exit 0
}

Write-Step "Preparing dependencies (completed archives will be reused on retry)"
& powershell -NoProfile -ExecutionPolicy Bypass -File $DepScript -CacheDir $CacheDir
if ($LASTEXITCODE -ne 0) {
    throw "Dependency setup failed with exit code $LASTEXITCODE"
}

if (-not (Test-DependenciesReady)) {
    throw "Dependency setup returned successfully but completion markers are missing"
}

if ($Action -eq "Build") {
    Write-Step "Configuring and building seekdb with $Jobs parallel jobs"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "build.ps1") `
        release --ninja -j $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "seekdb build failed with exit code $LASTEXITCODE"
    }
}

Show-Status
