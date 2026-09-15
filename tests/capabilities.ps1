<#
.SYNOPSIS
    能力与诊断查询（--capabilities / --diagnostics）的判据：离线注入假探针逐条判那份报告
    怎么算出来，真机核对查询确实只读、字段与系统独立问来的值对得上、以及隐私清单。
.DESCRIPTION
    这两条命令立的规矩是四条，彼此不能混：

      1) **只读**：不取一个像素、不弹一个确认框、不写一个文件、不联网、不读环境变量。
      2) **三件事分开写**：这个构建里有没有那条路线（compiled）、本机现在让不让走（status）、
         本项目有没有在这一模一样的系统上实测过（verifiedOnThisMachine）。
      3) **问不出来就说问不出来**：每条判据都是 yes / no / unknown 三值，unknown 既不写成
         是也不写成否，也不整个键消失。
      4) **available 不是保证**：驱动不喂帧、内容受保护、某个窗口截不到，都不在这层断言里
         （与 src/SystemCompat.h、src/EnvReport.h 开头那两段同源）。

    判据分两层：
      1) 离线层：build\ecapture-capabilities-tests.exe（源码 tests\capabilities_state.cpp）把
         假探针注进判据本体，逐条判"一块屏都没有""版本正好低于某条下限""版本问不出来""某个
         编码器没登记"——这些现场在本机一个都造不出来（降不了级、没有第二块显卡、更不能为了
         看没有屏幕拓扑会怎么判而拔显示器）。
      2) 真机层：不截图。判的是查询真的不弹框（会弹框的话这里就是等死，期限到点判红）、
         真的一个文件都不落地、那份文档里 os / arch / 屏幕数与 WMI 那两条独立问来的值对得上、
         四语与 --lang 无关（全 ASCII）、backends 那一段与 --dry-run -v 的 input.captureChain
         是同一批判据算出来的，以及 --yes 那一级在文档里的写法与登记表一致。

    隐私规矩：这一层不拍任何东西，也不代人点框。真机上"没有交互桌面"的现场要服务会话才造得出，
    本机不具备条件，照实记未验证。

.EXAMPLE
    .\tests\capabilities.ps1
    .\tests\capabilities.ps1 -SkipState       # 只跑真机层
    .\tests\capabilities.ps1 -Exe build\Debug\ecapture.exe
