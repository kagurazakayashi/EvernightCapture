<#
.SYNOPSIS
    wxWidgets 公共 GUI 层与确认框的组件判据（Windows 真桌面）。
.DESCRIPTION
    跑 build\ecapture-gui-tests.exe：它真的创建程序自己的 wx 对话框，自动替自己点
    "允许 / 不允许 / 关框"，并验证长中文标题、长路径、默认焦点、可访问名称、同意 / 拒绝 /
    关框 / 超时 / 程序取消六档结局。它不截屏、不取像素、不碰系统授权窗口，也不要求
    操作者代答任何按钮 —— 这些对话框是测试进程自己的，几秒内自动关闭。
.EXAMPLE
    .\tests\gui.ps1
#>
param(
    [string]$Exe
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Reset-EcSuite

$root = Get-EcRepoRoot
$guiExe = Join-Path $root 'build\ecapture-gui-tests.exe'

if (-not (Test-Path -LiteralPath $guiExe)) {
    Assert-Ec $false "缺少 GUI 判据程序 $guiExe。它由 wxWidgets GUI 层构建产生：先跑 .\build.ps1（需要 wxWidgets 3.2.x）。"
} else {
    $r = Invoke-EcProcess -FilePath $guiExe -TimeoutMs 180000
    $lines = @($r.Stdout -split "`r?`n" | Where-Object { $_ })
    $tail = [string]($lines | Select-Object -Last 1)
    Assert-Ec ($r.Exit -eq 0) "GUI 判据没全绿（exit=$($r.Exit)）：$tail"
    $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
    $failed = if ($m.Success) { [int]$m.Groups[2].Value } else { -1 }
    $passed = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
    Assert-Ec ($failed -eq 0) "GUI 判据有失败项，或摘要读不出来：$tail"
    Assert-Ec ($passed -ge 20) "GUI 判据通过数不对劲（$passed），判据被删了？"
    Write-Host "  $tail" -ForegroundColor DarkGray

    Skip-Ec '无图形会话时 GUI 初始化失败' 'Windows 上本机没有可安全模拟"服务会话/无交互桌面"的入口；这一条由 Linux 的 gui-runtime.sh 与 --init-failure 覆盖。Windows 侧按未验证记录。'
    Skip-Ec '系统通知真的弹出' '通知内容与策略留给结果通知那一步；本步只建立统一 wxWidgets 接口并在 Linux 侧编译链接。'
}

exit (Complete-EcSuite -Title 'wxWidgets GUI 层')
