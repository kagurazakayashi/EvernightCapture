<#
.SYNOPSIS
    启动 EvernightCapture（运行已构建的 ecapture.exe）。
.DESCRIPTION
    启动 build\ecapture.exe。产物不存在时提示先运行 .\build.ps1。
    默认不等待程序退出；加 -Wait 则阻塞到程序结束并返回其退出码。
.EXAMPLE
    .\run.ps1
    .\run.ps1 -Wait
#>
[CmdletBinding()]
param(
    [switch]$Wait
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$exe = Join-Path $root 'build\ecapture.exe'
if (-not (Test-Path -LiteralPath $exe)) {
    throw "找不到产物 $exe —— 请先运行 .\build.ps1 构建。"
}

Write-Host "启动 $exe" -ForegroundColor Green
if ($Wait) {
    & $exe
    exit $LASTEXITCODE
}
Start-Process -FilePath $exe
