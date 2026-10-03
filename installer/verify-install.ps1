<#
.SYNOPSIS
    EvernightCapture 安装版只读自检：核对随包文件与哈希、文档相对引用、二进制身份与只读查询，再按声明跑离线测试。
.DESCRIPTION
    这是**安装目录里**的自检，不依赖源码仓库、Visual Studio 或网络：

      1. 载荷完整性：逐条核对 install-manifest.json 里列出的文件是否存在、大小与 SHA-256 是否一致。
      2. 文档相对引用：四份 README 与 SKILL.md 里的相对链接/图片都指向安装目录里真实存在的文件
         （安装目录里的文档不能布满失效链接）。
      3. 二进制身份：清单必须自己带齐身份字段（version / arch / buildId / config），缺关键身份就直接判失败，
         不许"跳过断言后报通过"；再拿 ECAPTURE.EXE 的三条只读查询（--version / --capabilities /
         --diagnostics）与清单互相核对，并把 buildId 里的时间戳与这份 EXE 自己 PE 头里的链接时间戳、
         Machine 字段对上 —— 同版本号不等于同一份构建，所以看 buildId 与 PE 头，不只看版本号。
      4. 只读契约：--help 退出码为 3（这是约定，不是失败）；只读查询不落地文件、不弹框。
         这一节以及身份核对都跑在**不受用户偏好影响**的条件下：直接发起的查询一律显式带 --no-config，
         共享身份探测跑在字节相同副本 + 没有任何配置的空目录里（见下面第 4b 节的说明）。用户的
         ECAPTURE.ini 写得再飘，也不该把安装自检变成"看起来安装坏了"。
      4b. 配置的两件事分开报：
         * 模板（受管载荷）：清单里声明的配置模板必须在安装目录里、字节与发行清单一致 —— 这是安装完整性。
         * 实际用户配置（ECAPTURE.ini）：只报它的**加载/解析状态**，不判安装完整性。缺失是正常状态
           （内置默认值生效）；损坏或读不出来是一份清晰的使用错误（config.* 错误码 + 指向那个文件），
           这里如实说"这是配置的使用问题，不是安装文件损坏"，绝不因此把安装判成失败，也不修它。
         配置专用检查用**本次自建的临时目录里的一份临时 INI** 通过 --config 跑：读得到、--show-config
         报对来源（config / cli 覆盖 / builtin）、--no-config 明确不读、--history off 真的生效、
         点名不存在的配置退出码 8、损坏的配置退出码 1 且文件字节不变。全程不碰安装目录里的配置。
         再证一条：--show-config 这类只读查询跑完，安装目录里既没有新增/改动任何文件，也没有多出
         输出图片或 history\ 归档目录（前后各取一次整目录指纹来比，不靠"应该没写"这种说法）。
      5. 离线计划的可运行性：清单声明"安装后可跑"的每一套，脚本、依赖测试程序与辅助文件都必须真的在
         安装目录里；少一件就如实判失败，不会等到套件自己去"补编译一次"。
      6. 可选离线测试（-RunOfflineTests）：用随包的 test-all.ps1 跑本安装目录里的离线套件，
         指向本目录的 ECAPTURE.EXE，绝不误用 PATH 上的旧版。日志写到系统临时目录，不污染安装目录。

    进程调用一律复用随包的 tests\harness.psm1 调用器（Invoke-EcProcess）：两条标准流并发消费、
    从进程启动那一刻起就在期限内等待、超时只结束本次启动的进程树并保留部分输出。这里不再抄第二套
    生命周期逻辑，也不再用"先 ReadToEnd 再判超时"那种挂住就拿不到日志的写法。

    全程只读：不修改安装目录里的任何文件（包括用户的 ECAPTURE.ini 与 history\），不写注册表，不装依赖，
    不启动真实截图，也不替任何人确认桌面授权。身份核对与配置检查用的临时副本/临时 INI 都建在系统临时
    目录里本次独占的目录中，结束只清理自己那一个。
    默认不跑真实窗口/HDR/多屏/确认框测试 —— 那些要由用户在合适的环境里亲自安排。

    退出码：0 = 全部通过（SKIP 是环境边界，不算失败也不算通过）；1 = 有 FAIL；2 = 前置不成立（路径/清单缺失、参数非法）。
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File verify-install.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File verify-install.ps1 -RunOfflineTests
    powershell -NoProfile -ExecutionPolicy Bypass -File verify-install.ps1 -InstallDir "D:\我的 工具\ecapture"
    powershell -NoProfile -ExecutionPolicy Bypass -File verify-install.ps1 -RunOfflineTests -OverallTimeoutSec 5400 -KeepLogs
