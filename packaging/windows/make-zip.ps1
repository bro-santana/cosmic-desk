# Cosmic Desk - Windows standalone zip packaging (plan M6.2).
#
# Usage (run from the repo root, after `cmake --build build`):
#   powershell -ExecutionPolicy Bypass -File packaging\windows\make-zip.ps1
#
# Produces dist\CosmicDesk\ (cosmicdesk.exe + assets + MinGW DLLs + LICENSE +
# README.md + tools\cosmicsvc.exe + install-service.ps1 +
# uninstall-service.ps1) and dist\CosmicDesk-windows-x64.zip. The zip is
# self-contained: MSYS2 is NOT required on the target machine.
#
# DLL discovery: `ntldd -R` (recursive, reliable) from the MSYS2 UCRT64 bin
# dir; if it is unavailable, copy the usual-suspects wildcard list from the
# same dir. ASCII only.

param(
    [string]$OutPath = ""
)

$ErrorActionPreference = "Stop"

# Repo root = parent of the script's parent (packaging/windows/make-zip.ps1).
$RepoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Exe = Join-Path $RepoRoot "build\cosmicdesk.exe"
if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Error "build\cosmicdesk.exe not found. Run 'cmake --build build' first."
}

$DistRoot = Join-Path $RepoRoot "dist"
$DistDir = Join-Path $DistRoot "CosmicDesk"
if ($OutPath) {
    $ZipPath = $OutPath
} else {
    $ZipPath = Join-Path $DistRoot "CosmicDesk-windows-x64.zip"
}

# Clean and recreate the dist folder. A running CosmicDeskService locks
# dist\CosmicDesk\tools\cosmicsvc.exe (it is the service's executable image),
# which makes this step fail with a raw permission error; translate that into
# an actionable message instead.
if (Test-Path -LiteralPath $DistDir) {
    try {
        Remove-Item -LiteralPath $DistDir -Recurse -Force -ErrorAction Stop
    } catch {
        Write-Error "Could not clean $DistDir. The CosmicDeskService is probably running from a previous bundle and locks tools\cosmicsvc.exe. Stop it first with: sc.exe stop CosmicDeskService (or packaging\windows\uninstall-service.ps1), then re-run this script."
        exit 1
    }
}
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null

# 1. Core files: exe, whole assets tree (incl. shaders), LICENSE, README.
Copy-Item -LiteralPath $Exe -Destination $DistDir
Copy-Item -LiteralPath (Join-Path $RepoRoot "assets") -Destination $DistDir -Recurse
Copy-Item -LiteralPath (Join-Path $RepoRoot "LICENSE") -Destination $DistDir
Copy-Item -LiteralPath (Join-Path $RepoRoot "README.md") -Destination $DistDir

# 1b. Service files: cosmicsvc.exe into tools\, plus the service scripts.
$ServiceExe = Join-Path $RepoRoot "build\tools\cosmicsvc.exe"
if (-not (Test-Path -LiteralPath $ServiceExe)) {
    Write-Error "build\tools\cosmicsvc.exe not found. Run 'cmake --build build' first."
}
New-Item -ItemType Directory -Path (Join-Path $DistDir "tools") -Force | Out-Null
Copy-Item -LiteralPath $ServiceExe -Destination (Join-Path $DistDir "tools")
Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows\install-service.ps1") -Destination $DistDir
Copy-Item -LiteralPath (Join-Path $RepoRoot "packaging\windows\uninstall-service.ps1") -Destination $DistDir

