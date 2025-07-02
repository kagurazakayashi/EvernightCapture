<#
.SYNOPSIS
    真机整屏截图测试（--monitor）：验证屏幕通道的尺寸、坐标映射与"真的抓到了屏幕内容"。
.DESCRIPTION
    判据不是"有没有报错 / 文件多大"，而是三件事同时成立：
      1. 图片宽高 == 该屏在系统里的物理像素矩形（抓错屏、偏移、DPI 缩放都会在这里露出来）
      2. 目标屏左上角摆一个置顶纯红小方块之后，图片对应位置必须是红
         —— 这条能区分"画面整体平移了"和"画面确实是这块屏"
      3. 撤掉红块后同一位置必须不再红 —— 上面那条的阴性对照，
         否则"该位置是红"可能只是恰好成立
    还检查 --monitor all 的出图数量与每块屏各自的尺寸、--monitor 1 --out - 的通道分离
    （PNG 占 stdout，JSON 走 stderr）。
    会在屏幕左上角短暂弹出一个置顶纯红小方块（约 3 秒），结束或失败时自动关闭。
    屏幕编号取的是 EnumDisplayMonitors 的顺序，所以测试里的"1 号屏"就是 $screens[0]，
    主屏只用来做 SM_CXSCREEN 的自洽校验。
.EXAMPLE
    .\tests\screen.ps1
    .\tests\screen.ps1 -Keep        # 保留截图以便人眼看
    .\tests\screen.ps1 -Channels wgc,bitblt
