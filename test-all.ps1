<#
.SYNOPSIS
    一键跑完所有测试：先构建，再按固定顺序**串行**跑 tests\ 下的每一套判据，最后给出汇总与日志位置。
.DESCRIPTION
    这个脚本只做编排，不复制任何判据逻辑：每一套 tests\*.ps1 自己决定判什么、自己给退出码
    （`Complete-EcSuite`：有 FAIL 才是 1，SKIP 不判失败）。汇总里的"通过 / 失败 / 跳过"读的是该套件
    自己写下的那行汇总，读不到就如实显示 "-"，不拿猜测的数字冒充计数。

    子进程一律走测试共享基础设施 `tests\harness.psm1` 里的调用器：两条标准流并发消费、原始字节不经
    文本转码、等待有明确期限、超时只结束本次启动的那棵进程树并保留部分输出。这里不再抄第二套生命周期逻辑。

    为什么一定串行：桌面判据会建自己的窗口、数屏幕上的像素、还会真的弹确认框，两套同时跑会互相看到对方
    的画面与框（实测过这种偶发干扰）。所以一套跑完才跑下一套，并用一个命名互斥体拦住"同时开两个总跑"。

    默认**无人值守安全**：不代答任何真实确认框，凡是需要有人点"是"的那几项都由套件自己记 SKIP
    （SKIP 是环境与安全边界，不是通过）。要跑那部分必须显式加 -SimulateConsent，脚本还会停下来要求确认
    —— 那意味着桌面上此刻有什么就被真的拍进图里，只在专门腾出来、没有隐私内容的桌面上做。
    -TimeoutConsent 只让 timeout.ps1 跑那两条"代答否"的判据：答"否"不采任何像素。
    代答确认框这一步**永远**要当面输入 yes：-Force 只免去那句"按回车开始"，不免它；输入被重定向、
    非交互、无控制台一律按"没确认"中止（拿不到确认就是非成功，绝不默认同意）。-Offline 与
    -SimulateConsent / -TimeoutConsent 互相矛盾，在任何实际执行之前就报 OFFLINE-CONSENT-CONFLICT 退出。

    离线选路：-Offline 按登记表里的模式元数据选路，不靠套件名字猜 —— 纯离线套件（只做只读 CLI
    查询、纯函数/协议、临时文件）照常跑；有离线层的混合套件带上那个开关跑（它自己声明了才递，
    声明有离线层却在源码里找不到那个参数 = 登记表与源码不一致，整套不跑并判失败）；只有真实层、
    又没有可单独跑的离线入口的套件，整套移出计划并逐条给原因（OFFLINE-EXCLUDED）—— 它们一个字节
    都不会被启动，不是"跑了但报跳过"。AST 参数检查只是接口一致性检查，真正"不碰桌面"由套件自己
    在离线开关下保证。

    其它边界：不动 git（不 commit / 不 push / 不 add），不改显示设置、不装任何工具；日志写在
    build\test-logs\（build\ 已在 .gitignore 里）。没有 MSVC 时构建失败会以退出码 2 结束，不会拿一份旧
    产物继续跑；只想用现成产物就加 -NoBuild，或用 -Exe 直接指过去。

    在 Windows PowerShell 5.1 与 PowerShell 7 上都能跑（实测 5.1.26100 与 7.6.2 结果一致）：套件源码用
    AST 读，不依赖 `Get-Command -Path`（后者在 7 上会报 ArgumentList 那条错）。每一套默认用**跑本脚本的
    同一个 shell** 起（-Shell auto）；显式 -Shell powershell / pwsh 换另一种时，会先问一次目标 shell 自己
    那份 PSModulePath 并在起子进程期间临时换上 —— 否则 PowerShell 7 的父进程用 .NET 起 Windows PowerShell
    5.1 子进程时，子进程会拿着 7 的模块路径去找核心模块，`Get-FileHash` 这类命令当场变成"不认识"（本机实测
    踩过，save.ps1 就是这么红的）。正常结束、被 -SuiteTimeoutSec 判超时、还是中途异常（trap 收尾），都会把
    并发互斥体放手并关闭句柄、把控制台编码还原 —— 上一版只在正常出口 Release 而不 Dispose，于是一次异常
    中止后，同一个交互式 shell 会一直占着那份句柄，下一次运行被自己的残留挡住。

    退出码：0 = 计划内的套件全部跑到并通过（各套件自己记的 SKIP 算未验证，不算失败）；
    1 = 至少一套 FAIL / TIMEOUT / NO EXIT / NO START，**或计划里点名要跑的整套没执行起来**
        （NOT RUN：脚本文件不见了、参数解析不干净、离线层元数据与源码不符）—— "这套根本没跑"不是一张
        合格的成绩单；
    2 = 前置没成立（构建失败、找不到产物、参数不认识、筛完没有可执行测试、参数互相冲突、已有另一个
        总跑在跑）。拿不到代答确认框的那句 yes 也归这一档。

    汇总里三种"没跑/没验"分开写，不合并成一个数字：按 -Quick / -Only / -Except / -Offline 刻意没排的、
    因 -StopOnFail 提前停下的剩余、以及意外整套没跑成的（只有最后这一类判退出码 1）；各套件自己
    记的 SKIP 属于环境与安全边界，照旧既不升成失败、也不当通过。"跑到并交出结果"只算真正给出终态的
    套件，NO START / NO EXIT 不算。筛完是零套件的计划（例如 `-Only cli -Except cli`、`-Only save -Offline`）
    直接按前置不成立报"没有可执行测试"，不进汇总。
