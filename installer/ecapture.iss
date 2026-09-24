; EvernightCapture 安装器（Inno Setup 6.3+）
;
; 由仓库根的 build-installer.ps1 生成：它先构建、再按 installer\payload.manifest.json 收集载荷、
; 校验后把载荷目录、版本/架构/输出路径，以及一份"这次铺下去的相对路径清单"（编译期展开的
; managed-list 片段）用 /D 传给 ISCC。手工编译也可以，但必须先自行准备好载荷目录与那份片段：
;
;   ISCC.exe /DSourceDir="<载荷绝对路径>" /DAppVersion=0.4.0 /DArch=x64 ^
;            /DManagedList="<载荷目录>\managed-list.pinc" ^
;            /DOutputDir="<输出绝对路径>" /DOutputBase="EvernightCapture-0.4.0-x64-setup" ^
;            /DIconFile="<仓库>\resources\icon.ico" installer\ecapture.iss
;
; 为什么要编译期烘进清单，而不是运行时读一个随包文件：PrepareToInstall 在所有文件复制**之前**执行，
; 那时 {app} 里还没有载荷，任何随包列表都读不到；而本机实测 Inno 6.7.1 已经没有 OnFileCopy 回调
; （TFileCopyMode 是未知类型），也没有"逐文件覆盖前"的钩子。所以受管路径清单必须在打包时确定下来。
;
; 关键约定：
;   * 默认安装目录严格是 %UserProfile%\.agents\skills\yashi-evernight-capture（可用向导改）。
;     所选目录就是 Skill 根目录：SKILL.md 直接位于其中，不再多套一层同名目录。
;   * 按当前用户安装（PrivilegesRequired=lowest），不主动请求管理员；自定义目录不可写时如实报错，
;     既不自动提权，也不改装到管理员账户目录，更不改显示/系统设置。
;   * 目录归属只认安装器自己写下的 ownership 标记（默认 evernightcapture.owner.ini）：
;     目标目录里有 install-manifest.json 并不代表它属于本产品。标记缺失、格式不对、产品或 AppId
;     对不上，一律按"未知目录"处理 —— 继续前必须当面确认，静默模式默认按"否"中止，不写任何文件。
;     这只是"别把用户目录当自己的东西"的工程约束，不是防恶意本机用户的安全边界。
;   * 确认属于本产品的重装/升级，覆盖前会把两类文件先复制进 <安装目录>\.ecapture-backup-<UTC 时间>\
;     （保留相对路径；时间戳目录已存在时追加序号，绝不覆盖旧备份）：
;       1) 上次归属标记里记过、但现状的大小或写入时间已经不同的（用户改过，或被别的构建换过）；
;       2) 这次要写的同名文件在标记里没有记录（不能只说一句"这是产品文件"就覆盖）。
;     备份读写失败就中止安装，不带不确定的东西往下写。备份目录不是本产品的文件，卸载不碰它。
;   * 升级/重装尽量沿用上次选过的目录（UsePreviousAppDir）；换目录时旧目录原样保留，不隐式删除或迁移。
;   * 卸载只删安装器自己记录过的文件，外加那个 ownership 标记（[UninstallDelete] 里点名这一个）；
;     用户添加的文件、备份目录与相邻 Skill 一律保留，绝不递归通配删除父目录。
;   * 不按进程名终止用户程序（CloseApplications=no / RestartApplications=no）；ECAPTURE.EXE 被占用时
;     让它如实失败并可回滚，而不是强杀。
;   * ISPP 会把**行首**的 # 当成预处理器指令：续行如果以 #13#10 开头，6.7.1 实测报
;     "Unknown preprocessor directive" 并中止编译，所以长表达式里的 #13#10 必须留在行中间。
;   * 事务边界（本机 6.7.1 + Windows 10.0.19045 实测，判据见 tests\install-lifecycle.ps1）：
;       - 未知目录被静默拒绝时，安装器返回退出码 1，目录里一个文件都没写（不是"退出码 2"）。
;       - 文件复制阶段失败时（实测把 ECAPTURE.EXE 以独占方式打开后重装，退出码 5），Inno 会把本次
;         已复制的文件撤回去，磁盘上要么整套仍是失败前那份、要么整套是新包，不会留半新半旧的混装；
;         用户在安装目录里后加的文件不受影响。
;       - ssPostInstall 里安装器自己写的归属标记不在 Inno 的回滚记录范围内，所以中途失败时它不会被
;         重写（实测失败现场里标记仍是上一次成功安装时写的那份）。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Arch
  #define Arch "x64"
