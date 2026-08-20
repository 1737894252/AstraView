#define MyAppName "AstraView"
#define MyAppVersion "2.0.0-preview.3"
#define MyAppPublisher "AstraView"
#define MyAppExeName "AstraView.exe"
#define ThumbnailClsid "{5E2D8E48-6F15-4C3D-AED8-BDA6544D2253}"

[Setup]
AppId={{2F14B965-52AF-460E-88D1-8C8EDB070890}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\AstraView
DefaultGroupName={#MyAppName}
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
OutputDir=..\artifacts\installer
OutputBaseFilename=AstraView-Setup-2.0.0-preview.3-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesAssociations=yes
DisableProgramGroupPage=yes

[Files]
Source: "..\artifacts\native-package-2.0.0-preview.3\*"; DestDir: "{app}"; Excludes: "*.pdb;*.map"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："; Flags: unchecked
Name: "contextmenu"; Description: "添加“使用 AstraView 打开”到图片右键菜单"; GroupDescription: "资源管理器集成："; Flags: unchecked

[Registry]
Root: HKLM; Subkey: "SOFTWARE\Classes\AstraView.Image"; ValueType: string; ValueName: ""; ValueData: "AstraView Image"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AstraView.Image\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKLM; Subkey: "SOFTWARE\Classes\AstraView.Image\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

[Run]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\ShellExtension\AstraView.ThumbnailProvider.dll"""; StatusMsg: "正在注册资源管理器缩略图组件…"; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\ShellExtension\AstraView.ThumbnailProvider.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterNativeThumbnailProvider"

[Code]
const
  ImageProgId = 'AstraView.Image';
  Extensions = '.bmp;.gif;.heic;.heif;.ico;.jpeg;.jpg;.png;.psb;.psd;.raw;.dng;.cr2;.nef;.arw;.tif;.tiff;.webp;.pdf';
  SHCNE_ASSOCCHANGED = $08000000;
  SHCNF_IDLIST = $0000;

procedure SHChangeNotify(wEventId: Integer; uFlags: Cardinal; dwItem1, dwItem2: Integer);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure NotifyShell;
begin
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
end;

procedure ConfigureContextMenu(Enable: Boolean);
var
  Items: TStringList;
  I: Integer;
  Ext: String;
  VerbKey: String;
begin
  Items := TStringList.Create;
  try
    Items.Delimiter := ';';
    Items.StrictDelimiter := True;
    Items.DelimitedText := Extensions;
    for I := 0 to Items.Count - 1 do
    begin
      Ext := Items[I];
      VerbKey := 'SOFTWARE\Classes\SystemFileAssociations\' + Ext + '\shell\AstraView';
      if Enable then
      begin
        RegWriteStringValue(HKLM, VerbKey, 'MUIVerb', '使用 AstraView 打开');
        RegWriteStringValue(HKLM, VerbKey, 'Icon', ExpandConstant('{app}\{#MyAppExeName},0'));
        RegWriteStringValue(HKLM, VerbKey + '\command', '', ExpandConstant('"{app}\{#MyAppExeName}" "%1"'));
      end
      else RegDeleteKeyIncludingSubkeys(HKLM, VerbKey);
    end;
  finally
    Items.Free;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    ConfigureContextMenu(WizardIsTaskSelected('contextmenu'));
    NotifyShell;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    ConfigureContextMenu(False);
    NotifyShell;
  end;
end;
