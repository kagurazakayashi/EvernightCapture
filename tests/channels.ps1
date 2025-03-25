<#
.SYNOPSIS
    真机通道对比测试：同一个窗口分别用 wgc / dwm / printwindow / bitblt / auto 截图，
    并做遮挡对照，检查每条通道拿到的画面是否符合它的能力。
.DESCRIPTION
    会短暂在桌面上开一个字体查看器窗口（结束或失败时自动关闭），并压一个纯红的不透明窗口在它上面。
    判据不是"有没有报错"，而是画面内容：
      * 尺寸合理 + 颜色够多  ->  真的抓到了窗口内容，而不是空帧
      * 遮挡测试里红色占比    ->  wgc / dwm / printwindow 应几乎无红（拿的是窗口自己的画面），
                                bitblt 应几乎全红（拿的是屏幕合成画面，只能看到遮挡物）
    通道清单是手写的，但会用 --help 的实际输出核对，漏改会直接失败。
    目标窗口用 --class FontViewWClass 锁定：字体查看器打不开文件时弹的是 #32770 错误框，
    用类名过滤就不会把错误框当成成功。
.EXAMPLE
    .\tests\channels.ps1
    .\tests\channels.ps1 -Keep      # 保留截图以便人眼看
#>
param(
    [string]$Exe,
    [string]$Font = 'C:\Windows\Fonts\consola.ttf',
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
if (-not $Exe) { $Exe = Join-Path $root 'build\ecapture.exe' }
if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe（先运行 .\build.ps1）" }
$Exe = (Get-Item -LiteralPath $Exe).FullName

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$outDir = Join-Path ([IO.Path]::GetTempPath()) 'ecapture-channels'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# fontview.exe 不剥参数引号，路径必须不带引号也不带空格：用 8.3 短名
$shortFont = (New-Object -ComObject Scripting.FileSystemObject).GetFile($Font).ShortPath
if (-not $shortFont) { throw "字体文件不存在：$Font" }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Ec {
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static class Win {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  }
}
'@

function Get-TargetHwnd {
    for ($i = 0; $i -lt 40; $i++) {
        $p = Get-Process fontview -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) { return $p.MainWindowHandle }
        Start-Sleep -Milliseconds 250
    }
    throw '字体查看器窗口没起来'
}

# 画面统计：尺寸、不同颜色数、红色占比（红色 = 遮挡物）
function Measure-Image([string]$path) {
    if (-not (Test-Path $path)) { return $null }
    $bmp = [Drawing.Bitmap]::new($path)
    try {
        $colors = @{}; $red = 0; $samples = 0
        for ($y = 0; $y -lt $bmp.Height; $y += 5) {
            for ($x = 0; $x -lt $bmp.Width; $x += 5) {
                $c = $bmp.GetPixel($x, $y); $samples++
                $colors["$($c.R),$($c.G),$($c.B)"] = 1
                if ($c.R -gt 190 -and $c.G -lt 70 -and $c.B -lt 70) { $red++ }
            }
        }
        [pscustomobject]@{
            Width = $bmp.Width; Height = $bmp.Height; Colors = $colors.Count
            RedRatio = if ($samples) { [math]::Round($red / $samples, 3) } else { 0 }
            Bytes = (Get-Item $path).Length
        }
    } finally { $bmp.Dispose() }
}

function Invoke-Cap([string]$channel, [string]$path) {
    Remove-Item $path -ErrorAction SilentlyContinue
    # 目标窗口可能同时有多个（临时覆盖窗口不是 fontview，故 --class 足够）
    $json = & $Exe --process fontview.exe --class FontViewWClass --newest --capture $channel --out $path 2>&1 | Out-String
    [pscustomobject]@{ Exit = $LASTEXITCODE; Json = $json; Info = Measure-Image $path }
}

$fails = [System.Collections.Generic.List[string]]::new()
function Assert([bool]$cond, [string]$what) {
    if (-not $cond) { $fails.Add($what); Write-Host "  FAIL  $what" -ForegroundColor Red }
}

