<#
.SYNOPSIS
    Windows dependency initialization script - mirrors dep_create.sh on Linux/macOS.

.DESCRIPTION
    Reads oceanbase.windows.x86_64.deps, downloads tar.gz archives from the
    configured HTTP repo, and extracts them into deps/3rd/.

    After extraction the layout is:
        deps/3rd/vcpkg/x64-windows/   (vcpkg installed packages)
        deps/3rd/openssl/             (OpenSSL)
        deps/3rd/vsag/               (vsag vector search library)
        deps/3rd/tools/cmake/         (CMake)
        deps/3rd/tools/ninja/         (Ninja)
        deps/3rd/tools/llvm18/        (LLVM 18)
        deps/3rd/tools/win_flex_bison/(Flex & Bison)

.EXAMPLE
    powershell -File dep_create.ps1
#>

param(
    [string]$CacheDir = $env:SEEKDB_DEPS_CACHE
)

$ErrorActionPreference = "Stop"
$ScriptDir  = $PSScriptRoot
$TOPDIR     = (Resolve-Path "$ScriptDir\..\..").Path
$DEPS_3RD   = "$TOPDIR\deps\3rd"
$STAGE_DIR  = "$TOPDIR\deps\3rd.stage"
$DONE_FILE  = "$DEPS_3RD\DONE"
$DEP_FILE   = "$ScriptDir\oceanbase.windows.x86_64.deps"

if ([string]::IsNullOrWhiteSpace($CacheDir)) {
    $CacheDir = Join-Path $env:LOCALAPPDATA "seekdb\deps-cache\windows-x86_64"
}
$CacheDir = [System.IO.Path]::GetFullPath($CacheDir)

# -- Helpers ----------------------------------------------------------
function Write-Log  { param([string]$msg) Write-Host "[dep_create.ps1] $msg" }
function Write-Err  { param([string]$msg) Write-Host "[dep_create.ps1][ERROR] $msg" -ForegroundColor Red }

function Test-TarArchive {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path) -or (Get-Item -LiteralPath $Path).Length -le 0) {
        return $false
    }
    & tar -tzf $Path *> $null
    return $LASTEXITCODE -eq 0
}

function Invoke-PackageDownload {
    param(
        [string]$Url,
        [string]$Destination
    )

    $Partial = "$Destination.part"
    $CommonArgs = @(
        "-L", "-f", "--retry", "8", "--retry-all-errors", "--retry-delay", "3",
        "--connect-timeout", "30", "--speed-time", "60", "--speed-limit", "1024",
        "-o", $Partial
    )

    # Keep a partial archive between invocations and resume it when the mirror
    # supports byte ranges. If it does not, retry once from byte zero.
    & curl.exe @CommonArgs "-C" "-" $Url
    $CurlExit = $LASTEXITCODE
    if ($CurlExit -eq 33) {
        Write-Log "  mirror does not support resume; restarting this package"
        Remove-Item -LiteralPath $Partial -Force -ErrorAction SilentlyContinue
        & curl.exe @CommonArgs $Url
        $CurlExit = $LASTEXITCODE
    }
    if ($CurlExit -ne 0) {
        throw "curl exit code $CurlExit"
    }
    if (-not (Test-TarArchive -Path $Partial)) {
        throw "downloaded archive is incomplete or corrupt"
    }
    Move-Item -LiteralPath $Partial -Destination $Destination -Force
}

# -- Check deps file -------------------------------------------------
if (-not (Test-Path $DEP_FILE)) {
    Write-Err "Dependencies profile not found: $DEP_FILE"
    exit 2
}
Write-Log "Using dependencies profile: $DEP_FILE"

# -- MD5 cache check -------------------------------------------------
$md5 = (Get-FileHash -Path $DEP_FILE -Algorithm MD5).Hash.ToLower()
$MD5_FILE = "$DEPS_3RD\$md5"

if ((Test-Path $MD5_FILE) -and (Test-Path $DONE_FILE)) {
    Write-Log "Dependencies already initialized (MD5=$md5). Skipping."
    exit 0
}
Write-Log "Initializing dependencies ... (MD5=$md5)"

# -- Parse deps file (INI-like, same format as Linux) ----------------
$targets  = @{}          # target-name -> repo URL
$sections = [ordered]@{} # section-name -> string[] of package filenames

$curSection = ""
$curLines   = [System.Collections.Generic.List[string]]::new()

function Save-Section {
    if ($curSection -eq "") { return }
    if ($curSection -match "^target-(.+)$") {
        $tName = $Matches[1]
        foreach ($l in $curLines) {
            if ($l -match "^repo=(.+)$") {
                $script:targets[$tName] = $Matches[1].Trim()
                Write-Log "  target: $tName  repo: $($Matches[1].Trim())"
            }
        }
    } else {
        $script:sections[$curSection] = @($curLines)
    }
}

foreach ($rawLine in Get-Content $DEP_FILE -Encoding UTF8) {
    $line = $rawLine.Trim()
    if ($line -match "^\[(.+)\]$") {
        Save-Section
        $curSection = $Matches[1]
        $curLines   = [System.Collections.Generic.List[string]]::new()
    }
    elseif ($line -ne "" -and -not $line.StartsWith("#")) {
        $curLines.Add($line)
    }
}
Save-Section

