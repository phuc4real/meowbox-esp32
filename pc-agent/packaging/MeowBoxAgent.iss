; Inno Setup 6 script for MeowBox Agent (docs/firmware_v0.2.0.md, 2.11). Built by build.ps1:
;   iscc /DAppVersion=0.3.1 MeowBoxAgent.iss   ->  build\MeowBoxAgent-Setup-0.3.1.exe
; Two exes from `cargo build --release`: MeowBoxAgent.exe (tray) and meowbox.exe (command line).
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
SetupIconFile=..\assets\meowbox.ico
Compression=lzma2/max
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
ChangesEnvironment=yes
CloseApplications=force
LicenseFile=..\..\LICENSE
; the cat in Swiss colours instead of the stock pictures (made by assets/make_art.py), 100/150/200 %
WizardImageFile=wizard-100.bmp,wizard-150.bmp,wizard-200.bmp
WizardSmallImageFile=wizard-small-100.bmp,wizard-small-150.bmp,wizard-small-200.bmp
; Vietnamese on a Vietnamese Windows, English otherwise, without asking
ShowLanguageDialog=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
; Inno's unofficial translation (jrsoftware/issrc, Files/Languages/Unofficial), vendored with a UTF-8 BOM
Name: "vi"; MessagesFile: "Vietnamese.isl"

[CustomMessages]
en.TaskAutostart=Start MeowBox Agent with Windows
vi.TaskAutostart=Chạy MeowBox Agent cùng Windows
en.TaskPath=Add meowbox.exe to PATH (to type meowbox notify "Build done" in any terminal)
vi.TaskPath=Thêm meowbox.exe vào PATH (để gõ meowbox notify "Build xong" ở terminal bất kỳ)
en.Downloading=Downloading yt-dlp (for YouTube)...
vi.Downloading=Đang tải yt-dlp (cho YouTube)...
en.StartAgent=Start MeowBox Agent
vi.StartAgent=Mở MeowBox Agent
en.KeptSettings=Upgrade: your speaker, token and other settings are kept.
vi.KeptSettings=Cập nhật: loa, token và các cài đặt khác được giữ nguyên.
en.DeleteSettings=Delete the MeowBox Agent settings, logs and downloaded tools too (token, speaker, yt-dlp, ffmpeg)?
vi.DeleteSettings=Xoá luôn cài đặt, log và công cụ đã tải của MeowBox Agent (token, loa, yt-dlp, ffmpeg)?

[Tasks]
Name: "autostart"; Description: "{cm:TaskAutostart}"
Name: "addpath"; Description: "{cm:TaskPath}"; Flags: unchecked

[InstallDelete]
; left by the Python agent (0.2.x, PyInstaller): its runtime folder, and the yt-dlp zipapp it ran (0.3 uses the
; onedir build in bin\yt-dlp\<version>\)
Type: filesandordirs; Name: "{app}\_internal"
Type: files; Name: "{localappdata}\MeowBox\bin\yt-dlp.pyz"
Type: files; Name: "{localappdata}\MeowBox\bin\yt-dlp.tmp"
Type: files; Name: "{localappdata}\MeowBox\bin\yt-dlp.checked"

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
Filename: "{app}\meowbox.exe"; Parameters: "tools update"; StatusMsg: "{cm:Downloading}"; \
  Flags: runhidden runasoriginaluser waituntilterminated
; first install: open the agent window (it starts at the connect page); an upgrade starts it in the tray
Filename: "{app}\MeowBoxAgent.exe"; Parameters: "--open"; Description: "{cm:StartAgent}"; Check: not IsUpgrade; \
  Flags: nowait postinstall skipifsilent runasoriginaluser
Filename: "{app}\MeowBoxAgent.exe"; Description: "{cm:StartAgent}"; Check: IsUpgrade; \
  Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM MeowBoxAgent.exe /T"; Flags: runhidden; RunOnceId: "kill"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MeowBox Agent"""; Flags: runhidden; RunOnceId: "fw"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MeowBox Agent relay (Public)"""; Flags: runhidden; RunOnceId: "fwpublic"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""MeowBox Agent relay (Private)"""; Flags: runhidden; RunOnceId: "fwprivate"

[Code]
var
  Upgrade: Boolean;

// An agent (0.2.x Python or 0.3.x Rust) is installed already: same AppId.
function InitializeSetup(): Boolean;
var
  Key: string;
begin
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{52AF3136-FE9D-48AF-8145-C3506CF9C1E5}_is1';
  Upgrade := RegKeyExists(HKLM, Key) or RegKeyExists(HKCU, Key);
  Result := True;
end;

function IsUpgrade(): Boolean;
begin
  Result := Upgrade;
end;

// The "Ready to install" page says in one line that an upgrade keeps the settings.
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := '';
  if Upgrade then
    Result := CustomMessage('KeptSettings') + NewLine + NewLine;
  if MemoDirInfo <> '' then
    Result := Result + MemoDirInfo + NewLine + NewLine;
  if MemoGroupInfo <> '' then
    Result := Result + MemoGroupInfo + NewLine + NewLine;
  if MemoTasksInfo <> '' then
    Result := Result + MemoTasksInfo;
end;

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
      if MsgBox(CustomMessage('DeleteSettings'), mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then begin
        DelTree(ExpandConstant('{userappdata}\MeowBox'), True, True, True);
        DelTree(ExpandConstant('{localappdata}\MeowBox'), True, True, True);
      end;
  end;
end;
