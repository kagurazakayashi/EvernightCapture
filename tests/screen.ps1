<#
.SYNOPSIS
    真机整屏截图测试（--monitor）：验证确认框行为、屏幕通道的尺寸、坐标映射与"真的抓到了屏幕内容"。
.DESCRIPTION
    判据不是"有没有报错 / 文件多大"，而是：
      1. 全屏截图一定会弹确认框；答"否"必须 capture.access_denied + 退出码 6 且不写出文件
      2. --dry-run 不许弹框（弹了就说明它偷偷取了帧）
      3. 图片宽高 == 该屏在系统里的物理像素矩形（抓错屏、偏移、DPI 缩放都会在这里露出来）
      4. 目标屏左上角摆一个置顶纯红小方块之后，图片对应位置必须是红
         —— 这条能区分"画面整体平移了"和"画面确实是这块屏"
      5. 撤掉红块后同一位置必须不再红 —— 上面那条的阴性对照
    还检查 --monitor all 的出图数量与每块屏各自的尺寸、--monitor 1 --out - 的通道分离。

    关于确认框：工具本身没有跳过确认的开关，测试也不会给它开后门。默认运行**不去碰**屏幕上
    真的弹出来的确认框（那是给人回答的），只跑不弹框的那几项并把需要回答框的项记成 SKIP。
    只有在专门腾出来的、没有隐私内容的桌面上，才用 -SimulateConsent 让测试代答：
    它找到本进程弹出的 #32770 后按控件 ID（IDYES=6 / IDNO=7）点掉，与文案语言无关。
    代答的实现在测试这一侧，发布版 CLI 里没有任何开关或环境变量能跳过确认框。
    红块窗口由 tests\helper\ec_window.cs 编出的自有程序建立，只收尾自己起的那个进程。
.EXAMPLE
    .\tests\screen.ps1                          # 只跑不弹框的那些项，其余记 SKIP
    .\tests\screen.ps1 -SimulateConsent         # 无隐私专用桌面：代答确认框，跑全部
    .\tests\screen.ps1 -SimulateConsent -Keep   # 并保留截图
    .\tests\screen.ps1 -SimulateConsent -Channels wgc,bitblt
#>
param(
    [string]$Exe,
    [string[]]$Channels = @('wgc', 'duplication', 'bitblt', 'auto'),
    [switch]$SimulateConsent,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Reset-EcSuite

$run = New-EcRunDir -Tag 'screen'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

# ---------------------------------------------------------------------------
# 独立判据：自己枚举显示器矩形（不借工具的结论）。必须先让本进程 DPI aware，
# 否则拿到的是缩放后的虚拟坐标，与工具（per-monitor v2）看到的物理像素对不上。
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
    [DllImport("user32")] public static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, EnumProc proc, IntPtr data);
    [DllImport("user32", CharSet = CharSet.Unicode)] public static extern bool GetMonitorInfo(IntPtr hMon, ref MONITORINFOEX info);
    [DllImport("user32")] public static extern int GetSystemMetrics(int idx);
    [DllImport("user32")] public static extern IntPtr GetDC(IntPtr hwnd);
    [DllImport("gdi32")] public static extern int GetDeviceCaps(IntPtr dc, int idx);
    [DllImport("user32")] public static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
  }
}
'@
Set-EcDpiAware

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

$IDYES = 6
$IDNO = 7