$defaultRepo = if ($targets.ContainsKey("default")) { $targets["default"] } else { "" }
if ($defaultRepo -eq "") {
    Write-Err "No [target-default] repo defined in $DEP_FILE"
    exit 3
}

# -- Prepare persistent cache and staging directory ------------------
# Archives intentionally live outside deps/3rd. A failed or cancelled run can
# therefore resume without deleting already downloaded packages, while an
# existing complete dependency tree remains usable until staging succeeds.
Write-Log "Package cache: $CacheDir"
New-Item -ItemType Directory -Path $CacheDir -Force | Out-Null
if (Test-Path -LiteralPath $STAGE_DIR) {
    Write-Log "Removing stale staging directory: $STAGE_DIR"
    Remove-Item -LiteralPath $STAGE_DIR -Recurse -Force
}
New-Item -ItemType Directory -Path $STAGE_DIR -Force | Out-Null

# -- Download & extract ----------------------------------------------
$totalPkgs  = ($sections.Values | ForEach-Object { $_.Count } | Measure-Object -Sum).Sum
$currentPkg = 0

foreach ($sect in $sections.Keys) {
    Write-Log "-- section [$sect] --"
    foreach ($entry in $sections[$sect]) {
        $parts   = $entry -split '\s+'
        $pkgFile = $parts[0]

        # Optional per-package target override: "pkg.tar.gz target=xxx"
        $tName = "default"
        foreach ($p in $parts) {
            if ($p -match "^target=(.+)$") { $tName = $Matches[1] }
        }

        $repo = if ($targets.ContainsKey($tName)) { $targets[$tName] } else { $defaultRepo }
        $url     = "$repo/$pkgFile"
        $pkgPath = Join-Path $CacheDir $pkgFile

        $currentPkg++
        Write-Log "[$currentPkg/$totalPkgs] $pkgFile"

        # -- Download ------------------------------------------------
        if (Test-Path -LiteralPath $pkgPath) {
            if (Test-TarArchive -Path $pkgPath) {
                Write-Log "  cached"
            } else {
                Write-Log "  cached archive is corrupt; downloading again"
                Remove-Item -LiteralPath $pkgPath -Force
            }
        }
        if (-not (Test-Path -LiteralPath $pkgPath)) {
            Write-Log "  downloading from $url ..."
            try {
                Invoke-PackageDownload -Url $url -Destination $pkgPath
            }
            catch {
                Write-Err "Failed to download: $url"
                Write-Err "$_"
                Write-Log "Partial downloads are kept in $CacheDir for the next run."
                exit 4
            }
        }

        # -- Extract -------------------------------------------------
        Write-Log "  extracting ..."
        & tar -xzf $pkgPath -C $STAGE_DIR --strip-components=1
        if ($LASTEXITCODE -ne 0) {
            Write-Err "Failed to extract: $pkgFile"
            exit 5
        } else {
            Write-Log "  done"
        }
    }
}

# -- Mark done --------------------------------------------------------
$StageMd5File = Join-Path $STAGE_DIR $md5
$StageDoneFile = Join-Path $STAGE_DIR "DONE"
New-Item -ItemType File -Path $StageMd5File  -Force | Out-Null
New-Item -ItemType File -Path $StageDoneFile -Force | Out-Null

# Publish the complete tree only after every archive has downloaded, validated,
# and extracted. The old tree is restored if the directory swap fails.
$BackupDir = "$TOPDIR\deps\3rd.backup"
if (Test-Path -LiteralPath $BackupDir) {
    Remove-Item -LiteralPath $BackupDir -Recurse -Force
}
try {
    if (Test-Path -LiteralPath $DEPS_3RD) {
        Move-Item -LiteralPath $DEPS_3RD -Destination $BackupDir
    }
    Move-Item -LiteralPath $STAGE_DIR -Destination $DEPS_3RD
    if (Test-Path -LiteralPath $BackupDir) {
        Remove-Item -LiteralPath $BackupDir -Recurse -Force
    }
}
catch {
    if (-not (Test-Path -LiteralPath $DEPS_3RD) -and (Test-Path -LiteralPath $BackupDir)) {
        Move-Item -LiteralPath $BackupDir -Destination $DEPS_3RD
    }
    throw
}

Write-Log ""
Write-Log "Dependencies initialized successfully."
Write-Log ""
Write-Log "Layout:"
Write-Log "  deps/3rd/vcpkg/x64-windows/    vcpkg packages"
Write-Log "  deps/3rd/openssl/              OpenSSL"
Write-Log "  deps/3rd/vsag/                 vsag vector search library"
Write-Log "  deps/3rd/tools/cmake/          CMake"
Write-Log "  deps/3rd/tools/ninja/          Ninja"
Write-Log "  deps/3rd/tools/llvm18/         LLVM 18"
Write-Log "  deps/3rd/tools/win_flex_bison/ Flex & Bison"
