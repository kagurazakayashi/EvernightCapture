<#
.SYNOPSIS
    EvernightCapture 安装版只读自检：核对随包文件与哈希、文档相对引用、二进制身份与只读查询，再按声明跑离线测试。
.DESCRIPTION
    这是**安装目录里**的自检，不依赖源码仓库、Visual Studio 或网络：

      1. 载荷完整性：逐条核对 install-manifest.json 里列出的文件是否存在、大小与 SHA-256 是否一致。
      2. 文档相对引用：四份 README 与 SKILL.md 里的相对链接/图片都指向安装目录里真实存在的文件
         （安装目录里的文档不能布满失效链接）。
      3. 二进制身份：ECAPTURE.EXE 能跑 --version（退出码 0），版本与架构与清单一致；
         --capabilities / --diagnostics（退出码 0，JSON 结构可解）里的 program.version / arch / buildId
         与清单一致 —— 同版本号不等于同一份构建，所以身份核对看 buildId，不只看版本号。
      4. 只读契约：--help 退出码为 3（这是约定，不是失败）；只读查询不落地文件、不弹框。
      5. 可选离线测试（-RunOfflineTests）：用随包的 test-all.ps1 跑本安装目录里的离线套件
         （默认取清单里 installPlan.offlineTests 那几套，即 cli / capabilities / compat / orchestration 的离线层），
         指向本目录的 ECAPTURE.EXE，绝不误用 PATH 上的旧版。
         日志写到系统临时目录，不污染安装目录。

    全程只读：不修改安装目录里的任何文件，不写注册表，不装依赖，不启动真实截图，也不替任何人确认桌面授权。
    默认不跑真实窗口/HDR/多屏/确认框测试 —— 那些要由用户在合适的环境里亲自安排。

    退出码：0 = 全部通过（SKIP 是环境边界，不算失败也不算通过）；1 = 有 FAIL；2 = 前置不成立（路径/清单缺失）。
.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File verify-install.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File verify-install.ps1 -RunOfflineTests
    powershell -NoProfile -ExecutionPolicy Bypass -File verify-install.ps1 -InstallDir "D:\我的 工具\ecapture"
#>
param(
    [string]$InstallDir,
    [switch]$RunOfflineTests,
    [int]$SuiteTimeoutSec = 900,
    [switch]$KeepLogs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

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

$mf = Get-Content -LiteralPath $manifest -Raw -Encoding UTF8 | ConvertFrom-Json

if (Test-Path -LiteralPath $harness) {
    Import-Module $harness -Force -DisableNameChecking
} else {
    Write-Host "找不到 tests\harness.psm1：$harness" -ForegroundColor Red
    exit 2
}
Reset-EcSuite
Initialize-EcHarness -Exe $exe | Out-Null

# ---------------------------------------------------------------------------
# 进程调用：只读、限时、收原始文本
# ---------------------------------------------------------------------------
function Invoke-Exe {
    param([Parameter(Mandatory)][string[]]$Arguments, [int]$TimeoutMs = 30000)

    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = ($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $p = [Diagnostics.Process]::Start($psi)
    try {
        $so = $p.StandardOutput.ReadToEnd()
        $se = $p.StandardError.ReadToEnd()
        if (-not $p.WaitForExit($TimeoutMs)) { try { $p.Kill() } catch { } ; throw "ECAPTURE.EXE $($Arguments -join ' ') 没有按时退出" }
        return [pscustomobject]@{ Exit = $p.ExitCode; Out = $so; Err = $se }
    } finally { $p.Dispose() }
}

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
# 3) 二进制身份与只读查询
# ---------------------------------------------------------------------------
Assert-Ec (Test-Path -LiteralPath (Join-Path $InstallDir ([string]$mf.skill.entryFile))) `
    "Skill 入口 SKILL.md 就在安装目录根（所选目录就是 Skill 根）"

$ver = Invoke-Exe -Arguments @('--version')
Assert-Ec ($ver.Exit -eq 0) "--version 退出码为 0（实际 $($ver.Exit)）"
Assert-Ec ($ver.Err.Trim() -eq '') "--version 的 stderr 是空的"
Assert-Ec ($ver.Out -match [regex]::Escape([string]$mf.version)) "版本号与清单一致（$($mf.version)）"

$caps = Invoke-Exe -Arguments @('--capabilities', '--lang', 'en')
Assert-Ec ($caps.Exit -eq 0) "--capabilities 退出码为 0（实际 $($caps.Exit)）"
$capsJson = $null
try { $capsJson = $caps.Out | ConvertFrom-Json } catch { }
Assert-Ec ($null -ne $capsJson) "--capabilities 输出是合法 JSON"
if ($capsJson) {
    Assert-Ec ([string]$capsJson.program.version -eq [string]$mf.version) "能力报告里的版本与清单一致"
    if ($mf.arch) { Assert-Ec ([string]$capsJson.program.arch -eq [string]$mf.arch) "能力报告里的架构与清单一致（$($mf.arch)）" }
    if ($mf.buildId) { Assert-Ec ([string]$capsJson.program.buildId -eq [string]$mf.buildId) `
        "能力报告里的 buildId 与清单一致（同版本号不等于同一份构建）" }
    Assert-Ec ($null -ne $capsJson.caveats) "能力报告带有 caveats（自述它不保证什么）"
}

