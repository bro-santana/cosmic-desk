; Cosmic Desk - Windows installer (plan M6.4).
;
; Inno Setup script (6 or 7) wrapping the standalone bundle produced by
; packaging/windows/make-zip.ps1. Machine-wide install by default (the first
; wizard page offers a per-user install, also reachable as /CURRENTUSER),
; Start Menu and optional desktop shortcuts, uninstaller included. The
; shortcuts run cosmicdesk.exe --shortcut, which starts the Cosmic Desk
; service when needed (plan M9). A service task (checked by default, plan
; M10) installs the Cosmic Desk service, which streams UAC prompts, the lock
; screen and the logon screen; the uninstaller removes it first.
;
; The service is intentionally tied to the machine-wide install: the service
; runs as LocalSystem, and executing binaries from a user-writable per-user
; location (%LocalAppData%\Programs\...) would let any process of that user
; swap the exe/DLLs and run arbitrary code as SYSTEM. Program Files is
; admin-only, so the machine-wide install is the safe home for the service.
;
; Usage (from the repo root, after configuring CMake and running make-zip.ps1):
;   ISCC.exe packaging\windows\installer.iss
; Produces dist\CosmicDesk-windows-x64-setup.exe.

; The version is NOT derived here. cmake/CosmicDeskVersion.cmake derives it
; from `git describe` at configure time and writes the file included below, so
; this installer and the Debian package can never disagree about the version.
; Configure the build first (`cmake -B build`); a passed
; /DAppVersion=... still overrides the derived value.
#include "..\..\build\packaging\windows\version.iss"

#define AppName "Cosmic Desk"
#define AppExeName "cosmicdesk.exe"

[Setup]
; Doubled opening brace is required so Inno does not parse the GUID as a constant.
AppId={{8B6D4A2F-1C3E-4A5B-9D7F-2E4C6A8B0D1E}
AppName={#AppName}
AppVersion={#AppVersion}
VersionInfoVersion={#VersionInfoVersion}
AppPublisher=Cosmic Desk
DefaultDirName={autopf}\Cosmic Desk
DefaultGroupName=Cosmic Desk
DisableProgramGroupPage=yes
OutputBaseFilename=CosmicDesk-windows-x64-setup
OutputDir=..\..\dist
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Machine-wide by default (installs to {autopf}, one UAC prompt; the service
; runs from this admin-only location for security, see the header). A user
; can opt into a per-user install from the first wizard page or with
; /CURRENTUSER on the command line; the service task is skipped in that mode.
PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog commandline
UninstallDisplayIcon={app}\{#AppExeName}
WizardStyle=modern
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\assets\icon.ico

[Files]
Source: "..\..\dist\CosmicDesk\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"
Name: "service"; Description: "Install the Cosmic Desk service (recommended: lets you stream UAC prompts, the lock screen and the logon screen; requires the machine-wide install)"; GroupDescription: "Additional components:"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Parameters: "--shortcut"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Parameters: "--shortcut"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
; The service only makes sense (and is only safe) in admin install mode:
; per-user installs leave the binaries in a user-writable folder, which must
; never be executed by the LocalSystem service (see the file header).
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\install-service.ps1"" -ServiceExe ""{app}\tools\cosmicsvc.exe"""; Tasks: service; Check: IsAdminInstallMode; StatusMsg: "Installing the Cosmic Desk service..."; Flags: runhidden

[UninstallRun]
; Same gating as [Run]: only a machine-wide install can own the service.
; The script exits 0 without elevating when the service does not exist.
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\uninstall-service.ps1"" -ServiceExe ""{app}\tools\cosmicsvc.exe"""; Check: IsAdminInstallMode; Flags: runhidden
