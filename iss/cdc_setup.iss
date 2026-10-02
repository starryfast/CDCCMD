; ============================================================================
;  cdc_setup.iss -- CDCCMD 安装包 (Inno Setup 7)
;
;  产物: cdccmd-setup.exe (单文件, 可分发)
;  安装到: {localappdata}\Programs\cdccmd  (免管理员)
;  写用户 PATH + 添加/删除程序项
;
;  编译:
;    "%LOCALAPPDATA%\Programs\Inno Setup 7\ISCC.exe" cdc_setup.iss
;
;  前置: build\cdccmd.exe 与 build\uninstall.exe 已由 build.py 产出
; ============================================================================

#define AppName        "CDCCMD"
#define AppVer         "1.1.0"
#define AppPublisher   "Your Name"
#define AppURL         "https://github.com/your-github-name"
#define AppExeName     "cdccmd.exe"

[Setup]
; ★ 换成你自己的 GUID, 别和别人撞车 (可用 PowerShell 的 [guid]::NewGuid() 生成)
AppId={{8F3A1C42-7B6E-4D91-9C25-3E7A5B1D8F04}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppName} {#AppVer}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}

; --- 装到用户目录, 免管理员 ---
DefaultDirName={localappdata}\Programs\cdccmd
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=

; --- 单文件输出 ---
OutputDir=..\build
OutputBaseFilename=cdc-setup-iss
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

; --- 图标 ---
SetupIconFile=assets\cdccmd_multi.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName} (命令行工具)

; --- 关掉不需要的东西 ---
DisableDirPage=no
DisableReadyPage=no
AllowNoIcons=yes
DisableWelcomePage=no
ChangesAssociations=no

[Languages]
Name: "chinese"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinese.CreateDesktopIcon=创建桌面快捷方式(&D)
chinese.OpenNewCmd=安装完成后打开一个新的命令提示符(&O)
chinese.LaunchCmd=启动 CDCCMD 命令提示符
chinese.PathAdded=已将安装目录加入用户 PATH。
chinese.NeedNewWindow=注意：当前已打开的命令行窗口需要重开才能识别命令。

[Tasks]
; 默认勾选: 装完自动开一个新 CMD, 省得用户以为没装上
Name: "opennewcmd"; Description: "{cm:OpenNewCmd}"; GroupDescription: "{cm:AdditionalIcons}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\build\cdccmd.exe";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\uninstall.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md";           DestDir: "{app}"; Flags: ignoreversion
Source: "..\CHANGELOG.md";        DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\CDCCMD 帮助";        Filename: "{cmd}"; Parameters: "/k ""{app}\{#AppExeName}"" help"; WorkingDir: "{app}"
Name: "{group}\卸载 {#AppName}";    Filename: "{uninstallexe}"
Name: "{userdesktop}\CDCCMD 命令提示符"; Filename: "{cmd}"; Parameters: "/k ""{app}\{#AppExeName}"" help"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
; 装完开一个已在 PATH 里生效的新 CMD
Filename: "{cmd}"; Parameters: "/k """"{app}\{#AppExeName}"" help"""; WorkingDir: "{app}"; Description: "{cm:LaunchCmd}"; Flags: nowait postinstall skipifsilent; Tasks: opennewcmd

[Registry]
; --- 把安装目录写进用户 PATH (免管理员) ---
Root: HKCU; Subkey: "Environment"; ValueType: string; ValueName: "Path"; \
    ValueData: "{olddata};{app}"; Check: NeedsAddPath('{app}')

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
const
  EnvironmentKey = 'Environment';
  WM_SETTINGCHANGE = $001A;

{ 硬链接 API 导入 (kernel32), Inno 无内建封装 }
function CreateHardLinkW(lpFileName, lpExistingFileName: String;
                        lpSecurityAttributes: Cardinal): Boolean;
  external 'CreateHardLinkW@kernel32.dll stdcall';

{ ================================================================
  命令别名 (shim)
  ----------------------------------------------------------------
  Windows 只按"可执行文件名"解析 PATH, 不认 exe 的子命令。
  所以 cdccmd / cdc / cdcgb / jcgx / uncdccmd 每个都要有一个同名 exe。
  cdccmd.exe 内部按 argv[0] 分派 -> 这里做硬链接即可, 零额外磁盘占用。

  投放两个位置, 保证"整台电脑到处都能用":
    1) 安装目录                       —— 主位置
    2) LocalAppData\Microsoft\WindowsApps
       Windows 10/11 默认就在 PATH 里的目录, 放这里 = 天然全局可用,
       完全不依赖注册表 PATH(即使 PATH 被改坏也照样能敲)。

  硬链接失败(跨卷/文件系统不支持)退回复制; 再失败写 .cmd 转发脚本兜底。
  注意: 本注释块内不能出现花括号, 否则注释会被提前闭合 (老坑)。
  ================================================================ }
function MakeAliases(): Integer;
var
  App, Src, Dst, CmdFile: string;
  Names: array[0..4] of string;
  Dirs: array[0..1] of string;
  I, J, Count, NDirs: Integer;
  AnyOk: Boolean;
