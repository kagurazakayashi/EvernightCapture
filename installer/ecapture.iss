; EvernightCapture 安装器（Inno Setup 6.4+：除 x64compatible 外还要 GetSHA256OfFile 的内容基线判据）
;
; 由仓库根的 build-installer.ps1 生成：它先构建、再按 installer\payload.manifest.json 收集载荷、
; 校验后把载荷目录、版本/架构/输出路径，以及一份"这次铺下去的相对路径清单"（编译期展开的
; managed-list 片段）用 /D 传给 ISCC。手工编译也可以，但必须先自行准备好载荷目录与那份片段：
;
;   ISCC.exe /DSourceDir="<载荷绝对路径>" /DAppVersion=1.0.0 /DArch=x64 ^
;            /DManagedList="<载荷目录>\managed-list.pinc" ^
;            /DOutputDir="<输出绝对路径>" /DOutputBase="EvernightCapture-1.0.0-x64-setup" ^
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
;     对不上、版本不在认得的集合里（本代 2 与上一代 1），一律按"未知目录"处理 —— 继续前必须
;     当面确认，静默模式默认按"否"中止，不写任何文件。
;     这只是"别把用户目录当自己的东西"的工程约束，不是防恶意本机用户的安全边界。
;   * 确认属于本产品的重装/升级，覆盖前会把两类文件先复制进 <安装目录>\.ecapture-backup-<UTC 时间>\
;     （保留相对路径；时间戳目录已存在时追加序号，绝不覆盖旧备份）：
;       1) 上次归属标记里记过、但现状与记录对不上的。本代标记（markerversion 2）对每个受管文件
;          记录 大小 + 写入时间 + 内容 SHA-256：大小或时间先不同就判"变了"；两者一致时**仍要**
;          拿 GetSHA256OfFile 比内容 —— 同大小、同时间戳的修改（故意恢复时间戳的替换）也认得出，
;          哈希调用或读取失败不算"未变化"。上一代标记（markerversion 1）只有大小/时间，没有内容
;          基线：不能凭它们断言"一模一样"，将被覆盖又无法证明未改动的原件一律先备份再继续；
;          给旧标记伪造内容基线是禁止的。
;       2) 这次要写的同名文件在标记里没有记录（不能只说一句"这是产品文件"就覆盖）。
;     备份读写失败就中止安装，不带不确定的东西往下写。备份目录不是本产品的文件，卸载不碰它。
;     这里的哈希用于**变化检测**，不是签名校验，也不构成对本机攻击者的防护（标记本身不是安全边界）。
;   * 升级/重装尽量沿用上次选过的目录（UsePreviousAppDir）；换目录时旧目录原样保留，不隐式删除或迁移。
;   * 卸载只删安装器自己记录过的文件，外加那个 ownership 标记（[UninstallDelete] 里点名这一个）；
;     用户添加的文件、备份目录与相邻 Skill 一律保留，绝不递归通配删除父目录。
;   * 配置的两份文件是两种身份，不能混：模板 ECAPTURE.example.ini 是**受管载荷**（跟着 [Files] 的整目录
;     复制铺进 {app}，升级正常覆盖，也在编译期受管清单与归属标记记录里）；实际用户配置 ECAPTURE.ini
;     是**用户数据**（不在载荷里，所以不在受管清单与归属标记记录里）：安装时只在它确实不存在时从模板
;     独占创建（onlyifdoesntexist），已有那份无论有效还是损坏都不覆盖、不备份、不修复、不删除；
;     uninsneveruninstall 让卸载也不碰它。程序读的是 ECAPTURE.ini，缺失是正常状态（内置默认值），
;     损坏则是明确的配置错误 —— 那属于使用问题，不是"安装文件损坏"，随包自检据此分开报（见
;     verify-install.ps1 与 tests\install-lifecycle.ps1）。
;   * 截图历史 <安装目录>\history\日期\ 是本产品**不拥有也不管理**的那一类：它不在编译期受管清单
;     managed-list 里（打包阶段就把那棵树挡在载荷之外，见 build-installer.ps1 的内容守卫），所以
;     升级/重装既不会覆盖它也不会把它备份走，卸载不删它，归属判断也不会因为目录里多出这一棵就把
;     本产品的安装目录误认成"不受管目录"（DirOwnedByProduct 只认那个 ownership 标记）。要清历史由
;     用户自己显式做。
;   * 不按进程名终止用户程序（CloseApplications=no / RestartApplications=no）；ECAPTURE.EXE 被占用时
;     让它如实失败并可回滚，而不是强杀。
;   * ISPP 会把**行首**的 # 当成预处理器指令：续行如果以 #13#10 开头，6.7.1 实测报
;     "Unknown preprocessor directive" 并中止编译，所以长表达式里的 #13#10 必须留在行中间。
;   * 事务边界（本机 2026-10-11 实测重跑，判据见 tests\install-lifecycle.ps1）：
;       - 未知目录被静默拒绝时，安装器返回退出码 1，目录里一个文件都没写（不是"退出码 2"）。
;       - 引入内容基线后，"ECAPTURE.EXE 被独占打开再重装"这种占用现场在 PrepareToInstall 的备份读取
;         处就中止（实测退出码 7）：文件复制阶段根本没开始，目录里整套保持失败前原样，
;         用户后加的文件不受影响。旧版（只比大小/时间、无需备份读取）当时的实测是复制阶段失败、
;         退出码 5，Inno 会把已复制的文件撤回去 —— 两种走法都不留半新半旧的混装。
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
; 归属标记的格式版本：本代（2）的每条受管文件记录带内容 SHA-256。build-installer.ps1 从载荷清单
; 的 markerVersion 用 /DMarkerVersion 递下来，默认值必须与那份清单一致（打包阶段有静态核对）。
#ifndef MarkerVersion
  #define MarkerVersion "2"
