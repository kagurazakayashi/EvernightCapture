; EvernightCapture 安装器（Inno Setup 6.3+）
;
; 由仓库根的 build-installer.ps1 生成：它先构建、再按 installer\payload.manifest.json 收集载荷、
; 校验后把载荷目录与版本/架构/输出路径用 /D 传给 ISCC。手工编译也可以，但必须先自行准备好载荷目录：
;
;   ISCC.exe /DSourceDir="<载荷绝对路径>" /DAppVersion=0.4.0 /DArch=x64 ^
;            /DOutputDir="<输出绝对路径>" /DOutputBase="EvernightCapture-0.4.0-x64-setup" ^
;            /DIconFile="<仓库>\resources\icon.ico" installer\ecapture.iss
;
; 关键约定：
;   * 默认安装目录严格是 %UserProfile%\.agents\skills\yashi-evernight-capture（可用向导改）。
;     所选目录就是 Skill 根目录：SKILL.md 直接位于其中，不再多套一层同名目录。
;   * 按当前用户安装（PrivilegesRequired=lowest），不主动请求管理员；自定义目录不可写时如实报错，
;     既不自动提权，也不改装到管理员账户目录，更不改显示/系统设置。
;   * 升级/重装尽量沿用上次选过的目录（UsePreviousAppDir）；换目录时旧目录原样保留，不隐式删除或迁移。
;   * 卸载只删安装器自己记录过的文件；用户添加的文件、日志与相邻 Skill 一律保留，绝不递归通配删除父目录。
;   * 不按进程名终止用户程序；ECAPTURE.EXE 被占用时让它如实失败并可回滚，而不是强杀。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\installer-stage\payload"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\installer"
#endif
#ifndef OutputBase
  #define OutputBase "EvernightCapture-" + AppVersion + "-" + Arch + "-setup"
#endif
#ifndef IconFile
  #define IconFile "..\resources\icon.ico"
#endif

[Setup]
AppId={{B7A2E1C4-5D3F-4E6A-9C21-8F0B3D5E7A10}
AppName=EvernightCapture
AppVersion={#AppVersion}
AppVerName=EvernightCapture {#AppVersion} ({#Arch})
AppPublisher=EvernightCapture
DefaultDirName={code:GetDefaultSkillDir}
DefaultGroupName=EvernightCapture
DisableDirPage=no
DisableProgramGroupPage=yes
UsePreviousAppDir=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
LicenseFile={#SourceDir}\LICENSE
OutputDir={#OutputDir}
OutputBaseFilename={#OutputBase}
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\ECAPTURE.EXE
UninstallDisplayName=EvernightCapture {#AppVersion}（截图 CLI + Skill）
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
AllowNoIcons=yes
DisableReadyPage=no

[Languages]
; 只用 Inno 自带的默认语言（英文界面），避免依赖未随安装的 .isl 语言包而编译失败。
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; 载荷目录整份铺进 {app} 根：ECAPTURE.EXE / SKILL.md / references / README×4 / LICENSE /
; resources / tests / build / verify-install.ps1 与 build-installer 生成的 install-manifest.json。
; 不递归删除、不按通配删除用户内容；卸载走 Inno 自己的安装记录。
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Run]
Filename: "{app}"; Description: "打开安装目录"; Flags: postinstall shellexec nowait skipifsilent
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\verify-install.ps1"""; Description: "运行只读安装自检（verify-install.ps1）"; Flags: postinstall nowait skipifsilent

[Code]
const
  SKILL_ENTRY   = 'SKILL.md';
  MANIFEST      = 'install-manifest.json';
  SKILL_REL_DIR = '.agents\skills\yashi-evernight-capture';

{ Inno Setup 没有 userprofile 这个内建常量（6.7.1 实测会报 Unknown constant 并中止编译），
  所以默认目录只能自己算。取 USERPROFILE 环境变量：它指向当前登录用户自己的配置目录，
  且不受 OneDrive「文档」重定向影响（用 userdocs 常量反推会跑到 OneDrive 目录里去）。
  环境变量取不到时（例如以服务模式启动向导）退回 LocalAppData 上跳两级，
  AppData 同样不参与文件夹重定向。注意：这段注释里不能再出现成对的花括号，
  Pascal 的花括号块注释在第一个右花括号处就结束了。 }
function GetDefaultSkillDir(Param: String): String;
var
  Profile: String;
begin
  Profile := GetEnv('USERPROFILE');
  if Profile = '' then
    Profile := ExpandConstant('{localappdata}\..\..');
  Result := AddBackslash(Profile) + SKILL_REL_DIR;
end;

function DirHasEntries(const Dir: String): Boolean;
var
  FindRec: TFindRec;
begin
  Result := False;
  if FindFirst(AddBackslash(Dir) + '*', FindRec) then
  begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then
        begin
          Result := True;
          Exit;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir, ManifestPath: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Dir := WizardDirValue;
    ManifestPath := AddBackslash(Dir) + MANIFEST;
    { 已经装过本产品（有清单）→ 视为升级/重装，不打扰。
      未知同名且非空的目录 → 明说会写入什么，让用户自己决定，绝不静默覆盖。 }
    if DirExists(Dir) and (not FileExists(ManifestPath)) and DirHasEntries(Dir) then
    begin
      Log('目标目录非空且不是本产品安装过的目录：' + Dir);
      { 必须用 SuppressibleMsgBox：普通的 MsgBox 不受 /SUPPRESSMSGBOXES 管辖，实测会让
        /VERYSILENT 安装永久卡在框上（无人值守部署等于挂死）。Suppressible 版在被抑制时
        直接返回这里给的默认值 IDNO —— 也就是静默模式下**不写任何东西、如实中止**
        （Inno 退出码 2），而不是悄悄往一个自己不拥有的目录里铺文件。 }
      if SuppressibleMsgBox('这个目录已经存在，而且不是本产品安装过的目录：' + #13#10 + #13#10 + Dir + #13#10 + #13#10 +
                '继续会在这个目录里新增/覆盖本产品的文件。安装器只记录自己安装的文件，卸载时也只删这些；' +
                '目录里原有的其它文件会保留。' + #13#10 + #13#10 + '确认要装在这里吗？',
                mbConfirmation, MB_YESNO, IDNO) <> IDYES then
      begin
        Log('未确认装入未知目录，安装中止（静默模式下默认按"否"处理，没有写入任何文件）');
        Result := False;
      end;
    end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    { 装完把"装到哪、README 在哪、AI 下一步"直接写在完成页上。 }
    WizardForm.FinishedLabel.Caption :=
      'EvernightCapture 已安装到：' + #13#10 + ExpandConstant('{app}') + #13#10 + #13#10 +
      'Skill 入口：' + ExpandConstant('{app}') + '\' + SKILL_ENTRY + #13#10 +
      'README：' + ExpandConstant('{app}') + '\README.md（简体中文 README.zh-CN.md）' + #13#10 + #13#10 +
      '让 AI 工具用起来：把上面这个目录（含 SKILL.md 与 ECAPTURE.EXE）交给你的 AI 工具，' +
      '让它显式读取该 SKILL.md，并按其中说明用绝对路径调用同目录的 ECAPTURE.EXE。' + #13#10 +
      '注意：装到默认目录之外的路径时，多数工具不会自动发现，需要按工具的说明手动指向该目录。' + #13#10 + #13#10 +
      '建议先跑一次只读自检：powershell -NoProfile -ExecutionPolicy Bypass -File "' +
      ExpandConstant('{app}') + '\verify-install.ps1"';
  end;
end;
