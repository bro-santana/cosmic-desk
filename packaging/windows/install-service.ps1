# Cosmic Desk - Windows service installer (plan M7.3).
#
# Usage (run from the repo root, after building cosmicsvc.exe):
#   powershell -ExecutionPolicy Bypass -File packaging\windows\install-service.ps1
#
# Creates and starts the "CosmicDeskService" Windows service pointing at
# cosmicsvc.exe. The service spawns cosmicdesk.exe as SYSTEM in the active
# console session so UAC prompts, the lock screen and the logon screen stay
# streamable (PLAN.md M7). Self-elevates through a single UAC prompt.
#
# cosmicsvc.exe must sit in a tools\ subdir next to cosmicdesk.exe: the
# service sets its CWD by stripping two path components (PLAN.md M7.2).
# Dev build: build\tools\; package: dist\CosmicDesk\tools\.
# ASCII only.

param(
    [string]$ServiceExe = ""
)

$ErrorActionPreference = "Stop"

# Resolve the service binary. Default: the packaged bundle first (it is
# self-contained: the DLLs cosmicdesk.exe needs sit next to it), then the dev
# build for iterating on the service itself; an explicit -ServiceExe wins
# over both. The dev build CANNOT spawn cosmicdesk.exe unless its DLLs are
# next to it (the SCM has no MSYS2 PATH), so it only gets a warning.
if (-not $ServiceExe) {
    $PkgExe = Join-Path $PSScriptRoot "..\..\dist\CosmicDesk\tools\cosmicsvc.exe"
    $DevExe = Join-Path $PSScriptRoot "..\..\build\tools\cosmicsvc.exe"
    if (Test-Path -LiteralPath $PkgExe) {
        $ServiceExe = $PkgExe
    } elseif (Test-Path -LiteralPath $DevExe) {
        $ServiceExe = $DevExe
        Write-Output "WARNING: using the dev build ($DevExe). The service spawns"
        Write-Output "cosmicdesk.exe from the parent folder, which needs its DLLs next"
        Write-Output "to it. Run packaging\windows\make-zip.ps1 first (it produces the"
        Write-Output "self-contained dist\CosmicDesk bundle) or pass -ServiceExe"
        Write-Output "pointing into dist\CosmicDesk\tools\."
    } else {
        Write-Error "cosmicsvc.exe not found. Expected at: $PkgExe or $DevExe"
    }
}

# Whatever the source, the binary must exist and be an absolute path:
# sc.exe binPath= does not resolve relative paths.
if (-not (Test-Path -LiteralPath $ServiceExe)) {
    Write-Error "cosmicsvc.exe not found: $ServiceExe"
}
$ServiceExe = (Resolve-Path -LiteralPath $ServiceExe).Path
Write-Output "Using service binary: $ServiceExe"

# Self-elevate: sc.exe needs an admin console. Detect via the standard
# WindowsPrincipal check; if not elevated, re-launch ourselves elevated and
# wait for the child to finish (the child does the real work).
$Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$Principal = New-Object Security.Principal.WindowsPrincipal($Identity)
if (-not $Principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $p = Start-Process -FilePath "powershell" -Verb RunAs -Wait -PassThru -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", "`"$PSCommandPath`"",
        "-ServiceExe", "`"$ServiceExe`""
    )
    exit $p.ExitCode
}

# Note: no 2>&1 on the sc.exe calls. In Windows PowerShell 5.1, redirecting
# native stderr into the success stream makes it subject to
# $ErrorActionPreference = "Stop", so a failing sc.exe would throw instead of
# setting $LASTEXITCODE. sc.exe's own error text still prints to the console.

# Create the service. sc.exe requires a space after '=' in option= value.
& sc.exe create CosmicDeskService binPath= "`"$ServiceExe`"" start= auto
if ($LASTEXITCODE -eq 1073) {
    # ERROR_SERVICE_EXISTS: a previous install left the service behind.
    Write-Output "CosmicDeskService already exists; skipping create."
} elseif ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe create failed (exit code $LASTEXITCODE)."
} else {
    Write-Output "CosmicDeskService created."
}

# Failure recovery: restart the service up to 3 times, 60 s apart, after a
# crash; reset the counter after a day of uptime.
& sc.exe failure CosmicDeskService reset= 86400 actions= restart/60000/restart/60000/restart/60000
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe failure failed (exit code $LASTEXITCODE)."
}

# Human-readable description shown in services.msc.
& sc.exe description CosmicDeskService "Cosmic Desk host service. Runs the app elevated so UAC prompts, the lock screen and the logon screen stay streamable."
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe description failed (exit code $LASTEXITCODE)."
}

# Start it. 1056 = ERROR_SERVICE_ALREADY_RUNNING, which is fine.
& sc.exe start CosmicDeskService
if ($LASTEXITCODE -eq 1056) {
    Write-Output "CosmicDeskService is already running."
} elseif ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe start failed (exit code $LASTEXITCODE)."
}

# Poll Get-Service until the service reports Running (up to 30 s), printing a
# '.' per attempt. Get-Service is locale-independent, unlike parsing sc.exe
# query output: the STATE label is localized on non-English Windows.
$Running = $false
for ($i = 0; $i -lt 30; $i++) {
    Write-Output -NoNewline "."
    if ((Get-Service -Name CosmicDeskService).Status -eq 'Running') {
        $Running = $true
        break
    }
    Start-Sleep -Seconds 1
}
Write-Output ""
if (-not $Running) {
    Write-Error "CosmicDeskService did not reach RUNNING within 30 s. Check C:\Windows\Temp\cosmicsvc.log."
}

Write-Output "CosmicDeskService is installed and RUNNING."
Write-Output "The app now runs as SYSTEM in the active console session; UAC prompts,"
Write-Output "the lock screen and the logon screen stay streamable."
