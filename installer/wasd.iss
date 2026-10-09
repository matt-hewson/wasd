; WASD installer, Inno Setup 6.
;
;   ISCC installer\wasd.iss                      the real installer -> dist\WASD-<version>-setup.exe
;   ISCC /DTestBuild installer\wasd.iss          a separate test identity (tests\pkg\test_installer.py)
;   ISCC /DTestBuild /DVersionOverride=0.1.1 ... a "newer" test build, for the upgrade test
;   ISCC /O<dir> ...                             output folder (default dist\)
;   ISCC /DBinDir=<dir> ...                      where the exes are (default build\; release.ps1: build\release)
;
; Per-user install, no admin: %LOCALAPPDATA%\Programs\WASD. The name,
; version and publisher come from the built WASD.exe (src\version.h), so
; build first (build.ps1 -Release, tools\fetch_python.ps1).
;
; TestBuild changes every identity the tests touch -- AppId (so its own
; uninstall entry), names, Start menu shortcut, the app's single-instance
; mutex, the data folder an uninstall may remove -- so a test never meets a
; real install.

#define Root AddBackslash(SourcePath) + ".."
; BinDir: where the two exes are (release.ps1 passes build\release).
#ifndef BinDir
  #define BinDir Root + "\build"
#endif
#define AppExe BinDir + "\WASD.exe"
#if !FileExists(AppExe)
  #error WASD.exe is missing from BinDir: run build.ps1 first
#endif

#ifdef VersionOverride
  #define AppVersion VersionOverride
#else
  #define AppVersion GetStringFileInfo(AppExe, "ProductVersion")
#endif
#define FullName GetStringFileInfo(AppExe, "ProductName")
#define Publisher GetStringFileInfo(AppExe, "CompanyName")

#ifdef TestBuild
  #define AppGuid "{{6F0C5E1A-3B7D-4C2E-9A41-7E2B9D0F5C13}"
  #define ShortName "WASD (test)"
  #define DisplayName FullName + " (test)"
  #define InstanceMutex "AmishGoose.WASD.installertest"
  #define SetupName "WASD-test-" + AppVersion + "-setup"
#else
  #define AppGuid "{{A3E1F5D2-8C46-4B9A-B0E7-5D2C1F9A6E48}"
  #define ShortName "WASD"
  #define DisplayName FullName
  #define InstanceMutex "AmishGoose.WASD"
  #define SetupName "WASD-" + AppVersion + "-setup"
#endif

[Setup]
AppId={#AppGuid}
AppName={#DisplayName}
AppVersion={#AppVersion}
AppVerName={#DisplayName} {#AppVersion}
AppPublisher={#Publisher}
AppCopyright=Copyright (C) 2026 {#Publisher}
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#FullName}
UninstallDisplayName={#DisplayName}
UninstallDisplayIcon={app}\bin\WASD.exe
; Per user: no UAC prompt; {autopf} is %LOCALAPPDATA%\Programs here.
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#ShortName}
DefaultGroupName={#ShortName}
DisableProgramGroupPage=yes
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; The app's own single-instance mutex (WASD.exe): setup
; and uninstall ask for it to be closed first instead of meeting locked files.
AppMutex={#InstanceMutex}
CloseApplications=no
SetupIconFile={#Root}\assets\app.ico
WizardStyle=modern
OutputDir={#Root}\dist
OutputBaseFilename={#SetupName}
#ifdef TestBuild
Compression=lzma2/fast
#else
Compression=lzma2/max
SolidCompression=yes
#endif

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Files]
Source: "{#BinDir}\WASD.exe";       DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#BinDir}\wasd-cli.exe";   DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#Root}\data\*.tsv";            DestDir: "{app}\data"; Flags: ignoreversion
Source: "{#Root}\data\THIRD_PARTY_NOTICES.md";  DestDir: "{app}\data"; Flags: ignoreversion
Source: "{#Root}\LICENSE";                  DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#Root}\templates\live.html";             DestDir: "{app}\templates"; Flags: ignoreversion
Source: "{#Root}\templates\results_template.html"; DestDir: "{app}\templates"; Flags: ignoreversion
Source: "{#Root}\tools\ds3_archive.py";        DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#Root}\tools\extract_treasures.py";  DestDir: "{app}\tools"; Flags: ignoreversion
Source: "{#Root}\build\python\*";        DestDir: "{app}\python"; Flags: ignoreversion
Source: "{#Root}\installer\README.txt";  DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
; AppUserModelID matches WASD.exe's own (kAppUserModelId), so a pinned
; shortcut and the running window share one taskbar button.
Name: "{autoprograms}\{#ShortName}"; Filename: "{app}\bin\WASD.exe"; WorkingDir: "{app}\bin"; AppUserModelID: "AmishGoose.WASD"; Comment: "{#FullName}"
Name: "{autodesktop}\{#ShortName}"; Filename: "{app}\bin\WASD.exe"; WorkingDir: "{app}\bin"; AppUserModelID: "AmishGoose.WASD"; Comment: "{#FullName}"; Tasks: desktopicon

[Run]
Filename: "{app}\bin\WASD.exe"; Description: "Launch WASD"; Flags: nowait postinstall skipifsilent

[InstallDelete]
; Data files earlier versions installed and this one doesn't; an upgrade over an
; older install would otherwise leave them behind.
Type: files; Name: "{app}\data\item_names.tsv"
Type: files; Name: "{app}\data\region_names.tsv"
Type: files; Name: "{app}\data\bonfire_names.tsv"
Type: files; Name: "{app}\data\LICENSE-AGPL-3.0.txt"
Type: files; Name: "{app}\data\area_levels.tsv"

[UninstallDelete]
; Python is run with -B, so nothing should be here; just in case.
Type: filesandordirs; Name: "{app}\tools\__pycache__"

[Code]
// Your data (settings, sessions, reports, map data, logs) lives in
// %LOCALAPPDATA%\WASD, outside the program folder, so
// an upgrade or uninstall keeps it. Uninstall asks whether to delete it too
// (default No); a silent uninstall keeps it unless /REMOVEUSERDATA=1.
function UserDataDir(): String;
begin
  Result := ExpandConstant('{localappdata}\{#ShortName}');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Remove: Boolean;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  if not DirExists(UserDataDir()) then Exit;
  if UninstallSilent() then
    Remove := ExpandConstant('{param:REMOVEUSERDATA|0}') = '1'
  else
    Remove := MsgBox('Also delete your WASD data (sessions, settings, map data, logs)?' + #13#10#13#10 +
                     UserDataDir() + #13#10#13#10 +
                     'Choose No to keep it for a later install.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;
  if Remove then DelTree(UserDataDir(), True, True, True);
end;