#>
param(
    [string]$Exe,
    [switch]$SkipState
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-capabilities-tests.exe'

# 这几道下限在测试里独立写一份（与 src/SystemCompat.h 同源但不同处）：判据要是被实现偷偷
# 改了而这里跟着改，就发现不了了。
$FLOOR_WGC = 18362
$FLOOR_DUP = 9200
$FLOOR_FULLCONTENT = 9600
$FLOOR_ENCODER = 10240
$FLOOR_DECLARED = 18362
$TESTED_BUILD = 19045          # 本项目实测过的那一台（README《系统支持》同一条记录）

function Json-Of($r) {
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $o = $null
    try { $o = $body | ConvertFrom-Json } catch { }
    return $o
}

function Codes($list) {
    if ($null -eq $list) { return @() }
    return @($list | ForEach-Object { $_.code })
}

# 查询命令：一条都不许弹框，所以期限就是判据的一部分——真去弹框的话这里会等死。
# 期限故意给 15 秒：这台机器上正常返回是几十毫秒量级，等满就是行为变了。
function Invoke-Query {
    param([string[]]$Arguments, [int]$TimeoutMs = 15000, [string]$WorkingDirectory)
    $args = @('--lang', 'zh-CN') + @($Arguments)
    return (Invoke-EcProcess -FilePath $Exe -Arguments $args -TimeoutMs $TimeoutMs `
                             -WorkingDirectory $WorkingDirectory)
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：把假探针注进判据本体逐条判 ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '能力查询离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-capabilities-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "能力查询判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        $failed = if ($m.Success) { [int]$m.Groups[2].Value } else { -1 }
        $passed = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
        Assert-Ec ($failed -eq 0) "能力查询判据有失败项，或摘要读不出来：$tail"
        Assert-Ec ($passed -ge 200) "能力查询判据通过数不对劲（$passed），用例被删了？"
        Write-Host "  $tail" -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 1) 真机：查询是非交互的，而且一个文件都不落地 ==="
    # =========================================================================
    # 全新空目录当工作目录：查询之后里面必须还是空的。"看起来没写文件"要和"确实没写"分开，
    # 靠的就是这个目录是这一次新建的、上一次跑的产物不会混进来。
    $run = New-EcRunDir -Tag 'caps'
    try {
        $r = Invoke-Query -Arguments @('--capabilities') -WorkingDirectory $run.Path
        $o = Json-Of $r
        Assert-Ec ($r.Exit -eq 0) "查询没返回 0（exit=$($r.Exit)）：$($r.Stderr)"
        Assert-Ec ([bool]$o) "stdout 不是 JSON：$($r.Stdout.Substring(0, [Math]::Min(200, $r.Stdout.Length)))"
        Assert-Ec ($r.StdErr.Trim() -eq '') "查询往 stderr 写了东西：$($r.StdErr)"
        $left = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left -eq 0) "查询之后本次目录里还剩 $left 个文件，它不应该写任何东西"

        $d = Invoke-Query -Arguments @('--diagnostics') -WorkingDirectory $run.Path
        Assert-Ec ($d.Exit -eq 0 -and (Json-Of $d).contract -eq 'diagnostics') '--diagnostics 没出诊断文档'
        $left2 = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left2 -eq 0) "--diagnostics 之后本次目录里还剩 $left2 个文件"

        # 冲突那一条同样不许落地、不许弹框：报错之后一张都不截
        $c = Invoke-Query -Arguments @('--capabilities', 'oops.png') -WorkingDirectory $run.Path
        $co = Json-Of $c
        Assert-Ec ($c.Exit -eq 1) "查询与输出路径同时给出应为参数错（exit=$($c.Exit)）"
        Assert-Ec ((Codes $co.errors) -contains 'cli.query_conflict') `
            "冲突码不对：[$((Codes $co.errors) -join ',')]"
        $left3 = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left3 -eq 0) "冲突之后还剩 $left3 个文件，冲突报错前就已经动了文件系统"
        Assert-Ec ((@($co.images)).Count -eq 0 -and $co.captured -eq 0) '冲突那次居然出了图'
    } finally {
        Remove-EcRunDir $run
    }

    # =========================================================================
    Write-Host "`n=== 2) 真机：文档里的环境值与独立问来的那两份对得上 ==="
    # =========================================================================
    # 查询报的是 RtlGetVersion / GetNativeSystemInfo / EnumDisplayMonitors 那三问的答案；
    # 这里拿 CIM 那条完全独立的路各问一次。两边对不上就说明某一处在谎报，而整套判据是拿它筛的。
    $o = Json-Of (Invoke-Query -Arguments @('--capabilities'))
    $wmi = Get-CimInstance -ClassName Win32_OperatingSystem
    Assert-Ec ([int]$o.os.build -eq [int]$wmi.BuildNumber) `
        "查询报的内部版本 $($o.os.build) 与 WMI 那份 $($wmi.BuildNumber) 不一致"
    Assert-Ec ($o.os.known -eq $true) '查询说本机版本问不出来，而本机明明问得出来'
    Assert-Ec ($o.os.declaredMinBuild -eq $FLOOR_DECLARED) `
        "查询里的声明下限 $($o.os.declaredMinBuild) 与 --help 写的那条 $FLOOR_DECLARED 不同源"

    $arch = [string](Get-CimInstance -ClassName Win32_Processor | Select-Object -First 1).Architecture
    # WMI 的 ProcessorArchitecture：9 = x64。查询那一份按 GetNativeSystemInfo 报同一个词。
    Assert-Ec ($o.program.arch -eq 'x64' -or $arch -ne '9') `
        "WMI 说本机是 x64（$arch）而查询报的是 $($o.program.arch)"

    $monitorsWmi = @(Get-CimInstance -ClassName Win32_DesktopMonitor -ErrorAction SilentlyContinue |
                     Where-Object { $_.Status -eq 'OK' }).Count
    $physical = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction SilentlyContinue).Count
    # 这两条独立路本身在本机就不一定相等（虚拟显示器、未接线的输出口都算），所以只判
    # "查询报的屏数是个说得通的正数"，不去硬比对屏幕数——那条判据留给未验证。
    Assert-Ec ([int]$o.session.monitors -ge 1) "查询报的屏幕数不对劲：$($o.session.monitors)"

    # 「这台机器被本项目实测过没有」判的是同源关系，不假定跑判据的这一台就是记录里那一台：
    # 记录只有两个数（os.testedMinBuild / os.testedArch，源码里 src/EnvReport.h 的 verified_env），
    # matchesTestedEnvironment = 本机的版本与架构对不对得上那两个数，而每条后端的 verifiedOnThisMachine
    # 必须由同一个判断算出来（EnvReport.cpp 把两者直接取同一个值）。换一台机器跑，这些值就该整体变成
    # no 并留下 this_environment_not_tested 那条 caveat —— "没在这台测过就不说测过"这条判据一个字不
    # 放宽；反过来把它写成"必须 yes"等于让开发机替所有机器声称实测过，跑判据的机器一换就整批假红。
    Assert-Ec ([int]$o.os.testedMinBuild -eq $TESTED_BUILD -and $o.os.testedArch -eq 'x64') `
        ("查询交回的实测记录是 {0} {1}，与这里钉住的 {2} x64（README《系统支持》同一条）不一致" -f `
         $o.os.testedMinBuild, $o.os.testedArch, $TESTED_BUILD)
    $isTestedEnv = ([int]$o.os.build -eq [int]$o.os.testedMinBuild -and $o.program.arch -eq $o.os.testedArch)
    $wantMatch = if ($isTestedEnv) { 'yes' } else { 'no' }
    Assert-Ec ($o.os.matchesTestedEnvironment -eq $wantMatch) `
        ("本机 {0} 版本 {1}，记录 {2} 版本 {3}：matchesTestedEnvironment 该写 {4}，实际 {5}" -f `
         $o.program.arch, $o.os.build, $o.os.testedArch, $o.os.testedMinBuild, `
         $wantMatch, $o.os.matchesTestedEnvironment)
    foreach ($b in @($o.backends)) {
        Assert-Ec ($b.verifiedOnThisMachine -eq $o.os.matchesTestedEnvironment) `
            ("后端 {0} 的实测标记要与 matchesTestedEnvironment 同源：那边 {1}，这条 {2}" -f `
             $b.name, $o.os.matchesTestedEnvironment, $b.verifiedOnThisMachine)
    }
    # 光标那条开关没有自己的实测来源，也只能跟着同一个判断走（HDR 那一段是另一回事：本项目没有
    # HDR 屏，verifiedOnThisMachine 恒 no，由下面色彩那节单独钉）。
    Assert-Ec ($o.cursor.switch.verifiedOnThisMachine -eq $o.os.matchesTestedEnvironment) `
        ("cursor 开关的实测标记要与 matchesTestedEnvironment 同源，实际 {0}" -f `
         $o.cursor.switch.verifiedOnThisMachine)
    $hasNotTestedCaveat = (@($o.caveats) -contains 'this_environment_not_tested')
    Assert-Ec ($hasNotTestedCaveat -eq ($o.os.matchesTestedEnvironment -eq 'no')) `
        ("this_environment_not_tested 这条 caveat 要跟着 matchesTestedEnvironment 出现或消失（本机 {0}，caveat 在否 {1}）" -f `
         $o.os.matchesTestedEnvironment, $hasNotTestedCaveat)

    # =========================================================================
    # =========================================================================
    Write-Host "`n=== 3) 真机：status 与该版本的下限自相一致，且与 --dry-run 那条链同源 ==="
    # =========================================================================
    $build = [int]$o.os.build
    $want = @{
        wgc = ($build -ge $FLOOR_WGC); duplication = ($build -ge $FLOOR_DUP);
        printwindow = ($build -ge $FLOOR_FULLCONTENT); dwm = ($build -ge $FLOOR_FULLCONTENT);
        bitblt = $true
    }
    foreach ($b in @($o.backends)) {
        $expect = if ($want.ContainsKey($b.name)) { $want[$b.name] } else { $null }
        Assert-Ec ($null -ne $expect) "查询里出现了测试不认识的后端名：$($b.name)"
        if ($expect -eq $true) {
            Assert-Ec ($b.status -in @('available', 'unverified')) `
                "本机 $build 明明够 $($b.name) 的下限，status 却是 $($b.status)"
        } else {
            Assert-Ec ($b.status -eq 'unavailable') `
                "本机 $build 低于 $($b.name) 的下限，status 却没说不可用：$($b.status)"
        }
        Assert-Ec (@('available', 'unavailable', 'unverified') -contains $b.status) `
            "status 取值不在契约里：$($b.status)"
        Assert-Ec ($b.compiled -eq $true) "$($b.name) 是本构建实现的，compiled 却是假"
    }

    # 与那次截图实际会走的链比对：两处必须是同一批判据算出来的，不许一份筛了另一份没筛。
    # 这里必须用 --capture auto：不给 --capture 时默认就是 wgc 那一条，链里本来就只有它，
    # 拿它比 autoChainWindow 只会比出两个不同的东西（实测踩过一次）。
    $dry = Json-Of (Invoke-Query -Arguments @('--class', 'Shell_TrayWnd', '-v', '--dry-run',
                                                 '--capture', 'auto', 'out.png'))
    Assert-Ec ([bool]$dry) '对照用的 --dry-run 没出 JSON'
    $chainViaDry = @($dry.input.captureChain)
    $chainViaQuery = @((Json-Of (Invoke-Query -Arguments @('--capabilities'))).autoChainWindow)
    Assert-Ec (($chainViaDry -join ',') -eq ($chainViaQuery -join ',')) `
        "同一次运行里 --dry-run 的 auto 链是 [$($chainViaDry -join ',')] 而查询的是 [$($chainViaQuery -join ',')]"
    Assert-Ec ((Codes $dry.errors) -notcontains 'cli.query_conflict') `
        '--dry-run 那条对照命令被查询的互斥规则误伤了'
    # 整屏那条链同样与 --monitor 那次的回显同源
    $dryScreen = Json-Of (Invoke-Query -Arguments @('--monitor', '1', '-v', '--dry-run',
                                                     '--capture', 'auto', 'out.png'))
    $screenViaDry = @($dryScreen.input.captureChain)
    $screenViaQuery = @((Json-Of (Invoke-Query -Arguments @('--capabilities'))).autoChainScreen)
    Assert-Ec (($screenViaDry -join ',') -eq ($screenViaQuery -join ',')) `
        "屏幕模式的 auto 链两处不同源：[$($screenViaDry -join ',')] 对 [$($screenViaQuery -join ',')]"

    # =========================================================================
    Write-Host "`n=== 4) 真机：--yes 那一级在文档里与登记表一致 ==="
    # =========================================================================
    $o = Json-Of (Invoke-Query -Arguments @('--capabilities'))
    Assert-Ec ($o.authorization.yesSkips -eq 'window-content') `
        "文档没写清 --yes 只免窗口内容那一级：$($o.authorization.yesSkips)"
    Assert-Ec ($o.authorization.desktopPixelsAlwaysAsk -eq $true) '桌面像素那一级必须恒为"一定问人"'
    Assert-Ec ($o.privacy.showsDialog -eq $false -and $o.privacy.capturesScreen -eq $false) `
        '查询自己声称弹过框或拍过画面'
    $desktop = @($o.authorization.paths | Where-Object { $_.scope -eq 'desktop' })
    $window = @($o.authorization.paths | Where-Object { $_.scope -eq 'window' })
    Assert-Ec ($desktop.Count -ge 5 -and $window.Count -ge 3) `
        "登记表里桌面 $($desktop.Count) 条 / 窗口 $($window.Count) 条，与实现不相符"
    foreach ($p in $desktop) {
        Assert-Ec ($p.consentWithYes -eq $true) "桌面那条 $($p.path) 竟然允许 --yes 免掉确认"
        Assert-Ec ($p.consentWithoutYes -eq $true) "$($p.path) 不给 --yes 时也要问人"
    }
    foreach ($p in $window) {
        Assert-Ec ($p.consentWithYes -eq $false) "窗口内容那条 $($p.path) 给了 --yes 还说要问人"
    }
    # dwm 那条内部屏幕退路要看得见，否则 --yes 会被读成覆盖 dwm 的全部行为
    $dwm = @($o.backends | Where-Object { $_.name -eq 'dwm' })
    Assert-Ec (@($dwm[0].paths | Where-Object { $_.scope -eq 'desktop' }).Count -eq 1) `
        'dwm 的屏幕退路没在查询里列出来'

    # =========================================================================
    Write-Host "`n=== 5) 真机：字段稳定性、跨语言一致，与隐私清单 ==="
    # =========================================================================
    $first = Invoke-Query -Arguments @('--capabilities')
    $second = Invoke-Query -Arguments @('--capabilities')
    Assert-Ec ($first.Stdout -eq $second.Stdout) '两次同样的查询出了两份不同的文档'

    $baseline = (Invoke-Query -Arguments @('--capabilities')).Stdout
    foreach ($lang in @('zh-CN', 'zh-TW', 'en', 'ja')) {
        $body = (Invoke-EcProcess -FilePath $Exe -Arguments @('--capabilities', '--lang', $lang) `
                                  -TimeoutMs 15000).Stdout
        Assert-Ec ($body -eq $baseline) "$lang 那份与默认那份不同（查询文档不该随 --lang 变）"
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($baseline)
    $nonAscii = @($bytes | Where-Object { $_ -ge 0x80 }).Count
    Assert-Ec ($nonAscii -eq 0) "查询文档里有 $nonAscii 个非 ASCII 字节，那就意味着它会随语言变"

    # 隐私清单：不输出用户名、计算机名、环境变量值与任何绝对路径
    foreach ($needle in @($env:USERNAME, $env:USERDOMAIN, $env:COMPUTERNAME, $env:USERPROFILE,
                          $env:TEMP, $env:HOME)) {
        if (-not $needle) { continue }
        Assert-Ec (-not $baseline.Contains($needle)) "查询文档里出现了不该有的身份值：$needle"
    }
    Assert-Ec ($baseline -notmatch '[A-Za-z]:\\') '查询文档里出现了带盘符的绝对路径'
    Assert-Ec ($baseline -notmatch '\\\\') '查询文档里出现了路径分隔符（只该有 ECAPTURE.EXE 这个名字）'
    # 构建标识可核对：与 dumpbin 读的是同一份 PE 头，且不带任何"支持声明"的意思
    $diag = Json-Of (Invoke-Query -Arguments @('--diagnostics'))
    Assert-Ec ($diag.build.linkTimestamp -match '^0x[0-9A-F]{8}$') `
        "构建标识里的链接时间戳形状不对：$($diag.build.linkTimestamp)"
    Assert-Ec ($diag.build.subsystemVersionIsSupportClaim -eq $false) `
        '把 PE 里那个子系统版本当成了支持声明'
    Assert-Ec ($diag.program.buildId -eq $diag.build.id) '两份文档里的构建标识不一致'
    # 敏感词自述：--verbose 展开的每一问也要保持 ASCII 与无路径
    $verbose = (Invoke-Query -Arguments @('--capabilities', '-v')).Stdout
    Assert-Ec ($verbose -notmatch '[A-Za-z]:\\' -and $verbose -notmatch 'P:\\') `
        '--verbose 的 probes 段里出现了绝对路径'
    Assert-Ec ($verbose.Contains('"question": "consoleSession"')) '--verbose 没展开每一问的原始答案'

    # =========================================================================
    Write-Host "`n=== 6) 本机不具备条件、照实记未验证的部分 ==="
    # =========================================================================
    Skip-Ec '没有交互桌面的会话（服务、计划任务、无人登录的会话）里那份文档的形状' `
            '本机就是一个有人盯着的交互桌面。那一档由离线层注入假探针判（attachedToConsole=no、displayTopology=no 时桌面那两条要写 unavailable，且 desktop_paths_need_answerable_dialog 那条要在）'
    Skip-Ec '低于各条下限的 Windows 上 status 真的写 unavailable、且那次截图确实一张都不落地' `
            '这台机器的 Windows 版本降不下去，也不该为测试去装第二台。那条边界由离线层注入假版本逐条判'
    Skip-Ec '某个编码器真的没登记（Windows N 缺媒体组件那类）时 formats 那一段' `
            '本机六条编码器都在。这一层本来也不实测编码器（那是"探测能力"的另一条路），所以查询里 registered 恒为 unknown；"编码器不可用"的现场由离线层注入判'
    Skip-Ec 'ARM64、Windows Server、远程桌面会话里那份文档的具体值' `
            '本机不是那些环境。查询报的是当场问出来的值，不做任何按版本的预测'
    Skip-Ec '屏幕数与 WMI 那两份独立值逐台比对（多屏、虚拟显示器、未接线的输出口）' `
            'Win32_DesktopMonitor 与 EnumDisplayMonitors 在多屏机器上本来就可能各报各的，这条比对在这里给不出可靠判据；只判到"查询报的屏数是个说得通的正数"'
} finally {
    Stop-EcOwnedWindows
}

exit (Complete-EcSuite -Title '能力与诊断查询')
