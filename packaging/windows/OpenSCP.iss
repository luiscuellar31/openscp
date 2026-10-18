; Inno Setup script for the OpenSCP setup. scripts\package\windows.ps1 compiles
; it with ISCC /DAppVersion=<version> /DStageDir=<staged app> /DOutputDir=<dir>.
;
; The setup installs for the current user by default, without administrator
; rights, and offers an installation for all users. OpenSCP updates itself by
; running this setup silently with /CURRENTUSER or /ALLUSERS, /WAITPID=<its
; process id> and, for the current user, /RELAUNCH=1 (see
; ui/logic/updates/WindowsSetupInstaller.cpp).

#ifndef AppVersion
  #error Define AppVersion, for example /DAppVersion=1.2.0
#endif
#ifndef StageDir
  #error Define StageDir, the folder with the staged application
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; Names the uninstall key that OpenSCP looks for to update itself; it must
; match kWindowsSetupAppId in ui/logic/updates/UpdateMetadata.cpp and never
; change. The first brace is doubled because Inno Setup reads { as a constant.
AppId={{2C03B5C3-87DD-4DAB-B609-F89E831909E6}
AppName=OpenSCP
AppVersion={#AppVersion}
AppVerName=OpenSCP {#AppVersion}
AppPublisher=OpenSCP contributors
AppPublisherURL=https://github.com/luiscuellar31/openscp
AppSupportURL=https://github.com/luiscuellar31/openscp/issues
AppUpdatesURL=https://github.com/luiscuellar31/openscp/releases
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}
VersionInfoCompany=OpenSCP contributors
VersionInfoDescription=OpenSCP Setup
; Windows 10 1809, the oldest Windows that Qt 6 supports.
MinVersion=10.0.17763
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
DefaultDirName={autopf}\OpenSCP
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
SetupIconFile=..\..\assets\windows\OpenSCP.ico
UninstallDisplayIcon={app}\OpenSCP.exe
UninstallDisplayName=OpenSCP
OutputDir={#OutputDir}
OutputBaseFilename=OpenSCP-{#AppVersion}-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Files in use are closed through the Restart Manager; OpenSCP itself has
; ended by then when it runs an update.
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "portuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\OpenSCP"; Filename: "{app}\OpenSCP.exe"
Name: "{autodesktop}\OpenSCP"; Filename: "{app}\OpenSCP.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\OpenSCP.exe"; Description: "{cm:LaunchProgram,OpenSCP}"; Flags: nowait postinstall skipifsilent
; After an update of the current user's installation, OpenSCP starts again.
; An update for all users does not ask for it: OpenSCP starts that setup
; elevated, and runasoriginaluser has no effect in a setup started elevated,
; so OpenSCP would run with administrator rights.
Filename: "{app}\OpenSCP.exe"; Flags: nowait runasoriginaluser; Check: RelaunchRequested

[Code]
const
  SYNCHRONIZE = $00100000;
  WAIT_TIMEOUT = $00000102;
  // How long an update waits for OpenSCP to close.
  WAIT_FOR_OPENSCP_MS = 120000;

function OpenProcess(DesiredAccess: Cardinal; InheritHandle: Boolean;
  ProcessId: Cardinal): THandle;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(Handle: THandle; Milliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

// An update starts this setup and then closes OpenSCP, which must end
// before its files are replaced.
function InitializeSetup(): Boolean;
var
  ProcessId: Integer;
  Process: THandle;
begin
  Result := True;
  ProcessId := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if ProcessId <= 0 then
    Exit;
  Process := OpenProcess(SYNCHRONIZE, False, ProcessId);
  if Process = 0 then
    Exit;
  Log(Format('Waiting for OpenSCP (process %d) to close.', [ProcessId]));
  if WaitForSingleObject(Process, WAIT_FOR_OPENSCP_MS) = WAIT_TIMEOUT then
  begin
    Log('OpenSCP did not close; the update was not installed.');
    Result := False;
  end;
  CloseHandle(Process);
end;

function RelaunchRequested(): Boolean;
begin
  Result := WizardSilent() and (ExpandConstant('{param:RELAUNCH|0}') = '1');
end;
