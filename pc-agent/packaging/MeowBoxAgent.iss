; Inno Setup 6 script for MeowBox Agent (docs/firmware_v0.2.0.md, 2.11). Built by build.ps1:
;   iscc /DAppVersion=0.2.0 MeowBoxAgent.iss   ->  build\MeowBoxAgent-Setup-0.2.0.exe
; Installs for the whole machine (Program Files, admin) so it can add the firewall rules itself.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define AppName "MeowBox Agent"
#define Dist "build\dist\MeowBoxAgent"

[Setup]
AppId={{52AF3136-FE9D-48AF-8145-C3506CF9C1E5}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=MeowBox
AppPublisherURL=https://github.com/phuc4real/meowbox-esp32
DefaultDirName={autopf}\MeowBox Agent
DefaultGroupName=MeowBox Agent
UninstallDisplayIcon={app}\MeowBoxAgent.exe
OutputDir=build
OutputBaseFilename=MeowBoxAgent-Setup-{#AppVersion}
SetupIconFile=build\meowbox.ico
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
ChangesEnvironment=yes
CloseApplications=force
LicenseFile=..\..\LICENSE

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Run MeowBox Agent when Windows starts"
Name: "addpath"; Description: "Add meowbox.exe to PATH (meowbox notify ""Build done"" in any terminal)"; Flags: unchecked

[Files]
Source: "{#Dist}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\MeowBox Agent"; Filename: "{app}\MeowBoxAgent.exe"
Name: "{group}\Uninstall MeowBox Agent"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "MeowBox Agent"; \
  ValueData: """{app}\MeowBoxAgent.exe"""; Tasks: autostart; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; \
  Tasks: addpath; Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
; firewall: the relay (speaker -> PC TCP), Private networks only. The agent does no mDNS / UDP discovery.
; A Public network gets its own rule from the agent's "Connect speaker" window, only if the relay test fails there.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MeowBox Agent"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""MeowBox Agent"" dir=in action=allow program=""{app}\MeowBoxAgent.exe"" profile=private enable=yes"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""MeowBox Agent"" dir=in action=allow program=""{app}\meowbox.exe"" profile=private enable=yes"; Flags: runhidden
; yt-dlp is not shipped: fetch the latest one for the user (if offline, the agent fetches it on its first start).
; ffmpeg is fetched by the agent only if YouTube live is used.
Filename: "{app}\meowbox.exe"; Parameters: "tools update"; StatusMsg: "Downloading yt-dlp (YouTube)..."; \
  Flags: runhidden runasoriginaluser waituntilterminated
Filename: "{app}\MeowBoxAgent.exe"; Description: "Start MeowBox Agent"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM MeowBoxAgent.exe /T"; Flags: runhidden; RunOnceId: "kill"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MeowBox Agent"""; Flags: runhidden; RunOnceId: "fw"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MeowBox Agent relay (Public)"""; Flags: runhidden; RunOnceId: "fwpublic"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MeowBox Agent relay (Private)"""; Flags: runhidden; RunOnceId: "fwprivate"

[Code]
function NeedsAddPath(Dir: string): Boolean;
var
  Path: string;
begin
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', Path) then
    Path := '';
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Path) + ';') = 0;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if CurStep = ssInstall then begin
    // an agent already running would lock its files; the old Python agent ran from Task Scheduler / Startup
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM MeowBoxAgent.exe /T', '', SW_HIDE, ewWaitUntilTerminated, Code);
    Exec(ExpandConstant('{sys}\schtasks.exe'), '/Delete /TN "MeowBox Agent" /F', '', SW_HIDE, ewWaitUntilTerminated, Code);
    DeleteFile(ExpandConstant('{userstartup}\MeowBox Agent.lnk'));
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Path, Dir: string;
  P: Integer;
begin
  if CurUninstallStep = usPostUninstall then begin
    // take the install folder back out of the user PATH
    Dir := ExpandConstant('{app}');
    if RegQueryStringValue(HKCU, 'Environment', 'Path', Path) then begin
      P := Pos(';' + Uppercase(Dir), Uppercase(Path));
      if P > 0 then begin
        Delete(Path, P, Length(Dir) + 1);
        RegWriteExpandStringValue(HKCU, 'Environment', 'Path', Path);
      end;
    end;
    if DirExists(ExpandConstant('{userappdata}\MeowBox')) or DirExists(ExpandConstant('{localappdata}\MeowBox')) then
      if MsgBox('Delete the MeowBox Agent settings, logs and downloaded tools too (token, speaker, yt-dlp, ffmpeg)?',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then begin
        DelTree(ExpandConstant('{userappdata}\MeowBox'), True, True, True);
        DelTree(ExpandConstant('{localappdata}\MeowBox'), True, True, True);
      end;
  end;
end;
