<#
.SYNOPSIS
    只读屏幕枚举（--screens）与按标识精确选屏（--monitor=device: / --monitor=id:）的判据：
    离线注入假候选逐条判决据与文档形状；真机判"这份列表与本机另两条独立问答对得上号"
    "交回的标识原样抄回 --monitor 就点得到那块屏""不认识的标识绝不退化成主屏"。
.DESCRIPTION
    这个功能立的规矩：

      1) **只读**：一个像素都不取、不弹确认框、不写文件、不联网，也**不改任何显示设置** ——
         为了"看清这块屏转了多少度"去调 SetDisplayConfig 等于把考卷改了再答题。
      2) **一份列表交回几种身份，并逐条写清它稳到哪一层**：编号只是本次枚举的位置、
         设备名只是本次桌面连接的、监视器 devnode 路径与适配器设备路径才是跨会话的、
         适配器 LUID 只在这次会话里唯一。所以只有设备名与 devnode 路径有选择器写法。
      3) **点名叫屏不许靠猜**：`--monitor <n>` 的数字形式保留、语义不变（帮助里就怎么写），
         但调用方要的是那一块屏时，抄 --screens 交回的 device:… 或 id:… 回去即可。
      4) **定位不到就报错，绝不静默换一块屏**：没有 / 有多个 / 问不出来三种下场各一条稳定码；
         "改用主屏凑一张"是最坏的失败形状，因为那张画面没人批准过。
      5) **问不出来 ≠ 空值**：DPI、旋转、devnode 路径、适配器路径各带自己的下场与系统原因码。
      6) **真去截整屏仍然一定弹框**，`--yes` 对桌面像素那一级不生效（登记表见 CaptureScope.cpp）。

    判据分两层：
      1) 离线层：build\ecapture-screens-tests.exe（源码 tests\screens_state.cpp）注入假候选，逐条判
         "设备名被重新发给另一块面板""同一标识命中两块屏""显示配置整路问不出来""副屏在负坐标"
         "复制模式两条路径共享一个桌面""读不到的字段没被写成 0"。这些现场本机造不出来 ——
         本机只接了一块屏，也不能为测试去动使用者的显示设置或拔线。
      2) 真机层：拿**另外两条独立的问答**（.NET 的 Screen::AllScreens 与 Win32 的
         EnumDisplayDevices）对照 --screens 交回的矩形、设备名与监视器设备路径；再把交回的
         标识原样写回 --monitor 跑一次 --dry-run（不取帧、不弹框）。

    隐私规矩：这一层不拍任何东西，也不代人点框。要弹框的那条判据（整屏截图确实一定弹框）
    只在显式给了 -SimulateConsent 时才由测试侧代答"否"（拒绝不拍到任何东西），
    默认整条记未验证。

.EXAMPLE
    .\tests\screens.ps1
    .\tests\screens.ps1 -SkipState       # 只跑真机层
    .\tests\screens.ps1 -SkipReal        # 只跑离线判据层（没有交互桌面时用）
    .\tests\screens.ps1 -SimulateConsent # 加上它才代答确认框（只答"否"），判整屏一定弹框那条
    .\tests\screens.ps1 -Exe build\Debug\ecapture.exe
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$SkipReal,
    [switch]$SimulateConsent
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-screens-tests.exe'