$proc = $null
$cover = $null
try {
    Write-Host "目标：fontview.exe $shortFont"
    $proc = Start-Process "$env:SystemRoot\System32\fontview.exe" -ArgumentList $shortFont -PassThru
    $hwnd = Get-TargetHwnd

    # 支持的通道清单（与 src/CliOptions.cpp 的 kCaptureValues 手工保持一致；
    # 下面那步会用 --help 的实际输出核对，漏改就在这里失败）
    $channels = @('wgc', 'dwm', 'printwindow', 'bitblt', 'duplication', 'auto')
    $advertised = ((& $Exe --help) | Select-String -Pattern '--capture, -C' | ForEach-Object { $_.Line }) `
        | ForEach-Object { [regex]::Matches($_, '(?<=\s|^)(wgc|dwm|printwindow|bitblt|duplication|auto)(?=[\s/(])') } |
        ForEach-Object { $_.Value } | Select-Object -Unique
    Write-Host ("--help 声明的通道：{0}" -f ($advertised -join ', '))
    Assert (@(Compare-Object $channels $advertised).Count -eq 0) `
        "测试清单与 --help 不一致：测试=$($channels -join ',') 帮助=$($advertised -join ',')"

    # 基线：没有遮挡时每条通道都该拿到窗口内容
    Write-Host "`n=== 未遮挡：每条通道都要截到窗口内容 ==="
    $baseline = @{}
    foreach ($ch in $channels) {
        $path = Join-Path $outDir "open_$ch.png"
        $r = Invoke-Cap $ch $path
        $baseline[$ch] = $r
        $m = $r.Info
        Write-Host ("  {0,-14} exit={1} {2}x{3} colors={4} red={5} bytes={6}" -f `
                   $ch, $r.Exit, $m.Width, $m.Height, $m.Colors, $m.RedRatio, $m.Bytes)
        Assert ($r.Exit -eq 0) "$ch 通道退出码 $($r.Exit)，应为 0"
        Assert ($m -and $m.Width -ge 200 -and $m.Height -ge 150) "$ch 通道画面过小：$($m.Width)x$($m.Height)"
        Assert ($m -and $m.Colors -ge 12) "$ch 通道颜色过少（$($m.Colors)），可能是空帧"
    }

    # 遮挡：在目标窗口上压一个纯红不透明窗口。它必须是独立进程并自己泵消息，
    # 否则父脚本忙着跑截图时它不会重绘，屏幕上会出现"半红半原图"的假象。
    $rc = [Ec.RECT]::new()
    [void][Ec.Win]::GetWindowRect($hwnd, [ref]$rc)
    $coverScript = @'
Add-Type -AssemblyName System.Windows.Forms
$f = New-Object Windows.Forms.Form
$f.FormBorderStyle = 'None'
$f.StartPosition = 'Manual'
$f.Bounds = [Drawing.Rectangle]::FromLTRB([int]$args[0], [int]$args[1], [int]$args[2], [int]$args[3])
$f.BackColor = [Drawing.Color]::Red
$f.TopMost = $true
$f.ShowInTaskbar = $false
$f.Show()
[Windows.Forms.Application]::Run($f)
'@
    $tmpCover = Join-Path $env:TEMP 'ecapture-channels\cover.ps1'
    Set-Content -LiteralPath $tmpCover -Value $coverScript -Encoding ASCII
    $cover = Start-Process powershell -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy',
        'Bypass', '-File', $tmpCover, $rc.Left, $rc.Top, $rc.Right, $rc.Bottom)
    Start-Sleep -Milliseconds 2500
    Write-Host "`n=== 已遮挡：目标窗口被纯红窗口完全盖住 ==="

    $seeThrough = @('wgc', 'dwm', 'printwindow')   # 拿的是窗口自己的画面，不该看到红色
    $screenOnly = @('bitblt', 'duplication')       # 拿的是屏幕合成画面，应该全是遮挡物
    $grouped = (($seeThrough + $screenOnly + 'auto') | Sort-Object) -join ','
    Assert ($grouped -eq (($channels | Sort-Object) -join ',')) `
        "遮挡分组没覆盖全部通道：$grouped"
    foreach ($ch in ($seeThrough + $screenOnly + 'auto')) {
        $path = Join-Path $outDir "covered_$ch.png"
        $r = Invoke-Cap $ch $path
        $m = $r.Info
        Write-Host ("  {0,-14} exit={1} {2}x{3} colors={4} red={5}" -f `
                   $ch, $r.Exit, $m.Width, $m.Height, $m.Colors, $m.RedRatio)
        Assert ($r.Exit -eq 0) "$ch 通道遮挡时退出码 $($r.Exit)，应为 0"
        if ($ch -in $seeThrough) {
            Assert ($m.RedRatio -lt 0.15) "$ch 应能截到被遮挡窗口的内容，实际红色占比 $($m.RedRatio)"
        } elseif ($ch -eq 'auto') {
            Assert ($m.RedRatio -lt 0.15) "auto 首选 wgc，不该退化成屏幕取图（红色占比 $($m.RedRatio)）"
        } else {
            Assert ($m.RedRatio -gt 0.6) "$ch 只能取屏幕画面，红色占比应远高于其它通道，实际 $($m.RedRatio)"
        }
    }

    # 已删除的方案必须在解析期就拒绝，不能留一个"能传但截不出东西"的取值
    Write-Host "`n=== 已删除的取值必须被拒绝 ==="
    foreach ($gone in 'magnification') {
        $path = Join-Path $outDir "removed_$gone.png"
        $r = Invoke-Cap $gone $path
        Write-Host ("  {0,-14} exit={1} -> {2}" -f $gone, $r.Exit, ($r.Json -replace '\s+', ' ').Trim())
        Assert ($r.Exit -eq 1) "$gone 退出码 $($r.Exit)，应为 1（解析期拒绝）"
        Assert ($r.Json -match 'cli\.unknown_capture_method') "$gone 应报 cli.unknown_capture_method"
        Assert (-not (Test-Path $path)) "$gone 不该写出文件"
    }

    Write-Host ''
    if ($fails.Count) {
        Write-Host "通道测试失败：$($fails.Count) 项" -ForegroundColor Red
        $fails | ForEach-Object { Write-Host "  - $_" }
    } else {
        Write-Host '通道测试全部通过' -ForegroundColor Green
    }
} finally {
    if ($cover) { Stop-Process -Id $cover.Id -Force -ErrorAction SilentlyContinue }
    Get-Process fontview -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Host '已关闭字体查看器'
    if (-not $Keep) { Get-ChildItem $outDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force }
    else { Write-Host "截图保留在 $outDir" }
}

exit [int][bool]$fails.Count