#>
param(
    [string]$Exe,
    [string[]]$Channels = @('wgc', 'duplication', 'bitblt', 'auto'),
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
if (-not $Exe) { $Exe = Join-Path $root 'build\ecapture.exe' }
if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe（先运行 .\build.ps1）" }
$Exe = (Get-Item -LiteralPath $Exe).FullName

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$outDir = Join-Path ([IO.Path]::GetTempPath()) 'ecapture-screen'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ---------------------------------------------------------------------------
# 独立判据：自己枚举显示器矩形。必须先让本进程 DPI aware，否则拿到的是缩放后的
# 虚拟坐标，与工具（per-monitor v2）看到的物理像素对不上。
# ---------------------------------------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace EcScreen {
  public static class Win {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MONITORINFOEX {
      public int cbSize; public RECT rcMonitor; public RECT rcWork; public int dwFlags;
      [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
    }
    public delegate bool EnumProc(IntPtr hMon, IntPtr hdc, ref RECT r, IntPtr data);
    [DllImport("user32")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32")] public static extern bool SetProcessDPIAware();
    [DllImport("user32")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, EnumProc proc, IntPtr data);
    [DllImport("user32", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr hMon, ref MONITORINFOEX info);
    [DllImport("user32")] public static extern int GetSystemMetrics(int idx);
    [DllImport("user32")] public static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("gdi32")] public static extern int GetDeviceCaps(IntPtr dc, int idx);
    [DllImport("user32")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
  }
}
'@

# -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2；老系统上退化成 SetProcessDPIAware
[void][EcScreen.Win]::SetProcessDpiAwarenessContext([IntPtr](-4))
[void][EcScreen.Win]::SetProcessDPIAware()

$screens = New-Object System.Collections.Generic.List[object]
$cb = [EcScreen.Win+EnumProc] {
    param($hMon, $hdc, [ref]$r, $data)
    $info = New-Object EcScreen.Win+MONITORINFOEX
    $info.cbSize = [Runtime.InteropServices.Marshal]::SizeOf([type][EcScreen.Win+MONITORINFOEX])
    if ([EcScreen.Win]::GetMonitorInfo($hMon, [ref]$info)) {
        $b = $info.rcMonitor
        $screens.Add([pscustomobject]@{
            Ordinal = $screens.Count + 1
            Device = $info.szDevice
            Left = [int]$b.Left; Top = [int]$b.Top
            Width = [int]($b.Right - $b.Left); Height = [int]($b.Bottom - $b.Top)
            Primary = (([int]$info.dwFlags -band 1) -ne 0)
        })
    }
    return $true
}
[void][EcScreen.Win]::EnumDisplayMonitors([IntPtr]::Zero, [IntPtr]::Zero, $cb, [IntPtr]::Zero)
if (-not $screens.Count) { throw '本机枚举不到任何显示器' }

$dc = [EcScreen.Win]::GetDC([IntPtr]::Zero)
$dpi = [EcScreen.Win]::GetDeviceCaps($dc, 88)   # LOGPIXELSX
[void][EcScreen.Win]::ReleaseDC([IntPtr]::Zero, $dc)
$primary = @($screens | Where-Object { $_.Primary })
if (-not $primary.Count) { $primary = @($screens[0]) }
$smCx = [EcScreen.Win]::GetSystemMetrics(0)     # SM_CXSCREEN
$smCy = [EcScreen.Win]::GetSystemMetrics(1)
Write-Host ("显示器 {0} 块；1 号屏 {1} {2}x{3}+{4}+{5}；主屏 {6} {7}x{8}，SM_CXSCREEN={9}x{10}，DPI={11}" -f `
           $screens.Count, $screens[0].Device, $screens[0].Width, $screens[0].Height,
           $screens[0].Left, $screens[0].Top, $primary[0].Device, $primary[0].Width,
           $primary[0].Height, $smCx, $smCy, $dpi)
if ($smCx -ne $primary[0].Width -or $smCy -ne $primary[0].Height) {
    throw ("判据自身不可信：SM_CXSCREEN 与 EnumDisplayMonitors 不一致（DPI 虚拟化？Scale={0}%）" -f `
           [math]::Round($dpi * 100 / 96))
}

# ---------------------------------------------------------------------------
# 红色小方块：摆在 1 号屏左上角，用来证明"这张图确实是这块屏，而且没平移"。
# 必须是独立进程并自己泵消息，否则父脚本忙着截图时它不会重绘。
# ---------------------------------------------------------------------------
$markScript = @'
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
$markPath = Join-Path $outDir 'mark.ps1'
Set-Content -LiteralPath $markPath -Value $markScript -Encoding ASCII

function Start-Mark($screen) {
    $x = [int]$screen.Left + 8; $y = [int]$screen.Top + 8
    Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile',
        '-ExecutionPolicy', 'Bypass', '-File', $markPath, $x, $y, ($x + 64), ($y + 64))
}
function Stop-Mark($proc) {
    if ($proc) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

$fails = [System.Collections.Generic.List[string]]::new()
function Assert([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "  PASS  $what" -ForegroundColor DarkGreen }
    else { $fails.Add($what); Write-Host "  FAIL  $what" -ForegroundColor Red }
}

function Is-Red($m) { $m -and $m.R -gt 190 -and $m.G -lt 70 -and $m.B -lt 70 }

# 读图：尺寸、指定位置的像素、颜色数（采样步长放大，4K 屏也别停太久）
function Measure-Shot([string]$path, [int]$px, [int]$py) {
    if (-not (Test-Path $path)) { return $null }
    $bmp = [Drawing.Bitmap]::new($path)
    try {
        $colors = @{}
        for ($y = 0; $y -lt $bmp.Height; $y += 23) {
            for ($x = 0; $x -lt $bmp.Width; $x += 23) {
                $c = $bmp.GetPixel($x, $y); $colors["$($c.R),$($c.G),$($c.B)"] = 1
            }
        }
        $probe = $bmp.GetPixel($px, $py)
        [pscustomobject]@{
            Width = $bmp.Width; Height = $bmp.Height; Colors = $colors.Count
            R = [int]$probe.R; G = [int]$probe.G; B = [int]$probe.B
        }
    } finally { $bmp.Dispose() }
}

# 分别读两个流：PS 5.1 的 2>&1 会把原生 stderr 包成错误记录文字，那样没法校验 JSON。
# stdout 按字节读，--out - 时它就是 PNG。
function Invoke-EcapProcess([string[]]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.Arguments = ($Arguments | ForEach-Object {
        if ($_ -eq '' -or $_ -match '[\s"]') { '"' + (($_ -replace '(\\+)', '$1$1') -replace '"', '\"') + '"' }
        else { $_ } }) -join ' '
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $ms = New-Object System.IO.MemoryStream
    $buf = New-Object byte[] 65536
    while ($true) {
        $n = $p.StandardOutput.BaseStream.Read($buf, 0, $buf.Length)
        if ($n -le 0) { break }
        $ms.Write($buf, 0, $n)
    }
    $se = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    [pscustomobject]@{ Exit = $p.ExitCode; Bytes = $ms.ToArray(); Err = $se
                       Text = [Text.Encoding]::UTF8.GetString($ms.ToArray()) }
}

$target = $screens[0]      # 工具编号 1 = EnumDisplayMonitors 的第一个
$probeX = 40
$probeY = 40
$mark = $null
try {
    # ---------- 1) 每条屏幕通道：尺寸 + 红块位置 + 颜色数 + JSON 形状 ----------
    Write-Host "`n=== 1 号屏左上角已放红色小方块：每条通道都要抓到它 ==="
    $mark = Start-Mark $target
    Start-Sleep -Milliseconds 2500
    foreach ($ch in $Channels) {
        $path = Join-Path $outDir ("m1_{0}.png" -f $ch)
        Remove-Item $path -ErrorAction SilentlyContinue
        $r = Invoke-EcapProcess @('--monitor', '1', '--capture', $ch, '--out', $path, '-v')
        $o = $null
        try { $o = $r.Text | ConvertFrom-Json } catch { }
        $m = Measure-Shot $path $probeX $probeY
        Write-Host ("  {0,-12} exit={1} {2}x{3} colors={4} probe=({5},{6},{7})" -f `
                   $ch, $r.Exit, $m.Width, $m.Height, $m.Colors, $m.R, $m.G, $m.B)
        Assert ($r.Exit -eq 0 -and $o -and $o.captured -eq 1) `
            "$ch 通道应成功出 1 张图（exit=$($r.Exit)）"
        Assert ($m) "$ch 通道没写出 PNG 文件"
        if ($m) {
            Assert ($m.Width -eq $target.Width -and $m.Height -eq $target.Height) `
                "$ch 通道尺寸 $($m.Width)x$($m.Height)，应等于 1 号屏 $($target.Width)x$($target.Height)"
            Assert ($m.Colors -ge 12) "$ch 通道颜色过少（$($m.Colors)），可能是空帧"
            Assert (Is-Red $m) "$ch 通道 ($probeX,$probeY) 不是红色（$($m.R),$($m.G),$($m.B)），画面平移或没抓到屏幕"
        }
        if ($o) {
            $img = @($o.images)[0]
            Assert ($img.monitor -eq 1 -and $img.device -eq $target.Device) `
                "$ch 通道 JSON 的 monitor/device 不对：monitor=$($img.monitor) device=$($img.device)"
            Assert ($img.primary -eq $target.Primary) `
                "$ch 通道 JSON 的 primary 不对：$($img.primary) 期望 $($target.Primary)"
            Assert ($o.input.target -eq 'screen') "$ch 通道的 input 回显没标 target=screen"
            $fields = @($img.PSObject.Properties.Name)
            foreach ($k in @('hwnd', 'pid', 'title', 'class', 'image')) {
                Assert ($fields -notcontains $k) "屏幕图不该带窗口字段 $k"
            }
        }
    }

    # ---------- 2) 阴性对照：撤掉红块后同一位置必须不再红 ----------
    Write-Host "`n=== 撤掉红块：红色判据必须失效（否则上面的断言没有区分力）==="
    Stop-Mark $mark
    $mark = $null
    Start-Sleep -Milliseconds 1500
    $path = Join-Path $outDir 'negative.png'
    Remove-Item $path -ErrorAction SilentlyContinue
    [void](Invoke-EcapProcess @('--monitor', '1', '--capture', 'bitblt', '--out', $path))
    $neg = Measure-Shot $path $probeX $probeY
    Write-Host ("  {0,-12} probe=({1},{2},{3})" -f 'bitblt', $neg.R, $neg.G, $neg.B)
    Assert ($neg -and -not (Is-Red $neg)) `
        '阴性对照不成立：没有红块时该位置也是红的'

    # ---------- 3) --monitor all：出图数量与每块屏各自的尺寸 ----------
    Write-Host "`n=== --monitor all：每块屏一张，尺寸各自对上 ==="
    $allPattern = Join-Path $outDir 'all_%i.png'
    Remove-Item (Join-Path $outDir 'all_*.png') -ErrorAction SilentlyContinue
    $r = Invoke-EcapProcess @('--monitor', 'all', '--out', $allPattern, '-v')
    $o = $null
    try { $o = $r.Text | ConvertFrom-Json } catch { }
    Assert ($r.Exit -eq 0 -and $o -and $o.captured -eq $screens.Count) `
        "--monitor all 应出 $($screens.Count) 张：exit=$($r.Exit) captured=$($o.captured)"
    foreach ($sc in $screens) {
        $p = Join-Path $outDir ("all_{0}.png" -f $sc.Ordinal)
        $m = Measure-Shot $p 2 2
        Assert ($m -and $m.Width -eq $sc.Width -and $m.Height -eq $sc.Height) `
            ("{0} 尺寸不对：{1}x{2} 期望 {3}x{4}" -f $sc.Device, $m.Width, $m.Height, $sc.Width, $sc.Height)
        $img = @($o.images | Where-Object { $_.monitor -eq $sc.Ordinal })
        Assert ($img -and $img.device -eq $sc.Device) "$($sc.Device) 没出现在 images 里或设备名不对"
    }

    # ---------- 4) --monitor + 窗口条件：按屏过滤，出的仍是窗口图 ----------
    Write-Host "`n=== --monitor 1 + 窗口条件：按屏过滤，JSON 仍是窗口形状 ==="
    $r = Invoke-EcapProcess @('--monitor', '1', '--class', 'Shell_TrayWnd', '--dry-run', '--out', '-', '-v')
    $o = $null
    try { $o = $r.Err | ConvertFrom-Json } catch { }
    Assert ($r.Exit -eq 0 -and $o -and $o.input.target -eq 'window') `
        "按屏过滤模式没跑起来：exit=$($r.Exit)"
    Assert (@($o.notes | ForEach-Object code) -contains 'note.dry_run') '按屏过滤时 dry-run 仍应报 note.dry_run'
    $r = Invoke-EcapProcess @('--monitor', '99', '--class', 'Shell_TrayWnd', '--dry-run', '--out', '-', '-v')
    $o = $null
    try { $o = $r.Err | ConvertFrom-Json } catch { }
    Assert ($r.Exit -eq 1 -and (@($o.errors | ForEach-Object code) -contains 'match.monitor_out_of_range')) `
        "按屏过滤时屏幕编号越界没报 match.monitor_out_of_range（exit=$($r.Exit)）"

    # ---------- 5) 屏幕目标走 stdout：PNG 在 stdout，JSON 在 stderr ----------
    Write-Host "`n=== --monitor 1 --out - ：图片字节占 stdout，JSON 走 stderr ==="
    $r = Invoke-EcapProcess @('--monitor', '1', '--capture', 'bitblt', '--out', '-')
    $o = $null
    try { $o = $r.Err | ConvertFrom-Json } catch { }
    $b = $r.Bytes
    $isPng = $b.Length -gt 8 -and $b[0] -eq 0x89 -and $b[1] -eq 0x50 -and $b[2] -eq 0x4E -and $b[3] -eq 0x47
    $tmp = Join-Path $outDir 'stdout.png'
    [IO.File]::WriteAllBytes($tmp, $b)
    $m = Measure-Shot $tmp 2 2
    Write-Host ("  stdout {0} 字节，PNG 头={1}，尺寸 {2}x{3}" -f $b.Length, $isPng, $m.Width, $m.Height)
    Assert ($r.Exit -eq 0 -and $isPng) 'stdout 不是 PNG 字节'
    Assert ($o -and $o.captured -eq 1 -and @($o.images)[0].monitor -eq 1) 'stderr 里的 JSON 缺 captured/images'
    Assert ($m.Width -eq $target.Width -and $m.Height -eq $target.Height) `
        "stdout 的 PNG 尺寸 $($m.Width)x$($m.Height) 与 1 号屏不符"
} finally {
    Stop-Mark $mark
    if (-not $Keep) { Get-ChildItem $outDir -Filter *.png -ErrorAction SilentlyContinue | Remove-Item -Force }
    else { Write-Host "截图保留在 $outDir" -ForegroundColor DarkGray }
}

Write-Host ''
if ($fails.Count) {
    Write-Host "整屏测试失败：$($fails.Count) 项" -ForegroundColor Red
    $fails | ForEach-Object { Write-Host "  - $_" }
} else {
    Write-Host '整屏测试全部通过' -ForegroundColor Green
}
exit [int][bool]$fails.Count
