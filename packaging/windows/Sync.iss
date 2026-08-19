; Inno Setup script for the Sync Windows desktop preview.
;
; Driven by scripts/package-windows.ps1, which passes every path in so this
; file never guesses at a build layout:
;   ISCC /DSyncVersion=... /DSyncBundleDir=... /DSyncOutputDir=... /DSyncSourceDir=... Sync.iss
;
; This produces an UNSIGNED installer. Authenticode signing and publication
; belong to the Noise Factor release workflow so credentials never enter this
; public repository.

#ifndef SyncVersion
  #error SyncVersion must be defined
#endif
#ifndef SyncBundleDir
  #error SyncBundleDir must be defined
#endif
#ifndef SyncOutputDir
  #error SyncOutputDir must be defined
#endif
#ifndef SyncSourceDir
  #error SyncSourceDir must be defined
#endif

[Setup]
AppId={{4F0B6E2C-5C7D-4E38-9A5E-1E2B7C6D8A31}
AppName=Sync
AppVersion={#SyncVersion}
AppVerName=Sync {#SyncVersion}
AppPublisher=Noise Factor LLC
AppPublisherURL=https://noisedeck.app/docs/Sync.md
AppSupportURL=https://github.com/noisefactorllc/sync
DefaultDirName={autopf}\Noise Factor\Sync
DefaultGroupName=Sync
DisableProgramGroupPage=yes
LicenseFile={#SyncSourceDir}\LICENSE
OutputDir={#SyncOutputDir}
OutputBaseFilename=Sync-{#SyncVersion}-x64-Setup
SetupIconFile={#SyncBundleDir}\Sync.ico
UninstallDisplayIcon={app}\Sync.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The daemon and the Spout provider are x64 only, and the pairing store lives
; under the invoking user's LOCALAPPDATA, so a per-user install works without
; elevation while an admin install still serves every user correctly.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
MinVersion=10.0

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "startupicon"; Description: "Start Sync when I sign in"; GroupDescription: "Startup"; Flags: unchecked

[Files]
; Everything staged by package-windows.ps1, including the pinned
; SpoutLibrary.dll and the runtime DLLs CMake resolved. The NDI runtime is
; deliberately absent: its licence does not permit redistribution, so the user
; installs it themselves and Sync discovers it at run time.
Source: "{#SyncBundleDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Sync"; Filename: "{app}\Sync.exe"
Name: "{group}\Sync documentation"; Filename: "https://noisedeck.app/docs/Sync.md"
Name: "{userstartup}\Sync"; Filename: "{app}\Sync.exe"; Tasks: startupicon

[Run]
Filename: "{app}\Sync.exe"; Description: "Start Sync now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Sync's tray app owns the helper through a kill-on-close job object, so
; closing the tray app is enough to stop syncd; /IM matches whichever instance
; is running. Failure is ignored because "not running" is the common case.
Filename: "{sys}\taskkill.exe"; Parameters: "/IM Sync.exe /F"; Flags: runhidden skipifdoesntexist; RunOnceId: "StopSyncTray"
Filename: "{sys}\taskkill.exe"; Parameters: "/IM syncd.exe /F"; Flags: runhidden skipifdoesntexist; RunOnceId: "StopSyncHelper"

[Code]
function InitializeSetup(): Boolean;
begin
  Result := True;
end;