# ---------------------------------------------------------------------------
# 起工具进程：需要代答时用 -Probe 在等待期间点掉确认框；不代答时根本不去碰它。
# 两条流的读取由调用器并发完成（PNG 走 stdout 时不能被 stderr 堵住，反之也一样）。
# ---------------------------------------------------------------------------
function Invoke-EcapProcess2 {
    param(
        [Parameter(Mandatory)][string[]]$Arguments,
        [int]$Answer = 0,
        [switch]$ExpectNoDialog,
        [int]$DialogWaitMs = 8000
    )

    $script:EcDialogSeen = $false
    $script:EcDialogClicked = $false
    $probe = {
        param($p)
        $h = Find-EcDialog -ProcessId ([int]$p.Id)
        if ($h -eq [IntPtr]::Zero) { return $false }
        $script:EcDialogSeen = $true
        if ($Answer -gt 0) { $script:EcDialogClicked = (Click-EcDialogButton -Dialog $h -ButtonId $Answer) }
        return $true
    }
    $waitMs = 800
    if (-not $ExpectNoDialog) { $waitMs = $DialogWaitMs }
    $r = Invoke-EcProcess -FilePath $Exe -Arguments $Arguments -TimeoutMs 120000 `
        -Probe $probe -ProbeIntervalMs 60 -ProbeTimeoutMs $waitMs
    return [pscustomobject]@{
        Exit = $r.Exit; Bytes = $r.StdoutBytes; Err = $r.Stderr; Text = $r.Stdout
        Dialog = $script:EcDialogSeen; Clicked = $script:EcDialogClicked; Raw = $r
    }
}

function Read-Shot([string]$Path, [int]$Px, [int]$Py) {
    $stats = Get-EcImageStats -Path $Path -Step 23
    if (-not $stats) { return $null }
    Add-Type -AssemblyName System.Drawing | Out-Null
    $bmp = New-Object System.Drawing.Bitmap($Path)
    try {
        $c = $bmp.GetPixel($Px, $Py)
        $stats | Add-Member -NotePropertyName ProbeR -NotePropertyValue ([int]$c.R) -Force
        $stats | Add-Member -NotePropertyName ProbeG -NotePropertyValue ([int]$c.G) -Force
        $stats | Add-Member -NotePropertyName ProbeB -NotePropertyValue ([int]$c.B) -Force
    } finally { $bmp.Dispose() }
    return $stats
}

$target = $screens[0]      # 工具编号 1 = EnumDisplayMonitors 的第一个
$probeX = 40
$probeY = 40
$mark = $null
$skippedWhenNoSimulate = @(
    '确认框：一定弹、答"否"必须不落地',
    '每条屏幕通道：尺寸 + 红块位置 + 颜色数 + JSON 形状',
    '阴性对照：撤掉红块后同一位置必须不再红',
    '--monitor all：一次确认，每块屏一张',
    '--monitor 1 --out - ：PNG 字节占 stdout，JSON 走 stderr'
)

try {
    if (-not $SimulateConsent) {
        Write-Host "`n!! 未给 -SimulateConsent：不代答屏幕上的确认框，下面这些项记为 SKIP" -ForegroundColor DarkYellow
        Write-Host '   无人值守跑不出这些结果；在无隐私的专用桌面上加 -SimulateConsent 再跑一次。' -ForegroundColor DarkYellow
        foreach ($s in $skippedWhenNoSimulate) { Skip-Ec $s '没有 -SimulateConsent，确认框留给人回答' }
    }

    # ---------- 0) 确认框本身：一定弹；答否就不截图也不写文件 ----------
    if ($SimulateConsent) {
        Write-Host "`n=== 确认框：弹得出、答否必须不落地 ==="
        $path = Get-EcRunFile -RunDir $run -Name 'refused.png'
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $r = Invoke-EcapProcess2 @('--monitor', '1', '--capture', 'bitblt', '--out', $path) -Answer $IDNO
        Assert-Ec $r.Dialog '全屏截图没有弹确认框'
        Assert-Ec $r.Clicked '确认框上的"否"没点到（按钮找不到或窗口已关）' -Quiet
        Assert-Ec ($r.Exit -eq 6) "答`"否`"的退出码 $($r.Exit)，应为 6"
        $o = $null
        try { $o = $r.Text | ConvertFrom-Json } catch { }
        Assert-Ec ($o -and (@($o.errors | ForEach-Object code) -contains 'capture.access_denied')) `
            '答"否"没报 capture.access_denied'
        Assert-Ec ($o.captured -eq 0 -and @($o.images).Count -eq 0) '答"否"不该有图'
        Assert-Ec (-not (Test-Path -LiteralPath $path)) '答"否"却写出了文件'
    }

    Write-Host "`n=== --dry-run 不许弹确认框（它不该取帧）==="
    $dryPath = Get-EcRunFile -RunDir $run -Name 'dry.png'
    $r = Invoke-EcapProcess2 @('--monitor', '1', '--dry-run', '--out', $dryPath) -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) '--dry-run 弹了确认框'
    Assert-Ec ($r.Exit -eq 0) "--dry-run 退出码 $($r.Exit)，应为 0"
    Assert-Ec (-not (Test-Path -LiteralPath $dryPath)) '--dry-run 不该写出文件'

    # ---------- 4) --monitor + 窗口条件：按屏过滤，不该弹框（与是否代答无关）----------
    Write-Host "`n=== --monitor 1 + 窗口条件：按屏过滤，不弹框，JSON 仍是窗口形状 ==="
    $r = Invoke-EcapProcess2 @('--monitor', '1', '--class', 'Shell_TrayWnd', '--dry-run', '--out', '-', '-v') `
            -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) '按屏过滤（不截整屏）弹了确认框'
    $o = $null
    try { $o = $r.Err | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $o -and $o.input.target -eq 'window') "按屏过滤模式没跑起来：exit=$($r.Exit)"
    Assert-Ec (@($o.notes | ForEach-Object code) -contains 'note.dry_run') '按屏过滤时 dry-run 仍应报 note.dry_run'
    $r = Invoke-EcapProcess2 @('--monitor', '99', '--class', 'Shell_TrayWnd', '--out', '-', '-v') -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) '屏幕编号越界时不该先弹框（越界是参数错）'
    $o = $null
    try { $o = $r.Err | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 1 -and (@($o.errors | ForEach-Object code) -contains 'match.monitor_out_of_range')) `
        "按屏过滤时屏幕编号越界没报 match.monitor_out_of_range（exit=$($r.Exit)）"

    if (-not $SimulateConsent) {
        # 后面每一条都要真的取整屏画面：确认框没人回答就一直阻塞，所以整段跳过
        Write-Host "`n=== 需要代答确认框的项已跳过，见上面的 SKIP ==="
        exit (Complete-EcSuite -Title '整屏测试')
    }

    # ---------- 1) 每条屏幕通道：尺寸 + 红块位置 + 颜色数 + JSON 形状 ----------
    Write-Host "`n=== 1 号屏左上角放上红色小方块：每条通道都要抓到它 ==="
    $markRect = '{0},{1},{2},{3}' -f ($target.Left + 8), ($target.Top + 8), ($target.Left + 72), ($target.Top + 72)
    $mark = Start-EcWindow -RunDir $run -Class "ec-mark-$tag" -Mode solid -Rect $markRect `
        -Color 'FF0000' -TopMost -MaxLifeSeconds 600
    Start-Sleep -Milliseconds 2500
    foreach ($ch in $Channels) {
        $path = Get-EcRunFile -RunDir $run -Name ("m1_{0}.png" -f $ch)
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $r = Invoke-EcapProcess2 @('--monitor', '1', '--capture', $ch, '--out', $path, '-v') -Answer $IDYES
        $o = $null
        try { $o = $r.Text | ConvertFrom-Json } catch { }
        $m = Read-Shot $path $probeX $probeY
        Write-Host ("  {0,-12} exit={1} {2}x{3} colors={4} probe=({5},{6},{7})" -f `
                   $ch, $r.Exit, $m.Width, $m.Height, $m.Colors, $m.ProbeR, $m.ProbeG, $m.ProbeB)
        Assert-Ec $r.Dialog "$ch 那一次没弹确认框"
        Assert-Ec $r.Clicked "$ch 那一次的确认框没点到`"是`"" -Quiet
        Assert-Ec ($r.Exit -eq 0 -and $o -and $o.captured -eq 1) "$ch 通道应成功出 1 张图（exit=$($r.Exit)）"
        Assert-Ec ($m) "$ch 通道没写出 PNG 文件"
        if ($m) {
            Assert-Ec ($m.Width -eq $target.Width -and $m.Height -eq $target.Height) `
                "$ch 通道尺寸 $($m.Width)x$($m.Height)，应等于 1 号屏 $($target.Width)x$($target.Height)"
            Assert-Ec ($m.Colors -ge 12) "$ch 通道颜色过少（$($m.Colors)），可能是空帧"
            Assert-Ec ($m.ProbeR -gt 190 -and $m.ProbeG -lt 70 -and $m.ProbeB -lt 70) `
                "$ch 通道 ($probeX,$probeY) 不是红色（$($m.ProbeR),$($m.ProbeG),$($m.ProbeB)），画面平移或没抓到屏幕"
        }
        if ($o) {
            $img = @($o.images)[0]
            Assert-Ec ($img.monitor -eq 1 -and $img.device -eq $target.Device) `
                "$ch 通道 JSON 的 monitor/device 不对：monitor=$($img.monitor) device=$($img.device)"
            Assert-Ec ($img.primary -eq $target.Primary) `
                "$ch 通道 JSON 的 primary 不对：$($img.primary) 期望 $($target.Primary)"
            Assert-Ec ($o.input.target -eq 'screen') "$ch 通道的 input 回显没标 target=screen"
            $fields = @($img.PSObject.Properties.Name)
            foreach ($k in @('hwnd', 'pid', 'title', 'class', 'image')) {
                Assert-Ec ($fields -notcontains $k) "屏幕图不该带窗口字段 $k"
            }
        }
    }

    # ---------- 2) 阴性对照：撤掉红块后同一位置必须不再红 ----------
    Write-Host "`n=== 撤掉红块：红色判据必须失效（否则上面的断言没有区分力） ==="
    Stop-EcWindow -Window $mark
    $mark = $null
    Start-Sleep -Milliseconds 1500
    $path = Get-EcRunFile -RunDir $run -Name 'negative.png'
    Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
    [void](Invoke-EcapProcess2 @('--monitor', '1', '--capture', 'bitblt', '--out', $path) -Answer $IDYES)
    $neg = Read-Shot $path $probeX $probeY
    Write-Host ("  {0,-12} probe=({1},{2},{3})" -f 'bitblt', $neg.ProbeR, $neg.ProbeG, $neg.ProbeB)
    Assert-Ec ($neg -and -not ($neg.ProbeR -gt 190 -and $neg.ProbeG -lt 70 -and $neg.ProbeB -lt 70)) `
        '阴性对照不成立：没有红块时该位置也是红的'

    # ---------- 3) --monitor all：只弹一次框，每块屏一张 ----------
    Write-Host "`n=== --monitor all：一次确认，每块屏一张，尺寸各自对上 ==="
    $allPattern = Get-EcRunFile -RunDir $run -Name 'all_%i.png'
    Remove-Item -LiteralPath (Get-EcRunFile -RunDir $run -Name 'all_*.png') -ErrorAction SilentlyContinue
    $r = Invoke-EcapProcess2 @('--monitor', 'all', '--out', $allPattern, '-v') -Answer $IDYES
    $o = $null
    try { $o = $r.Text | ConvertFrom-Json } catch { }
    Assert-Ec $r.Dialog '--monitor all 没弹确认框'
    Assert-Ec ($r.Exit -eq 0 -and $o -and $o.captured -eq $screens.Count) `
        "--monitor all 应出 $($screens.Count) 张：exit=$($r.Exit) captured=$($o.captured)"
    foreach ($sc in $screens) {
        $p = Get-EcRunFile -RunDir $run -Name ("all_{0}.png" -f $sc.Ordinal)
        $m = Read-Shot $p 2 2
        Assert-Ec ($m -and $m.Width -eq $sc.Width -and $m.Height -eq $sc.Height) `
            ("{0} 尺寸不对：$(if($m){$m.Width})x$(if($m){$m.Height}) 期望 $($sc.Width)x$($sc.Height)" -f $sc.Device)
        $img = @($o.images | Where-Object { $_.monitor -eq $sc.Ordinal })
        Assert-Ec ($img -and $img.device -eq $sc.Device) "$($sc.Device) 没出现在 images 里或设备名不对"
    }

    # ---------- 5) 屏幕目标走 stdout：PNG 在 stdout，JSON 在 stderr ----------
    Write-Host "`n=== --monitor 1 --out - ：图片字节占 stdout，JSON 走 stderr ==="
    $r = Invoke-EcapProcess2 @('--monitor', '1', '--capture', 'bitblt', '--out', '-') -Answer $IDYES
    $o = $null
    try { $o = $r.Err | ConvertFrom-Json } catch { }
    $b = $r.Bytes
    $isPng = $b.Length -gt 8 -and $b[0] -eq 0x89 -and $b[1] -eq 0x50 -and $b[2] -eq 0x4E -and $b[3] -eq 0x47
    $tmp = Get-EcRunFile -RunDir $run -Name 'stdout.png'
    [IO.File]::WriteAllBytes($tmp, $b)
    $m = Read-Shot $tmp 2 2
    Write-Host ("  stdout {0} 字节，PNG 头={1}，尺寸 {2}x{3}" -f $b.Length, $isPng, $m.Width, $m.Height)
    Assert-Ec ($r.Exit -eq 0 -and $isPng) 'stdout 不是 PNG 字节'
    Assert-Ec ($o -and $o.captured -eq 1 -and @($o.images)[0].monitor -eq 1) 'stderr 里的 JSON 缺 captured/images'
    Assert-Ec ($m.Width -eq $target.Width -and $m.Height -eq $target.Height) `
        "stdout 的 PNG 尺寸 $($m.Width)x$($m.Height) 与 1 号屏不符"
} finally {
    if ($mark) { Stop-EcWindow -Window $mark }
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '整屏测试')