.EXAMPLE
    .\test-all.ps1                       # 构建 Release，跑全部（含需要真等 30 秒以上的那两节）
    .\test-all.ps1 -Quick                # 不跑 build-path.ps1，并给 timeout.ps1 传 -SkipLong
    .\test-all.ps1 -Offline              # 只跑各套件的离线层（向支持 -SkipReal 的套件传它）
    .\test-all.ps1 -Only cli,windows     # 只跑这两套
    .\test-all.ps1 -Except build-path    # 除了那套最重的
    .\test-all.ps1 -List                 # 只看执行计划，不构建也不跑
    .\test-all.ps1 -NoBuild              # 用现有 build\ecapture.exe
    .\test-all.ps1 -Exe D:\tools\ECAPTURE.EXE
    .\test-all.ps1 -SimulateConsent -TimeoutConsent   # 无隐私专用桌面：连需要应答的判据一起跑
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$NoBuild,
    [string]$Exe,

    [switch]$Quick,
    [switch]$Offline,
    [switch]$Keep,
    [switch]$TimeoutConsent,
    [switch]$SimulateConsent,

    [string[]]$Only,
    [string[]]$Except,
    [int]$SuiteTimeoutSec = 900,
    [switch]$StopOnFail,
    [string]$LogDir,
    [string]$Shell = 'auto',

    [switch]$List,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

# 这两个先立起来，任何一步出事都能被下面的 trap 干净收尾
$prevEncoding = $null
$lock = $null
$gotLock = $false

# 同一台机器上只允许一个总跑：桌面判据必须一套跑完再跑下一套。
# 名字带 -2：上一版只在正常出口 Release 而从不 Dispose 那个 Mutex 对象，句柄会跟着**交互式会话**一直留着，
# 于是一次异常中止之后，同一个 shell 里再跑就被自己留下的锁当成"已有另一个总跑"。这一版出口一律
# Release + Dispose，异常路径由 trap 收尾，不再把锁漏给会话。
$lockName = 'Local\ecapture-test-all-2'

function Exit-TestAll {
    <# 统一出口：还原本次会话改过的控制台编码，放手并关闭互斥体句柄，再交退出码。 #>
    param([Parameter(Mandatory)][int]$Code)

    if ($null -ne $prevEncoding) { try { [Console]::OutputEncoding = $prevEncoding } catch { } }
    if ($gotLock) { try { [void]$lock.ReleaseMutex() } catch { } }
    if ($lock) { try { $lock.Dispose() } catch { } }
    exit $Code
}

trap {
    <# 任何没被就近处理的错都走这里收尾：把锁与编码还给系统，别让下一次运行被上一次的中止挡住。 #>
    Write-Host "总跑异常中止：$_" -ForegroundColor Red
    if ($null -ne $prevEncoding) { try { [Console]::OutputEncoding = $prevEncoding } catch { } }
    if ($gotLock) { try { [void]$lock.ReleaseMutex() } catch { } }
    if ($lock) { try { $lock.Dispose() } catch { } }
    exit 2
}

# 子进程输出的是 UTF-8 字节；控制台码页不切到 UTF-8 时中文会花屏。只动当前会话，退出前还原
try {
    $prevEncoding = [Console]::OutputEncoding
    [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false, $false)
} catch { $prevEncoding = $null }

$lock = New-Object Threading.Mutex($false, $lockName)
try { $gotLock = $lock.WaitOne(0) } catch [Threading.AbandonedException] { $gotLock = $true }
if (-not $gotLock) {
    # 没拿到锁也照样关掉自己刚打开的那个句柄，不然本会话又白占一份
    try { $lock.Dispose() } catch { }
    $lock = $null
    Write-Host '已经有另一个 .\test-all.ps1 在跑：请先等它结束（并发跑会让桌面判据互相干扰）。' -ForegroundColor Red
    Exit-TestAll 2
}

$repo = (Get-Item -LiteralPath $PSScriptRoot).FullName
Import-Module (Join-Path $repo 'tests\harness.psm1') -Force -DisableNameChecking

function Get-SuiteParameterNames {
    <#
        读一份 .ps1 **脚本自己**声明了哪些参数（不执行它）：这样"给哪套传哪个开关"跟着套件源码走，
        不在这里维护一份会过期的对照表，也不会把开关递给不认识它的套件。

        用 AST 而不是 `Get-Command -Path`：后者在 PowerShell 7 上会报"ArgumentList 只能在取单条命令时
        指定"（本机 7.6.2 实测），而 ParseFile 在 5.1 与 7.x 上给出同一份结果。只取 ParamBlock，
        也就是脚本级参数，不含脚本内部函数各自的 param，免得把同名开关误递给脚本。

        返回 $null 表示"读不出来"（连解析都不干净），与"它一个参数都没有"的空数组分开。
        这个分开必须靠外面包一层对象来做：函数里 `return @()` 经过命令输出会被 PowerShell 摊平，
        调用方拿到的就是 $null（实测 `$null -eq (f)` 为真），于是"这份套件不收参数"会被误读成
        "读不出来"而整套不跑 —— 未运行现在是要判失败的，这种误读不能留。
    #>
    param([Parameter(Mandatory)][string]$Path)

    try {
        $tokens = $null
        $errors = $null
        $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
        if ($null -eq $ast) { return $null }
        if ($errors -and @($errors).Count) { return $null }
        $names = @($ast.ParamBlock.Parameters | ForEach-Object { [string]$_.Name.VariablePath.UserPath })
        return [pscustomobject]@{ Names = $names }
    } catch {
        return $null
    }
}

function Test-ShellIsPS7 {
    <# 只看可执行文件名分两种 shell；认不出来的返回 $null，让调用方按"另一种"处理（宁可多问一次模块路径）。 #>
    param([Parameter(Mandatory)][string]$Path)

    $leaf = (Split-Path -Path $Path -Leaf).ToLowerInvariant()
    if ($leaf -like 'pwsh*') { return $true }
    if ($leaf -like 'powershell*') { return $false }
    return $null
}

function Resolve-TestShell {
    <#
        auto = 用**跑本脚本的同一个 shell** 去起每一套（默认，也是最不会出问题的走法）；
        pwsh / powershell = 显式指定另一种；也可以给完整路径。
    #>
    param([Parameter(Mandatory)][string]$Spec)

    $self = $null
    try { $self = (Get-Process -Id $PID).MainModule.FileName } catch { }
    switch ($Spec.ToLowerInvariant()) {
        'auto' {
            if ($self) { return $self }
            return 'powershell.exe'
        }
        'pwsh' {
            $c = Get-Command pwsh.exe -ErrorAction SilentlyContinue
            if ($c) { return $c.Source }
            if ($self -and (Test-ShellIsPS7 -Path $self)) { return $self }
            throw 'PATH 里找不到 pwsh.exe（装 PowerShell 7 或改用 -Shell auto）'
        }
        'powershell' {
            $c = Get-Command powershell.exe -ErrorAction SilentlyContinue
            if ($c) { return $c.Source }
            if ($self -and -not (Test-ShellIsPS7 -Path $self)) { return $self }
            throw 'PATH 里找不到 powershell.exe（Windows PowerShell 5.1 是系统自带的）'
        }
        default {
            if (Test-Path -LiteralPath $Spec) { return (Get-Item -LiteralPath $Spec).FullName }
            throw "找不到 -Shell 指定的程序：$Spec"
        }
    }
}