#endif
#ifndef SourceDir
  #define SourceDir "..\build\installer-stage\payload"
#endif
#ifndef ManagedList
  #error 需要 /DManagedList="<载荷目录>\managed-list.pinc"：安装器靠这份编译期清单判断哪些文件归本产品管（见文件头注释）
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
#ifndef OwnershipMarker
  #define OwnershipMarker "evernightcapture.owner.ini"
#endif
#ifndef AppIdGuid
  #define AppIdGuid "{B7A2E1C4-5D3F-4E6A-9C21-8F0B3D5E7A10}"
#endif

; 构建链只有 x64（build.ps1 只调用 vcvars64.bat），所以这里拒绝编出一个"自称 arm64、里面是 x64"的包。
#if Arch != "x64"
  #error EvernightCapture 目前只有 x64 构建链，不接受 -Arch arm64；先补 ARM64 工具链、测试产物与架构核对，再放开这里。
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
; resources / tests / build / verify-install.ps1 与 build-installer 生成的 install-manifest.json、
; payload.sha256.txt。不递归删除、不按通配删除用户内容；卸载走 Inno 自己的安装记录。
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; 只清这一个安装器自己写的归属标记；备份目录、用户新增文件与相邻 Skill 都不在这里，故一律保留。
Type: files; Name: "{app}\{#OwnershipMarker}"

[Code]
const
  SKILL_ENTRY       = 'SKILL.md';
  MANIFEST          = 'install-manifest.json';
  SKILL_REL_DIR     = '.agents\skills\yashi-evernight-capture';
  PRODUCT_NAME      = 'EvernightCapture';
  PRODUCT_APP_ID    = '{#AppIdGuid}';
  MARKER_VERSION    = '1';
  BACKUP_PREFIX     = '.ecapture-backup-';
  FILES_SECTION_TAG = '--files--';

var
  ManagedPaths: TArrayOfString;      { 编译期由 managed-list 片段填好：这次要铺下去的相对路径 }
  BackupRoot: String;
  BackupCount: Integer;

{ ---------------------------------------------------------------------------
  小工具：Inno 的 Pascal 脚本没有 Pos / Fetch / Int64，所以按字符扫描拆分，
  大小与写入时间用 FILETIME 的高低 32 位分别存。
  --------------------------------------------------------------------------- }
function SplitByChar(const S: String; const D: Char; var OutArr: TArrayOfString): Boolean;
var
  I, N: Integer;
  Buf: String;
begin
  Result := True;
  SetArrayLength(OutArr, 0);
  Buf := '';
  for I := 1 to Length(S) do
  begin
    if Copy(S, I, 1) = D then
    begin
      N := GetArrayLength(OutArr);
      SetArrayLength(OutArr, N + 1);
      OutArr[N] := Buf;
      Buf := '';
    end
    else
      Buf := Buf + Copy(S, I, 1);
  end;
  N := GetArrayLength(OutArr);
  SetArrayLength(OutArr, N + 1);
  OutArr[N] := Buf;
end;

function ValueAfterPrefix(const Line, Prefix: String): String;
begin
  Result := '';
  if Copy(Line, 1, Length(Prefix)) = Prefix then
    Result := Copy(Line, Length(Prefix) + 1, Length(Line) - Length(Prefix));
end;

function ToNative(const RelPath: String): String;
var
  S: String;
