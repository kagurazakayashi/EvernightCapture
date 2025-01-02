<#
.SYNOPSIS
    EvernightCapture 构建脚本：自动定位 Visual Studio 的 C++ 工具链，再用 CMake + Ninja 构建。
.EXAMPLE
    .\build.ps1                # Release
    .\build.ps1 -Config Debug
    .\build.ps1 -Clean         # 删除 build 目录后重新配置
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath $PSScriptRoot).FullName
$buildDir = Join-Path $root 'build'

# --- 定位 Visual Studio（vswhere 路径固定，不依赖 VS 版本）--------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "找不到 vswhere.exe：$vswhere" }

$vs = (& $vswhere -latest -prerelease -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath) | Select-Object -First 1
if (-not $vs) { throw '未找到安装 "使用 C++ 的桌面开发" 工作负载的 Visual Studio' }

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "找不到 vcvars64.bat：$vcvars" }

# --- 优先使用 VS 自带的 cmake / ninja（MSYS 版 cmake 会改写盘符路径，必须避开）---
$toolDirs = @(
    (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
    (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
) | Where-Object { Test-Path $_ }
$pathAdd = ($toolDirs -join ';')

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "清理 $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

# 把命令写进临时 .bat，避免 PowerShell -> cmd 的多层引号问题
$bat = Join-Path $env:TEMP ("ecapture_build_{0}.bat" -f $PID)
$lines = @(
    '@echo off',
    'chcp 65001 >nul',
    "call `"$vcvars`" >nul",
    "set `"PATH=$pathAdd;%PATH%`"",
    "cmake -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config",
    'if errorlevel 1 exit /b 1',
    "cmake --build `"$buildDir`"",
    'exit /b %errorlevel%'
)
Set-Content -Path $bat -Value $lines -Encoding ascii

Write-Host "配置/构建 ($Config) ..." -ForegroundColor Cyan
Write-Host "  MSVC: $vs"
Write-Host "  cmake/ninja: $(if ($pathAdd) { $pathAdd } else { 'PATH 中的版本' })"
& cmd.exe /c "`"$bat`""
$code = $LASTEXITCODE
Remove-Item -Force $bat -ErrorAction SilentlyContinue

if ($code -ne 0) { throw "构建失败，退出码 $code" }

$exe = Join-Path $buildDir 'ecapture.exe'
if (-not (Test-Path $exe)) { throw "构建完成但找不到产物：$exe" }
Write-Host "`n产物: $exe" -ForegroundColor Green