function Get-CanonicalModulePath {
    <#
        问一次目标 shell 它自己要用的 PSModulePath：这里走 PowerShell 自己的原生调用，
        不是 .NET 那套把父进程环境原样递下去的起法，所以拿回来的是目标 shell 干净的那份。
    #>
    param([Parameter(Mandatory)][string]$Path)

    try {
        $v = (& $Path -NoProfile -Command '$env:PSModulePath' 2>$null | Out-String)
        $v = @($v -split "[`r`n]+" | Where-Object { $_ } | Select-Object -First 1)
        if ($v.Count -ge 1) { return ([string]$v[0]).Trim() }
    } catch { }
    return $null
}

function Invoke-Step {
    <#
        跑一步（构建 / 产物自检 / 一套判据）：两条流按原始字节落盘，文本用调用器解好的那份。
        超时就没有可信的退出码，如实交回 $null；被截断时也带上标记，不拿半份输出当全部。
    #>
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [Parameter(Mandatory)][string]$OutFile,
        [Parameter(Mandatory)][string]$ErrFile,
        [int]$TimeoutSec = 900,
        [string]$ChildModulePath
    )

    $prevPsmp = $env:PSModulePath
    # 毫秒数走 [long] 再夹到 [int]：-SuiteTimeoutSec 给得很大时，$TimeoutSec * 1000 会先溢出成负数，
    # 于是调用器一上来就判"超时"，把健康的套件全部误杀。
    $timeoutMs = [long]$TimeoutSec * 1000L
    if ($timeoutMs -lt 1000L) { $timeoutMs = 1000L }
    if ($timeoutMs -gt [long][int]::MaxValue) { $timeoutMs = [long][int]::MaxValue }
    $r = if ($ChildModulePath) {
        try {
            $env:PSModulePath = $ChildModulePath
            Invoke-EcProcess -FilePath $FilePath -Arguments $Arguments -TimeoutMs ([int]$timeoutMs)
        } finally { $env:PSModulePath = $prevPsmp }
    } else {
        Invoke-EcProcess -FilePath $FilePath -Arguments $Arguments -TimeoutMs ([int]$timeoutMs)
    }
    # 注意：PowerShell 5.1 里 `$x = if (cond) { 空数组 }` 会被管道拆成 $null，所以这里显式补一份空 byte[]
    $outBytes = $r.StdoutBytes
    $errBytes = $r.StderrBytes
    if ($null -eq $outBytes) { $outBytes = New-Object byte[] 0 }
    if ($null -eq $errBytes) { $errBytes = New-Object byte[] 0 }
    [IO.File]::WriteAllBytes($OutFile, $outBytes)
    [IO.File]::WriteAllBytes($ErrFile, $errBytes)

    return [pscustomobject]@{
        Exit = $(if ($r.TimedOut) { $null } else { $r.Exit })
        TimedOut = [bool]$r.TimedOut
        Sec = [int][math]::Ceiling($r.DurationMs / 1000)
        Text = ('{0}{1}{2}' -f $r.Stdout, [Environment]::NewLine, $r.Stderr)
        KilledBy = $r.KilledBy
        StartError = $r.StartError
        Truncated = [bool]$r.Truncated
    }
}

function Get-LogCounts {
    <# 从该套件自己写的汇总行里读计数；读不到就留空，由调用方如实显示 "-"。 #>
    param([Parameter(Mandatory)][string]$Text)

    $pass = $null; $fail = $null; $skip = $null
    # Complete-EcSuite：失败时"失败：N 项，通过 M 项"，成功时"全部通过：N 项（，跳过 M 项）"
    if ($Text -match '失败：\s*(\d+)\s*项，通过\s*(\d+)\s*项') { $fail = $Matches[1]; $pass = $Matches[2] }
    elseif ($Text -match '全部通过：\s*(\d+)\s*项') { $pass = $Matches[1]; $fail = '0' }
    if ($Text -match '跳过\s*(\d+)\s*项') { $skip = $Matches[1] }
    # cli.ps1 自己那份汇总：共 N 例，通过 M，失败 K
    if ($Text -match '共\s*(\d+)\s*例，通过\s*(\d+)，失败\s*(\d+)') { $pass = $Matches[2]; $fail = $Matches[3] }
    return [pscustomobject]@{ Pass = $pass; Fail = $fail; Skip = $skip }
}

function Show-TextTail {
    <# 失败时把尾部贴出来，省得每个人都去翻日志；成功时细节留在日志文件里。 #>
    param([Parameter(Mandatory)][string]$Text, [int]$Lines = 40)

    foreach ($line in @($Text -split "[`r`n]+" | Where-Object { $_ } | Select-Object -Last $Lines)) {
        Write-Host "    $line"
    }
}

