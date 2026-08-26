; Cosmic Desk - Windows installer (plan M6.4).
;
; Inno Setup script (6 or 7) wrapping the standalone bundle produced by
; packaging/windows/make-zip.ps1. Per-user install by default (no admin/UAC,
; machine-wide is offered), Start Menu and optional desktop shortcuts,
; uninstaller included.
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
; Per-user by default (installs to %LocalAppData%\Programs\Cosmic Desk, no UAC
; prompt); an admin can opt into a machine-wide install from the first wizard
; page or with /ALLUSERS on the command line.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
UninstallDisplayIcon={app}\{#AppExeName}
WizardStyle=modern
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\assets\icon.ico

[Files]
Source: "..\..\dist\CosmicDesk\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