begin
  { StringChange 是过程（第一个参数要 var），不是返回字符串的函数 —— 本机实测。 }
  S := RelPath;
  StringChange(S, '/', '\');
  Result := S;
end;

{ 编译期展开的受管路径清单：ManagedPaths[i] := '<相对路径>' }
procedure LoadManagedPaths;
begin
  #include ManagedList
end;

{ ---------------------------------------------------------------------------
  ownership 标记：安装器在 ssPostInstall 写下，记录"这个目录里哪些文件是本产品装的、
  装下来时大小与写入时间是多少"；下次重装/升级靠它认出被改过的已管理文件。
  格式：若干 key=value 头 + 一行 --files-- + 若干 relpath|大小|写入时间(FILETIME, Int64)
  --------------------------------------------------------------------------- }
function MarkerFileIn(const Dir: String): String;
begin
  Result := AddBackslash(Dir) + '{#OwnershipMarker}';
end;

function ParseMarker(const Lines: TArrayOfString; var Product, AppId, MarkerVer: String;
                     var Files: TArrayOfString): Boolean;
var
  I, N: Integer;
  InFiles: Boolean;
  Line: String;
begin
  Product := ''; AppId := ''; MarkerVer := '';
  SetArrayLength(Files, 0);
  InFiles := False;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if Line = '' then Continue;
    if Line = FILES_SECTION_TAG then InFiles := True
    else if Copy(Line, 1, 1) = '#' then Continue
    else if InFiles then
    begin
      N := GetArrayLength(Files);
      SetArrayLength(Files, N + 1);
      Files[N] := Line;
    end
    else if Product = '' then Product := ValueAfterPrefix(Line, 'product=')
    else if AppId = '' then AppId := ValueAfterPrefix(Line, 'appid=')
    else if MarkerVer = '' then MarkerVer := ValueAfterPrefix(Line, 'markerversion=');
  end;
  Result := (Product <> '') and (AppId <> '') and (MarkerVer <> '');
end;

function LoadMarker(const Dir: String; var Product, AppId, MarkerVer: String;
                    var Files: TArrayOfString): Boolean;
var
  Lines: TArrayOfString;
begin
  Product := ''; AppId := ''; MarkerVer := '';
  SetArrayLength(Files, 0);
  Result := False;
  if not FileExists(MarkerFileIn(Dir)) then Exit;
  if not LoadStringsFromFile(MarkerFileIn(Dir), Lines) then
  begin
    Log('归属标记读不出来：' + MarkerFileIn(Dir));
    Exit;
  end;
  Result := ParseMarker(Lines, Product, AppId, MarkerVer, Files);
  if not Result then Log('归属标记格式不合格（缺 product/appid/markerversion）：' + MarkerFileIn(Dir));
end;

{ 这个目录是不是本产品的安装目录：只认归属标记，不认"有没有 install-manifest.json"。 }
function DirOwnedByProduct(const Dir: String): Boolean;
var
  Product, AppId, MarkerVer: String;
  Files: TArrayOfString;
begin
  Result := False;
  if not DirExists(Dir) then Exit;
  if not LoadMarker(Dir, Product, AppId, MarkerVer, Files) then
  begin
    if FileExists(AddBackslash(Dir) + MANIFEST) then
      Log('目录里有 install-manifest.json，但没有本安装器认得的归属标记（缺失 / 格式不对 / 不是本产品 / AppId 或标记版本不符）——按未知目录处理：' + Dir);
    Exit;
  end;
  if Product <> PRODUCT_NAME then
  begin
    Log('目录里的归属标记产品名是 ' + Product + '，不是本产品，按未知目录处理。');
    Exit;
  end;
  if AppId <> PRODUCT_APP_ID then
  begin
    Log('目录里的归属标记 AppId 与本产品的不一致，按未知目录处理。');
    Exit;
  end;
  if MarkerVer <> MARKER_VERSION then
  begin
    Log('目录里的归属标记版本是 ' + MarkerVer + '，本安装器认的是 ' + MARKER_VERSION + '，按未知目录处理。');
    Exit;
  end;
  Result := True;
end;

{ ---------------------------------------------------------------------------
  文件档案（大小 + 写入时间）
  用 Int64 合并 FILETIME 与文件大小的高低 32 位：LongWord 的低 32 位可以大到超过
  StrToIntDef 能解析的有符号范围（>2^31-1），那样读回来会变成 0，比对就永远"不一致"，
  每次重装都会把所有文件当用户改动备份一遍（本机实测踩过）。Int64 一步到位。
  --------------------------------------------------------------------------- }
const
  TICK32 = 4294967296;   { 2^32 }

function FileFacts(const FullPath: String; var Size: Int64; var WriteTime: Int64): Boolean;
var
  Rec: TFindRec;
begin
  Result := False;
  Size := 0; WriteTime := 0;
  if FindFirst(FullPath, Rec) then
  begin
    try
      Size := Int64(Rec.SizeHigh) * TICK32 + Int64(Rec.SizeLow);
      WriteTime := Int64(Rec.LastWriteTime.dwHighDateTime) * TICK32 +
                   Int64(Rec.LastWriteTime.dwLowDateTime);
      Result := True;
    finally
      FindClose(Rec);
    end;
  end;
end;

function FieldAsInt64(const Fields: TArrayOfString; const Index: Integer): Int64;
begin
  Result := 0;
  if Index < GetArrayLength(Fields) then
    Result := StrToInt64Def(Trim(Fields[Index]), 0);
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

{ ---------------------------------------------------------------------------
  备份：把原件复制到带时间戳、不覆盖旧备份的目录里；失败由调用方中止安装。
  复制走 LoadStringFromFile / SaveStringToFile 的字节往返，不经任何文本转换。
  --------------------------------------------------------------------------- }
function BackupDirFor(const Dir: String): String;
var
  Base, Candidate: String;
  Suffix: Integer;
begin
  Base := BACKUP_PREFIX + GetDateTimeString('yyyymmdd-hhnnss', '-', '-');
  Candidate := AddBackslash(Dir) + Base;
  Suffix := 0;
  while DirExists(Candidate) do
  begin
    Suffix := Suffix + 1;
    Candidate := AddBackslash(Dir) + Base + '-' + IntToStr(Suffix);
  end;
  Result := Candidate;
end;

function BackupFile(const Dir, RelPath, Reason: String): String;
var
  Src, Dst, Acc: String;
  Parts: TArrayOfString;
  I, Last: Integer;
  Bytes: AnsiString;
begin
  Result := '';
  if BackupRoot = '' then
  begin
    BackupRoot := BackupDirFor(Dir);
    if not CreateDir(BackupRoot) then
    begin
      Result := '建不出备份目录：' + BackupRoot;
      Exit;
    end;
    Log('本次备份目录：' + BackupRoot);
  end;
  if not SplitByChar(RelPath, '/', Parts) then
  begin
    Result := '拆不开相对路径：' + RelPath;
    Exit;
  end;
  Last := GetArrayLength(Parts) - 1;
  Acc := BackupRoot;
  for I := 0 to Last - 1 do
  begin
    if Parts[I] = '' then Continue;
    Acc := AddBackslash(Acc) + Parts[I];
    if not DirExists(Acc) and not CreateDir(Acc) then
    begin
      Result := '建不出备份子目录：' + Acc;
      Exit;
    end;
  end;
  Dst := AddBackslash(Acc) + Parts[Last];
  Src := AddBackslash(Dir) + ToNative(RelPath);
  if not LoadStringFromFile(Src, Bytes) then
  begin
    Result := '读不出原件（被占用或没有权限？），已中止安装：' + Src;
    Exit;
  end;
  if not SaveStringToFile(Dst, Bytes, False) then
  begin
    Result := '备份写不出来，已中止安装：' + Dst;
    Exit;
  end;
  BackupCount := BackupCount + 1;
  Log('已备份（' + Reason + '）：' + Src + ' -> ' + Dst);
end;

procedure WriteBackupNote;
var
  Note: TArrayOfString;
begin
  if BackupRoot = '' then Exit;
  SetArrayLength(Note, 6);
  Note[0] := 'EvernightCapture 安装前的备份';
  Note[1] := '这个目录里的文件是安装器在覆盖前复制出来的原件。安装器不拥有这些备份，卸载不会删除它们。';
  Note[2] := '产品：' + PRODUCT_NAME + '   本次安装版本：{#AppVersion} ({#Arch})';
  Note[3] := '备份条目数：' + IntToStr(BackupCount);
  Note[4] := '判定依据：上一次安装记下的归属标记里的大小 / 写入时间与磁盘现状不一致；';
  Note[5] := '或者这个同名文件从没在归属标记里记录过。确认不需要原件之后可以自己删掉整个目录。';
  if not SaveStringsToFile(AddBackslash(BackupRoot) + 'BACKUP-INFO.txt', Note, False) then
    Log('备份说明写不出来：' + BackupRoot);
end;

{ 返回非空字符串 = 中止安装（Inno 会把它作为错误信息报出来，不写任何文件）。 }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dir, RelPath, Reason: String;
  Product, AppId, MarkerVer: String;
  OldFiles, Fields: TArrayOfString;
  I, L: Integer;
  Size: Int64;
  WriteTime: Int64;
  Recorded, Same, Found: Boolean;
begin
  Result := '';
  Dir := ExpandConstant('{app}');
  BackupRoot := '';
  BackupCount := 0;
  if not DirExists(Dir) then
  begin
    Log('目标目录还不存在，没有需要保护的旧文件：' + Dir);
    Exit;
  end;
  LoadManagedPaths;
  if GetArrayLength(ManagedPaths) = 0 then
  begin
    Result := '编译期清单里没有受管路径（managed-list 片段是空的？），无法做覆盖前保护，已中止。';
    Exit;
  end;
  if not LoadMarker(Dir, Product, AppId, MarkerVer, OldFiles) then
  begin
    Log('没有可用的归属标记：本次不比对旧文件（全新目录，或刚被"未知目录"确认拦住过）。');
    SetArrayLength(OldFiles, 0);
  end;

  for I := 0 to GetArrayLength(ManagedPaths) - 1 do
  begin
    RelPath := ManagedPaths[I];
    if RelPath = '' then Continue;
    if not FileExists(AddBackslash(Dir) + ToNative(RelPath)) then Continue;   { 这个位置还没有同名文件 }

    Found := False;
    Recorded := False;
    Same := False;
    for L := 0 to GetArrayLength(OldFiles) - 1 do
    begin
      if not SplitByChar(OldFiles[L], '|', Fields) then Continue;
      if GetArrayLength(Fields) <> 3 then Continue;
      if Trim(Fields[0]) <> RelPath then Continue;
      Recorded := True;
      if FileFacts(AddBackslash(Dir) + ToNative(RelPath), Size, WriteTime) then
      begin
        Same := (Size = FieldAsInt64(Fields, 1)) and (WriteTime = FieldAsInt64(Fields, 2));
        Found := True;
      end;
      Break;
    end;

    if Recorded and Found and Same then Continue;                 { 与上次装下来时一模一样，不用备份 }
    if Recorded then
      Reason := '归属标记里记过，但现状的大小或写入时间已经变了'
    else
      Reason := '要写的同名文件从没在本产品的归属标记里记录过';
    Result := BackupFile(Dir, RelPath, Reason);
    if Result <> '' then Exit;
  end;

  if BackupCount > 0 then
  begin
    WriteBackupNote;
    Log('覆盖前共备份 ' + IntToStr(BackupCount) + ' 个文件到：' + BackupRoot);
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir, Why: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Dir := WizardDirValue;
    if DirExists(Dir) and DirOwnedByProduct(Dir) then
    begin
      Log('目标目录是本产品自己装过的（归属标记合格），按升级/重装继续：' + Dir);
      Exit;
    end;
    if not (DirExists(Dir) and DirHasEntries(Dir)) then Exit;   { 空目录或还不存在：没有可保护的东西 }
    if FileExists(AddBackslash(Dir) + MANIFEST) then
      Why := '这个目录里有一份 install-manifest.json，但本安装器认不出它的归属标记（缺失 / 格式不对 / 不是本产品 / AppId 或标记版本不符）'
    else
      Why := '这个目录已经存在，而且不是本产品安装过的目录';
    Log('目标目录非空且归属不明（' + Why + '）：' + Dir);
    { 必须用 SuppressibleMsgBox：普通的 MsgBox 不受 /SUPPRESSMSGBOXES 管辖，实测会让
      /VERYSILENT 安装永久卡在框上（无人值守部署等于挂死）。Suppressible 版在被抑制时
      直接返回这里给的默认值 IDNO —— 也就是静默模式下**不写任何东西、如实中止**
      （本机 6.7.1 实测：/VERYSILENT 下退出码 1，目录里一个文件都没落），而不是悄悄往一个自己不拥有的目录里铺文件。 }
    if SuppressibleMsgBox(Why + '：' + #13#10 + #13#10 + Dir + #13#10 + #13#10 +
              '继续会在这个目录里新增/覆盖本产品的文件。安装器只记录自己安装的文件，卸载时也只删这些；' +
              '目录里原有的其它文件会保留。' + #13#10 + #13#10 + '确认要装在这里吗？',
              mbConfirmation, MB_YESNO, IDNO) <> IDYES then
    begin
      Log('未确认装入未知目录，安装中止（静默模式下默认按"否"处理，没有写入任何文件）');
      Result := False;
    end;
  end;
end;

{ Inno 没有 userprofile 这个内建常量（6.7.1 实测会报 Unknown constant 并中止编译），
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

procedure WriteOwnershipMarker;
var
  Dir, Src, RelPath: String;
  OutLines: TArrayOfString;
  I, N: Integer;
  Size: Int64;
  WriteTime: Int64;
  Recorded: Integer;
begin
  Dir := ExpandConstant('{app}');
  LoadManagedPaths;
  SetArrayLength(OutLines, 8);
  OutLines[0] := '# EvernightCapture 安装器写下的目录归属标记';
  OutLines[1] := '# 只用于判断"这个目录里哪些文件是本产品装的、装下来是什么样"。不是安全边界，也不是签名。';
  OutLines[2] := 'product=' + PRODUCT_NAME;
  OutLines[3] := 'appid=' + PRODUCT_APP_ID;
  OutLines[4] := 'markerversion=' + MARKER_VERSION;
  OutLines[5] := 'version={#AppVersion}';
  OutLines[6] := 'arch={#Arch}';
  OutLines[7] := 'installedutc=' + GetDateTimeString('yyyymmdd-hhnnss', '-', '-');
  N := GetArrayLength(OutLines);
  SetArrayLength(OutLines, N + 1);
  OutLines[N] := FILES_SECTION_TAG;
  Recorded := 0;
  for I := 0 to GetArrayLength(ManagedPaths) - 1 do
  begin
    RelPath := ManagedPaths[I];
    if RelPath = '' then Continue;
    Src := AddBackslash(Dir) + ToNative(RelPath);
    if not FileFacts(Src, Size, WriteTime) then
    begin
      Log('装完找不到这个文件，不记进归属标记：' + Src);
      Continue;
    end;
    N := GetArrayLength(OutLines);
    SetArrayLength(OutLines, N + 1);
    OutLines[N] := RelPath + '|' + IntToStr(Size) + '|' + IntToStr(WriteTime);
    Recorded := Recorded + 1;
  end;
  if SaveStringsToFile(MarkerFileIn(Dir), OutLines, False) then
    Log('归属标记已写下：' + MarkerFileIn(Dir) + '（' + IntToStr(Recorded) + ' 个文件）')
  else
    Log('归属标记写不出来：' + MarkerFileIn(Dir));
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Extra: String;
begin
  if CurStep = ssPostInstall then
  begin
    WriteOwnershipMarker;
    if BackupCount > 0 then
      Extra := #13#10#13#10 + '覆盖前备份了 ' + IntToStr(BackupCount) + ' 个与上次记录不一致（或从没记录过）的文件，原件在：' + #13#10 + BackupRoot + #13#10 + '卸载不会删它；确认不需要原件后可以自己清理。'
    else
      Extra := '';
    { 装完把"装到哪、README 在哪、AI 下一步"直接写在完成页上。 }
    WizardForm.FinishedLabel.Caption :=
      'EvernightCapture 已安装到：' + #13#10 + ExpandConstant('{app}') + #13#10 + #13#10 +
      'Skill 入口：' + ExpandConstant('{app}') + '\' + SKILL_ENTRY + #13#10 +
      'README：' + ExpandConstant('{app}') + '\README.md（简体中文 README.zh-CN.md）' + #13#10 + #13#10 +
      '让 AI 工具用起来：把上面这个目录（含 SKILL.md 与 ECAPTURE.EXE）交给你的 AI 工具，' +
      '让它显式读取该 SKILL.md，并按其中说明用绝对路径调用同目录的 ECAPTURE.EXE。' + #13#10 +
      '注意：装到默认目录之外的路径时，多数工具不会自动发现，需要按工具的说明手动指向该目录。' + #13#10 + #13#10 +
      '建议先跑一次只读自检：powershell -NoProfile -ExecutionPolicy Bypass -File "' +
      ExpandConstant('{app}') + '\verify-install.ps1"' + Extra;
  end;
end;