# ---------------------------------------------------------------------------
# 执行计划：顺序是有意的（先纯离线与只读契约，再自建窗口的桌面判据，最后是最重的构建套）
#
# 模式元数据（test-all 自己维护，不再靠"把开关名字硬编码进传参逻辑"）：
#   Real        ——  这套到底碰不碰真实桌面：'none' = 纯离线（只做只读 CLI 查询、纯函数/协议、
#                    临时文件与受控进程），-Offline 下照常跑；'desktop' = 有真实桌面层；
#                    'build' = 只碰构建工具链。
#   OfflineLayer ——  把它变成"离线安全"的那个脚本级开关名（'' = 没有可单独跑的离线入口）。
#                    'SkipReal' / 'OfflineOnly' 两种。
# 这三类的用法：Real='none' 的照常跑；Real!='none' 且有 OfflineLayer 的，在 -Offline 下带上那个
# 开关跑（套件自己的 AST 里找不到那个参数 = 登记表与源码不一致，整套不跑并判失败）；
# Real!='none' 又没有 OfflineLayer 的，在 -Offline 下整套移出计划并逐条给原因（OFFLINE-EXCLUDED）。
# 元数据是接口约定，不是安全证明：真正的"不碰桌面"仍由各套件自己在 -SkipReal 下保证。
# ---------------------------------------------------------------------------
$suites = @(
    [pscustomobject]@{ Name = 'invoker';      Real = 'none';    OfflineLayer = '';            Note = '测试调用器自身（argv 引号、双流、二进制、超时、并发）' },
    [pscustomobject]@{ Name = 'orchestration'; Real = 'none';   OfflineLayer = '';            Note = '总跑自身：空计划校验与"意外没跑成/主动没跑/SKIP"的分类与退出码' },
    [pscustomobject]@{ Name = 'cli';          Real = 'none';    OfflineLayer = '';            Note = '输出契约与退出码（只跑 --dry-run，不截图不落地）' },
    [pscustomobject]@{ Name = 'windows';      Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '--list / --inspect 结构化窗口发现' },
    [pscustomobject]@{ Name = 'screens';      Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '--screens 与按标识选屏的三种下场' },
    [pscustomobject]@{ Name = 'capabilities'; Real = 'none';    OfflineLayer = '';            Note = '--capabilities / --diagnostics 判据与真机自述（只读查询，不拍像素）' },
    [pscustomobject]@{ Name = 'compat';       Real = 'none';    OfflineLayer = '';            Note = '版本下限闸门与发布产物的 API set 导入（只读探测，不拍像素）' },
    [pscustomobject]@{ Name = 'cursor';       Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '--cursor 逐路径登记表与结果三键' },
    [pscustomobject]@{ Name = 'hdr';          Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '--hdr 色彩事实分层与 tone mapping 数学' },
    [pscustomobject]@{ Name = 'delivery';     Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '交付事实与期限合规分开记' },
    [pscustomobject]@{ Name = 'history';      Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '主交付之外那一份历史副本：命名、独占提交、失败归类与清理/落点跟随' },
    [pscustomobject]@{ Name = 'save';         Real = 'desktop'; OfflineLayer = '';            Note = '原子写、覆盖保护、批次命名' },
    [pscustomobject]@{ Name = 'image';        Real = 'desktop'; OfflineLayer = '';            Note = '帧形状判据与像素操作' },
    [pscustomobject]@{ Name = 'crop';         Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '--roi / --client-area' },
    [pscustomobject]@{ Name = 'scale';        Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '--scale 只缩不放与最近邻映射' },
    [pscustomobject]@{ Name = 'wgc';          Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = 'WGC 实时尺寸与帧池重建' },
    [pscustomobject]@{ Name = 'identity';     Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '取帧前的身份复核与 Z 序选择' },
    [pscustomobject]@{ Name = 'streams';      Real = 'desktop'; OfflineLayer = '';            Note = '标准流分工与异常边界' },
    [pscustomobject]@{ Name = 'isolation';    Real = 'desktop'; OfflineLayer = '';            Note = '进程归属：不碰别人的同名进程' },
    [pscustomobject]@{ Name = 'timeout';      Real = 'desktop'; OfflineLayer = '';            Note = '一份预算与辅助进程自己的时钟（含真等 30 秒以上的两节）' },
    [pscustomobject]@{ Name = 'dup';          Real = 'desktop'; OfflineLayer = 'SkipReal';    Note = '桌面复制几何与那块屏变了那几条' },
    [pscustomobject]@{ Name = 'channels';     Real = 'desktop'; OfflineLayer = '';            Note = '六条通道对自建窗口的成像' },
    [pscustomobject]@{ Name = 'consent';      Real = 'desktop'; OfflineLayer = '';            Note = '授权两级与拒绝传播（会真的弹框，只代答否）' },
    [pscustomobject]@{ Name = 'screen';       Real = 'desktop'; OfflineLayer = '';            Note = '整屏三条桌面路径（要 -SimulateConsent 才跑全）' },
    [pscustomobject]@{ Name = 'smoke';        Real = 'desktop'; OfflineLayer = '';            Note = '端到端出图与像素内容' },
    [pscustomobject]@{ Name = 'installer-package'; Real = 'build'; OfflineLayer = '';           Note = '打包判据：staging 归属、离线依赖闭包、产物身份、缺依赖不出包（需要 installer\packaging.psm1 与 Inno 编译器）' },
    [pscustomobject]@{ Name = 'install-lifecycle'; Real = 'build'; OfflineLayer = '';           Note = '真装真卸判据：只在本次自建的临时目录里安装/卸载，判归属保护、用户改动备份、失败现场与卸载边界（需要安装包）' },
    [pscustomobject]@{ Name = 'build-path';   Real = 'build';   OfflineLayer = 'OfflineOnly';  Note = '构建路径（把仓库复制进怪路径反复构建，最慢）' }
)

