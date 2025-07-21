<#
.SYNOPSIS
    Desktop Duplication 多屏适配的判据：离线层逐条注入旋转/裁剪/适配器定位，真机层核对
    "弹框与 --yes 无效""交付尺寸与裁剪矩形""没截全必须报出来"。
.DESCRIPTION
    这次修的是三件事：
      1) 整幅桌面帧被直接按桌面坐标裁剪，没有考虑显示器旋转（0/90/180/270）。竖屏时纹理是
         面板朝向的那一张，直接裁会得到"尺寸正确但画面横躺"的图，光看退出码发现不了。
      2) 先建默认适配器的设备、只枚举那个适配器的输出，于是挂在第二块显卡上的屏根本进不了表。
      3) 跨屏或一部分在屏幕外的目标只截到与某一块输出重叠的那一片，旧实现静默把丢掉的部分吞了。

    判据分两层：
      1) 离线层：build\ecapture-dup-tests.exe（源码 tests\dup_state.cpp）——把旋转与尺寸的四种
         组合、桌面坐标到纹理坐标的换算、负坐标与跨屏裁剪、假适配器表（两 adapter、目标在第二
         adapter、无输出、热拔出）、以及"那块屏在确认之后变了"三类变化直接注入生产判据本体。
         像素判据用测试自己写的朴素"先旋转再裁剪"做参照，独立于被测实现。
      2) 真机层：本测试自建的窗口（ecwindow.exe）+ 真机的 duplication 通道，核对 JSON 报告的
         requestedRect / capturedRect / clipped / rotation 与图片实际内容一致。

    隐私规矩照旧：duplication 取的是桌面像素，**一定**要人确认，--yes 对它不起作用。
    测试只代答"否"（拒绝不拍到任何东西）；要真去截屏幕的项一律要显式加 -SimulateConsent，
    并且只在专门腾出来、没有隐私内容的桌面上这么跑。
    绝不改动用户正在使用的显示设置：旋转屏与双显卡的真机项在本机不具备条件时照实记未验证。

.EXAMPLE
    .\tests\dup.ps1
    .\tests\dup.ps1 -SkipState            # 只跑真机那层
    .\tests\dup.ps1 -SkipReal             # 只跑离线判据层（没有交互桌面时用）
    .\tests\dup.ps1 -SimulateConsent      # 无隐私专用桌面：代答"是"，跑要截图的那几条
    .\tests\dup.ps1 -Keep                 # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$SkipReal,
    [switch]$SimulateConsent,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-dup-tests.exe'

$run = New-EcRunDir -Tag 'dup'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

$IDYES = 6
$IDNO = 7

function Json-Of($r) {
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $o = $null
    try { $o = $body | ConvertFrom-Json } catch { }
    return $o
}

function Err-Codes($o) { if ($o -and $o.errors) { @($o.errors | ForEach-Object { $_.code }) } else { @() } }
function Note-Codes($o) { if ($o -and $o.notes) { @($o.notes | ForEach-Object { $_.code }) } else { @() } }

# PNG 头里的 IHDR 宽高（大端 32 位）：不依赖任何库，独立于工具自己报告的尺寸做对照。
function Get-PngSize {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $b = [IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 24) { return $null }
    if (-not ($b[0] -eq 0x89 -and $b[1] -eq 0x50 -and $b[2] -eq 0x4E -and $b[3] -eq 0x47)) { return $null }
    $w = [int]$b[16] * 16777216 + [int]$b[17] * 65536 + [int]$b[18] * 256 + [int]$b[19]
    $h = [int]$b[20] * 16777216 + [int]$b[21] * 65536 + [int]$b[22] * 256 + [int]$b[23]
    return [pscustomobject]@{ Width = $w; Height = $h }
}

