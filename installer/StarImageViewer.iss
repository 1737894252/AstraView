#define MyAppName "AstraView"
#define MyAppVersion "1.3.3"
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
OutputBaseFilename=AstraView-Setup-1.3.3-x64
SetupIconFile=..\src\StarImageViewer\astraview.ico
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupLogging=yes
CloseApplications=no
RestartApplications=no
ChangesAssociations=yes
DisableProgramGroupPage=yes

[Files]
Source: "..\artifacts\publish\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
Type: files; Name: "{app}\Magick.Native-Q16-x64.dll"
Type: files; Name: "{app}\Magick.NET-Q16-x64.dll"
Type: files; Name: "{app}\StarImageViewer.ThumbnailProvider.dll"
Type: filesandordirs; Name: "{app}\ShellExtension\StarImageViewer.ThumbnailProvider.*"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："; Flags: unchecked
Name: "contextmenu"; Description: "添加“使用 AstraView 打开”到图片右键菜单"; GroupDescription: "资源管理器集成："; Flags: unchecked
Name: "refreshthumbcache"; Description: "清理旧缩略图缓存并重启资源管理器"; GroupDescription: "资源管理器集成："; Flags: unchecked

[Registry]
Root: HKLM; Subkey: "SOFTWARE\Classes\AstraView.Image"; ValueType: string; ValueName: ""; ValueData: "Image File"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\AstraView.Image\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName},0"
Root: HKLM; Subkey: "SOFTWARE\Classes\AstraView.Image\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKLM; Subkey: "SOFTWARE\AstraView\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "{#MyAppName}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\AstraView\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Modern image viewer with PSD, RAW, HEIC and WebP support"
Root: HKLM; Subkey: "SOFTWARE\RegisteredApplications"; ValueType: string; ValueName: "AstraView"; ValueData: "SOFTWARE\AstraView\Capabilities"; Flags: uninsdeletevalue

[Run]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\ShellExtension\AstraView.ThumbnailProvider.dll"""; StatusMsg: "正在注册资源管理器缩略图组件…"; Flags: runhidden waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\ShellExtension\AstraView.ThumbnailProvider.dll"""; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterThumbnailProvider"

[Code]
const
  ImageProgId = 'AstraView.Image';
  Extensions = '.3fr;.arw;.avif;.bmp;.cr2;.cr3;.crw;.dcr;.dds;.dng;.emf;.erf;.exr;.gif;.heic;.heif;.ico;.jfif;.jpe;.jpeg;.jpg;.jxl;.kdc;.miff;.mos;.mrw;.nef;.nrw;.orf;.pbm;.pcx;.pef;.pgm;.png;.pnm;.ppm;.psb;.psd;.raf;.raw;.rw2;.rwl;.sgi;.sr2;.srf;.svg;.svgz;.tga;.tif;.tiff;.webp;.wmf;.x3f;.xbm;.xpm;.pdf';
  SHCNE_ASSOCCHANGED = $08000000;
  SHCNF_IDLIST = $0000;

procedure SHChangeNotify(wEventId: Integer; uFlags: Cardinal;
  dwItem1, dwItem2: Integer);
  external 'SHChangeNotify@shell32.dll stdcall';

procedure NotifyShellAssociationsChanged;
begin
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, 0, 0);
end;

procedure RefreshThumbnailCache;
var
  ResultCode: Integer;
  CachePattern: String;
begin
  WizardForm.StatusLabel.Caption := '正在清理旧缩略图缓存…';
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM explorer.exe', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Sleep(800);
  CachePattern := ExpandConstant(
    '{localappdata}\Microsoft\Windows\Explorer\thumbcache_*.db');
  DelTree(CachePattern, False, True, False);
  Exec(ExpandConstant('{win}\explorer.exe'), '', '',
    SW_SHOWNORMAL, ewNoWait, ResultCode);
end;

procedure RegisterFileAssociations;
var
  Items: TStringList;
  I: Integer;
  Ext: String;
begin
  Items := TStringList.Create;
  try
    Items.Delimiter := ';';
    Items.StrictDelimiter := True;
    Items.DelimitedText := Extensions;
    for I := 0 to Items.Count - 1 do
    begin
      Ext := Items[I];
      RegWriteStringValue(HKLM, 'SOFTWARE\Classes\' + Ext + '\OpenWithProgids', ImageProgId, '');
      RegWriteStringValue(HKLM, 'SOFTWARE\AstraView\Capabilities\FileAssociations', Ext, ImageProgId);
    end;
  finally
    Items.Free;
  end;
end;

procedure UnregisterFileAssociations;
var
  Items: TStringList;
  I: Integer;
  Ext: String;
begin
  Items := TStringList.Create;
  try
    Items.Delimiter := ';';
    Items.StrictDelimiter := True;
    Items.DelimitedText := Extensions;
    for I := 0 to Items.Count - 1 do
    begin
      Ext := Items[I];
      RegDeleteValue(HKLM, 'SOFTWARE\Classes\' + Ext + '\OpenWithProgids', ImageProgId);
    end;
  finally
    Items.Free;
  end;
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
        RegWriteStringValue(HKLM, VerbKey + '\command', '',
          ExpandConstant('"{app}\{#MyAppExeName}" "%1"'));
      end
      else
        RegDeleteKeyIncludingSubkeys(HKLM, VerbKey);
    end;
  finally
    Items.Free;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    RegisterFileAssociations;
    ConfigureContextMenu(WizardIsTaskSelected('contextmenu'));
    NotifyShellAssociationsChanged;
    if WizardIsTaskSelected('refreshthumbcache') then
      RefreshThumbnailCache;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  LegacyProvider: String;
  NativeProvider: String;
begin
  Result := '';
  LegacyProvider := ExpandConstant('{app}\ShellExtension\StarImageViewer.ThumbnailProvider.dll');
  NativeProvider := ExpandConstant('{app}\ShellExtension\AstraView.ThumbnailProvider.dll');
  { Thumbnail handlers are hosted by dllhost.exe. Release the old DLL before
    Setup tries to overwrite it; this avoids Inno's unreliable COM Surrogate page. }
  if FileExists(LegacyProvider) or FileExists(NativeProvider) then
  begin
    WizardForm.StatusLabel.Caption := '正在关闭旧版缩略图组件…';
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM dllhost.exe', '',
      SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(300);
  end;
  { Upgrades from 1.2 and earlier used a managed .NET Framework provider. }
  if FileExists(LegacyProvider) then
    Exec(ExpandConstant('{win}\Microsoft.NET\Framework64\v4.0.30319\RegAsm.exe'),
      '"' + LegacyProvider + '" /unregister /nologo', '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    UnregisterFileAssociations;
    ConfigureContextMenu(False);
    NotifyShellAssociationsChanged;
  end;
end;