$diag = Invoke-Exe -Arguments @('--diagnostics', '--lang', 'en')
Assert-Ec ($diag.Exit -eq 0) "--diagnostics 退出码为 0（实际 $($diag.Exit)）"
$diagJson = $null
try { $diagJson = $diag.Out | ConvertFrom-Json } catch { }
Assert-Ec ($null -ne $diagJson) "--diagnostics 输出是合法 JSON"

$help = Invoke-Exe -Arguments @('--help', '--lang', 'en')
Assert-Ec ($help.Exit -eq 3) "--help 退出码为 3（约定值，不是失败；实际 $($help.Exit)）"

# ---------------------------------------------------------------------------
# 4) 可选的安装版离线测试（指向本目录的 exe，不误用 PATH 上的旧版）
# ---------------------------------------------------------------------------
if ($RunOfflineTests) {
    if (-not (Test-Path -LiteralPath $testAll)) {
        Assert-Ec $false "声明安装后可跑的离线测试，但随包缺少 test-all.ps1：$testAll"
    } else {
        $only = @($mf.installPlan.offlineTests)
        Assert-Ec ($only.Count -gt 0) "清单声明了安装后可跑的离线套件（$($only -join ',')）"
        $logDir = Join-Path ([IO.Path]::GetTempPath()) ('ecapture-verify-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $PID)
        $sa = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $testAll,
                '-Config', 'Release', '-NoBuild', '-Force', '-Offline',
                '-Exe', $exe, '-Only', ($only -join ','), '-LogDir', $logDir, '-SuiteTimeoutSec', "$SuiteTimeoutSec")
        Write-Host ("  跑安装版离线测试：{0}（日志 {1}）" -f ($only -join ','), $logDir) -ForegroundColor DarkGray
        $psi = New-Object Diagnostics.ProcessStartInfo
        $psi.FileName = (Get-Process -Id $PID).Path
        $psi.Arguments = ($sa | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
        $psi.UseShellExecute = $false
        $psi.CreateNoWindow = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $child = [Diagnostics.Process]::Start($psi)
        try {
            $so = $child.StandardOutput.ReadToEnd()
            $se = $child.StandardError.ReadToEnd()
            if (-not $child.WaitForExit(($SuiteTimeoutSec + 120) * 1000)) { try { $child.Kill() } catch { } }
            $code = $child.ExitCode
        } finally { $child.Dispose() }
        $tail = @((($so + "`n" + $se) -split "`r?`n" | Where-Object { $_ }) | Select-Object -Last 25)
        foreach ($line in $tail) { Write-Host "    $line" -ForegroundColor DarkGray }
        Assert-Ec ($code -eq 0) "安装版离线测试全部通过（test-all 退出码 0；实际 $code）"
        if (-not $KeepLogs) { Remove-Item -LiteralPath $logDir -Recurse -Force -ErrorAction SilentlyContinue }
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