function Normalize-SuiteName {
    param([string]$n)
    return ($n -replace '\.ps1$', '' -replace '^tests\\', '' -replace '/', '\').Trim().ToLowerInvariant()
}

# -File 传进来的 "a,b" 会整串绑成一个元素，这里再按逗号拆开，两种写法都认
function Split-NameList {
    param([string[]]$Values)
    if (-not $Values) { return @() }
    $parts = @()
    foreach ($v in $Values) { $parts += ($v -split ',') }
    return @($parts | ForEach-Object { Normalize-SuiteName $_ } | Where-Object { $_ })
}

$Only = Split-NameList -Values $Only
$Except = Split-NameList -Values $Except

$plan = @($suites)
if ($Quick) { $plan = @($plan | Where-Object { $_.Name -ne 'build-path' }) }
if ($Only) {
    $unknown = @($Only | Where-Object { $suites.Name -notcontains $_ })
    if ($unknown) { Write-Host "不认识这些套件名：$($unknown -join ', ')" -ForegroundColor Red; Exit-TestAll 2 }
    $plan = @($plan | Where-Object { $Only -contains $_.Name })
}
if ($Except) {
    $dropBad = @($Except | Where-Object { $suites.Name -notcontains $_ })
    if ($dropBad) { Write-Host "不认识这些套件名：$($dropBad -join ', ')" -ForegroundColor Red; Exit-TestAll 2 }
    $plan = @($plan | Where-Object { $Except -notcontains $_.Name })
}

# ---------------------------------------------------------------------------
# 参数合法性与冲突：在任何实际执行之前就拒绝。
# -SuiteTimeoutSec 是**逐套**预算，非法值（0、负数、大得离谱）不该跑完才发现，
# 更不该在换算成毫秒时溢出成负数、把每一套都误判成超时。
# 离线层与"代答真实确认框"是互相矛盾的两句话 ——
# -Offline 要求不碰真实确认框，-SimulateConsent / -TimeoutConsent 正是去操作它。
# 这两个检查不受 -Force 影响，也不因筛掉了哪几套而跳过。
# ---------------------------------------------------------------------------
if ($SuiteTimeoutSec -lt 1 -or $SuiteTimeoutSec -gt 86400) {
    Write-Host '-SuiteTimeoutSec 必须在 1..86400 秒之间（它是每套的预算，不是总预算）' -ForegroundColor Red
    Exit-TestAll 2
}
if ($Offline -and $SimulateConsent) {
    Write-Host 'OFFLINE-CONSENT-CONFLICT：-Offline 与 -SimulateConsent 不能同时给（前者要求不碰真实确认框，后者正是去代答它）。' -ForegroundColor Red
    Exit-TestAll 2
}
if ($Offline -and $TimeoutConsent) {
    Write-Host 'OFFLINE-CONSENT-CONFLICT：-Offline 与 -TimeoutConsent 不能同时给（-TimeoutConsent 会让 timeout.ps1 去操作真实确认框）。' -ForegroundColor Red
    Exit-TestAll 2
}

# ---------------------------------------------------------------------------
# -Offline：按模式元数据把"只有真实层、又没有可单独跑的离线入口"的套件整套移出计划，逐条给原因。
# 这不是"跑了但报跳过"：它们一个字节都不会被启动。声明了离线层但源码里找不到那个开关的套件
# 仍留在计划里，由运行期判成 NOT RUN(offline-switch-missing)（登记表与源码不一致，判失败）。
# ---------------------------------------------------------------------------
$offlineExcluded = @()
if ($Offline) {
    $offlineExcluded = @($plan | Where-Object { $_.Real -ne 'none' -and -not $_.OfflineLayer })
    $plan = @($plan | Where-Object { $_.Real -eq 'none' -or $_.OfflineLayer })
}
if ($offlineExcluded.Count) {
    Write-Host '  -Offline：以下套件只有真实层、没有可单独跑的离线入口，整套不进计划（OFFLINE-EXCLUDED）：' -ForegroundColor DarkYellow
    foreach ($x in $offlineExcluded) {
        Write-Host ("  - {0} 没有可单独跑的离线层（{1}）" -f $x.Name, $x.Note) -ForegroundColor DarkYellow
    }
}

# 所有筛选都做完之后才验计划非空：-Only 那一步非空，不代表 -Except 之后还非空
# （`-Only cli -Except cli` 筛出来的就是零套件，`-Only save -Offline` 也会被离线排除筛空）。
# 空计划不是一张"全部通过"的成绩单，而是一次根本不成立的调用，按前置没成立报出去，不进汇总、也不给 0。
if (-not $plan) {
    $filters = @()
    if ($Quick) { $filters += '-Quick' }
    if ($Offline) { $filters += '-Offline' }
    if ($Only) { $filters += ("-Only {0}" -f ($Only -join ',')) }
    if ($Except) { $filters += ("-Except {0}" -f ($Except -join ',')) }
    $why = if ($filters) { "生效的筛选：$($filters -join '、')" } else { '没有给任何筛选开关（登记表是空的？）' }
    Write-Host "筛完没有可执行测试，不构成验收：$why（NO-TESTS-PLANNED）" -ForegroundColor Red
    Exit-TestAll 2
}

if ($List) {
    Write-Host "执行计划（$($plan.Count) 套，串行）：" -ForegroundColor Cyan
    $i = 0
    foreach ($s in $plan) { $i++; Write-Host ('  {0,2}. {1,-14} {2}' -f $i, $s.Name, $s.Note) }
    if ($Quick) { Write-Host '  -Quick：不排 build-path.ps1，并给 timeout.ps1 传 -SkipLong' }
    if ($Offline) { Write-Host '  -Offline：按模式元数据选路（纯离线套件照跑；有离线层的带那个开关跑；只有真实层的整套排除并给原因）' }
    if (-not $SimulateConsent) { Write-Host '  未给 -SimulateConsent：需要有人答"是"的判据由套件自己记 SKIP' }
    Exit-TestAll 0
}

# ---------------------------------------------------------------------------
# 前置：确认要跑、构建、验产物能启动
# ---------------------------------------------------------------------------
$log = $LogDir
if (-not $log) { $log = Join-Path $repo ('build\test-logs\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$log = [IO.Path]::GetFullPath($log)

Write-Host ''
Write-Host "EvernightCapture 一键测试：$($plan.Count) 套判据，串行跑（预计十几分钟起步，取决于机器）。" -ForegroundColor Cyan
Write-Host ('  配置：{0}{1}' -f $Config, $(if ($Clean) { '（-Clean）' } else { '' }))
Write-Host ('  日志：{0}' -f $log)
Write-Host ('  每套上限：{0}s（到点结束本次启动的进程树并记 TIMEOUT）' -f $SuiteTimeoutSec)
if (-not $SimulateConsent) {
    Write-Host '  需要有人点"是"的那几项会记 SKIP —— 不是失败，也不算通过。'
    Write-Host '  consent.ps1 / timeout.ps1 的少数判据仍会真的弹确认框，但只代答"否"：答"否"不采任何像素。'
} else {
    Write-Host '  !! -SimulateConsent：测试会替人点"是"，桌面上此刻有什么就被真的拍进图里。' -ForegroundColor Yellow
    Write-Host  '     这只该在专门腾出来、没有隐私内容的桌面上做。' -ForegroundColor Yellow
}

# 代答真实确认框这一步**永不**由 -Force 免掉：Force 只免去下面那句"按回车开始"。
# 拿不到那句 yes（输入被重定向、非交互、无控制台）一律按"没确认"中止 —— 绝不允许把
# "无人应答"当成默认同意。这一步在建任何日志目录、起任何套件之前完成。
if ($SimulateConsent) {
    $answer = $null
    $canAsk = $true
    try { $canAsk = -not [Console]::IsInputRedirected } catch { $canAsk = $true }
    if ($canAsk) {
        try { $answer = Read-Host '  确认这就是可以拍的桌面？输入 yes 继续（其它任意输入中止）' } catch { $answer = $null }
    }
    if ($answer -ne 'yes') {
        Write-Host 'CONSENT-YES-REQUIRED：-SimulateConsent 必须当场拿到 yes；拿不到（含输入重定向/非交互/无控制台）按未确认中止，-Force 不免这一步。' -ForegroundColor Red
        Exit-TestAll 2
    }
}
if (-not $Force -and -not $List) {
    $null = Read-Host '按回车开始（Ctrl+C 退出）'
}

New-Item -ItemType Directory -Force -Path $log | Out-Null

$shell = Resolve-TestShell -Spec $Shell
$childIs7 = Test-ShellIsPS7 -Path $shell
$parentIs7 = [bool]($PSVersionTable.PSVersion.Major -ge 6)
# 混合 shell（父 7 子 5.1，或反过来）会把父进程那份 PSModulePath 原样递给子进程，
# Windows PowerShell 5.1 于是从 PowerShell 7 的目录里找核心模块，Get-FileHash 这类命令直接变成不认识。
# 同 shell 不用管；跨 shell 时先问一次目标 shell 自己那份干净的 PSModulePath，起子进程期间临时换上。
$childModulePath = $null
if ($childIs7 -ne $parentIs7) {
    $childModulePath = Get-CanonicalModulePath -Path $shell
    if (-not $childModulePath) {
        Write-Host "问不出 $shell 自己的 PSModulePath，不敢混着 shell 跑（请直接用同一个 shell 跑本脚本）。" -ForegroundColor Red
        Exit-TestAll 2
    }
    Write-Host ("  子进程 shell：{0}（与父进程不同种，已备好它自己的模块搜索路径）" -f (Split-Path $shell -Leaf)) -ForegroundColor DarkGray
} else {
    Write-Host ("  子进程 shell：{0}（与本脚本同一个）" -f (Split-Path $shell -Leaf)) -ForegroundColor DarkGray
}

$target = $Exe
if (-not $target -and -not $NoBuild) {
    Write-Host "`n=== 前置：构建 ($Config) ===" -ForegroundColor Cyan
    $buildPs = Join-Path $repo 'build.ps1'
    if (-not (Test-Path -LiteralPath $buildPs)) { Write-Host "找不到 $buildPs" -ForegroundColor Red; Exit-TestAll 2 }
    $bArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildPs, '-Config', $Config)
    if ($Clean) { $bArgs += '-Clean' }
    $b = Invoke-Step -FilePath $shell -Arguments $bArgs `
            -OutFile (Join-Path $log '00-build.out.txt') -ErrFile (Join-Path $log '00-build.err.txt') `
            -TimeoutSec 3600 -ChildModulePath $childModulePath
    Show-TextTail -Text $b.Text -Lines 200
    if ($b.TimedOut -or $b.Exit -ne 0) {
        $why = if ($b.TimedOut) { '超时' } else { "退出码 $($b.Exit)" }
        Write-Host "构建没成功（$why），不跑判据。" -ForegroundColor Red
        Exit-TestAll 2
    }
    $target = Join-Path $repo 'build\ecapture.exe'
}
if (-not $target) { $target = Join-Path $repo 'build\ecapture.exe' }
$target = [IO.Path]::GetFullPath($target)
if (-not (Test-Path -LiteralPath $target)) {
    Write-Host "找不到要测的二进制：$target（先跑 .\build.ps1，或用 -Exe 指过去）" -ForegroundColor Red
    Exit-TestAll 2
}

# 只读自检：这份产物连 --version 都跑不起来就别浪费十几分钟（不截图、不弹框、不写文件）
$ver = Invoke-Step -FilePath $target -Arguments @('--version') `
        -OutFile (Join-Path $log '00-version.out.txt') -ErrFile (Join-Path $log '00-version.err.txt') -TimeoutSec 60
if ($ver.Exit -ne 0) {
    $why = if ($null -eq $ver.Exit) { '无退出码' } else { "退出码 $($ver.Exit)" }
    Write-Host "产物跑不了 --version（$why）：$target" -ForegroundColor Red
    Show-TextTail -Text $ver.Text -Lines 10
    Exit-TestAll 2
}
$verLine = (@($ver.Text -split "[`r`n]+" | Where-Object { $_ }) | Select-Object -First 1)
Write-Host ('  被测产物：{0}' -f $target) -ForegroundColor DarkGray
Write-Host ('            {0}' -f $verLine) -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# 串行跑每一套
# ---------------------------------------------------------------------------
$results = @()
$startedAll = Get-Date
$index = 0

# 把 -StopOnFail 之后剩余的套件一次性记为"调用方主动没跑"。缺失/解析失败/参数不符/真失败
# 四条路都走它，所以"给了 -StopOnFail 却在缺失套件之后默默继续"不再可能。
# 点号调用（. $stopRemaining $index）在当前作用域里跑，改的就是脚本级的 $results。
$stopRemaining = {
    param([int]$FromIndex)
    if ($FromIndex -lt $plan.Count) {
        foreach ($p in @($plan[$FromIndex..($plan.Count - 1)])) {
            $results += [pscustomobject]@{ Name = $p.Name; Status = 'NOT RUN'; Reason = 'stopped-on-fail'
                                           Pass = ''; Fail = ''; Skip = ''; Sec = 0; Exit = $null; Log = '' }
        }
    }
}
foreach ($s in $plan) {
    $index++
    $path = Join-Path $repo ("tests\$($s.Name).ps1")
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "`n[$index/$($plan.Count)] $($s.Name).ps1 —— 找不到脚本，记为未运行（这一条会让总退出码判失败）" -ForegroundColor Red
        $results += [pscustomobject]@{ Name = $s.Name; Status = 'NOT RUN'; Reason = 'missing-script'
                                       Pass = ''; Fail = ''; Skip = ''; Sec = 0; Exit = $null; Log = '' }
        # 缺失也要过同一个 StopOnFail 决策：它是"计划内没跑成"，不是调用方主动跳过
        if ($StopOnFail) {
            Write-Host "`n给了 -StopOnFail，$($s.Name).ps1 之后停止；剩下的按'调用方主动没跑'记，不混进意外未运行。" -ForegroundColor Red
            . $stopRemaining $index
            break
        }
        continue
    }

    # 只递该套件自己声明的开关，其它参数一律不塞进去
    $declared = Get-SuiteParameterNames -Path $path
    if ($null -eq $declared) {
        # 读不出参数就不猜：猜着传要么传给不认的套件（当场用法错），要么漏传 -Exe 而测到别的产物
        Write-Host '  读不出这份脚本自己声明的参数（解析不干净），不猜着传参：记为未运行（这一条会让总退出码判失败）' -ForegroundColor Red
        $results += [pscustomobject]@{ Name = $s.Name; Status = 'NOT RUN'; Reason = 'unparsable-parameters'
                                       Pass = ''; Fail = ''; Skip = ''; Sec = 0; Exit = $null; Log = '' }
        if ($StopOnFail) {
            Write-Host "`n给了 -StopOnFail，$($s.Name).ps1 之后停止；剩下的按'调用方主动没跑'记，不混进意外未运行。" -ForegroundColor Red
            . $stopRemaining $index
            break
        }
        continue
    }
    # 收得住哪些开关由这一步决定；套件一个脚本级参数都没有时这里就是空表，照常跑它
    $names = $declared.Names
    $suiteArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $path)
    if ($names -contains 'Exe') { $suiteArgs += @('-Exe', $target) }
    if ($Keep -and $names -contains 'Keep') { $suiteArgs += '-Keep' }

    # 离线层开关按模式元数据递：声明有离线层却找不到那个参数 = 登记表与源码不一致。
    # 记 NOT RUN(offline-switch-missing) 判失败 —— 既不许退化成"没开关也照跑真实层"，
    # 也不许悄悄当成离线排除（那等于把"没验证"写成"不归我管"）。
    if ($Offline) {
        $layer = [string]$s.OfflineLayer
        if ($layer) {
            if ($names -contains $layer) {
                $suiteArgs += ('-' + $layer)
            } else {
                Write-Host "`n[$index/$($plan.Count)] $($s.Name).ps1 —— 登记表声明离线层 -$layer，源码里找不到这个参数，记为未运行（这一条会让总退出码判失败）" -ForegroundColor Red
                $results += [pscustomobject]@{ Name = $s.Name; Status = 'NOT RUN'; Reason = 'offline-switch-missing'
                                               Pass = ''; Fail = ''; Skip = ''; Sec = 0; Exit = $null; Log = '' }
                if ($StopOnFail) {
                    Write-Host "`n给了 -StopOnFail，$($s.Name).ps1 之后停止；剩下的按'调用方主动没跑'记，不混进意外未运行。" -ForegroundColor Red
                    . $stopRemaining $index
                    break
                }
                continue
            }
        }
    }

    if ($SimulateConsent -and $names -contains 'SimulateConsent') { $suiteArgs += '-SimulateConsent' }
    if ($Quick -and $names -contains 'SkipLong') { $suiteArgs += '-SkipLong' }
    if ($TimeoutConsent -and $names -contains 'Consent') { $suiteArgs += '-Consent' }

    Write-Host "`n=== [$index/$($plan.Count)] $($s.Name).ps1 ===" -ForegroundColor Cyan
    $outFile = Join-Path $log ('{0:d2}-{1}.out.txt' -f $index, $s.Name)
    $errFile = Join-Path $log ('{0:d2}-{1}.err.txt' -f $index, $s.Name)
    $r = Invoke-Step -FilePath $shell -Arguments $suiteArgs -OutFile $outFile -ErrFile $errFile `
            -TimeoutSec $SuiteTimeoutSec -ChildModulePath $childModulePath

    $counts = Get-LogCounts -Text $r.Text
    $status = if ($r.StartError) { 'NO START' }
              elseif ($r.TimedOut) { 'TIMEOUT' }
              elseif ($null -eq $r.Exit) { 'NO EXIT' }
              elseif ($r.Exit -eq 0) { 'PASS' }
              else { 'FAIL' }
    $color = if ($status -eq 'PASS') { 'Green' }
             elseif ($status -eq 'NOT RUN' -or $status -eq 'NO START') { 'Yellow' }
             else { 'Red' }

    Write-Host ('  {0}  用时 {1}s  通过 {2}  失败 {3}  跳过 {4}  退出码 {5}' -f `
        $status, $r.Sec,
        $(if ($counts.Pass) { $counts.Pass } else { '-' }),
        $(if ($counts.Fail) { $counts.Fail } else { '-' }),
        $(if ($counts.Skip) { $counts.Skip } else { '-' }),
        $(if ($null -ne $r.Exit) { $r.Exit } else { '-' })) -ForegroundColor $color

    if ($status -ne 'PASS') {
        Write-Host '  ---- 末尾输出（完整见下面那行日志）----' -ForegroundColor DarkGray
        Show-TextTail -Text $r.Text
        if ($r.StartError) { Write-Host "  没起来：$($r.StartError)" -ForegroundColor Red }
        if ($r.TimedOut) { Write-Host "  收尾方式：$($r.KilledBy)" -ForegroundColor DarkGray }
        if ($r.Truncated) { Write-Host '  输出被调用器截断过，上面那份不是全部' -ForegroundColor DarkYellow }
    }
    Write-Host "  日志：$outFile" -ForegroundColor DarkGray

    $results += [pscustomobject]@{ Name = $s.Name; Status = $status; Pass = $counts.Pass; Fail = $counts.Fail
                                   Skip = $counts.Skip; Sec = $r.Sec; Exit = $r.Exit; Log = $outFile }

    if ($StopOnFail -and $status -ne 'PASS') {
        Write-Host "`n给了 -StopOnFail，$($s.Name).ps1 之后停止；剩下的按'调用方主动没跑'记，不混进意外未运行。" -ForegroundColor Red
        . $stopRemaining $index
        break
    }
}

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
$totalSec = [int]((Get-Date) - $startedAll).TotalSeconds
# "没跑成"分三类，去向各不相同，不许混成一句"未运行"：
#   * notPlanned    —— 调用方用 -Quick / -Only / -Except 刻意没排的，本来就不该跑，不进判据；
#   * stoppedOnFail —— 给了 -StopOnFail，撞到第一个没通过的套件就主动停下的剩余，同样是调用方的决定；
#   * unexpectedNotRun —— 计划里点名要跑、这一步却整套没执行起来（脚本文件不见了、参数解析不干净）。
#     这一类不是"结果不理想"，而是"这次验收根本没发生"：机器调用方只看退出码时，
#     把它当成 0 就等于替一套从没跑过的判据发合格证明，所以它和 FAIL 一样判 1。
# 各套件自己记的 SKIP（下面那个 $sumSkip）是环境与安全边界，照旧既不升成失败、也不算通过。
$notRun = @($results | Where-Object { $_.Status -eq 'NOT RUN' })
$stoppedOnFail = @($notRun | Where-Object { $_.Reason -eq 'stopped-on-fail' })
$unexpectedNotRun = @($notRun | Where-Object { $_.Reason -ne 'stopped-on-fail' })
$bad = @($results | Where-Object { $_.Status -eq 'FAIL' -or $_.Status -eq 'TIMEOUT' -or
                                    $_.Status -eq 'NO EXIT' -or $_.Status -eq 'NO START' })