begin
  App := ExpandConstant('{app}');
  Src := App + '\{#AppExeName}';

  Names[0] := 'cdccmd';
  Names[1] := 'cdc';
  Names[2] := 'cdcgb';
  Names[3] := 'jcgx';
  Names[4] := 'uncdccmd';

  Dirs[0] := App;
  Dirs[1] := ExpandConstant('{localappdata}\Microsoft\WindowsApps');
  NDirs := 1;
  if DirExists(Dirs[1]) then
    NDirs := 2;

  Count := 0;
  if not FileExists(Src) then
  begin
    Result := 0;
    Exit;
  end;

  for I := 0 to 4 do
  begin
    AnyOk := False;
    for J := 0 to NDirs - 1 do
    begin
      Dst := Dirs[J] + '\' + Names[I] + '.exe';
      { 安装目录里的 cdccmd.exe 就是源文件本身, 不能删掉再建链接 }
      if CompareText(Dst, Src) = 0 then
      begin
        AnyOk := True;
      end
      else
      begin
        if FileExists(Dst) then
          DeleteFile(Dst);
        { 硬链接需目标与源同卷; 失败则退回复制 }
        if CreateHardLinkW(Dst, Src, 0) then
          AnyOk := True
        else if CopyFile(Src, Dst, False) then
          AnyOk := True
        else
        begin
          CmdFile := Dirs[J] + '\' + Names[I] + '.cmd';
          if SaveStringToFile(CmdFile,
               '@echo off' + #13#10 + '"' + Src + '" ' + Names[I] + ' %*' + #13#10,
               False) then
            AnyOk := True;
        end;
      end;
    end;
    if AnyOk then
      Count := Count + 1;
  end;
  Result := Count;
end;

{ 卸载时清掉 WindowsApps 里的别名(只删我们自己的 5 个名字) }
procedure CleanWindowsAppsShims();
var
  Wa: string;
  Names: array[0..4] of string;
  I: Integer;
begin
  Wa := ExpandConstant('{localappdata}\Microsoft\WindowsApps');
  Names[0] := 'cdccmd';
  Names[1] := 'cdc';
  Names[2] := 'cdcgb';
  Names[3] := 'jcgx';
  Names[4] := 'uncdccmd';
  for I := 0 to 4 do
  begin
    DeleteFile(Wa + '\' + Names[I] + '.exe');
    DeleteFile(Wa + '\' + Names[I] + '.cmd');
  end;
end;

{ ---- 读取用户 PATH ---- }
function GetUserPath(): string;
var
  S: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', S) then
    S := '';
  Result := S;
end;

{ ---- 判断安装目录是否已在 PATH 里(按分号切分精确比对) ---- }
function PathContains(const Paths, Dir: string): Boolean;
var
  P, Seg: string;
  I: Integer;
begin
  Result := False;
  P := Paths;
  { 统一去掉末尾反斜杠再比, 避免 C:\a\ 与 C:\a 被判为不同 }
  while (Length(P) > 0) and (P[Length(P)] = '\') do
    Delete(P, Length(P), 1);

  while P <> '' do
  begin
    I := Pos(';', P);
    if I > 0 then
    begin
      Seg := Copy(P, 1, I - 1);
      Delete(P, 1, I);
    end
    else
    begin
      Seg := P;
      P := '';
    end;

    { 去空白 + 去尾反斜杠 }
    while (Length(Seg) > 0) and ((Seg[Length(Seg)] = '\') or (Seg[Length(Seg)] = ' ')) do
      Delete(Seg, Length(Seg), 1);

    if CompareText(Seg, Dir) = 0 then
    begin
      Result := True;
      Exit;
    end;
  end;
end;

{ ---- Registry 段的 Check 回调: 不在 PATH 里才写 ---- }
function NeedsAddPath(Param: string): Boolean;
var
  Dir: string;
begin
  Dir := Param;
  while (Length(Dir) > 0) and (Dir[Length(Dir)] = '\') do
    Delete(Dir, Length(Dir), 1);
  Result := not PathContains(GetUserPath(), Dir);
end;

{ ---- 卸载时从 PATH 里摘掉自己 ---- }
procedure RemoveFromUserPath(const Dir: string);
var
  Paths, Seg, NewPaths: string;
  I: Integer;
  D: string;
begin
  Paths := GetUserPath();
  D := Dir;
  while (Length(D) > 0) and (D[Length(D)] = '\') do
    Delete(D, Length(D), 1);

  NewPaths := '';
  while Paths <> '' do
  begin
    I := Pos(';', Paths);
    if I > 0 then
    begin
      Seg := Copy(Paths, 1, I - 1);
      Delete(Paths, 1, I);
    end
    else
    begin
      Seg := Paths;
      Paths := '';
    end;

    if Seg <> '' then
    begin
      { 比对前先去尾反斜杠, 但保留原样写回 }
      while (Length(Seg) > 0) and ((Seg[Length(Seg)] = '\') or (Seg[Length(Seg)] = ' ')) do
        Delete(Seg, Length(Seg), 1);
      if CompareText(Seg, D) <> 0 then
      begin
        if NewPaths <> '' then
          NewPaths := NewPaths + ';';
        NewPaths := NewPaths + Seg;
      end;
    end;
  end;

  RegWriteStringValue(HKEY_CURRENT_USER, EnvironmentKey, 'Path', NewPaths);
end;

{ ---- 广播 WM_SETTINGCHANGE, 让 shell 重新读环境变量 ---- }
procedure SendBroadcastMessageWrap();
begin
  { Inno 自带 SendBroadcastMessage; 失败也无所谓, 不算致命 }
  SendBroadcastMessage(WM_SETTINGCHANGE, 0, 'Environment');
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
  begin
    { 先摘 PATH, 免得残留 }
    RemoveFromUserPath(ExpandConstant('{app}'));
    { 再清掉 WindowsApps 里的命令别名 }
    CleanWindowsAppsShims();
  end;
end;

{ ---- 装完广播环境变量变更, 让新开的窗口立刻生效 ---- }
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    { 建立 cdccmd / cdc / cdcgb / jcgx / uncdccmd 五个别名,
      同时投放到安装目录与 WindowsApps, 保证整台电脑到处都能敲 }
    MakeAliases();
    { 通知系统环境变量已改 (WM_SETTINGCHANGE) }
    SendBroadcastMessageWrap();
  end;
end;
