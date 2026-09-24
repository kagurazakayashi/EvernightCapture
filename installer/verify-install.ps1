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
      5. 离线计划的可运行性：清单声明"安装后可跑"的每一套，脚本、依赖测试程序与辅助文件都必须真的在
         安装目录里；少一件就如实判失败，不会等到套件自己去"补编译一次"。
      6. 可选离线测试（-RunOfflineTests）：用随包的 test-all.ps1 跑本安装目录里的离线套件，
         指向本目录的 ECAPTURE.EXE，绝不误用 PATH 上的旧版。日志写到系统临时目录，不污染安装目录。

    进程调用一律复用随包的 tests\harness.psm1 调用器（Invoke-EcProcess）：两条标准流并发消费、
    从进程启动那一刻起就在期限内等待、超时只结束本次启动的进程树并保留部分输出。这里不再抄第二套
    生命周期逻辑，也不再用"先 ReadToEnd 再判超时"那种挂住就拿不到日志的写法。

    全程只读：不修改安装目录里的任何文件，不写注册表，不装依赖，不启动真实截图，也不替任何人确认桌面授权。
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
$id = Test-EcArtifactIdentity -Exe $exe -ExpectedVersion $declaredVersion -ExpectedArch $declaredArch -TimeoutMs $identityMs
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

$diag = Invoke-EcReadOnlyQuery -Exe $exe -Arguments @('--diagnostics', '--lang', 'en') -TimeoutMs $identityMs
Assert-Ec ($diag.Exit -eq 0 -and $null -ne $diag.Json) "--diagnostics 正常退出且输出是合法 JSON（退出码 $($diag.Exit)）"

$help = Invoke-EcReadOnlyQuery -Exe $exe -Arguments @('--help', '--lang', 'en') -TimeoutMs $identityMs -ExpectExit 3
Assert-Ec ($help.Exit -eq 3) "--help 退出码为 3（约定值，不是失败；实际 $($help.Exit)）"

# 安装器写下的归属标记：存在就必须说清这是本产品装的；不存在只如实报告，不当成失败
# （手工复制出来的目录没有标记，但文件与身份照样可以核对）。
if (Test-Path -LiteralPath $ownerIni) {
    # 归属标记是 CRLF 文本：按行比，别让 ^...$ 被行尾的 \r 绊住。
    $ownerLines = @((Get-Content -LiteralPath $ownerIni -Encoding UTF8) | ForEach-Object { $_.Trim() })
    Assert-Ec (@($ownerLines -contains 'product=EvernightCapture').Count -gt 0) '归属标记写明产品是 EvernightCapture'
    Assert-Ec (@($ownerLines -contains 'markerversion=1').Count -gt 0) '归属标记的 markerversion 是本安装器认得的那一档'
    if ($mf.appId) {
        $plainAppId = ([string]$mf.appId) -replace '^\{\{', '{'
        $plainAppId = $plainAppId -replace '\}\}$', '}'
        $wantAppId = 'appid=' + $plainAppId
        Assert-Ec (@($ownerLines -contains $wantAppId).Count -gt 0) "归属标记里的 AppId 与清单不一致（要找：$wantAppId）"
    }
    Assert-Ec (@($ownerLines | Where-Object { $_ -match '^(.+)\|\d+\|\d+$' }).Count -ge 1) '归属标记里要有已管理文件的记录（下次重装靠它认出被改过的文件）'
} else {
    Skip-Ec '安装器归属标记' ("没有 {0}：多半不是安装器铺的目录，而是手工复制出来的或开发目录" -f $ownerIni)
}

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
        elseif ($r.Exit -ne 0) { Assert-Ec $false "安装版离线测试没有全部通过（test-all 退出码 $($r.Exit)；日志：$logDir）"; $okState = $false }
        else { Assert-Ec $true '安装版离线测试全部通过（test-all 退出码 0）' }

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

Write-Host ''
Write-Host ("  安装位置：{0}" -f $InstallDir) -ForegroundColor DarkGray
Write-Host ("  README：{0}" -f (Join-Path $InstallDir 'README.md')) -ForegroundColor DarkGray
Write-Host '  AI 部署：让 AI 工具显式读取上面的 SKILL.md，并按其中说明用绝对路径调用同目录的 ECAPTURE.EXE；装到默认目录之外的路径通常不会被自动发现。' -ForegroundColor DarkGray

exit (Complete-EcSuite -Title '安装版自检')
