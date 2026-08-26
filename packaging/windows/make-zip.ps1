# Cosmic Desk - Windows standalone zip packaging (plan M6.2).
#
# Usage (run from the repo root, after `cmake --build build`):
#   powershell -ExecutionPolicy Bypass -File packaging\windows\make-zip.ps1
#
# Produces dist\CosmicDesk\ (cosmicdesk.exe + assets + MinGW DLLs + LICENSE +
# README.md) and dist\CosmicDesk-windows-x64.zip. The zip is self-contained:
# MSYS2 is NOT required on the target machine.
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

# Clean and recreate the dist folder.
if (Test-Path -LiteralPath $DistDir) {
    Remove-Item -LiteralPath $DistDir -Recurse -Force
}
New-Item -ItemType Directory -Path $DistDir -Force | Out-Null

# 1. Core files: exe, whole assets tree (incl. shaders), LICENSE, README.
Copy-Item -LiteralPath $Exe -Destination $DistDir
Copy-Item -LiteralPath (Join-Path $RepoRoot "assets") -Destination $DistDir -Recurse
Copy-Item -LiteralPath (Join-Path $RepoRoot "LICENSE") -Destination $DistDir
Copy-Item -LiteralPath (Join-Path $RepoRoot "README.md") -Destination $DistDir

# 2. DLL bundling.
$DllPaths = @()
# MSYS2 root: when running inside an MSYS2 shell, MSYSTEM_PREFIX points at the
# active prefix (e.g. C:/msys64/ucrt64); the root is its parent. Outside MSYS2,
# fall back to the default install location.
$MsysRoot = if ($env:MSYSTEM_PREFIX) {
    if ($env:MSYSTEM_PREFIX -match '/(ucrt64|mingw64|clang64)$') {
        Split-Path -Parent $env:MSYSTEM_PREFIX
    } else {
        $env:MSYSTEM_PREFIX
    }
} else {
    "C:\msys64"
}
# ntldd lives in the UCRT64 bin dir under the root.
$MsysBin = Join-Path $MsysRoot "ucrt64\bin"
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
# used when ntldd is unavailable. Copies only what exists.
if (-not $Ntldd) {
    $FallbackPatterns = @(
        'SDL2.dll',
        'libSDL2-2-0.dll',
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
        'libglib-2.0-0.dll',
        'libpcre2-8-0.dll',
        'libharfbuzz-0.dll',
        'libfreetype-6.dll',
        'libpng16-16.dll',
        'libgraphite2.dll',
        'libbrotli*.dll',
        'libffi-8.dll',
        'libxml2-2.dll',
        'libmp3lame-0.dll',
        'libvorbis*.dll',
        'libogg-0.dll',
        'libspeex-1.dll',
        'libwebp*.dll',
        'libdav1d-*.dll',
        'libaom*.dll',
        'libva*.dll',
        'libvpl*.dll'
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