#>
param(
    [string]$InstallDir,
    [switch]$RunOfflineTests,
    [int]$SuiteTimeoutSec = 900,
    [int]$OverallTimeoutSec = 0,
    [switch]$KeepLogs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

# 预算参数在起任何进程之前先验：-SuiteTimeoutSec 是**逐套**预算，非法值不该跑完才发现。
if ($SuiteTimeoutSec -lt 1 -or $SuiteTimeoutSec -gt 86400) {
    Write-Host '-SuiteTimeoutSec 必须在 1..86400 秒之间（它是每套的预算，不是总预算）' -ForegroundColor Red
    exit 2
}
if ($OverallTimeoutSec -lt 0 -or $OverallTimeoutSec -gt 864000) {
    Write-Host '-OverallTimeoutSec 必须在 0..864000 秒之间（0 = 按套件数自动算）' -ForegroundColor Red
    exit 2
}

# ---------------------------------------------------------------------------
# 解析安装目录：$PSScriptRoot 在 .ps1 里一定有值；被交互式 dot-source 时退回脚本自身路径；
# 两者都拿不到就要求显式 -InstallDir。绝不假设当前工作目录，也不写死任何开发者路径。
# ---------------------------------------------------------------------------
if (-not $InstallDir) {
    if ($PSScriptRoot) { $InstallDir = $PSScriptRoot }
    elseif ($MyInvocation.MyCommand.Path) { $InstallDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
    else { Write-Host '无法确定安装目录：请显式给 -InstallDir <安装目录>' -ForegroundColor Red; exit 2 }
}
if (-not (Test-Path -LiteralPath $InstallDir)) {
    Write-Host "安装目录不存在：$InstallDir" -ForegroundColor Red
    exit 2
}
$InstallDir = (Get-Item -LiteralPath $InstallDir).FullName
$exe       = Join-Path $InstallDir 'ECAPTURE.EXE'
$manifest  = Join-Path $InstallDir 'install-manifest.json'
$harness   = Join-Path $InstallDir 'tests\harness.psm1'
$testAll   = Join-Path $InstallDir 'test-all.ps1'
$ownerIni  = Join-Path $InstallDir 'evernightcapture.owner.ini'

# 整目录指纹：只读查询跑前后各取一次，用来**证明**这些查询没在安装目录里落任何文件
# （新增/改动都逃不过这张表；目录条目也记，空的 history\ 一样看得出来）。
function Get-EcDirFingerprint {
    param([Parameter(Mandatory)][string]$Dir)
    $items = @(Get-ChildItem -LiteralPath $Dir -Recurse -Force -ErrorAction SilentlyContinue |
               ForEach-Object {
                   if ($_.PSIsContainer) { 'D|' + $_.FullName }
                   else { 'F|' + $_.FullName + '|' + $_.Length + '|' + $_.LastWriteTimeUtc.Ticks }
               } | Sort-Object)
    return ($items -join "`n")
}

# 叶子名比较：Windows 上 ECAPTURE.INI / ECAPTURE.ini 是同一个文件，判"是不是那一份用户配置"
# 不能按大小写敏感比。
function Test-EcLeafNameIs {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Leaf)
    $native = $Path -replace '/', '\'
    return [bool](((($native.TrimEnd('\')).Split('\') | Select-Object -Last 1)) -ieq $Leaf)
}

# 配置类查询：结果 JSON 正常走 stdout，配置失败时走 stderr（图像字节那一路才独占 stdout）。
# 这里两条流都拿、先按 stdout 解，解不动再按 stderr 解，并保留退出码 —— 不靠"应该在哪条流"猜。
function Invoke-EcConfigQuery {
    param(
        [Parameter(Mandatory)][string]$ExePath,
        [Parameter(Mandatory)][string[]]$Arguments,
        [int]$TimeoutMs = 60000
    )
    $r = Invoke-EcProcess -FilePath $ExePath -Arguments $Arguments -TimeoutMs $TimeoutMs
    $json = $null; $from = $null; $err = $null
    foreach ($side in @('Stdout', 'Stderr')) {
        $text = if ($side -eq 'Stdout') { $r.Stdout } else { $r.Stderr }
        if (-not $text) { continue }
        try { $json = $text | ConvertFrom-Json; $from = $side; break } catch { $err = $_.Exception.Message }
    }
    return [pscustomobject]@{ Result = $r; Exit = $r.Exit; Json = $json; Stream = $from;
                              TimedOut = [bool]$r.TimedOut; StartError = $r.StartError }
}

Write-Host 'EvernightCapture 安装版只读自检' -ForegroundColor Cyan
Write-Host ("  安装目录：{0}" -f $InstallDir)

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "找不到 ECAPTURE.EXE：$exe" -ForegroundColor Red
    exit 2
}
if (-not (Test-Path -LiteralPath $manifest)) {
    Write-Host "找不到 install-manifest.json：$manifest（这不是本产品的安装目录？）" -ForegroundColor Red
    exit 2
}
if (-not (Test-Path -LiteralPath $harness)) {
    Write-Host "找不到 tests\harness.psm1：$harness" -ForegroundColor Red
    exit 2
}

$mf = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json

Import-Module $harness -Force -DisableNameChecking
Reset-EcSuite
Initialize-EcHarness -Exe $exe | Out-Null

# ---------------------------------------------------------------------------
# 1) 载荷完整性
# ---------------------------------------------------------------------------
$files = @($mf.files)
Assert-Ec ($files.Count -gt 0) "install-manifest.json 列出了随包文件（$($files.Count) 个）"
$missing = @(); $sizeBad = @(); $hashBad = @()
foreach ($f in $files) {
    $p = Join-Path $InstallDir $f.path
    if (-not (Test-Path -LiteralPath $p)) { $missing += $f.path; continue }
    if ($null -ne $f.size -and (Get-Item -LiteralPath $p).Length -ne [long]$f.size) { $sizeBad += $f.path }
    if ($f.sha256) {
        $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($h -ne ([string]$f.sha256).ToLowerInvariant()) { $hashBad += $f.path }
    }
}
Assert-Ec ($missing.Count -eq 0) "清单里的文件都在安装目录里$(if ($missing.Count) { "；缺：" + ($missing -join ', ') })"
Assert-Ec ($sizeBad.Count -eq 0) "文件大小与清单一致$(if ($sizeBad.Count) { "；不符：" + ($sizeBad -join ', ') })"
Assert-Ec ($hashBad.Count -eq 0) "文件 SHA-256 与清单一致$(if ($hashBad.Count) { "；不符：" + ($hashBad -join ', ') })"

# ---------------------------------------------------------------------------
# 1b) 配置的两份文件是两种身份：模板 = 受管载荷（安装完整性）；ECAPTURE.ini = 用户数据（不是载荷）
#     文件名只取自随包 install-manifest.json 的 configPayload（与载荷清单同一来源，本脚本不硬写第二份）。
#     用户配置的**加载/解析状态**在第 3c 节单独报，它坏了属于使用问题，不属于这里。
# ---------------------------------------------------------------------------
$cfgTpl = [string]$mf.configPayload.templateFile
$cfgUser = [string]$mf.configPayload.userConfigFile
Assert-Ec ($cfgTpl -and $cfgUser) "清单必须声明配置模板名与用户配置名（configPayload.templateFile / userConfigFile）"
Assert-Ec ($cfgTpl -ine $cfgUser) "配置模板与实际用户配置不能是同一个文件名（模板 $cfgTpl / 用户 $cfgUser）"

$tplPath = Join-Path $InstallDir $cfgTpl
$userCfgPath = Join-Path $InstallDir $cfgUser
# 模板是受管载荷：装不出来、或与发行清单对不上，就是安装完整性问题，如实判失败。
Assert-Ec (Test-Path -LiteralPath $tplPath) "配置模板 $cfgTpl 必须在安装目录里、与 ECAPTURE.EXE 同目录（它是受管载荷）"
if (Test-Path -LiteralPath $tplPath) {
    $tplFileEntry = @($files | Where-Object { Test-EcLeafNameIs -Path ([string]$_.path) -Leaf $cfgTpl })
    Assert-Ec ($tplFileEntry.Count -eq 1) "发行清单里应恰好有一条 $cfgTpl 的记录（实际 $($tplFileEntry.Count) 条）" -Quiet
    $tplHashNow = (Get-FileHash -LiteralPath $tplPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($mf.configPayload.templateSha256) {
        Assert-Ec ($tplHashNow -eq ([string]$mf.configPayload.templateSha256).ToLowerInvariant()) `
            "配置模板 $cfgTpl 的字节与打包时记录的模板哈希不一致（模板内容变了就必须跟上，不能留旧的）"
    }
    $tplText = Get-Content -LiteralPath $tplPath -Raw -Encoding UTF8
    Assert-Ec ($tplText -and ($tplText -match '(?m)^\s*\[config\]\s*$')) "配置模板 $cfgTpl 不是一份配置模板（读不出内容或没有 [config] 段）"
    Write-Host ("  配置模板：{0}（受管载荷，升级会覆盖成当前版本）" -f $tplPath) -ForegroundColor DarkGray
}
# 反过来的那一条也要判：用户配置一旦被打进发行清单，就等于把某个人的路径与偏好当产品文件分发，
# 也让升级/卸载有理由去覆盖或删除它。
$userInManifest = @($files | Where-Object { Test-EcLeafNameIs -Path ([string]$_.path) -Leaf $cfgUser })
Assert-Ec ($userInManifest.Count -eq 0) "实际用户配置 $cfgUser 不该出现在发行清单里（它是用户数据，不是产品文件）"
$hashListPath = Join-Path $InstallDir 'payload.sha256.txt'
if (Test-Path -LiteralPath $hashListPath) {
    $userInHashList = @(Get-Content -LiteralPath $hashListPath -Encoding UTF8 | Where-Object {
        $p = ($_ -split '\s+', 2)[-1]; Test-EcLeafNameIs -Path $p -Leaf $cfgUser
    })
    Assert-Ec ($userInHashList.Count -eq 0) "实际用户配置 $cfgUser 不该被列进 payload.sha256.txt"
}
# 归属标记的受管记录里也不许有它（有记录就意味着升级会拿它做覆盖前保护、卸载会认它是产品文件）。
if (Test-Path -LiteralPath $ownerIni) {
    $ownerUserCfgRecords = @(Get-Content -LiteralPath $ownerIni -Encoding UTF8 | Where-Object {
        $_ -match '^[^|]+\|' -and (Test-EcLeafNameIs -Path (($_ -split '\|')[0]) -Leaf $cfgUser)
    })
    Assert-Ec ($ownerUserCfgRecords.Count -eq 0) "归属标记把实际用户配置 $cfgUser 记成了受管文件"
}
# 用户配置在不在都正常：缺失 = 内置默认值生效（程序不会替你写一份）；存在 = 用户自己的东西。
# 这里只报告它的存在性，它的**能不能解析**由第 3c 节按"使用问题"这一档来报。
if (Test-Path -LiteralPath $userCfgPath) {
    Write-Host ("  实际用户配置：{0}（存在；本次自检只读它，绝不改写它）" -f $userCfgPath) -ForegroundColor DarkGray
} else {
    Write-Host ("  实际用户配置：不存在（{0}）——正常状态，内置默认值生效" -f $userCfgPath) -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 2) 文档相对引用（README 的 logo 等必须随包发出）
# ---------------------------------------------------------------------------
$docFiles = @('README.md', 'README.zh-CN.md', 'README.zh-TW.md', 'README.ja-JP.md', 'SKILL.md')
$broken = @()
foreach ($doc in $docFiles) {
    $dp = Join-Path $InstallDir $doc
    if (-not (Test-Path -LiteralPath $dp)) { $broken += "$doc（文档缺失）"; continue }
    $text = Get-Content -LiteralPath $dp -Raw -Encoding UTF8
    $matches = [regex]::Matches($text, '!?\[[^\]]*\]\(([^)]+)\)')
    foreach ($m in $matches) {
        $target = $m.Groups[1].Value.Trim()
        if ($target -match '^[a-zA-Z][a-zA-Z0-9+.-]*:') { continue }   # http(s): / mailto: 等外部链接
        if ($target.StartsWith('#')) { continue }                       # 文档内锚点
        $pathOnly = ($target -split '#')[0]
        if (-not $pathOnly) { continue }
        $rel = $pathOnly -replace '/', '\'
        if (-not (Test-Path -LiteralPath (Join-Path $InstallDir $rel))) { $broken += "$doc -> $target" }
    }
}
Assert-Ec ($broken.Count -eq 0) "文档里的相对引用都指向安装目录里存在的文件$(if ($broken.Count) { "；失效：" + ($broken -join '; ') })"
Assert-Ec (Test-Path -LiteralPath (Join-Path $InstallDir 'resources\icon.ico')) "README 引用的 resources\icon.ico 随包发出"
Assert-Ec (Test-Path -LiteralPath (Join-Path $InstallDir 'LICENSE')) "LICENSE 原文随包发出"

# ---------------------------------------------------------------------------
# 3) 清单里的身份字段必须齐备，再由三条只读查询互相核对
# ---------------------------------------------------------------------------
Assert-Ec (Test-Path -LiteralPath (Join-Path $InstallDir ([string]$mf.skill.entryFile))) `
    "Skill 入口 SKILL.md 就在安装目录根（所选目录就是 Skill 根）"

foreach ($key in @('version', 'arch', 'buildId', 'config')) {
    Assert-Ec ([bool]$mf.$key) "清单带有身份字段 $key（缺关键身份不能算通过）"
}
$declaredVersion = [string]$mf.version
$declaredArch    = [string]$mf.arch
$declaredBuildId = [string]$mf.buildId
if ($mf.provenance) {
    Assert-Ec (@('rebuilt', 'reused') -contains [string]$mf.provenance.source) `
        "清单说明这批产物是本次重建还是显式复用（实际值：$([string]$mf.provenance.source)）"
    Write-Host ("  来源：{0}；配置核对：{1}；git：{2}{3}" -f `
                [string]$mf.provenance.source, [string]$mf.provenance.configVerified,
                [string]$mf.provenance.gitCommit, $(if ($mf.provenance.gitDirty) { '（工作树有未提交改动）' } else { '' })) -ForegroundColor DarkGray
}

$identityMs = 60000
# 身份核对要跑三条只读查询（--version / --capabilities / --diagnostics），而 --capabilities 与
# --diagnostics 会读 ECAPTURE.EXE 旁边那份 ECAPTURE.ini：用户把配置写坏了（退出码 1/8、config.* 错误），
# 这三条查询就全成了"失败"，报出来的却是"安装的文件有问题"——那是假证据。
# 共享身份探测（tests\harness.psm1 的 Test-EcArtifactIdentity）不接额外参数，所以这里在系统临时目录里
# 本次独占的空目录中跑它：目录里除了这份**逐字节相同**的副本什么都没有，也就没有任何配置可读。
# "字节相同"先由 SHA-256 证出来，所以身份结论仍然绑在安装目录里那一份产物上。
$probeRoot = Join-Path ([IO.Path]::GetTempPath()) ('ecapture-verify-probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
New-Item -ItemType Directory -Force -Path $probeRoot | Out-Null
$probeExe = Join-Path $probeRoot 'ECAPTURE.EXE'
Copy-Item -LiteralPath $exe -Destination $probeExe -Force
$installedHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
$probeHash = (Get-FileHash -LiteralPath $probeExe -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-Ec ($probeHash -eq $installedHash) '身份核对用的临时副本与安装目录里的 ECAPTURE.EXE 字节不一致（这一节的结论不能代表装出来的那份）'
# 只读查询跑前先取一次安装目录指纹，第 3c 节末尾再取一次比对：证明这些查询没有在安装目录里落东西。
$fpBefore = Get-EcDirFingerprint -Dir $InstallDir
$histBefore = Test-Path -LiteralPath (Join-Path $InstallDir 'history')
$id = Test-EcArtifactIdentity -Exe $probeExe -ExpectedVersion $declaredVersion -ExpectedArch $declaredArch -TimeoutMs $identityMs
foreach ($p in @($id.Problems)) { Assert-Ec $false "产物身份核对：$p" }
if (-not @($id.Problems).Count) {
    Assert-Ec $true ("产物身份三条查询一致且与 PE 头自洽：version={0} arch={1} buildId={2}（PE Machine=0x{3:X4}，链接时间戳=0x{4:X8}）" -f `
                    $declaredVersion, $declaredArch, $id.BuildId, $id.Pe.Machine, $id.Pe.TimeDateStamp)
}
if ($declaredBuildId) {
    Assert-Ec ([string]$id.BuildId -eq $declaredBuildId) "查询到的 buildId 与清单声明的一致（清单 $declaredBuildId，实际 $([string]$id.BuildId)）"
}
if ($mf.provenance -and $mf.provenance.exeSha256) {
    $nowHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert-Ec ($nowHash -eq ([string]$mf.provenance.exeSha256).ToLowerInvariant()) `
        "provenance 里的 EXE SHA-256 与安装目录里这份 ECAPTURE.EXE 对得上（身份核对绑到具体字节）"
}

# 基础 CLI / 能力 / 契约类查询一律显式带 --no-config：用户的偏好不能把安装自检搞得飘忽
# （--capabilities 与 --diagnostics 都会读 exe 旁边那份 ECAPTURE.ini，读不出来就直接退出码 1）。
# 这一节判的是"装出来的程序自己说不说得清"，配置那一档另有第 3c 节专门判。
$diag = Invoke-EcReadOnlyQuery -Exe $exe -Arguments @('--diagnostics', '--lang', 'en', '--no-config') -TimeoutMs $identityMs
Assert-Ec ($diag.Exit -eq 0 -and $null -ne $diag.Json) "--diagnostics --no-config 正常退出且输出是合法 JSON（退出码 $($diag.Exit)）"

$help = Invoke-EcReadOnlyQuery -Exe $exe -Arguments @('--help', '--lang', 'en', '--no-config') -TimeoutMs $identityMs -ExpectExit 3
Assert-Ec ($help.Exit -eq 3) "--help 退出码为 3（约定值，不是失败；实际 $($help.Exit)）"

$caps = Invoke-EcReadOnlyQuery -Exe $exe -Arguments @('--capabilities', '--lang', 'en', '--no-config') -TimeoutMs $identityMs
Assert-Ec ($caps.Exit -eq 0 -and $null -ne $caps.Json) "--capabilities --no-config 正常退出且输出是合法 JSON（退出码 $($caps.Exit)）"
if ($caps.Exit -eq 0 -and $caps.Json) {
    # 历史这一档是配置功能里唯一会改变落盘行为的字段：能力文档必须把"默认开、可由哪一条改"说清楚，
    # 并且如实声明它不报本次运行的归档结果（history_run_state_not_reported）。
    $h = $caps.Json.history
    Assert-Ec ($null -ne $h) '--capabilities 里有 history 段（历史是可用的落盘行为，必须报）'
    foreach ($hk in @('supported', 'enabledByDefault', 'configKey', 'configOverride')) {
        Assert-Ec ($null -ne $h.$hk) "--capabilities 的 history 段带有 $hk 字段"
    }
    Assert-Ec ([bool]$h.supported) 'history.supported 应为 true'
    Assert-Ec ([bool]$h.enabledByDefault) 'history.enabledByDefault 应为 true（默认就归档，除非用户关掉）'
    Assert-Ec (([string]$h.configKey) -eq 'history.enabled') "history.configKey 应为 history.enabled（实际 $([string]$h.configKey)）"
    Assert-Ec (([string]$h.configOverride) -match '--history') "history.configOverride 要指名 CLI 那一条（实际 $([string]$h.configOverride)）"
    Assert-Ec (@($caps.Json.caveats) -contains 'history_run_state_not_reported') 'caveats 里必须有 history_run_state_not_reported（能力文档不报本次归档结果）'
}

# 安装器写下的归属标记：存在就必须说清这是本产品装的；不存在只如实报告，不当成失败
# （手工复制出来的目录没有标记，但文件与身份照样可以核对）。
# 这一节与上面的载荷哈希是两份**不同**的证据：哈希全绿不能替归属字段免检，反过来也一样。
# 期望值全部取自随包 install-manifest.json 的同源字段（product / appId / 标记版本），
# 本脚本不藏第二份产品名或 AppId 的定义 —— 免得安装器、自检与判据各抄一份慢慢漂开。
# 旧写法 @($ownerLines -contains '...').Count -gt 0 是恒真的：-contains 交回的是布尔，
# @($false) 也是一个元素、.Count 恒 1。判据本体在随包 tests\harness.psm1 的 Test-EcOwnershipMarker：
# 先把 key=value 头与 --files-- 记录段解析成字段表，再拿字段值与期望比、并核对记录形状。
if (Test-Path -LiteralPath $ownerIni) {
    foreach ($key in @('product', 'markerVersion', 'markerAcceptedVersions')) {
        Assert-Ec ($null -ne $mf.$key) "清单带有 $key（核对归属标记要用它；缺关键身份不能算通过）"
    }
    $ownerLines = @((Get-Content -LiteralPath $ownerIni -Encoding UTF8) | ForEach-Object { $_.Trim() })
    $wantAppId = ''
    if ($mf.appId) {
        $wantAppId = (([string]$mf.appId) -replace '^\{\{', '{') -replace '\}\}$', '}'
    }
    $ownerCheck = Test-EcOwnershipMarker -Lines $ownerLines -ExpectedProduct ([string]$mf.product) `
        -ExpectedAppId $wantAppId `
        -AcceptedMarkerVersions @($mf.markerAcceptedVersions | ForEach-Object { [string]$_ }) `
        -HashRequiredVersion ([string]$mf.markerVersion)
    foreach ($p in @($ownerCheck.Problems)) { Assert-Ec $false "归属标记归属核对：$p" }
    if ($ownerCheck.Accepted) {
        Assert-Ec $true ('归属标记与清单一致（product / appid / markerversion，受管文件记录 {0} 条、带内容 SHA-256 的 {1} 条）' -f `
                        $ownerCheck.FileCount, $ownerCheck.HashedRecordCount)
    }
} else {
    Skip-Ec '安装器归属标记' ("没有 {0}：多半不是安装器铺的目录，而是手工复制出来的或开发目录" -f $ownerIni)
}

# ---------------------------------------------------------------------------
# 3c) 配置专用检查：临时目录里自建一份 INI，用 --config 跑；安装目录里那份只读、绝不改
# ---------------------------------------------------------------------------
$cfgDir = Join-Path $probeRoot 'config'
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
# 这份临时 INI 故意不叫用户配置那个名字，也故意不在安装目录里：判的是"程序按 --config 点名的
# 那一份文件工作"，而不是"改一下用户的设置看看会怎样"。
$probeIni = Join-Path $cfgDir ('probe-' + $PID + '.ini')
Set-Content -LiteralPath $probeIni -Value "[config]`r`nversion = 1`r`n`r`n[history]`r`nenabled = false`r`n" -Encoding UTF8

$sv = Invoke-EcConfigQuery -ExePath $exe -Arguments @('--config', $probeIni, '--show-config', '--lang', 'en') -TimeoutMs $identityMs
Assert-Ec ($sv.Exit -eq 0 -and $sv.Json) "临时配置能被 --config 读到（退出码 $($sv.Exit)，JSON 取自 $($sv.Stream)）"
if ($sv.Json) {
    Assert-Ec (([string]$sv.Json.contract) -eq 'configview') "--show-config 的 contract 应为 configview（实际 $([string]$sv.Json.contract)）"
    Assert-Ec (([string]$sv.Json.config.selection) -eq 'explicit') "--config 点名时 selection 应为 explicit（实际 $([string]$sv.Json.config.selection)）"
    Assert-Ec (([string]$sv.Json.config.state) -eq 'loaded') "--config 读到文件时 state 应为 loaded（实际 $([string]$sv.Json.config.state)）"
    Assert-Ec ([bool]$sv.Json.config.loaded) 'config.loaded 应为 true'
    Assert-Ec (([string]$sv.Json.config.file) -ieq $probeIni) "config.file 应指向本次点名那份（实际 $([string]$sv.Json.config.file)）"
    $setList = @($sv.Json.settings)
    Assert-Ec ($setList.Count -gt 0) '--show-config 必须列出每一项生效偏好'
    $badSrc = @($setList | Where-Object { @('cli', 'config', 'builtin') -notcontains [string]$_.source })
    Assert-Ec ($badSrc.Count -eq 0) "来源字段只认 cli/config/builtin$(@($badSrc | ForEach-Object { '；' + $_.key + '=' + $_.source }))"
    $hist = @($setList | Where-Object { [string]$_.key -eq 'history.enabled' })
    Assert-Ec ($hist.Count -eq 1) '--show-config 里要有一条 history.enabled'
    if ($hist.Count -eq 1) {
        Assert-Ec (([string]$hist[0].value) -eq 'false') "临时配置里的 history.enabled=false 应被读到（实际 $([string]$hist[0].value)）"
        Assert-Ec (([string]$hist[0].source) -eq 'config') "--history 没上命令行时该项来源应为 config（实际 $([string]$hist[0].source)）"
    }
}
# 显式 CLI 覆盖配置：来源必须换成 cli，值也必须跟着换 —— 只报"读到了配置"不等于报对了优先级。
$ov = Invoke-EcConfigQuery -ExePath $exe -Arguments @('--config', $probeIni, '--history', 'on', '--show-config', '--lang', 'en') -TimeoutMs $identityMs
$ovHist = @($(if ($ov.Json) { @($ov.Json.settings | Where-Object { [string]$_.key -eq 'history.enabled' }) }))
Assert-Ec ($ov.Exit -eq 0 -and $ovHist.Count -eq 1) "命令行覆盖那一次应正常读到（退出码 $($ov.Exit)）" -Quiet
if ($ovHist.Count -eq 1) {
    Assert-Ec (([string]$ovHist[0].value) -eq 'true') "命令行 --history on 应压过配置里的 false（实际值 $([string]$ovHist[0].value)）"
    Assert-Ec (([string]$ovHist[0].source) -eq 'cli') "被命令行压过的那一项来源应为 cli（实际 $([string]$ovHist[0].source)）"
}
# --no-config 那一条要真的"完全忽略配置"：selection=none、state=not_read，且不报任何文件路径。
$nc = Invoke-EcConfigQuery -ExePath $exe -Arguments @('--no-config', '--show-config', '--lang', 'en') -TimeoutMs $identityMs
Assert-Ec ($nc.Exit -eq 0 -and $nc.Json) "--no-config --show-config 应正常退出（退出码 $($nc.Exit)）" -Quiet
if ($nc.Json) {
    Assert-Ec (([string]$nc.Json.config.selection) -eq 'none') "--no-config 时 selection 应为 none（实际 $([string]$nc.Json.config.selection)）"
    Assert-Ec (([string]$nc.Json.config.state) -eq 'not_read') "--no-config 时 state 应为 not_read（实际 $([string]$nc.Json.config.state)）"
    Assert-Ec (-not [string]$nc.Json.config.file) "--no-config 时不该报任何配置文件路径"
}
# 点名的配置读不出来：这一类（config.not_found / config.read_failed）退出码 8，且不回退默认那一份。
$gone = Invoke-EcConfigQuery -ExePath $exe -Arguments @('--config', (Join-Path $cfgDir 'no-such-file.ini'), '--show-config', '--lang', 'en') -TimeoutMs $identityMs
Assert-Ec ($gone.Exit -eq 8) "点名不存在的配置应退出码 8（读不出来那一类；实际 $($gone.Exit)）"
$goneCode = $(if ($gone.Json) { [string](@($gone.Json.errors)[0].code) } else { '' })
Assert-Ec ($goneCode -match '^config\.') "错误码应落在 config.* 家族（实际 $goneCode）" -Quiet
# 语法/类型/版本那一类是"说不通"：退出码 1；同时程序绝不把坏文件"顺手修好"。
$badIni = Join-Path $cfgDir ('bad-' + $PID + '.ini')
Set-Content -LiteralPath $badIni -Value "[config]`r`nversion = 99`r`n" -Encoding UTF8
$badBefore = (Get-FileHash -LiteralPath $badIni -Algorithm SHA256).Hash
$bad = Invoke-EcConfigQuery -ExePath $exe -Arguments @('--config', $badIni, '--show-config', '--lang', 'en') -TimeoutMs $identityMs
Assert-Ec ($bad.Exit -eq 1) "不合语法的配置应退出码 1（说不通那一类；实际 $($bad.Exit)）"
$badCode = $(if ($bad.Json) { [string](@($bad.Json.errors)[0].code) } else { '' })
Assert-Ec ($badCode -match '^config\.') "坏配置的错误码也应在 config.* 家族（实际 $badCode）" -Quiet
Assert-Ec ($badCode -and (([string](@($bad.Json.errors)[0].option)) -ieq $badIni)) '诊断必须点名是哪一份文件'
Assert-Ec ((Get-FileHash -LiteralPath $badIni -Algorithm SHA256).Hash -eq $badBefore) '程序不该改写或修复用户的配置文件'

# 安装目录里那份**实际用户配置**：只报加载/解析状态。读不出来或说不通是使用问题（config.* 错误码 +
# 指向那个文件），不是安装文件损坏 —— 这里如实报成使用错误并记 SKIP，绝不因此把安装判成失败，
# 也不去动它。模板完整性（第 1b 节）与这一条是两份不同的证据。
$uc = Invoke-EcConfigQuery -ExePath $exe -Arguments @('--show-config', '--lang', 'en') -TimeoutMs $identityMs
if ($uc.Exit -eq 0 -and $uc.Json -and ([string]$uc.Json.contract) -eq 'configview') {
    $ucState = [string]$uc.Json.config.state
    Assert-Ec (([string]$uc.Json.config.selection) -eq 'default') "默认读取那一条的 selection 应为 default（实际 $([string]$uc.Json.config.selection)）" -Quiet
    if ($ucState -eq 'loaded') {
        Assert-Ec (([string]$uc.Json.config.file) -ieq $userCfgPath) "state=loaded 时报的 file 应是安装目录里那份（实际 $([string]$uc.Json.config.file)）"
        Assert-Ec $true "实际用户配置读取正常（$cfgUser：loaded）"
    } elseif ($ucState -eq 'absent') {
        Assert-Ec $true "实际用户配置不存在（$cfgUser：absent）——正常状态，内置默认值生效，程序也没替你写一份"
    } else {
        Assert-Ec $false "实际用户配置状态异常：state=$ucState（只认 loaded / absent）"
    }
} elseif (@('config.not_found', 'config.read_failed') -contains $(if ($uc.Json) { [string](@($uc.Json.errors)[0].code) } else { '' })) {
    Skip-Ec '实际用户配置可读性' ("安装目录里的 $cfgUser 读不出来（退出码 $($uc.Exit)，错误码 $([string](@($uc.Json.errors)[0].code))）：这是配置的使用问题，不是安装文件损坏。程序不会替你修它 —— 自己检查一下那个文件，或先用 --no-config 跑一次确认与偏好无关。")
} elseif ($uc.Exit -eq 1 -and $uc.Json -and (([string](@($uc.Json.errors)[0].code)) -match '^config\.')) {
    Skip-Ec '实际用户配置可读性' ("安装目录里的 $cfgUser 说不通（错误码 $([string](@($uc.Json.errors)[0].code))，第 $([string](@($uc.Json.errors)[0].line)) 行）：这是配置的使用问题，不是安装文件损坏。按同目录的 $cfgTpl 里的说明改；程序不会覆盖或修复它。")
} else {
    Assert-Ec $false "实际用户配置那一条只读查询交回了没预料到的结果（退出码 $($uc.Exit)，JSON 取自 $($uc.Stream)）：既不是 configview 也不是 config.* 错误，不能当成'配置正常'放过"
}

# 上面这些只读查询（含 --show-config）必须一条文件都不落地：不建输出图片、不建 history\ 归档、
# 也不碰安装目录里任何既有文件的字节或时间戳。前后各取一次整目录指纹来比 —— 不靠"应该不会写"。
$fpAfter = Get-EcDirFingerprint -Dir $InstallDir
$fpDiff = @(Compare-Object -ReferenceObject (@($fpBefore -split "`n")) -DifferenceObject (@($fpAfter -split "`n")))
Assert-Ec ($fpDiff.Count -eq 0) "只读查询在安装目录里落了东西（不该有新增/改动/删除；差异：$(($fpDiff | ForEach-Object { $_.SideIndicator + ' ' + $_.InputObject }) -join ' | '))"
$histDirNow = Join-Path $InstallDir 'history'
Assert-Ec (-not ((Test-Path -LiteralPath $histDirNow) -and -not $histBefore)) '只读查询不该创建 history\ 归档目录（截图历史只在真的交付图片时才有）'

# ---------------------------------------------------------------------------
# 4) 离线计划声明的依赖必须真的随包（少一件就判失败，不给"安装后再编译"留路）
# ---------------------------------------------------------------------------
$offlineSuites = @($mf.installPlan.offlineTests)
Assert-Ec ($offlineSuites.Count -gt 0) '清单声明了安装后可跑的离线套件'
$depsMissing = @()
$depsTable = $mf.installPlan.offlineSuiteDeps
foreach ($suite in $offlineSuites) {
    $deps = $null
    if ($depsTable) { $deps = $depsTable.$suite }
    if (-not $deps) { $depsMissing += "[$suite] 清单里没有它的依赖条目"; continue }
    $need = @()
    foreach ($x in @($deps.scripts))  { if ($x) { $need += ($x -replace '/', '\') } }
    foreach ($x in @($deps.binaries)) { if ($x) { $need += ('build\' + $x) } }
    foreach ($x in @($deps.aux))      { if ($x) { $need += ($x -replace '/', '\') } }
    foreach ($rel in $need) {
        if (-not (Test-Path -LiteralPath (Join-Path $InstallDir $rel))) { $depsMissing += "[$suite] 缺 $rel" }
    }
}
Assert-Ec ($depsMissing.Count -eq 0) "离线计划的依赖齐全（$($offlineSuites.Count) 套：$($offlineSuites -join ', ')）$(if ($depsMissing.Count) { '；缺：' + ($depsMissing -join ' | ') })"

# ---------------------------------------------------------------------------
# 5) 可选的安装版离线测试（指向本目录的 exe，不误用 PATH 上的旧版）
# ---------------------------------------------------------------------------
if ($RunOfflineTests) {
    Assert-Ec (Test-Path -LiteralPath $testAll) "声明安装后可跑的离线测试，但随包缺少 test-all.ps1：$testAll"
    if (Test-Path -LiteralPath $testAll) {
        # 外层预算必须覆盖"逐套预算 × 实际套件数 + 收尾裕量"：用单套预算去等一套 14 个串的串行计划，
        # 会把健康的计划误判成超时。换算全部走 [long]，再由下面这行显式夹到 [int] 范围内，
        # 免得毫秒数溢出成负数（负数会让调用器一上来就判超时）。
        $autoOverall = [long]($offlineSuites.Count * [long]$SuiteTimeoutSec) + 600L
        $overallSec = if ($OverallTimeoutSec -gt 0) { [long]$OverallTimeoutSec } else { $autoOverall }
        if ($overallSec -gt 864000L) { $overallSec = 864000L }
        $overallMs = $overallSec * 1000L
        if ($overallMs -lt 1000L) { $overallMs = 1000L }
        if ($overallMs -gt [long][int]::MaxValue) { $overallMs = [long][int]::MaxValue }

        $logDir = Join-Path ([IO.Path]::GetTempPath()) ('ecapture-verify-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
        # 把清单里核对过的配置递下去，而不是硬写 Release：-NoBuild 下它不参与构建，
        # 但汇总与日志里出现的配置必须是这份包真实核对出来的那一个。
        $mfConfig = [string]$mf.config
        if (@('Debug', 'Release', 'RelWithDebInfo') -notcontains $mfConfig) { $mfConfig = 'Release' }
        $sa = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $testAll,
                '-Config', $mfConfig, '-NoBuild', '-Force', '-Offline',
                '-Exe', $exe, '-Only', ($offlineSuites -join ','), '-LogDir', $logDir,
                '-SuiteTimeoutSec', [string]$SuiteTimeoutSec)
        Write-Host ("  跑安装版离线测试：{0}（{1} 套，每套 {2}s，外层总预算 {3}s；日志 {4}）" -f `
                    ($offlineSuites -join ','), $offlineSuites.Count, $SuiteTimeoutSec, $overallSec, $logDir) -ForegroundColor DarkGray
        # 用"跑本自检的同一个 shell"起总跑：不跨 PowerShell 5.1 / 7 两种运行时，
        # 所以不需要像 test-all.ps1 那样先问一次目标 shell 的 PSModulePath（那一步是防跨 shell 递环境）。
        $engine = (Get-Process -Id $PID).Path
        $r = Invoke-EcProcess -FilePath $engine -Arguments $sa -TimeoutMs ([int]$overallMs)
        $outFile = Join-Path $logDir 'test-all.out.txt'
        $errFile = Join-Path $logDir 'test-all.err.txt'
        try {
            New-Item -ItemType Directory -Force -Path $logDir | Out-Null
            $ob = $r.StdoutBytes; if ($null -eq $ob) { $ob = New-Object byte[] 0 }
            $eb = $r.StderrBytes; if ($null -eq $eb) { $eb = New-Object byte[] 0 }
            [IO.File]::WriteAllBytes($outFile, $ob)
            [IO.File]::WriteAllBytes($errFile, $eb)
        } catch {
            Write-Host ("  !! 落盘测试日志失败（不影响判定，只影响排障）：{0}" -f $_.Exception.Message) -ForegroundColor DarkYellow
        }
        $text = ('{0}{1}{2}' -f $r.Stdout, [Environment]::NewLine, $r.Stderr)
        $tail = @(($text -split "`r?`n" | Where-Object { $_ }) | Select-Object -Last 25)
        foreach ($line in $tail) { Write-Host "    $line" -ForegroundColor DarkGray }

        $okState = $true
        if ($r.StartError) { Assert-Ec $false "离线总跑没能启动：$($r.StartError)（日志：$logDir）"; $okState = $false }
        elseif ($r.TimedOut) { Assert-Ec $false "离线总跑在 ${overallSec}s 外层预算内没有退出（已结束本次启动的进程树；日志：$logDir）"; $okState = $false }
        elseif ($r.Exit -ne 0) {
            # 失败原因里补一句配置那一档的现场：离线套件跑的是安装目录里那份 exe，它会读同目录的
            # $cfgUser。装了这份偏好之后，套件的"某项没被给出"那类断言可能改变结论 —— 那是套件的
            # 期望与"配置已存在"这件事要对齐，不是安装文件坏了。这里只补线索，不因此放宽判定。
            $cfgHint = $(if (Test-Path -LiteralPath $userCfgPath) { "（安装目录里有 $cfgUser：套件读到的默认值来自它，必要时用 --no-config 复核那一条判据是不是被偏好影响的）" } else { '' })
            Assert-Ec $false "安装版离线测试没有全部通过（test-all 退出码 $($r.Exit)；日志：$logDir）$cfgHint"; $okState = $false
        }        else { Assert-Ec $true '安装版离线测试全部通过（test-all 退出码 0）' }

        # 只在成功且没要求保留时清理；失败一定留日志，并把位置说清楚。
        # 清理本身出错不能盖掉前面的判定，所以这里吞掉的是清理异常，不是被检进程的异常。
        if ($okState -and -not $KeepLogs) {
            try { Remove-Item -LiteralPath $logDir -Recurse -Force -ErrorAction Stop }
            catch { Write-Host "  !! 清理临时日志目录失败（不影响判定）：$logDir" -ForegroundColor DarkYellow }
        } elseif (-not $okState -or $KeepLogs) {
            Write-Host ("  测试日志留在：{0}" -f $logDir) -ForegroundColor DarkGray
        }
    }
} else {
    Skip-Ec '安装版离线测试' '未给 -RunOfflineTests（默认只做文件/哈希、引用、身份与只读查询）'
}

Skip-Ec '真实窗口 / HDR / 多屏 / 确认框测试' '需要由用户在合适的环境里亲自安排，安装后自检不代跑'

# 本次自建的临时探针目录（身份核对用的字节相同副本 + 两份临时 INI）只清自己那一个；
# 安装目录里的东西从头到尾没被写过（上面已经用整目录指纹证过一遍）。
if ($probeRoot -and (Test-Path -LiteralPath $probeRoot)) {
    try { Remove-Item -LiteralPath $probeRoot -Recurse -Force -ErrorAction Stop }
    catch { Write-Host ("  !! 临时探针目录清理失败（不影响判定）：{0}" -f $probeRoot) -ForegroundColor DarkYellow }
}

Write-Host ''
Write-Host ("  安装位置：{0}" -f $InstallDir) -ForegroundColor DarkGray
Write-Host ("  README：{0}" -f (Join-Path $InstallDir 'README.md')) -ForegroundColor DarkGray
Write-Host ("  偏好配置：读的是 {0}（不存在就用内置默认值）；模板与单位区间见同目录的 {1}" -f $userCfgPath, $cfgTpl) -ForegroundColor DarkGray
Write-Host '  只想看当前生效的设置来自哪一层：ECAPTURE.EXE --show-config（只读，不取像素、不建文件）' -ForegroundColor DarkGray
Write-Host '  AI 部署：让 AI 工具显式读取上面的 SKILL.md，并按其中说明用绝对路径调用同目录的 ECAPTURE.EXE；装到默认目录之外的路径通常不会被自动发现。' -ForegroundColor DarkGray

exit (Complete-EcSuite -Title '安装版自检')