# "交出结果"只算真正跑起来、又给出了终态的：NOT RUN 是压根没启动，
# NO START / NO EXIT 是起来了但没交回任何可判的结局 —— 三者都不算。
$delivered = @($results | Where-Object { $_.Status -ne 'NOT RUN' -and $_.Status -ne 'NO START' -and
                                            $_.Status -ne 'NO EXIT' })
$noResult = @($results | Where-Object { $_.Status -eq 'NO START' -or $_.Status -eq 'NO EXIT' })
$sumPass = 0; $sumFail = 0; $sumSkip = 0
foreach ($x in $results) {
    if ($x.Pass) { $sumPass += [int]$x.Pass }
    if ($x.Fail) { $sumFail += [int]$x.Fail }
    if ($x.Skip) { $sumSkip += [int]$x.Skip }
}
$notPlanned = @($suites | Where-Object { $plan.Name -notcontains $_.Name })

Write-Host ''
Write-Host '================ 汇总 ================' -ForegroundColor Cyan
Write-Host ('{0,-13} {1,-9} {2,6} {3,6} {4,6} {5,7} {6,8}' -f 'suite', 'result', 'pass', 'fail', 'skip', 'sec', 'exit')
foreach ($x in $results) {
    # 未运行必须带原因：只看状态分不出"调用方没排它"和"它根本没能被启动"
    $shown = $x.Status
    if ($x.Status -eq 'NOT RUN' -and $x.Reason) { $shown = ('{0}({1})' -f $x.Status, $x.Reason) }
    Write-Host ('{0,-13} {1,-9} {2,6} {3,6} {4,6} {5,7} {6,8}' -f `
        $x.Name, $shown,
        $(if ($x.Pass) { $x.Pass } else { '-' }),
        $(if ($x.Fail) { $x.Fail } else { '-' }),
        $(if ($x.Skip) { $x.Skip } else { '-' }),
        $x.Sec,
        $(if ($null -ne $x.Exit) { $x.Exit } else { '-' }))
}
# 意外没跑成的与真失败的同等严重：都说明这份成绩单不完整
$incomplete = @($bad) + @($unexpectedNotRun)
$head = if ($incomplete.Count) { 'Red' } else { 'Green' }
Write-Host ''
Write-Host ('跑到并交出结果的套件 {0} 套：判据通过 {1} 项、失败 {2} 项、未验证 {3} 项；' -f `
    $delivered.Count, $sumPass, $sumFail, $sumSkip) -ForegroundColor $head
if ($noResult.Count) {
    Write-Host ('  起来了但没交出结果的套件：{0}（NO START / NO EXIT，不计入上面那一行）' -f ($noResult.Name -join ', ')) -ForegroundColor Red
}
Write-Host ('计划 {0} 套里：意外没跑成 {1} 套，因 -StopOnFail 主动没跑 {2} 套；总用时 {3}s。' -f `
    $plan.Count, $unexpectedNotRun.Count, $stoppedOnFail.Count, $totalSec) -ForegroundColor $head
if ($notPlanned.Count) {
    Write-Host ('  按开关没排的套件：{0}（是 -Quick / -Only / -Except 筛掉的，不是跑过）' -f ($notPlanned.Name -join ', ')) -ForegroundColor DarkYellow
}
if ($unexpectedNotRun.Count) {
    Write-Host ('  意外没跑成的套件：{0}（计划里要跑，整套没执行起来 —— 判失败）' -f ($unexpectedNotRun.Name -join ', ')) -ForegroundColor Red
}
if ($stoppedOnFail.Count) {
    Write-Host ('  因 -StopOnFail 没跑到的剩余：{0}' -f ($stoppedOnFail.Name -join ', ')) -ForegroundColor DarkYellow
}
if ($sumSkip -gt 0) {
    Write-Host '  未验证的那些是环境与安全边界（没有 HDR 屏、只接了一块屏、没给 -SimulateConsent 等），' -ForegroundColor DarkYellow
    Write-Host '  既不算失败也不算通过；要补上它们请照各套件自己的说明显式加开关、并换到合适的环境。' -ForegroundColor DarkYellow
}
if (-not $incomplete.Count) { Write-Host '  计划内的套件全部跑到并通过（SKIP 仍按未验证列出）。' -ForegroundColor Green }
Write-Host ('  日志目录：{0}' -f $log) -ForegroundColor DarkGray

Exit-TestAll $(if ($incomplete.Count) { 1 } else { 0 })
