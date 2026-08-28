# Cosmic Desk - Windows service uninstaller (plan M7.3).
#
# Usage (run from the repo root):
#   powershell -ExecutionPolicy Bypass -File packaging\windows\uninstall-service.ps1
#
# Stops and deletes the "CosmicDeskService" Windows service. A missing
# service is a clean no-op (exit 0), not an error. Self-elevates through a
# single UAC prompt. Files are NOT touched - the installer removes them.
# ASCII only.

param(
    [string]$ServiceExe = ""
)

$ErrorActionPreference = "Stop"

# The -ServiceExe param is accepted for symmetry with install-service.ps1
# (the self-elevation re-launch passes it through); uninstall does not need
# the binary path.

# Note: no 2>&1 on the sc.exe calls. In Windows PowerShell 5.1, redirecting
# native stderr into the success stream makes it subject to
# $ErrorActionPreference = "Stop", so a failing sc.exe would throw instead of
# setting $LASTEXITCODE. sc.exe's own error text still prints to the console.

# Query the service first: a missing service is a clean no-op, not an error.
# This check needs no admin rights, so it runs before self-elevation: a user
# who never installed the service gets no UAC prompt.
$Query = & sc.exe query CosmicDeskService
if ($LASTEXITCODE -eq 1060) {
    # ERROR_SERVICE_DOES_NOT_EXIST.
    Write-Output "CosmicDeskService is not installed; nothing to do."
    exit 0
}

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

# Query the service again: the elevated child re-runs the same query, which
# confirms the service still exists before the Get-Service checks below.
$Query = & sc.exe query CosmicDeskService

# Stop it if it is not already stopped. sc.exe stop on a stopped service
# fails with 1062 (ERROR_SERVICE_NOT_ACTIVE), handled gracefully below.
# Get-Service is locale-independent, unlike parsing sc.exe query output (the
# STATE label is localized on non-English Windows); safe here because the
# query above already confirmed the service exists.
if ((Get-Service -Name CosmicDeskService).Status -eq 'Stopped') {
    Write-Output "CosmicDeskService is already stopped."
} else {
    & sc.exe stop CosmicDeskService
    if ($LASTEXITCODE -eq 1062) {
        # ERROR_SERVICE_NOT_ACTIVE: it stopped between the query and the stop.
        Write-Output "CosmicDeskService is already stopped."
    } elseif ($LASTEXITCODE -ne 0) {
        Write-Error "sc.exe stop failed (exit code $LASTEXITCODE)."
    } else {
        # Poll Get-Service until the service reports Stopped (up to 30 s),
        # printing a '.' per attempt. Get-Service is locale-independent,
        # unlike parsing sc.exe query output: the STATE label is localized on
        # non-English Windows.
        $Stopped = $false
        for ($i = 0; $i -lt 30; $i++) {
            Write-Output -NoNewline "."
            if ((Get-Service -Name CosmicDeskService).Status -eq 'Stopped') {
                $Stopped = $true
                break
            }
            Start-Sleep -Seconds 1
        }
        Write-Output ""
        if (-not $Stopped) {
            Write-Error "CosmicDeskService did not stop within 30 s."
        }
        Write-Output "CosmicDeskService stopped."
    }
}

# Delete the service and confirm.
& sc.exe delete CosmicDeskService
if ($LASTEXITCODE -ne 0) {
    Write-Error "sc.exe delete failed (exit code $LASTEXITCODE)."
}
Write-Output "CosmicDeskService deleted."
Write-Output "Uninstall complete. The service is removed; the files were not touched."