#endif
; 配置这一对文件名的唯一来源是载荷清单的 configPayload：build-installer.ps1 用 /DConfigTemplate 与
; /DUserConfig 把清单里的值递下来，下面的默认值必须与那份清单一致（打包阶段有静态核对，不一致就
; 拒绝出包）。模板是受管载荷；ECAPTURE.ini 是用户数据，永远不是载荷。
#ifndef ConfigTemplate
  #define ConfigTemplate "ECAPTURE.example.ini"
#endif
#ifndef UserConfig
  #define UserConfig "ECAPTURE.ini"
#endif
#ifndef AppIdGuid
  #define AppIdGuid "{B7A2E1C4-5D3F-4E6A-9C21-8F0B3D5E7A10}"
#endif
; 通知身份（AppUserModelID）的唯一来源是 src/Notification.h 里的 kNotifyAumid：build-installer.ps1
; 从那一份头文件读出来，用 /DAumid 递下来，并在打包阶段静态核对这里的默认值与下面 [Icons] 真的用了
; {#Aumid} —— 不一致就拒绝出包。为什么要它：未打包的 Win32 程序交出去的 toast 只能靠这个字符串在
; 开始菜单快捷方式上对号；对不上时系统把它归成未知应用或直接丢掉，而程序自己仍会如实报
; "已交给通知平台"（那一句从来不证明屏幕上出现过什么）。所以这一条不是装饰，是通知功能的前提。
#ifndef Aumid
  #define Aumid "yashi.EvernightCapture"
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
; AllowNoIcons **故意不开**。这条快捷方式不是可有可无的启动入口，而是通知功能唯一的身份登记点：
; Windows 只认开始菜单快捷方式上的 AppUserModelID。一旦允许"不创建"，Inno 就会把这个选择记进卸载键
; （实测值 "Inno Setup: No Icons" = 1），之后的安装沿用同一条记录，于是界面与命令行都不报错、
; 而屏幕上永远不会有通知 —— 程序那边仍然只报"已交给通知平台"，那种现场最难查。
; 所以这里宁可没有开关：功能载荷无条件铺下去，/NOICONS 也不再能把它跳掉。
DisableReadyPage=no

[Languages]
; 只用 Inno 自带的默认语言（英文界面），避免依赖未随安装的 .isl 语言包而编译失败。
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; 载荷目录整份铺进 {app} 根：ECAPTURE.EXE / SKILL.md / references / README×4 / LICENSE /
; ECAPTURE.example.ini / resources / tests / build / verify-install.ps1 与 build-installer 生成的
; install-manifest.json、payload.sha256.txt。不递归删除、不按通配删除用户内容；卸载走 Inno 自己的安装记录。
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; 实际用户配置：只在**确实不存在**时由模板独占创建一份（onlyifdoesntexist —— 已存在的文件，无论
; 有效还是损坏，都不覆盖、不"顺手修好"），并且 uninsneveruninstall：卸载不删它。
; 它因此也不在编译期受管清单 managed-list 里（载荷里根本没有这个文件），升级时覆盖前保护不遍历它、
; 归属标记也不记录它 —— 用户自己写的输出路径模板与偏好不会被打进发行清单或被当成产品文件。
; 模板本身（上面那条整目录复制）才是受管载荷：升级会把它更新成当前版本，用户的这份 ECAPTURE.ini 不会。
Source: "{#SourceDir}\{#ConfigTemplate}"; DestDir: "{app}"; DestName: "{#UserConfig}"; Flags: ignoreversion onlyifdoesntexist uninsneveruninstall

[Icons]
; 这一条不是"给人从开始菜单双击启动截图 CLI"用的 —— 那是个命令行工具，没人这么用它。它存在的唯一
; 理由是给通知一个登记得上的系统身份：Windows 只认**开始菜单快捷方式上的 AppUserModelID**，缺了它，
; 本工具交出去的 toast 会被归成"未知应用"甚至直接丢弃，而程序这边只能如实报"已交给通知平台"（那
; 从来不代表屏幕上真的出现过什么）。所以它是通知功能的载荷，不是装饰。
; 它不碰任何相邻的开始菜单项，也不改 PATH、不改任何 AI 工具的配置；这条快捷方式由 Inno 自己创建、
; 自己记进卸载记录、自己回收，删除范围只有它建的这一个文件。
Name: "{group}\EvernightCapture"; Filename: "{app}\ECAPTURE.EXE"; AppUserModelID: "{#Aumid}"; Comment: "EvernightCapture screenshot CLI (this entry also registers the desktop notification identity)"

[UninstallDelete]
; 只清这一个安装器自己写的归属标记；备份目录、用户新增文件、用户配置 {#UserConfig}、
; 截图历史 history\ 与相邻 Skill 都不在这里，故一律保留。
Type: files; Name: "{app}\{#OwnershipMarker}"

[Code]
const
  SKILL_ENTRY       = 'SKILL.md';
  MANIFEST          = 'install-manifest.json';
  SKILL_REL_DIR     = '.agents\skills\yashi-evernight-capture';
  PRODUCT_NAME      = 'EvernightCapture';
  PRODUCT_APP_ID    = '{#AppIdGuid}';
  MARKER_VERSION    = '{#MarkerVersion}';
  { 上一代标记（1）只有 大小|写入时间、没有内容基线。目录归属上仍然认它 —— 不该把装过本产品的
    用户拦在"未知目录"确认上；但覆盖前保护不拿它的 大小/时间 断言"一模一样"：认得出那是
    没有内容证据的记录，将被覆盖的原件先备份再继续。 }
  MARKER_VERSION_LEGACY = '1';
  BACKUP_PREFIX     = '.ecapture-backup-';
  FILES_SECTION_TAG = '--files--';

var
  ManagedPaths: TArrayOfString;      { 编译期由 managed-list 片段填好：这次要铺下去的相对路径 }
  BackupRoot: String;
  BackupCount: Integer;
  { 用户那份 ECAPTURE.ini 在**本次复制动作之前**在不在。完成页要说"这一次实际发生了什么"，
    所以这一条必须在 [Files] 那一步动手之前记下来，事后不能再猜。 }
  ConfigPreexisting: Boolean;

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
  装下来时是什么样子"；下次重装/升级靠它认出被改过的已管理文件。
  格式：若干 key=value 头 + 一行 --files-- + 若干文件记录行。
    markerversion 2：相对路径|大小|写入时间(FILETIME, Int64)|内容SHA-256(小写十六进制)
    markerversion 1（上一代）：相对路径|大小|写入时间 —— 没有内容基线，只能当"认得这个目录"，
    不能当"这个文件没被改过"的证据；将被覆盖的原件按"无法证明未改动"先备份。
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
  if (MarkerVer <> MARKER_VERSION) and (MarkerVer <> MARKER_VERSION_LEGACY) then
  begin
    Log('目录里的归属标记版本是 ' + MarkerVer + '，本安装器认的是 ' + MARKER_VERSION +
        '（或上一代 ' + MARKER_VERSION_LEGACY + '），按未知目录处理。');
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

{ 那一个文件此刻的内容 SHA-256（小写十六进制）：用 Inno Setup 自带的 GetSHA256OfFile。
  "调用抛异常"与"返回空串/长度不对"两种失败都按读不出来处理 —— 读不出来不等于未变化：
  升级这一头的哈希是**变化检测**（覆盖前保护要问"能不能证明这个原件没被动过"），
  不是签名校验，也不是对本机攻击者的防护；归属标记自己就不是安全边界。 }
function TryFileSha256(const FullPath: String; var Hash: String): Boolean;
begin
  Hash := '';
  Result := False;
  try
    Hash := Lowercase(GetSHA256OfFile(FullPath));
    Result := Length(Hash) = 64;
    if not Result then Hash := '';
  except
    Log('GetSHA256OfFile 失败（读不出来？）：' + FullPath);
    Hash := '';
    Result := False;
  end;
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
  Note[4] := '判定依据：与上一次安装记下的归属基线（大小 / 写入时间 / 内容 SHA-256）对不上；';
  Note[5] := '或者那份标记是上一代、只有大小/时间而没有内容基线；或者这个同名文件从没在归属标记里记录过。确认不需要原件之后可以自己删掉整个目录。';
  if not SaveStringsToFile(AddBackslash(BackupRoot) + 'BACKUP-INFO.txt', Note, False) then
    Log('备份说明写不出来：' + BackupRoot);
end;

{ 返回非空字符串 = 中止安装（Inno 会把它作为错误信息报出来，不写任何文件）。 }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dir, RelPath, Reason, Target: String;
  Product, AppId, MarkerVer: String;
  OldFiles, Fields: TArrayOfString;
  I, L: Integer;
  Size: Int64;
  WriteTime: Int64;
  CurSha: String;
  Recorded, Proven: Boolean;
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
    Target := AddBackslash(Dir) + ToNative(RelPath);
    if not FileExists(Target) then Continue;   { 这个位置还没有同名文件 }

    { "未改动"必须是**证明**出来的：本代记录（4 条字段带内容 SHA-256）先比大小/写入时间这个
      快速判别，两者一致时仍要拿内容证据；上一代记录（3 条字段）只有大小/时间，一致也
      拿不出内容证据，同样不算证明。证明不了又将被覆盖的原件 → 先备份再继续。 }
    Recorded := False;
    Proven := False;
    Reason := '';
    for L := 0 to GetArrayLength(OldFiles) - 1 do
    begin
      if not SplitByChar(OldFiles[L], '|', Fields) then Continue;
      if (GetArrayLength(Fields) < 3) or (GetArrayLength(Fields) > 4) then Continue;
      if Trim(Fields[0]) <> RelPath then Continue;
      Recorded := True;
      if not FileFacts(Target, Size, WriteTime) then
        Reason := '现状的大小 / 写入时间读不出来，证明不了未改动'
      else if (Size <> FieldAsInt64(Fields, 1)) or (WriteTime <> FieldAsInt64(Fields, 2)) then
        Reason := '归属标记里记过，但现状的大小或写入时间已经变了'
      else if GetArrayLength(Fields) < 4 then
        Reason := '上一代标记只有大小/时间、没有内容基线；同大小同时间戳的改动证明不了是未改动'
      else if Trim(Fields[3]) = '' then
        Reason := '标记里这个文件没有内容哈希记录，不伪造基线'
      else if not TryFileSha256(Target, CurSha) then
        Reason := '现状文件的内容哈希读不出来（被占用？），证明不了未改动'
      else if Lowercase(CurSha) = Lowercase(Trim(Fields[3])) then
        Proven := True
      else
        Reason := '大小与写入时间没变，但内容 SHA-256 与记录不同（同大小同时间戳的修改）';
      Break;
    end;

    if Recorded and Proven then Continue;    { 内容与上次装下来时逐字节一致，不用备份 }
    if Reason = '' then
    begin
      if Recorded then
        Reason := '归属标记里记过，但现状证明不了未改动'
      else
        Reason := '要写的同名文件从没在本产品的归属标记里记录过';
    end;
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
  Sha: String;
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
    { 内容基线尽力记全：这一刻读不出哈希（极少见，例如被别的进程独占）就把第四段留空 ——
      空基线不冒充"有记录"，下次升级会把这份原件当作"证明不了未改动"先备份再继续。 }
    if not TryFileSha256(Src, Sha) then
      Log('这个文件的内容哈希此刻读不出来，标记里留空（下次覆盖前会先备份它）：' + Src);
    N := GetArrayLength(OutLines);
    SetArrayLength(OutLines, N + 1);
    OutLines[N] := RelPath + '|' + IntToStr(Size) + '|' + IntToStr(WriteTime) + '|' + Sha;
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
  ConfigNote: String;
  UserCfg: String;
begin
  if CurStep = ssInstall then
  begin
    { 复制动作还没开始了，先把"这一份在不在"记下来。[Files] 那一条带 onlyifdoesntexist，
      装前已经在的那一份一个字都不动 —— 完成页要说的是这一次的**实际**下场，不能事后猜。 }
    ConfigPreexisting := FileExists(ExpandConstant('{app}\{#UserConfig}'));
    Exit;
  end;
  if CurStep = ssPostInstall then
  begin
    WriteOwnershipMarker;
    if BackupCount > 0 then
      Extra := #13#10#13#10 + '覆盖前备份了 ' + IntToStr(BackupCount) + ' 个与上次记录不一致（或从没记录过）的文件，原件在：' + #13#10 + BackupRoot + #13#10 + '卸载不会删它；确认不需要原件后可以自己清理。'
    else
      Extra := '';
    { 偏好配置那一句按实际执行结果说三种情况：装前就有 = 原样保留；装前没有而现在有了 =
      这一次确实从模板建出来了；装前没有而现在还是没有 = 如实说没建出来，不谎称复制成功。 }
    UserCfg := ExpandConstant('{app}\{#UserConfig}');
    if ConfigPreexisting then
      ConfigNote := '装前这一份已经存在，本次原样保留（没覆盖、没删，你改过的内容还在）。'
    else if FileExists(UserCfg) then
      ConfigNote := '装前这一份不存在，本次已从模板 {#ConfigTemplate} 复制出一份来。'
    else
      ConfigNote := '本次没能建出这一份（模板复制被跳过或没写成功）。程序照内置默认运行，需要偏好时可以自己从 {#ConfigTemplate} 复制一份再改。';
    { 装完把"装到哪、README 在哪、AI 下一步"直接写在完成页上。 }
    WizardForm.FinishedLabel.Caption :=
      'EvernightCapture 已安装到：' + #13#10 + ExpandConstant('{app}') + #13#10 + #13#10 +
      'Skill 入口：' + ExpandConstant('{app}') + '\' + SKILL_ENTRY + #13#10 +
      'README：' + ExpandConstant('{app}') + '\README.md（简体中文 README.zh-CN.md）' + #13#10 + #13#10 +
      '让 AI 工具用起来：把上面这个目录（含 SKILL.md 与 ECAPTURE.EXE）交给你的 AI 工具，' +
      '让它显式读取该 SKILL.md，并按其中说明用绝对路径调用同目录的 ECAPTURE.EXE。' + #13#10 +
      '注意：装到默认目录之外的路径时，多数工具不会自动发现，需要按工具的说明手动指向该目录。' + #13#10 + #13#10 +
      '偏好配置：' + UserCfg + '（' + ConfigNote + ' 这一份不会被升级覆盖，卸载也不会删。' +
      '想知道当前生效的设置来自哪一层，跑一次只读的 ECAPTURE.EXE --show-config；' +
      '这份文件改坏了程序会明确报错，不会静默回退默认值。' + #13#10 + #13#10 +
      '建议先跑一次只读自检：powershell -NoProfile -ExecutionPolicy Bypass -File "' +
      ExpandConstant('{app}') + '\verify-install.ps1"' + Extra;
  end;
end;