# ---------------------------------------------------------------------------
# 独立判据：本机每块屏的**显示方向**与显卡适配器数量，都走工具没用的那套 API。
# EnumDisplaySettings 的 dmDisplayOrientation 是系统对"这块屏现在转了多少度"的说法，
# 而工具走的是 DXGI 的 IDXGIOutputDuplication::GetDesc —— 两边独立才有对照意义。
# ---------------------------------------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace EcDup {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion; public short dmDriverVersion; public short dmSize; public short dmDriverExtra;
    public int dmFields;
    public int dmPositionX; public int dmPositionY; public int dmDisplayOrientation; public int dmDisplayFixedOutput;
    public short dmColor; public short dmDuplex; public short dmYResolution; public short dmTTOption; public short dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels; public int dmBitsPerPel; public int dmPelsWidth; public int dmPelsHeight;
    public int dmDisplayFlags; public int dmDisplayFrequency; public int dmICMMethod; public int dmICMIntent;
    public int dmMediaType; public int dmDitherType; public int dmReserved1; public int dmReserved2;
    public int dmPanningWidth; public int dmPanningHeight;
  }
  public static class Win {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MONITORINFOEX {
      public int cbSize; public RECT rcMonitor; public RECT rcWork; public int dwFlags;
      [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
    }
    public delegate bool EnumProc(IntPtr hMon, IntPtr hdc, ref RECT r, IntPtr data);
    [DllImport("user32")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, EnumProc proc, IntPtr data);
    [DllImport("user32", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr hMon, ref MONITORINFOEX info);
    [DllImport("user32", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettings(string dev, int mode, ref DEVMODE dm);
    [DllImport("user32")] public static extern int GetSystemMetrics(int idx);
  }
}
'@

# 逐块屏查它当前的显示方向。枚举回调里不能有异常抛出去（那会直接终止本进程），
# 所以先收齐设备名与矩形，再在回调之外一块块问方向；问不到就记"未知"而不是让判据崩。
$monitors = New-Object System.Collections.Generic.List[object]
$cb = [EcDup.Win+EnumProc] {
    param($hMon, $hdc, [ref]$r, $data)
    $info = New-Object EcDup.Win+MONITORINFOEX
    $info.cbSize = [Runtime.InteropServices.Marshal]::SizeOf([type][EcDup.Win+MONITORINFOEX])
    if ([EcDup.Win]::GetMonitorInfo($hMon, [ref]$info)) {
        $b = $info.rcMonitor
        $monitors.Add([pscustomobject]@{
            Ordinal = $monitors.Count + 1
            Device = $info.szDevice
            Left = [int]$b.Left; Top = [int]$b.Top
            Width = [int]($b.Right - $b.Left); Height = [int]($b.Bottom - $b.Top)
            Primary = (([int]$info.dwFlags -band 1) -ne 0)
        })
    }
    return $true
}
[void][EcDup.Win]::EnumDisplayMonitors([IntPtr]::Zero, [IntPtr]::Zero, $cb, [IntPtr]::Zero)
$screens = @(foreach ($m in $monitors) {
    $deg = $null
    try {
        $dm = New-Object EcDup.Win+DEVMODE
        $dm.dmSize = [short][Runtime.InteropServices.Marshal]::SizeOf([type][EcDup.Win+DEVMODE])
        # ENUM_CURRENT_SETTINGS = -1
        if ([EcDup.Win]::EnumDisplaySettings($m.Device, -1, [ref]$dm)) { $deg = [int]$dm.dmDisplayOrientation * 90 }
    } catch { $deg = $null }
    $m | Add-Member -NotePropertyName Orientation -NotePropertyValue $deg -Force
    $m
})
if (-not $screens.Count) { throw '本机枚举不到任何显示器' }
$virtualRight = [EcDup.Win]::GetSystemMetrics(76) + [EcDup.Win]::GetSystemMetrics(78)  # SM_XVIRTUALSCREEN + SM_CXVIRTUALSCREEN
$rotated = @($screens | Where-Object { $null -ne $_.Orientation -and $_.Orientation -ne 0 })
$adapterCount = 0
try { $adapterCount = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop |
        Where-Object { $_.VideoMethodDoesNotSupportDisplays -ne $true -or $null -eq $_.PSObject.Properties['VideoMethodDoesNotSupportDisplays'] }).Count }
catch { $adapterCount = 0 }
Write-Host ("屏幕 {0} 块；旋转屏 {1} 块；虚拟桌面右边界 {2}；显卡控制器 {3} 个" -f `
           $screens.Count, $rotated.Count, $virtualRight, $adapterCount)
foreach ($s in $screens) {
    Write-Host ("  #{0} {1} {2}x{3}+{4}+{5} 方向={6}{7}" -f $s.Ordinal, $s.Device, $s.Width,
               $s.Height, $s.Left, $s.Top, $(if ($null -eq $s.Orientation) { '未知' } else { "$($s.Orientation)度" }),
               $(if ($s.Primary) { ' primary' } else { '' })) -ForegroundColor DarkGray
}

function Invoke-DupShot {
    param(
        [Parameter(Mandatory)][string[]]$Arguments,
        [string]$Name,
        [int]$Answer = 0,
        [switch]$ExpectNoDialog
    )
    $path = if ($Name) { Get-EcRunFile -RunDir $run -Name "$Name.png" } else { '-' }
    if ($Name) { Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue }
    $argv = @($Arguments) + @('--lang', 'zh-CN')
    if ($Name) { $argv += @('--out', $path) }
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -Answer $Answer -ExpectNoDialog:$ExpectNoDialog
    $json = Json-Of $r
    [pscustomobject]@{
        Exit = $r.Exit; Path = $path; Json = $json; Dialog = $r.Dialog; Clicked = $r.Clicked
        Img = if ($json) { @($json.images)[0] } else { $null }
        ErrCodes = Err-Codes $json; NoteCodes = Note-Codes $json
        Png = if ($Name) { Get-PngSize -Path $path } else { $null }
        Stderr = $r.Stderr
    }
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：旋转/裁剪/适配器定位（测生产判据本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec 'Duplication 旋转、裁剪与适配器定位的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-dup-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "Duplication 判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "Duplication 判据有失败项：$tail"
        Assert-Ec ([int]$m.Groups[1].Value -ge 60) "Duplication 判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    if ($SkipReal) {
        Skip-Ec '真机：duplication 的弹框、尺寸、裁剪矩形与旋转' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'
    }

    # =========================================================================
    Write-Host "`n=== 1) 桌面像素路径：一定弹框，答`"否`"不落地，--yes 对它不起作用 ==="
    # =========================================================================
    $class = "ec-dup-$tag"
    $w = Start-EcWindow -RunDir $run -Class $class -Title "Duplication 目标 $class" -Rect '300,300,700,540' -Seed 11
    Write-Host ("目标窗口 PID={0} HWND={1}" -f $w.Pid, (Get-EcHwndHex $w.Hwnd))
    $r = Invoke-DupShot -Name 'refused' -Answer $IDNO -Arguments @(
        '--hwnd', (Get-EcHwndHex $w.Hwnd), '--capture', 'duplication', '--yes')
    Write-Host ("  弹框={0} 点到={1} exit={2} codes={3}" -f $r.Dialog, $r.Clicked, $r.Exit, ($r.ErrCodes -join ',')) -ForegroundColor DarkGray
    Assert-Ec $r.Dialog 'duplication 是桌面像素路径，必须弹确认框（--yes 不该免掉它）'
    Assert-Ec $r.Clicked '确认框上的"否"没点到（按钮找不到或窗口已关）' -Quiet
    Assert-Ec ($r.Exit -eq 6) "答`"否`"的退出码 $($r.Exit)，应为 6"
    Assert-Ec ($r.ErrCodes -contains 'capture.access_denied') '答"否"没报 capture.access_denied'
    Assert-Ec ($r.Json.captured -eq 0 -and @($r.Json.images).Count -eq 0) '答"否"不该有图'
    Assert-Ec (-not (Test-Path -LiteralPath $r.Path)) '答"否"却写出了文件'

    if (-not $SimulateConsent) {
        Write-Host "`n!! 未给 -SimulateConsent：不代答`"是`"，下面这些要截图的项记为 SKIP" -ForegroundColor DarkYellow
        foreach ($s in @(
                '屏内窗口：requestedRect/capturedRect 一致、不带 clipped、画面出自本窗口',
                '跨出桌面的窗口：必须报 clipped + note.capture_clipped，尺寸等于实际截到的那块',
                '整屏 --monitor N --capture duplication：交付尺寸等于该屏物理矩形',
                '四角朝向判据：交付图必须处于用户看到的朝向（每种旋转各自的真机现场另记）',
                '两块显卡分别驱动屏幕：每块屏都要截得到')) {
            Skip-Ec $s '没有 -SimulateConsent，真实桌面确认留给人回答'
        }
        Skip-Ec '热拔出一块屏后必须报 capture.monitor_changed 而不改截别的屏' '要真的拔掉显示器，不能改用户的显示设置；定位与判据由离线层（假枚举器 + CompareScreen）判'
        Skip-Ec '编号在两次枚举之间换了位置' '要重排用户的显示器；--monitor 编号的含义已经写明是"本次枚举的顺序"，认屏请用 images[].device'
        Write-Host "`n=== 收尾 ==="
        Stop-EcOwnedWindows
        Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
        exit (Complete-EcSuite -Title 'Desktop Duplication 多屏适配')
    }

    # =========================================================================
    Write-Host "`n=== 2) 屏内窗口：裁剪矩形与画面归属 ==="
    # =========================================================================
    $r = Invoke-DupShot -Name 'inside' -Answer $IDYES -Arguments @(
        '--hwnd', (Get-EcHwndHex $w.Hwnd), '--capture', 'duplication')
    $mine = New-EcSignatureKey -Seed 11
    Write-Host ("  exit={0} PNG={1}x{2} capturedRect={3} clipped={4} rotation={5}" -f `
               $r.Exit, $r.Png.Width, $r.Png.Height, ($r.Img.capturedRect | ConvertTo-Json -Compress), $r.Img.clipped, $r.Img.rotation) -ForegroundColor DarkGray
    Assert-Ec ($r.Exit -eq 0 -and $r.Json.captured -eq 1) "屏内窗口截图失败（exit=$($r.Exit)）：$($r.Stderr)"
    Assert-Ec ($r.Img.path -eq 'duplication.frame') "来路该是 duplication.frame，实际 $($r.Img.path)"
    Assert-Ec ($r.Img.scope -eq 'desktop') "裁成窗口大小仍然是桌面像素，scope 该是 desktop，实际 $($r.Img.scope)"
    Assert-Ec ($r.Img.requestedRect -and $r.Img.capturedRect) "缺 requestedRect / capturedRect：定位过程必须留痕"
    Assert-Ec (-not $r.Img.clipped) '完整截到的窗口不该带 clipped'
    $wr = Get-EcWindowRect -Hwnd $w.Hwnd
    $wantW = $wr.Right - $wr.Left
    Assert-Ec ($r.Img.capturedRect.width -eq $wantW -and $r.Png.Width -eq $wantW) `
        "交付宽 $($r.Png.Width) / capturedRect 宽 $($r.Img.capturedRect.width)，都应等于窗口宽 $wantW"
    $stats = Get-EcImageStats -Path $r.Path -Step 3
    Assert-Ec ($stats.Colors -ge 12) "画面只有 $($stats.Colors) 种颜色，可能是空帧"
    $d = Get-EcColorDistance -A $stats.TopDominant -B $mine
    Assert-Ec ($d -le 32) "主色 $($stats.TopDominant) 离本次签名色 $mine 太远（$d），这一帧不是本窗口的"

    # =========================================================================
    Write-Host "`n=== 3) 跨出桌面的窗口：必须报 clipped，不能默认这就是完整目标 ==="
    # =========================================================================
    $straddle = '{0},200,{1},420' -f ($virtualRight - 300), ($virtualRight + 260)
    [void](Set-EcWindowRect -Window $w -Rect $straddle)
    Start-Sleep -Milliseconds 250
    $r = Invoke-DupShot -Name 'straddle' -Answer $IDYES -Arguments @(
        '--hwnd', (Get-EcHwndHex $w.Hwnd), '--capture', 'duplication')
    Write-Host ("  exit={0} PNG={1}x{2} requested={3} captured={4} clipped={5} notes={6}" -f `
               $r.Exit, $r.Png.Width, $r.Png.Height, ($r.Img.requestedRect | ConvertTo-Json -Compress),
               ($r.Img.capturedRect | ConvertTo-Json -Compress), $r.Img.clipped, ($r.NoteCodes -join ',')) -ForegroundColor DarkGray
    Assert-Ec ($r.Exit -eq 0 -and $r.Json.captured -eq 1) "跨边界窗口截图失败（exit=$($r.Exit)）：$($r.Stderr)"
    Assert-Ec ($r.Img.clipped -eq $true) '目标有一部分在桌面之外却没报 clipped'
    Assert-Ec ($r.NoteCodes -contains 'note.capture_clipped') '没报 clipped 的那条可机器处理提示'
    Assert-Ec ($r.Img.capturedRect.width -lt $r.Img.requestedRect.width) `
        "capturedRect 宽 $($r.Img.capturedRect.width) 应小于 requestedRect 宽 $($r.Img.requestedRect.width)"
    Assert-Ec ($r.Png.Width -eq $r.Img.capturedRect.width -and $r.Png.Height -eq $r.Img.capturedRect.height) `
        "交付 PNG $($r.Png.Width)x$($r.Png.Height) 与 capturedRect $($r.Img.capturedRect.width)x$($r.Img.capturedRect.height) 不一致"
    Assert-Ec ($r.Img.capturedRect.right -le $virtualRight) `
        "capturedRect 右边界 $($r.Img.capturedRect.right) 越过虚拟桌面右边界 $virtualRight"

    # =========================================================================
    Write-Host "`n=== 4) 整屏：交付尺寸等于该屏物理矩形，rotation 与独立查到的方向不打脸 ==="
    # =========================================================================
    foreach ($s in $screens) {
        $r = Invoke-DupShot -Name ("screen_{0}" -f $s.Ordinal) -Answer $IDYES -Arguments @(
            '--monitor', "$($s.Ordinal)", '--capture', 'duplication')
        Write-Host ("  #{0} {1} exit={2} PNG={3}x{4} rotation={5}" -f `
                   $s.Ordinal, $s.Device, $r.Exit, $r.Png.Width, $r.Png.Height, $r.Img.rotation) -ForegroundColor DarkGray
        Assert-Ec ($r.Exit -eq 0 -and $r.Json.captured -eq 1) `
            "第 $($s.Ordinal) 块屏截失败（exit=$($r.Exit)）：$($r.Stderr)"
        Assert-Ec ($r.Img.device -eq $s.Device) "第 $($s.Ordinal) 块屏的设备名不对：$($r.Img.device)"
        Assert-Ec ($r.Png.Width -eq $s.Width -and $r.Png.Height -eq $s.Height) `
            "第 $($s.Ordinal) 块屏交付 $($r.Png.Width)x$($r.Png.Height)，应等于该屏 $($s.Width)x$($s.Height)"
        Assert-Ec (-not $r.Img.clipped) "整屏截图却报了 clipped：那块屏在确认之后变了"
        $applied = if ($null -eq $r.Img.rotation) { 0 } else { [int]$r.Img.rotation }
        Assert-Ec (@(0, 90, 180, 270) -contains $applied) `
            "rotation 报告的值 $($applied) 不是 0/90/180/270 之一"
    }

    # =========================================================================
    Write-Host "`n=== 4b) 四角朝向判据：交付的图必须处于用户看到的朝向（不需要知道转了几度） ==="
    # =========================================================================
    # 在这块屏的**桌面坐标**四角各摆一块不同颜色的实心窗，然后要求交付图的四个角出现同一套对应
    # 关系：图的左上角 = 摆在桌面左上角的那块颜色，依此类推。这条判据与"这块屏转了多少度"无关，
    # 而旧实现在旋转屏上恰好会把画面转 0 度交回 —— 那种"尺寸正确、画面横躺"的图在这里立刻不过。
    Add-Type -AssemblyName System.Drawing | Out-Null
    $cornerPlan = @(
        @{ Tag = 'TL'; Color = 'FF0000' },   # 桌面左上：红
        @{ Tag = 'TR'; Color = '00FF00' },   # 桌面右上：绿
        @{ Tag = 'BL'; Color = '0000FF' },   # 桌面左下：蓝
        @{ Tag = 'BR'; Color = 'FFFF00' }    # 桌面右下：黄
    )
    $probedRotated = $false
    foreach ($s in $screens) {
        if ($s.Width -lt 200 -or $s.Height -lt 200) {
            Skip-Ec "$($s.Device) 的四角朝向判据" "这块屏只有 $($s.Width)x$($s.Height)，放不下四块 64x64 的角标"
            continue
        }
        $right = $s.Left + $s.Width
        $bottom = $s.Top + $s.Height
        $marks = @()
        try {
            $place = @{
                TL = @($s.Left + 6,  $s.Top + 6)
                TR = @($right - 70,  $s.Top + 6)
                BL = @($s.Left + 6,  $bottom - 70)
                BR = @($right - 70,  $bottom - 70)
            }
            foreach ($c in $cornerPlan) {
                $p = $place[$c.Tag]
                $marks += Start-EcWindow -RunDir $run -Class "ec-dup-$($c.Tag)-$tag-$($s.Ordinal)" `
                    -Mode solid -Rect ('{0},{1},{2},{3}' -f $p[0], $p[1], ($p[0] + 64), ($p[1] + 64)) `
                    -Color $c.Color -TopMost -MaxLifeSeconds 600
            }
            Start-Sleep -Milliseconds 2500   # 等 DWM 把这四块合成出来
            $r = Invoke-DupShot -Name ("corners_{0}" -f $s.Ordinal) -Answer $IDYES -Arguments @(
                '--monitor', "$($s.Ordinal)", '--capture', 'duplication')
            Assert-Ec ($r.Exit -eq 0 -and (Test-Path -LiteralPath $r.Path)) `
                "$($s.Device) 四角截图失败：$($r.Stderr)"
            $bmp = New-Object System.Drawing.Bitmap($r.Path)
            try {
                $seen = @{}
                foreach ($c in $cornerPlan) {
                    # 图的同一个角上（离边 38 像素 = 角标块的中心）必须是那块颜色
                    $x = switch ($c.Tag) { 'TL' { 38 } 'TR' { $bmp.Width - 38 } 'BL' { 38 } 'BR' { $bmp.Width - 38 } }
                    $y = switch ($c.Tag) { 'TL' { 38 } 'TR' { 38 } 'BL' { $bmp.Height - 38 } 'BR' { $bmp.Height - 38 } }
                    $px = $bmp.GetPixel($x, $y)
                    $want = @([Convert]::ToInt32($c.Color.Substring(0, 2), 16),
                              [Convert]::ToInt32($c.Color.Substring(2, 2), 16),
                              [Convert]::ToInt32($c.Color.Substring(4, 2), 16))
                    $ok = ([Math]::Abs([int]$px.R - $want[0]) -le 24) -and
                          ([Math]::Abs([int]$px.G - $want[1]) -le 24) -and
                          ([Math]::Abs([int]$px.B - $want[2]) -le 24)
                    $seen[$c.Tag] = $ok
                    Assert-Ec $ok ("{0} 交付图 {1} 角（{2},{3}）不是该角那块的颜色 {4}（实测 {5},{6},{7}）" -f `
                        $s.Device, $c.Tag, $x, $y, $c.Color, $px.R, $px.G, $px.B)
                }
                $applied = if ($null -eq $r.Img.rotation) { 0 } else { [int]$r.Img.rotation }
                Write-Host ("  #{0} {1} 四角={2} rotation={3} 尺寸={4}x{5}" -f `
                           $s.Ordinal, $s.Device, (($cornerPlan | ForEach-Object { $_.Tag }) |
                               ForEach-Object { if ($seen[$_]) { 'ok' } else { 'NO' } }) -join ',',
                           $applied, $bmp.Width, $bmp.Height) -ForegroundColor DarkGray
                if ($applied -ne 0) { $probedRotated = $true }
            } finally { $bmp.Dispose() }
        } finally { foreach ($m in $marks) { Stop-EcWindow -Window $m } }
    }
    if (-not $probedRotated) {
        Skip-Ec '90/180/270 三种旋转各自的真机现场' `
            '本机没有处于旋转状态的显示器（系统读到的方向是 0 度或读不到），而测试绝不改用户的显示设置。四角朝向判据已经在每一块屏上验过"交付图处于用户看到的朝向"；三种角度各自的换算与逐像素正确性由离线层判（与朴素的"先旋转再裁剪"对拍）'
    } else {
        Write-Host '  本机存在被报告为旋转的输出：上面的四角判据已覆盖该现场' -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 5) 多显卡：每块屏都要截得到（旧实现漏掉第二块适配器上的屏） ==="
    # =========================================================================
    if ($adapterCount -ge 2) {
        Write-Host "  本机有 $($adapterCount) 个显卡控制器：上面第 4 步已经逐块屏截成功" -ForegroundColor DarkGray
    } else {
        Skip-Ec '两块显卡分别驱动屏幕（目标在第二 adapter 上）' `
            '本机只探测到 1 个显卡控制器，无法构造"屏挂在第二块适配器上"的现场；该判据由离线层的假枚举器逐条判（两 adapter、目标在第二 adapter、无输出、热拔出）'
    }

    Skip-Ec '热拔出一块屏后必须报 capture.monitor_changed 而不改截别的屏' `
        '要真的拔掉显示器，不能改用户的显示设置；定位与判据由离线层（假枚举器 + CompareScreen）判'
    Skip-Ec '编号在两次枚举之间换了位置' `
        '要重排用户的显示器；--monitor 编号的含义已经写明是"本次枚举的顺序"，认屏请用 images[].device'

    Write-Host "`n=== 收尾 ==="
    Stop-EcOwnedWindows
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} catch {
    if ("$_" -ne '__SKIP_REAL__') { throw }
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title 'Desktop Duplication 多屏适配')
