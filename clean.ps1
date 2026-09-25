<#
.SYNOPSIS
    清理 EvernightCapture 的构建产物（build 目录里的东西），并保留截图历史。
.DESCRIPTION
    与 .\build.ps1 -Clean 共用同一份判据（scripts\build-clean.psm1）：删掉 CMake 缓存、编译产物、
    测试程序与打包中间目录，**不删 build\history**。

    为什么特别提这一句：开发版 ECAPTURE.EXE 就躺在 build\ 里，而默认开启的截图历史住在"实际运行
    的那个 exe 所在目录\history\日期\"——于是开发时截的每一张图，历史正好也落在 build\history\。
    那些是持续保留的截图数据，不是构建产物，也不该被一次"清理"顺手带走。

    判据本体在那份模块里，三条都判：build\ 自己是重解析点、build\history 是重解析点、或那个名字
    被一个文件占着时，这里**拒绝清理并说清为什么**，既不静默删除也不静默搬移。
    不会触碰源码与 tests 目录。截图历史要清由用户自己显式做（工具不自动轮转、不自动删）。
.EXAMPLE
    .\clean.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

Import-Module (Join-Path $root 'scripts\build-clean.psm1') -Force
$verdict = Remove-EcBuildArtifacts -BuildDir (Join-Path $root 'build')

if ($verdict.NothingToDo) {
    Write-Host $verdict.Reason -ForegroundColor DarkGray
    exit 0
}
if (-not $verdict.Removed) {
    Write-Host "清理被拒绝：$($verdict.Reason)" -ForegroundColor Yellow
    exit 1
}
$color = if ($verdict.Kept -eq 'history') { 'Yellow' } else { 'Green' }
Write-Host $verdict.Reason -ForegroundColor $color
if ($verdict.Kept -eq 'history') {
    Write-Host '  要连历史一起清掉请自己确认内容后手动删那棵目录；工具不会自动清它。' -ForegroundColor DarkGray
}