# 屏幕查询一律 15 秒期限：它真去弹框或真去截一张图的话这里会等死，期限本身就是判据。
function Invoke-Sq {
    param([Parameter(Mandatory)][string[]]$Arguments, [int]$TimeoutMs = 15000,
          [string]$WorkingDirectory)
    $r = Invoke-EcProcess -FilePath $Exe -Arguments (@('--lang', 'zh-CN') + @($Arguments)) `
                          -TimeoutMs $TimeoutMs -WorkingDirectory $WorkingDirectory
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $json = $null
    try { $json = $body | ConvertFrom-Json } catch { }
    return [pscustomobject]@{ Raw = $r; Exit = $r.Exit; Json = $json; Body = $body }
}

function Codes($list) { if ($null -eq $list) { @() } else { @($list | ForEach-Object { $_.code }) } }

# 本机屏幕的**第二条独立问答**：.NET 的 Screen::AllScreens（自己调 EnumDisplayMonitors +
# GetMonitorInfo），用来对照 --screens 交回的矩形、设备名与主屏标记。
$dotNetType = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows.Forms;

public static class EcScreensRef {
    public static List<string[]> Dump() {
        var rows = new List<string[]>();
        foreach (Screen s in Screen.AllScreens) {
            rows.Add(new string[] {
                s.DeviceName,
                s.Bounds.X.ToString(), s.Bounds.Y.ToString(),
                s.Bounds.Width.ToString(), s.Bounds.Height.ToString(),
                s.Primary ? "1" : "0" });
        }
        return rows;
    }
}
'@
Add-Type -AssemblyName System.Windows.Forms | Out-Null
Add-Type -AssemblyName System.Drawing | Out-Null
# Screen.Bounds 的类型是 System.Drawing.Rectangle，所以那条引用不能少（实测报 CS0618 之外的
# "未引用的程序集"，第一次跑就撞上了）。
Add-Type -TypeDefinition $dotNetType -ReferencedAssemblies `
    'System.Windows.Forms', 'System.Drawing', 'mscorlib', 'System' | Out-Null

# 本机监视器设备路径的**第二条独立问答**：EnumDisplayDevices 第二层带
# EDD_GET_DEVICE_INTERFACE_NAME，交回的就是监视器的设备接口路径。它与 --screens 的
# monitorDevicePath 来自不同的调用链，所以能真正核对"那个字段不是编出来的"。
$devPathType = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class EcDevPaths {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DISPLAY_DEVICE {
        public int cb;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]  public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
        public int StateFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 50)]  public string DeviceKey;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum,
                                                 ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion; public short dmDriverVersion; public short dmSize;
        public short dmDriverExtra; public int dmFields;
        public int dmPositionX; public int dmPositionY; public int dmDisplayOrientation;
        public int dmDisplayFixedOutput; public short dmColor; public short dmDuplex;
        public short dmYResolution; public short dmTTOption; public short dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels; public int dmBitsPerPel; public int dmPelsWidth;
        public int dmPelsHeight; public int dmDisplayFlags; public int dmDisplayFrequency;
        public int dmICMMethod; public int dmICMIntent; public int dmMediaType;
        public int dmDitherType; public int dmReserved1; public int dmReserved2;
        public int dmPanningWidth; public int dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern bool EnumDisplaySettings(string lpszDeviceName, int iModeNum,
                                                  ref DEVMODE lpDevMode);

    // 每块**视图设备**（\\.\DISPLAYn）下面挂着的监视器：DeviceID 就是设备接口路径。
    public static List<string[]> MonitorPaths() {
        var rows = new List<string[]>();
        for (uint i = 0; i < 16; ++i) {
            var ad = new DISPLAY_DEVICE();
            ad.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
            if (!EnumDisplayDevices(null, i, ref ad, 0)) break;
            for (uint j = 0; j < 8; ++j) {
                var mon = new DISPLAY_DEVICE();
                mon.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
                if (!EnumDisplayDevices(ad.DeviceName, j, ref mon, 0x1)) break;  // EDD_GET_DEVICE_INTERFACE_NAME
                rows.Add(new string[] { ad.DeviceName, mon.DeviceID });
            }
        }
        return rows;
    }

    // 当前模式的朝向（0/1/2/3 = 0/90/180/270 度）与像素尺寸，独立问一遍。
    public static int[] CurrentOrientation(string device) {
        var dm = new DEVMODE();
        dm.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
        if (!EnumDisplaySettings(device, -1 /* ENUM_CURRENT_SETTINGS */, ref dm)) {
            return new int[] { -1, -1, -1 };
        }
        return new int[] { dm.dmDisplayOrientation, dm.dmPelsWidth, dm.dmPelsHeight };
    }
}
'@
Add-Type -TypeDefinition $devPathType | Out-Null

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：把假候选注进判据本体逐条判 ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '屏幕枚举与选屏离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-screens-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "屏幕判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        $failed = if ($m.Success) { [int]$m.Groups[2].Value } else { -1 }
        $passed = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
        Assert-Ec ($failed -eq 0) "屏幕判据有失败项，或摘要读不出来：$tail"
        Assert-Ec ($passed -ge 50) "屏幕判据通过数不对劲（$passed），用例被删了？"
        Write-Host "  $tail" -ForegroundColor DarkGray
    }

    if ($SkipReal) {
        Write-Host '  真机层按调用方要求跳过' -ForegroundColor DarkGray
        exit (Complete-EcSuite -Title '屏幕枚举与精确选屏（只离线层）')
    }
    Set-EcDpiAware

    # =========================================================================
    Write-Host "`n=== 1) 真机：这份列表确实只读 —— 不落地、不弹框、不弹第二条 ==="
    # =========================================================================
    $run = New-EcRunDir -Tag 'sq-readonly'
    try {
        $a = Invoke-Sq -Arguments @('--screens') -WorkingDirectory $run.Path
        Assert-Ec ($a.Exit -eq 0) "--screens 没返回 0（exit=$($a.Exit)）：$($a.Raw.Stderr)"
        Assert-Ec ($a.Json.contract -eq 'screens') "契约名不对：$($a.Json.contract)"
        Assert-Ec ($a.Raw.StdErr.Trim() -eq '') "--screens 往 stderr 写了东西：$($a.Raw.Stderr)"
        Assert-Ec ($a.Json.authorization.pixelsRead -eq 0 -and
                    $a.Json.authorization.consentDialogShown -eq $false -and
                    $a.Json.authorization.filesWritten -eq $false -and
                    $a.Json.authorization.displaySettingsChanged -eq $false) `
            '这份文档自己声称取了像素/弹了框/写了文件/改了显示设置'
        $left = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left -eq 0) "--screens 之后本次目录里还剩 $left 个文件，它不应该写任何东西"

        # 改显示设置这件事不能只信自述：跑前跑后各问一次拓扑（.NET 那条独立问答），必须一致。
        $before = [EcScreensRef]::Dump()
        $null = Invoke-Sq -Arguments @('--screens', '-v')
        $after = [EcScreensRef]::Dump()
        $sameBefore = ($before.Count -eq $after.Count)
        for ($i = 0; $i -lt [Math]::Min($before.Count, $after.Count); ++$i) {
            if (($before[$i] -join ',') -ne ($after[$i] -join ',')) { $sameBefore = $false }
        }
        Assert-Ec $sameBefore '跑过一次 --screens 之后本机拓扑变了（这一路不许动任何显示设置）'

        # --yes 在这条路上算冲突：它属于截图授权那一级，而一次不出图的查询没有可授权的事。
        $y = Invoke-Sq -Arguments @('--screens', '--yes') -WorkingDirectory $run.Path
        Assert-Ec ($y.Exit -eq 1 -and (Codes $y.Json.errors) -contains 'cli.query_conflict') `
            "--screens --yes 应该是冲突（exit=$($y.Exit) codes=[$($(Codes $y.Json.errors) -join ','))]"
        $left2 = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left2 -eq 0) "冲突那次之后本次目录里还剩 $left2 个文件"
    } finally {
        Remove-EcRunDir $run
    }

    # =========================================================================
    Write-Host "`n=== 2) 与本机另两条独立问答对照：矩形、主屏、监视器设备路径、朝向 ==="
    # =========================================================================
    $q = Invoke-Sq -Arguments @('--screens')
    $screens = @($q.Json.screens)
    $refRows = @([EcScreensRef]::Dump())
    Assert-Ec ($screens.Count -eq $refRows.Count -and $screens.Count -ge 1) `
        "--screens 报了 $($screens.Count) 块屏，.NET 那条独立问答报了 $($refRows.Count) 块"

    $refByName = @{}
    foreach ($row in $refRows) {
        $refByName[$row[0]] = [pscustomobject]@{ X = [int]$row[1]; Y = [int]$row[2]
                                                 ; W = [int]$row[3]; H = [int]$row[4]
                                                 ; Primary = ($row[5] -eq '1') }
    }
    foreach ($s in $screens) {
        $ref = $refByName[$s.deviceName]
        Assert-Ec ($null -ne $ref) "设备名 $($s.deviceName) 在 .NET 那条问答里不存在"
        if ($null -eq $ref) { continue }
        Assert-Ec ($s.rect.x -eq $ref.X -and $s.rect.y -eq $ref.Y -and
                    $s.rect.width -eq $ref.W -and $s.rect.height -eq $ref.H) `
            "$($s.deviceName) 的矩形对不上：--screens 给 $($s.rect.x),$($s.rect.y) $($s.rect.width)x$($s.rect.height)，对照给 $($ref.X),$($ref.Y) $($ref.W)x$($ref.H)"
        Assert-Ec ([bool]$s.primary -eq $ref.Primary) "$($s.deviceName) 的主屏标记对不上"
        # 朝向：--screens 那一问读的是 DEVMODE 的当前模式，这里另调一次同一条 API 的另一个入口
        $ori = [EcDevPaths]::CurrentOrientation($s.deviceName)
        if ($ori[0] -ge 0) {
            Assert-Ec ([int]$s.rotation.degrees -eq ($ori[0] * 90)) `
                "$($s.deviceName) 的朝向对不上：--screens 给 $($s.rotation.degrees)，独立问答给 $($ori[0])（0/1/2/3 = 0/90/180/270 度）"
        } else {
            Skip-Ec "$($s.deviceName) 的朝向对照" 'EnumDisplaySettings 这次没能给出当前模式（与 --screens 里 readability.rotation 的下场一致才算对）'
        }
        Assert-Ec ($s.dpi.effectiveX -gt 0 -and $s.dpi.effectiveY -gt 0) `
            "$($s.deviceName) 的 DPI 没问出来（本机是 Win11，shcore 那条应该有答案）"
    }
    $primaryCount = @($screens | Where-Object { $_.primary }).Count
    Assert-Ec ($primaryCount -le 1) "$primaryCount 块屏被标成主屏，最多只能有一块"

    # 监视器设备路径：EnumDisplayDevices 的第二层是**另一条**拿法，逐字比一次。
    $devRows = @([EcDevPaths]::MonitorPaths())
    foreach ($s in $screens) {
        if (-not $s.selectors.id) {
            Skip-Ec "$($s.deviceName) 的监视器设备路径对照" `
                    "--screens 这一问没给出 id（readability.monitorDevicePath=$($s.readability.monitorDevicePath.state)），对照由离线层判"
            continue
        }
        $want = $s.selectors.id.Substring(3)   # 去掉 "id:" 前缀
        $hit = @($devRows | Where-Object { $_[0] -eq $s.deviceName -and $_[1] -eq $want })
        Assert-Ec ($hit.Count -ge 1) `
            "$($s.deviceName) 的跨会话标识与 EnumDisplayDevices 交回的那条不一致：--screens=[$want]"
    }

    # 两次调用之间同一条标识逐字相同（本机能判的就这么多；跨重启与跨会话没有实测，
    # 文档里的 caveats 就钉的是这件事，别把它写成"保证跨重启不变"）。
    $again = @((Invoke-Sq -Arguments @('--screens')).Json.screens)
    $stableOk = ($again.Count -eq $screens.Count)
    for ($i = 0; $i -lt [Math]::Min($again.Count, $screens.Count); ++$i) {
        if ($again[$i].selectors.id -ne $screens[$i].selectors.id -or
            $again[$i].selectors.device -ne $screens[$i].selectors.device) { $stableOk = $false }
    }
    Assert-Ec $stableOk '同一次会话里两次 --screens 交回的标识不一样（那这份列表就没法用来点名了）'

    # =========================================================================
    Write-Host "`n=== 3) 交回的标识原样写回 --monitor 就点得到那块屏 ==="
    # =========================================================================
    $run3 = New-EcRunDir -Tag 'sq-select'
    try {
        foreach ($s in $screens) {
            foreach ($kind in @('device', 'id')) {
                $sel = $s.selectors.$kind
                if (-not $sel) {
                    Skip-Ec "$($s.deviceName) 的 $kind 选择器回写" '这一问没给出那条标识'
                    continue
                }
                $r = Invoke-Sq -Arguments @('--monitor', $sel, '--dry-run', '-v', 'out.png') `
                               -WorkingDirectory $run3.Path
                Assert-Ec ($r.Exit -eq 0) `
                    "--monitor $sel --dry-run 失败（exit=$($r.Exit) codes=[$($(Codes $r.Json.errors) -join ',')])"
                Assert-Ec ($r.Json.input.monitorKind -eq $kind -and
                            $r.Json.input.target -eq 'screen' -and
                            $r.Json.input.monitor -eq $sel) `
                    "--monitor $sel 的回显不对劲：kind=$($r.Json.input.monitorKind) target=$($r.Json.input.target)"
                # 选中的必须是那块屏本身：dry-run 那一行里有设备名与尺寸
                $line = [string](@($r.Json.notes | Where-Object { $_.code -eq 'note.dry_run' }).value)
                Assert-Ec ($line -like ('*' + $s.displayName + '*') -and
                            $line -like ('*' + $s.rect.width + 'x' + $s.rect.height + '*')) `
                    "--monitor $sel 选中的不是那块屏：dry-run 行是 [$line]"
                # 按屏过滤窗口那一路共用同一条判据（--list 也认这两条标识）
                $l = Invoke-Sq -Arguments @('--list', '--monitor', $sel)
                Assert-Ec ($l.Exit -eq 0 -and $l.Json.contract -eq 'windowquery') `
                    "--list --monitor $sel 没走通（exit=$($l.Exit)）"
            }
        }
        $left3 = @(Get-ChildItem -LiteralPath $run3.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left3 -eq 0) "--dry-run 那几次之后本次目录里还剩 $left3 个文件"

        # 旧的数字形式语义没动：编号还是"本次枚举顺序里的位置"，与上面按标识点到的同一块屏。
        $byNum = Invoke-Sq -Arguments @('--monitor', '1', '--dry-run', '-v', 'out.png') `
                           -WorkingDirectory $run3.Path
        Assert-Ec ($byNum.Exit -eq 0 -and $byNum.Json.input.monitorKind -eq 'ordinal') `
            "--monitor 1 那条老写法不再按编号解释了（exit=$($byNum.Exit) kind=$($byNum.Json.input.monitorKind)）"
        Assert-Ec ($byNum.Json.input.monitor -eq 1) `
            "--monitor 1 的回显变成别的东西了：$($byNum.Json.input.monitor)"

        # 不认识的标识：报错，且**不**去截主屏。
        foreach ($bad in @('device:NOSUCHSCREEN', 'id:\\?\DISPLAY#NOPE#0')) {
            $e = Invoke-Sq -Arguments @('--monitor', $bad, '--dry-run', 'out.png') `
                           -WorkingDirectory $run3.Path
            Assert-Ec ($e.Exit -eq 4) "--monitor $bad 期望退出码 4（实际 $($e.Exit)）"
            Assert-Ec ((Codes $e.Json.errors) -contains 'match.monitor_unknown_id') `
                "--monitor $bad 报的不是 match.monitor_unknown_id：[$($(Codes $e.Json.errors) -join ',')]"
            Assert-Ec ($e.Json.captured -eq 0 -and @($e.Json.images).Count -eq 0) `
                "--monitor $bad 竟然出了图：静默换了一块屏就是这条判据要拦的事"
            $leftBad = @(Get-ChildItem -LiteralPath $run3.Path -Recurse -File -ErrorAction SilentlyContinue).Count
            Assert-Ec ($leftBad -eq 0) "标识不存在那次之后还剩 $leftBad 个文件"
        }

        # 写法不合语法的三条：都在解析期就拒，不去猜"是不是想要主屏"。
        $emptyId = Invoke-Sq -Arguments @('--monitor', 'device:', 'out.png')
        Assert-Ec ($emptyId.Exit -eq 1 -and (Codes $emptyId.Json.errors) -contains 'cli.monitor_selector_empty') `
            "空标识没被报成 cli.monitor_selector_empty（codes=[$($(Codes $emptyId.Json.errors) -join ',')]）"
        $badKind = Invoke-Sq -Arguments @('--monitor=foo:1', 'out.png')
        Assert-Ec ($badKind.Exit -eq 1 -and (Codes $badKind.Json.errors) -contains 'cli.monitor_selector_kind') `
            "不认识的前缀没被报成 cli.monitor_selector_kind（codes=[$($(Codes $badKind.Json.errors) -join ',')]）"
        $pathNotEaten = Invoke-Sq -Arguments @('--monitor', 'D:\shots\a.png', '--dry-run', '-v')
        Assert-Ec ($pathNotEaten.Exit -eq 0 -and $pathNotEaten.Json.input.monitorKind -eq 'primary' -and
                    $pathNotEaten.Json.input.output -like '*a.png') `
            "盘符路径被 --monitor 吃掉了（output=$($pathNotEaten.Json.input.output)）"
    } finally {
        Remove-EcRunDir $run3
    }

    # =========================================================================
    Write-Host "`n=== 4) 这份文档里的取舍写得出来，不靠调用方读源码 ==="
    # =========================================================================
    $doc = Invoke-Sq -Arguments @('--screens', '-v')
    Assert-Ec ($doc.Json.identity.deviceName.usableAsSelector -eq $true -and
                $doc.Json.identity.monitorDevicePath.usableAsSelector -eq $true -and
                $doc.Json.identity.adapterLuid.usableAsSelector -eq $false) `
        '身份那一段没有把"哪两种能拿去点名"写清楚'
    Assert-Ec ($doc.Json.identity.adapterLuid.stableAcross -eq 'this_session' -and
                $doc.Json.identity.ordinal.stableAcross -eq 'this_invocation') `
        '稳定性写错了：编号只是本次枚举的位置，LUID 只在本次会话里唯一'
    Assert-Ec (@($doc.Json.caveats) -contains 'cross_session_stability_not_tested') `
        '跨会话稳定性本项目只在同一会话里观察过，文档必须自己钉住这一点'
    Assert-Ec (@($doc.Json.caveats) -contains 'screen_capture_always_asks' -and
                $doc.Json.authorization.screenCaptureConsent.yesSkipsThisLevel -eq $false) `
        '整屏一定要问人、--yes 跳不过这件事没写在文档里'
    Assert-Ec ($doc.Json.privacy.includesFileSystemPaths -eq $false -and
                $doc.Json.privacy.includesUsernames -eq $false -and
                $doc.Json.privacy.includesDevicePaths -eq $true) `
        '隐私自述与实际不符：这里有设备路径，没有文件系统路径与用户名'
    Assert-Ec ($doc.Json.limits.maxOrdinal -eq 65535) `
        "limits.maxOrdinal 与解析层不同源（实际 $($doc.Json.limits.maxOrdinal)）"
    # 每条屏都带着适配器关联（显卡的 LUID 与它自己的设备接口路径），这是"哪块屏挂在哪块卡上"的答案
    foreach ($s in @($doc.Json.screens)) {
        Assert-Ec ($s.adapter.luid -and $s.adapter.outputTechnology) `
            "$($s.deviceName) 没有适配器关联信息（luid=$($s.adapter.luid)）"
    }

    # =========================================================================
    Write-Host "`n=== 5) 整屏截图仍然强制人工确认（--yes 不生效）==="
    # =========================================================================
    if (-not $SimulateConsent) {
        $firstId = $screens[0].selectors.id
        if (-not $firstId) { $firstId = $screens[0].selectors.device }
        Skip-Ec '按标识选屏之后真去截整屏：一定弹框、--yes 跳不过、答"否"不落地' `
                '这要弹一个真确认框。测试侧代答等于替人回答一次隐私授权，只在显式给了 -SimulateConsent 且桌面没有隐私内容时才做（本工具不给任何免确认旁路）'
    } else {
        $run5 = New-EcRunDir -Tag 'sq-consent'
        try {
            $sel = $screens[0].selectors.id
            if (-not $sel) { $sel = $screens[0].selectors.device }
            # 桌面像素那一级：带 --yes 也要问；测试侧只答"否"，所以一张都不该落地。
            $shot = Invoke-EcConsentShot -Exe $Exe -Answer 7 `
                        -Arguments @('--lang', 'zh-CN', '--monitor', $sel, '--yes',
                                     '--capture', 'duplication',
                                     (Join-Path $run5.Path 'shot.png'))
            Assert-Ec ($shot.Dialog) '按标识选屏 + --yes 截整屏竟然没弹框（桌面像素那一级必须问人）'
            Assert-Ec ($shot.Exit -eq 6) "答\"否\"之后退出码应为 6，实际 $($shot.Exit)"
            $body = if ($shot.Stdout.Trim()) { $shot.Stdout } else { $shot.Stderr }
            $o = $null
            try { $o = $body | ConvertFrom-Json } catch { }
            Assert-Ec ($o -and (@($o.errors | ForEach-Object { $_.code }) -contains 'capture.access_denied')) `
                "拒绝的原因没照实交回：[$body]"
            $files = @(Get-ChildItem -LiteralPath $run5.Path -Recurse -File -ErrorAction SilentlyContinue).Count
            Assert-Ec ($files -eq 0) "答\"否\"之后本次目录里还剩 $files 个文件，被拒绝的截图绝不落地"
        } finally {
            Remove-EcRunDir $run5
        }
    }

    # =========================================================================
    Write-Host "`n=== 6) 本机不具备条件、照实记未验证的部分 ==="
    # =========================================================================
    if ($screens.Count -lt 2) {
        Skip-Ec '多屏现场：副屏在负坐标、跨屏窗口在两块屏上都算命中、--monitor all 每块屏各一张' `
                ('本机只接了 ' + $screens.Count + ' 块屏。为测试去接第二台显示器或改使用者的显示设置都不允许；这些现场的判据由离线层注入假候选逐条判（负坐标并集、两条路径共享一个桌面、同一标识命中两块）')
    } else {
        $neg = @($screens | Where-Object { $_.rect.x -lt 0 -or $_.rect.y -lt 0 })
        if ($neg.Count -eq 0) {
            Skip-Ec '负坐标的副屏' '本机几块屏都在主屏右下方，虚拟屏幕并集没有出现负坐标'
        } else {
            Write-Host "  本机确有负坐标的屏：$(@($neg | ForEach-Object { $_.deviceName }) -join ', ')" -ForegroundColor DarkGray
        }
        $rot = @($screens | Where-Object { [int]$_.rotation.degrees -ne 0 })
        if ($rot.Count -eq 0) {
            Skip-Ec '旋转屏（90/270 度时交付图朝向与纹理尺寸的关系）' `
                    '本机没有竖屏，而为测试改使用者的屏幕朝向是禁止的；那条判据由 tests\dup.ps1 的离线层逐条注入判'
        }
    }
    Skip-Ec '热插拔之后"设备名被重新发给另一块面板"的真实现场' `
            '这要真的拔掉并重新插入显示器，本机做不到也不该做。这一条由离线层注入"同名不同 devnode"的假候选判（结果是 capture.monitor_changed 而绝不是照名字截下去）'
    Skip-Ec '跨重启、跨会话之后同一条标识仍然有效' `
            '本项目只在一台机器的同一个会话里观察过（文档 caveats 里的 cross_session_stability_not_tested 钉的就是这点）。要判这条得重启或换会话，属于使用者的机器状态，不代做'
    Skip-Ec '复制模式（两块面板共享一个桌面）下的 pathsMatched 与 clonedScreens' `
            '本机没接两台面板，也不能为测试去按 Win+P 改使用者的投影模式；判据本体由离线层注入假候选判'
    Skip-Ec '没有可交互桌面的会话里这份文档的形状' `
            '本机就是一个有人盯着的交互桌面。这一路与弹框无关（它本来一条像素都不取）'
} finally {
    Stop-EcOwnedWindows
}

exit (Complete-EcSuite -Title '屏幕枚举与精确选屏')
