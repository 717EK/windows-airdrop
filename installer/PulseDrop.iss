#ifndef Stage
  #define Stage "..\build\installer-stage"
#endif
[Setup]
AppId={{DFB09F42-B316-45D4-92DC-9D9966086D99}
AppName=PulseDrop
AppVersion=0.2.7
DefaultDirName={autopf}\PulseDrop
DefaultGroupName=PulseDrop
OutputDir=..\dist
OutputBaseFilename=PulseDrop-Setup-0.2.7-x64
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19041
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\brand\PulseDrop.ico
WizardImageFile=..\assets\installer\wizard.bmp
WizardSmallImageFile=..\assets\installer\wizard-small.bmp
WizardImageStretch=yes
DisableWelcomePage=no
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\PulseDrop.exe
InfoBeforeFile=setup-notes.txt

[Tasks]
Name: "configure"; Description: "Configure TP-Link Archer T3U USB and TAP drivers (normal Wi-Fi on this USB adapter becomes unavailable)"; Flags: checkedonce
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: checkedonce

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "*.pdb,self-test.txt,*-test.txt,preview*.png"

[Icons]
Name: "{group}\PulseDrop"; Filename: "{app}\PulseDrop.exe"
Name: "{group}\PulseDrop quick start"; Filename: "{app}\QUICK-START.html"; IconFilename: "{app}\PulseDrop.exe"
Name: "{group}\Configure USB adapter"; Filename: "{app}\PulseDrop.exe"; Parameters: "--configure-driver"
Name: "{group}\Restore USB Wi-Fi driver"; Filename: "{app}\PulseDrop.exe"; Parameters: "--restore-driver"
Name: "{autodesktop}\PulseDrop"; Filename: "{app}\PulseDrop.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\PulseDrop.exe"; Description: "Open PulseDrop"; Flags: nowait postinstall skipifsilent runascurrentuser
Filename: "{app}\QUICK-START.html"; Description: "Read the quick start guide"; Flags: postinstall shellexec skipifsilent unchecked

[Code]
function RunDeviceSetup(Action: String): Boolean;
var ExitCode: Integer;
begin
  Result := Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\Setup-Devices.ps1') + '" -Action ' + Action + ' -Root "' + ExpandConstant('{app}') + '"',
    ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ExitCode);
  Result := Result and (ExitCode = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('configure') then
    if not RunDeviceSetup('Configure') then
      RaiseException('Adapter configuration did not finish. See C:\ProgramData\PulseDrop\setup.log.');
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  if MsgBox('Restore the TP-Link adapter to normal Wi-Fi before removing PulseDrop? Close PulseDrop first. Received files and saved driver backups are kept.', mbConfirmation, MB_YESNO) = IDYES then
    if not RunDeviceSetup('Restore') then
      Result := MsgBox('Driver restoration failed. Cancel removal to keep the restore tool?', mbError, MB_YESNO) = IDNO;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then RunDeviceSetup('Cleanup');
end;
