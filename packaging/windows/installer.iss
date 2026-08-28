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
; The service task is hidden (Check: IsAdminInstallMode) in per-user mode.
;
; Usage (from the repo root, after configuring CMake and running make-zip.ps1):
;   ISCC.exe packaging\windows\installer.iss
; Produces dist\CosmicDesk-windows-x64-setup.exe.

; The version is NOT derived here. cmake/CosmicDeskVersion.cmake derives it
; from `git describe` and writes the file included below, so this installer and
; the Debian package can never disagree about the version. It is refreshed on
; every build, so BUILD BEFORE PACKAGING - `git tag` changes what describe
; reports, and only a build picks that up. A passed /DAppVersion= still wins.
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
; Without this the Add/Remove Programs title defaults to AppVerName,
; i.e. "Cosmic Desk version 1.2.3" - the version belongs in the Version
; column (DisplayVersion), not baked into the product name.
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
WizardStyle=modern
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\assets\icon.ico

[Files]
Source: "..\..\dist\CosmicDesk\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional icons:"
; Check: IsAdminInstallMode hides the task entirely for per-user installs:
; the LocalSystem service must never execute binaries from a user-writable
; folder (see the file header).
Name: "service"; Description: "Install the Cosmic Desk service (recommended: lets you stream UAC prompts, the lock screen and the logon screen)"; GroupDescription: "Additional components:"; Check: IsAdminInstallMode

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

[Code]
{ Must match AppId above. Inno registers the uninstall entry under this name in
  HKCU for a per-user install and in HKLM for a machine-wide one - two separate
  keys, not one. }
const
  UninstKey =
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\{8B6D4A2F-1C3E-4A5B-9D7F-2E4C6A8B0D1E}_is1';

{ Releases up to v0.1.5-alpha shipped PrivilegesRequired=lowest and so
  registered under HKCU; v0.2.2-alpha onwards installs machine-wide and
  registers under HKLM. Because Inno keys the entry by AppId *within a hive*,
  installing one over the other leaves the older install's entry and files
  untouched, and Apps & Features lists a stale Cosmic Desk with the old version
  indefinitely. The same happens whenever the install-mode dialog is answered
  differently between two runs, so this is not only a one-off migration.

  ArchitecturesInstallIn64BitMode makes this a 64-bit install on x64, so plain
  HKEY_LOCAL_MACHINE is the 64-bit view - the same view Setup writes to.

  Known gap: PrepareToInstall runs after elevation, so HKEY_CURRENT_USER is the
  ELEVATING account's profile. If the user elevates with a different admin
  account, a per-user install belonging to the logged-on user is invisible here
  and has to be removed by hand. Same account + UAC, the common case, is fine. }
function OtherScopeRootKey: Integer;
begin
  if IsAdminInstallMode then
    Result := HKEY_CURRENT_USER
  else
    Result := HKEY_LOCAL_MACHINE;
end;

function OtherScopeUninstallString(var UninstallString: String): Boolean;
begin
  Result := RegQueryStringValue(OtherScopeRootKey, UninstKey, 'UninstallString',
                                UninstallString) and (UninstallString <> '');
end;

function OtherScopeInstalled: Boolean;
var
  Ignored: String;
begin
  Result := OtherScopeUninstallString(Ignored);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  UninstallString, OtherName: String;
  ResultCode, Waited: Integer;
begin
  Result := '';
  if not OtherScopeUninstallString(UninstallString) then
    Exit;

  if not RegQueryStringValue(OtherScopeRootKey, UninstKey, 'DisplayName', OtherName) then
    OtherName := '{#AppName}';

  if SuppressibleMsgBox('Another copy of Cosmic Desk is already installed, in the'
       + ' other install scope:' + #13#10#13#10 + OtherName + #13#10#13#10
       + 'Leaving it in place would leave a second, permanently stale entry in'
       + ' Apps & Features. Remove it before continuing?',
       mbConfirmation, MB_YESNO, IDYES) <> IDYES then
  begin
    Result := 'The existing installation must be removed before Setup can continue.';
    Exit;
  end;

  if not Exec(RemoveQuotes(UninstallString), '/SILENT /NORESTART /SUPPRESSMSGBOXES',
              '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Result := 'Could not run the existing uninstaller: ' + SysErrorMessage(ResultCode);
    Exit;
  end;

  { unins000.exe relaunches itself from a copy in the temp directory and the
    process we waited on exits immediately, so a successful Exec proves
    nothing. Poll the registry until the entry is actually gone. }
  Waited := 0;
  while OtherScopeInstalled and (Waited < 60000) do
  begin
    Sleep(500);
    Waited := Waited + 500;
  end;

  if OtherScopeInstalled then
    Result := 'The existing installation was not removed. Uninstall Cosmic Desk from'
              + ' Apps & Features, then run Setup again.';
end;