# 2. DLL bundling.
$DllPaths = @()
# MSYS2 bin-dir discovery, in order of preference:
# 1. MSYSTEM_PREFIX (set inside an MSYS2 shell; e.g. C:/msys64/ucrt64)
# 2. The compiler recorded in build\CMakeCache.txt (CMAKE_CXX_COMPILER) — the
#    exact toolchain this build used, so this works from plain PowerShell too
#    and survives non-default MSYS2 install locations
# 3. The default install location
$MsysBin = $null
if ($env:MSYSTEM_PREFIX) {
    $Prefix = $env:MSYSTEM_PREFIX -replace '/', '\'
    if ($Prefix -notmatch '^[A-Za-z]:') {
        # Inside-MSYS form like "/ucrt64": anchor it to the default root.
        $Prefix = "C:\msys64" + $Prefix
    }
    if ($Prefix -notmatch '\\(ucrt64|mingw64|clang64)$') {
        # MSYSTEM_PREFIX is the MSYS root itself (MSYS shell): use UCRT64.
        $Prefix = Join-Path $Prefix "ucrt64"
    }
    $MsysBin = Join-Path $Prefix "bin"
}
if (-not $MsysBin -or -not (Test-Path -LiteralPath $MsysBin)) {
    $CacheFile = Join-Path $RepoRoot "build\CMakeCache.txt"
    $CompilerLine = $null
    if (Test-Path -LiteralPath $CacheFile) {
        $CompilerLine = Select-String -LiteralPath $CacheFile -Pattern '^CMAKE_CXX_COMPILER:(FILEPATH|STRING)=(.+)$' | Select-Object -First 1
    }
    if ($CompilerLine) {
        $MsysBin = Split-Path -Parent ($CompilerLine.Matches.Groups[2].Value -replace '/', '\')
    }
}
if (-not $MsysBin -or -not (Test-Path -LiteralPath $MsysBin)) {
    $MsysBin = "C:\msys64\ucrt64\bin"
}
# ntldd lives in the MSYS2 bin dir (note: it is an optional package; the
# wildcard fallback below covers machines without it).
$MsysRoot = Split-Path -Parent (Split-Path -Parent $MsysBin)
$Ntldd = @(
    (Join-Path $MsysBin "ntldd.exe"),
    (Join-Path $MsysRoot "bin\ntldd.exe")
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
$Cygpath = Join-Path $MsysRoot "usr\bin\cygpath.exe"
$Tool = $Ntldd

if ($Tool) {
    # ntldd needs -R (recursive).
    $Output = & $Tool -R $Exe 2>&1
    # Match "name => path (0x...)" lines; "not found" and "(?)" lines are
    # skipped by the regex.
    $Pattern = '^\s*(\S+) => (.*?) \(0x[0-9a-f]+\)$'
    foreach ($line in $Output) {
        if ($line -match $Pattern) {
            $dll = $matches[2].Trim()
            # Convert MSYS paths (/c/..., /ucrt64/...) to Windows paths via
            # cygpath; fall back to a manual /c/... conversion.
            if ($dll -match '^/') {
                if (Test-Path -LiteralPath $Cygpath) {
                    $dll = (& $Cygpath -w $dll).Trim()
                } elseif ($dll -match '^/([a-zA-Z])/(.*)$') {
                    $dll = "$($matches[1]):\$($matches[2])" -replace '/', '\'
                } else {
                    continue
                }
            }
            # Skip system DLLs (already on every Windows install).
            if ($dll -match '^C:\\WINDOWS') {
                continue
            }
            if (Test-Path -LiteralPath $dll) {
                $DllPaths += $dll
            }
        }
    }
}

# Fallback: usual suspects copied by wildcard from the MSYS2 UCRT64 bin dir,
# used when ntldd is unavailable. Copies only what exists. The list must stay
# a superset of ntldd's real closure: the critical-DLL check below exists
# because a hole here silently ships a bundle that dies at startup with a
# missing-DLL dialog (that exact bug happened when avcodec/avutil and the
# nghttp stack were absent from this list).
if (-not $Ntldd) {
    $FallbackPatterns = @(
        'SDL3.dll',
        'avcodec-*.dll',
        'avutil-*.dll',
        'swscale-*.dll',
        'avfilter-*.dll',
        'avformat-*.dll',
        'swresample-*.dll',
        'libavcodec-*.dll',
        'libavutil-*.dll',
        'libswscale-*.dll',
        'libavfilter-*.dll',
        'libavformat-*.dll',
        'libswresample-*.dll',
        'libcurl-4.dll',
        'libssl-3-x64.dll',
        'libcrypto-3-x64.dll',
        'libexpat-1.dll',
        'libopus-0.dll',
        'libstdc++-6.dll',
        'libgcc_s_seh-1.dll',
        'libwinpthread-1.dll',
        'zlib1.dll',
        'libz-*.dll',
        'libx264-*.dll',
        'libx265-*.dll',
        'libbz2-*.dll',
        'libiconv-2.dll',
        'liblzma-5.dll',
        'libzstd.dll',
        'libintl-8.dll',
        'libglib-*.dll',
        'libgobject-*.dll',
        'libgio-*.dll',
        'libgmodule-*.dll',
        'libpcre2-*.dll',
        'libharfbuzz-*.dll',
        'libfreetype-*.dll',
        'libpng16-*.dll',
        'libgraphite2.dll',
        'libbrotli*.dll',
        'libffi-*.dll',
        'libxml2-*.dll',
        'libmp3lame-*.dll',
        'libvorbis*.dll',
        'libogg-*.dll',
        'libspeex-*.dll',
        'libwebp*.dll',
        'libdav1d-*.dll',
        'libaom*.dll',
        'libva*.dll',
        'libvpl*.dll',
        'libcairo*.dll',
        'libdatrie-*.dll',
        'libdeflate*.dll',
        'libfontconfig-*.dll',
        'libfribidi-*.dll',
        'libgdk_pixbuf-*.dll',
        'libgomp-*.dll',
        'libgsm.dll',
        'libhwy*.dll',
        'libidn2-*.dll',
        'libjbig-*.dll',
        'libjpeg-*.dll',
        'libjxl*.dll',
        'liblc3-*.dll',
        'liblcms2-*.dll',
        'libLerc*.dll',
        'libnghttp*.dll',
        'libngtcp2*.dll',
        'libopencore-*.dll',
        'libopenjp2-*.dll',
        'libpango*.dll',
        'libpixman-*.dll',
        'libpsl-*.dll',
        'librav1e*.dll',
        'librsvg-*.dll',
        'libshaderc*.dll',
        'libsharpyuv-*.dll',
        'libsoxr*.dll',
        'libssh2-*.dll',
        'libSvtAv1Enc-*.dll',
        'libthai-*.dll',
        'libtheora*.dll',
        'libtiff-*.dll',
        'libunistring-*.dll',
        'libvpx-*.dll',
        'libxvidcore*.dll',
        'xvidcore.dll',
        'libzvbi-*.dll'
    )
    foreach ($pattern in $FallbackPatterns) {
        Get-ChildItem -Path $MsysBin -Filter $pattern -ErrorAction SilentlyContinue |
            ForEach-Object { $DllPaths += $_.FullName }
    }
}

# Dedupe and copy into the bundle.
$DllPaths = $DllPaths | Sort-Object -Unique
$DllCount = 0
foreach ($dll in $DllPaths) {
    if (Test-Path -LiteralPath $dll) {
        Copy-Item -LiteralPath $dll -Destination $DistDir
        $DllCount++
    }
}

# Guard: a bundle without DLLs is not self-contained. The ntldd path can
# silently find nothing (e.g. if MSYSTEM_PREFIX is wrong), so fail loudly
# instead of publishing a broken artifact.
if ($DllCount -eq 0) {
    Write-Error "No DLLs were bundled; aborting. Looked for ntldd and DLLs in: $MsysBin. If MSYS2 is installed elsewhere, configure the build from that toolchain and re-run (build\CMakeCache.txt is used to find it), or set MSYSTEM_PREFIX."
    exit 1
}

# Guard: the wildcard fallback above is a best effort; verify the
# load-critical DLLs actually made it into the bundle, or fail loudly instead
# of shipping a bundle that dies at startup with a missing-DLL dialog.
$MissingCritical = @()
foreach ($name in @('SDL3.dll', 'libstdc++-6.dll', 'libgcc_s_seh-1.dll', 'libwinpthread-1.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $DistDir $name))) {
        $MissingCritical += $name
    }
}
if (-not (Get-ChildItem -LiteralPath $DistDir -Filter 'avcodec-*.dll' -ErrorAction SilentlyContinue)) {
    $MissingCritical += 'avcodec-*.dll'
}
if (-not (Get-ChildItem -LiteralPath $DistDir -Filter 'avutil-*.dll' -ErrorAction SilentlyContinue)) {
    $MissingCritical += 'avutil-*.dll'
}
if ($MissingCritical.Count -gt 0) {
    Write-Error "Bundle is missing critical DLLs: $($MissingCritical -join ', '). Reinstall the MSYS2 dependency set (docs/BUILDING.md) so the DLLs exist in $MsysBin, then re-run this script."
    exit 1
}

# 3. Zip it up (overwrite).
if (Test-Path -LiteralPath $ZipPath) {
    Remove-Item -LiteralPath $ZipPath -Force
}
Compress-Archive -Path $DistDir -DestinationPath $ZipPath -CompressionLevel Optimal

# 4. Summary.
$TotalBytes = (Get-ChildItem -LiteralPath $DistDir -Recurse |
    Measure-Object -Property Length -Sum).Sum
$SizeMB = [math]::Round($TotalBytes / 1MB, 1)
Write-Output "Cosmic Desk bundle: $DllCount DLL(s), $SizeMB MB"
Write-Output "Folder: $DistDir"
Write-Output "Zip: $ZipPath"
Write-Output ""
Write-Output "Usage: unzip CosmicDesk-windows-x64.zip and run CosmicDesk.exe."
Write-Output "MSYS2 is NOT required on the target machine."
Write-Output "First run: allow Cosmic Desk through the Windows Firewall prompt."

# 5. Sanity check: the zip exists and its contents are listed.
if (-not (Test-Path -LiteralPath $ZipPath)) {
    Write-Error "Zip was not created: $ZipPath"
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    Write-Output ""
    Write-Output "Zip contents ($($zip.Entries.Count) entries):"
    $zip.Entries | ForEach-Object { Write-Output "  $($_.FullName)" }
} finally {
    $zip.Dispose()
}