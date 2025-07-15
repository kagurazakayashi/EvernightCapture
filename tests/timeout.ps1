<#
.SYNOPSIS
    截图任务超时与阻塞隔离的回归判据：离线格式与预算算术 + 真机期限/辅助进程行为。
.DESCRIPTION
    判据分三层：

      1) 离线层：build\ecapture-worker-tests.exe（源码 tests\isolation_state.cpp）判两件事 ——
         父子之间那份内部消息格式（截断、超长、保留段非 0、没登记的状态码、像素数与行距
         不自洽、句柄为 0 的条目：任何一条都必须整条作废，不许"看起来成功"）；
         以及期限本体（预算被前一步消耗之后，后一步只能拿到剩下的；用尽之后任何一步都
         拿不到等待时间 —— 这就是"每一步不许重新领一份预算"的算式）。
      2) 入口层：辅助模式不是公共选项。照它内部约定的样子直接启动必须被绑定校验拒绝，
         而且两条标准流一个字节都不许写 —— 这一层判的就是"隐私旁路"：不能有人靠
         "--worker --yes" 这种入口绕过截图确认去取画面。
      3) 真机层：目标一律是自建窗口（tests\helper\ec_window.cs）：
         --stall-ms 让那扇窗口的消息线程坐牢（等价于"目标应用卡住了"），
         --block-print-ms 让它故意不处理 WM_PRINT（等价于"这个窗口自己画不完"），
         --no-redirect 不给它 DWM 缓存面，于是取图只能靠发给窗口自己的那次绘制请求。

    真机层判的是五件事，光看退出码会全部放过：
      * 父命令是否**按期限**返回（用本进程自己的秒表计时）；
      * 期限到点时一张图都不落地（直接数文件）；
      * 目标窗口没被杀（IsWindow + 它的 PID 还在 + 线程回到消息循环之后仍能正常截图）；
      * 辅助进程被回收（按"这次构建的产物的完整路径"数进程，不按映像名猜）；
      * 隔离执行真的在用（父命令还在跑的时候，它名下能看到一个同一路径的子进程）。

    实测边界（写在判据里，不藏在注释里）：Win10 19045 上普通 DWM 合成窗口的
    PrintWindow(PW_RENDERFULLCONTENT) 取的是 DWM 缓存面，压根不把 WM_PRINT 发给目标线程
    （实测：那条线程自己堵住时这次调用仍 15 ms 返回，连窗口标题都是 user32 里的一份缓存，
    0 ms 就读到了）。所以"把目标卡住"在这台机器上造不出"父进程被拖住"的现场，
    第 4 节因此判两种结局都必须成立的不变式（父命令在期限内返回），没触发到的那半如实
    记未验证，不假装已经判过。而期限本身是现场判的：给一个连辅助进程都来不及起来的
    预算（--timeout-ms 1），父命令必须立刻带着稳定码回来 —— 第 3 节。

    隐私规矩不动：辅助进程只做"读某个窗口自己的画面"和"把顶层窗口列一遍"，桌面像素那几条
    照旧要弹框；本脚本代答确认框时一律只答"否"（拒绝不拍到任何东西）。
    默认只有一条会弹框的判据：确认框到点自己关（期限取 400 ms，没代人决定，也没拍到任何东西）。
    那条框是真弹在人手上的：若有人在 400 ms 之内就碰了它（点"是"或点"否"），这条判据的前提
    "没人回答"不成立，记 SKIP 说明原因，不会算通过，也不会算失败——测试侧绝不代答"是"。
    要跑那两条需要往框上点"否"的判据，请显式加 -Consent。
.EXAMPLE
    .\tests\timeout.ps1
    .\tests\timeout.ps1 -SkipProtocol   # 跳过离线层
    .\tests\timeout.ps1 -Consent        # 加上它才跑那两条要代答"否"的确认框判据
    .\tests\timeout.ps1 -Keep           # 保留临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SkipProtocol,
    [switch]$Consent,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$root = Get-EcRepoRoot
$protoExe = Join-Path $root 'build\ecapture-worker-tests.exe'
$IDNO = 7

$stallMs = 6000          # 一次人为阻塞的时长：明显长于任何一条期限
$budgetMs = 1200

$run = New-EcRunDir -Tag 'timeout'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

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

function First-Error($o) { return @($o.errors)[0] }

function Invoke-Ec($argv, $probe = $null, $probeMs = 60000) {
    <# 整轮锁 zh-CN：断言写的是简体字（与 cli.ps1 同一约定）。 #>
    $arguments = @('--lang', 'zh-CN') + @($argv)
    return (Invoke-EcProcess -FilePath $Exe -Arguments $arguments -TimeoutMs 120000 `
            -Probe $probe -ProbeIntervalMs 40 -ProbeTimeoutMs $probeMs)
}

function Wait-EcIdleHelpers([int]$waitMs = 3000) {
    <# 辅助进程应当随父命令一起收干净。等一小会儿再数，报出来的东西才有意义。 #>
    $deadline = (Get-Date).AddMilliseconds($waitMs)
    while ((Get-Date) -lt $deadline) {
        if (@(Get-EcProcessesFromPath -Path $Exe).Count -eq 0) { return @() }
        Start-Sleep -Milliseconds 100
    }
    return @(Get-EcProcessesFromPath -Path $Exe)
}

function Wait-EcWindowResponsive($window, [int]$waitMs = 30000) {
    <#
        等那扇故意堵住消息线程的窗口重新回到消息循环。判据独立取：给它发一条 WM_NULL 并
        限时等回话（Test-EcWindowResponsive），线程还堵着就拿不到回话。
        这条等待是测试自己的事，与被测工具的期限无关，所以给足时间。
    #>
    $deadline = (Get-Date).AddMilliseconds($waitMs)
    while ((Get-Date) -lt $deadline) {
        if (Test-EcWindowResponsive -Hwnd $window.Hwnd) { return $true }
        Start-Sleep -Milliseconds 100
    }
    return $false
}

# 父命令还在跑的时候，它名下有没有一个"同一个 exe 起的子进程"（= 本工具的辅助进程）
$script:EcSawHelper = $false
$probeSee = {
    param($p)
    # 只认"和父命令同一个 exe 路径、又不是父命令自己"的进程：辅助进程就是它。
    # 不用 CIM 查父子关系：那第一条查询要几百毫秒，父命令早跑完了才回结果（实测踩过）。
    $others = @(Get-EcProcessesFromPath -Path $Exe | Where-Object { $_ -ne [int]$p.Id })
    if ($others.Count -gt 0) { $script:EcSawHelper = $true; return 'seen' }
    return ''
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：内部消息格式 + 预算算术 ==="
    # =========================================================================
    if ($SkipProtocol) {
        Skip-Ec '隔离执行与期限的离线判据' '调用方给了 -SkipProtocol'
    } else {
        if (-not (Test-Path -LiteralPath $protoExe)) {
            Write-Host '  没有 build\ecapture-worker-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $protoExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "离线判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '共 (\d+) 项检查，失败 (\d+)')
        Assert-Ec ($m.Success) "离线判据的摘要读不出来：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) '离线判据里有失败项'
        Assert-Ec ([int]$m.Groups[1].Value -ge 55) "离线判据的检查数不对劲（$($m.Groups[1].Value)），是不是被删了"
        Write-Host "  $tail" -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 1) 辅助模式不是公共入口：没有本次绑定就拒绝执行，且不写任何流 ==="
    # =========================================================================
    $refusals = @(
        @{ Name = '参数齐全但发起方不是本工具'; A = @('/ecapture-worker', '\\.\pipe\ecapture-worker-1-DEADBEEF', 'DEADBEEFCAFEBABE', '4') },
        @{ Name = '只给标记、没有绑定参数'; A = @('/ecapture-worker') },
        @{ Name = '多给一条参数（约定不是这个样子）'; A = @('/ecapture-worker', '\\.\pipe\ecapture-worker-1-AA', 'AA', '4', '--yes') },
        @{ Name = 'nonce 写成非法字符'; A = @('/ecapture-worker', '\\.\pipe\ecapture-worker-1-DEADBEEF', 'zzz', '4') }
    )
    foreach ($case in $refusals) {
        $r = Invoke-EcProcess -FilePath $Exe -Arguments $case.A -TimeoutMs 30000
        Assert-Ec ($r.Exit -ne 0) "$($case.Name)：辅助模式竟然按成功退出（exit=$($r.Exit)）"
        Assert-Ec (-not $r.TimedOut) "$($case.Name)：辅助模式在没有任务时没有自己退出"
        Assert-Ec ($r.Stdout.Length -eq 0) "$($case.Name)：辅助模式往 stdout 写了东西"
        Assert-Ec ($r.Stderr.Length -eq 0) "$($case.Name)：辅助模式往 stderr 写了东西"
    }
    # 那个标记不在选项目录里，也不该出现在 --help 里
    $rHelp = Invoke-Ec @('--help') -probeMs 1000
    Assert-Ec ($rHelp.Stdout -notmatch 'ecapture-worker') '帮助文本里出现了内部工作模式'

    # =========================================================================
    Write-Host "`n=== 2) 会卡住的那几条取图调用确实在辅助进程里跑 ==="
    # =========================================================================
    $class = "ec-to-plain-$tag"
    $plain = Start-EcWindow -RunDir $run -Class $class -Title "对照窗口 $class" -Rect '160,160,640,500' -Seed 21
    $hPlain = Get-EcHwndHex $plain.Hwnd
    Write-Host ("对照目标 PID={0} HWND={1}" -f $plain.Pid, $hPlain)

    Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) '开始之前就已经有遗留的辅助进程'

    foreach ($channel in @('printwindow', 'dwm')) {
        $script:EcSawHelper = $false
        $shot = Get-EcRunFile -RunDir $run -Name ("isolated_$channel.png")
        $r = Invoke-Ec @('--hwnd', $hPlain, '--capture', $channel, '--yes', '--out', $shot) -probe $probeSee
        $o = Json-Of $r
        Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 1) "$channel 没截到（exit=$($r.Exit)）$($r.Stderr)"
        Assert-Ec ([bool]$script:EcSawHelper) "$channel 这条通道没有走辅助进程（隔离执行没生效）"
        Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) "$channel 这一轮的辅助进程留到了现在"
        $img = @($o.images)[0]
        Assert-Ec ($img.source -eq $channel) "source 该写实际那条通道 $channel：$($img.source)"
        Assert-Ec ($img.scope -eq 'window') "这两条都是窗口内容路径，scope 该是 window：$($img.scope)"
        $stats = Get-EcImageStats -Path $shot -Step 7
        $d = Get-EcColorDistance -A $stats.TopDominant -B (New-EcSignatureKey -Seed 21)
        Assert-Ec ($stats.Colors -ge 8 -and $d -le 32) "$channel 的画面不是本次窗口内容（颜色 $($stats.Colors)，距离 $d）"
    }
    # dwm 那条要泵 700 毫秒等 DWM 合成，父命令因此至少跑这么久：证明"等待发生在辅助进程里"
    Assert-Ec (@(Get-ChildItem -LiteralPath $run.Path -Filter '*.png' -File).Count -ge 2) '两条通道都没写出文件'

    # =========================================================================
    Write-Host "`n=== 3) 期限现场生效：给一个连辅助进程都来不及起的预算 ==="
    # =========================================================================
    # 1 毫秒的预算必然在任何一步之前就已经用尽。判据不是"有没有报错"，而是这四件事同时成立：
    # 稳定码 + 一条都不落地 + 立刻返回 + 一个进程都不留。旧实现没有这条路（超时参数被忽略）。
    $tinyCases = @(
        @{ Name = '极小预算 + 正则（隔离求值来不及）'; A = @('--title-regex', '.', '--dry-run', '--out', 'x.png', '--timeout-ms', '1'); Codes = @('match.timeout', 'capture.timeout') },
        @{ Name = '极小预算 + 取帧（辅助进程来不及）'; A = @('--hwnd', '__HWND__', '--capture', 'dwm', '--yes', '--out', '__OUT__', '--timeout-ms', '1'); Codes = @('capture.timeout', 'match.timeout') }
    )
    for ($i = 0; $i -lt $tinyCases.Count; $i++) {
        $case = $tinyCases[$i]
        $out = Get-EcRunFile -RunDir $run -Name ('timeout_case' + $i + '.png')
        $argv = @($case.A | ForEach-Object { if ($_ -eq '__HWND__') { $hPlain } elseif ($_ -eq '__OUT__') { $out } else { $_ } })
        $r = Invoke-Ec $argv
        $o = Json-Of $r
        $e = First-Error $o
        Assert-Ec ($r.Exit -eq 7) "$($case.Name)：退出码该是 7，实际 $($r.Exit)"
        Assert-Ec (@($case.Codes) -contains $e.code) "$($case.Name)：该报期限耗尽，实际 $($e.code) - $($e.message)"
        Assert-Ec ($e.option -eq '--timeout-ms') "$($case.Name)：诊断要指着 --timeout-ms：$($e.option)"
        Assert-Ec ($e.message -match '\d+') "$($case.Name)：诊断里要写预算与实际用时：$($e.message)"
        Assert-Ec ($o.captured -eq 0) "$($case.Name)：还交出了图片"
        Assert-Ec (@(Get-ChildItem -LiteralPath $run.Path -Filter 'timeout_*.png' -File).Count -eq 0) `
            "$($case.Name)：期限到点之后仍落了文件"
        Assert-Ec ($r.DurationMs -lt 4000) "$($case.Name)：没有按期限返回（$($r.DurationMs) ms）"
        Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) "$($case.Name)：留下了没被回收的辅助进程"
    }

    # =========================================================================
    Write-Host "`n=== 4) 目标线程卡住：父命令仍必须按期限返回，且不被牵连 ==="
    # =========================================================================
    # 这一节按 Windows 的行为有两种结局，两种都必须满足同一个不变式：父命令在自己的期限
    # 附近回来，而不是等满目标线程那次 6 秒的坐牢。
    $classStall = "ec-to-stall-$tag"
    $stall = Start-EcWindow -RunDir $run -Class $classStall -Title "卡住窗口 $classStall" `
        -Rect '700,160,1160,500' -Seed 22 -MaxLifeSeconds 900 `
        -ExtraArgs @('--stall-ms', [string]$stallMs, '--block-print-ms', [string]$stallMs)
    $hStall = Get-EcHwndHex $stall.Hwnd

    $p4 = Get-EcRunFile -RunDir $run -Name 'stalled_target.png'
    [void](Start-EcThreadStall -Window $stall)
    $r = Invoke-Ec @('--hwnd', $hStall, '--capture', 'printwindow', '--yes', '--out', $p4,
                    '--timeout-ms', [string]$budgetMs)
    $o = Json-Of $r
    Assert-Ec ($r.DurationMs -lt 4500) "没有按期限返回，用了 $($r.DurationMs) ms"
    Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) '这一轮的辅助进程留到了现在'
    Assert-Ec ([EcHarnessWin]::IsWindow($stall.Hwnd)) '目标窗口在这次调用之后已经不在了'
    Assert-Ec (Test-EcProcessAlive -ProcessId $stall.Pid) '目标窗口的进程在这次调用之后已经退出'
    if ($o.captured -eq 0) {
        $e = First-Error $o
        Assert-Ec (@('capture.timeout', 'match.timeout') -contains $e.code) "该报期限耗尽，实际 $($e.code)"
        Assert-Ec ($e.backend) "backend 要写清是哪条通道：$($e.backend)"
        Assert-Ec (-not (Test-Path -LiteralPath $p4)) '超时的这一次落了文件'
    } else {
        Assert-Ec ($r.DurationMs -lt $stallMs) "这台机器上 PrintWindow 没等目标线程，但父命令也不该等满 $($stallMs) ms"
        Skip-Ec '本机 PrintWindow 没把 WM_PRINT 发给目标线程，取帧阶段的期限抢占没能现场触发' `
            '同一件事的另一半（期限内返回、目标不被牵连、进程被回收）已判过；DWM 缓存面那条路不经过目标线程'
        Write-Host ("  （这台机器：{0} 张，{1} ms，没有等那条线程）" -f $o.captured, $r.DurationMs) -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 5) 线程回到消息循环之后，同一个目标照样截得到 ==="
    # =========================================================================
    Assert-Ec (Wait-EcWindowResponsive $stall) '目标线程在人为阻塞之后没有恢复'
    $p5 = Get-EcRunFile -RunDir $run -Name 'after_stall_ok.png'
    $r = Invoke-Ec @('--hwnd', $hStall, '--capture', 'printwindow', '--yes', '--out', $p5) -probeMs 20000
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 1) "线程恢复后仍截不到（exit=$($r.Exit)）$($r.Stderr)"
    $stats = Get-EcImageStats -Path $p5 -Step 7
    $d = Get-EcColorDistance -A $stats.TopDominant -B (New-EcSignatureKey -Seed 22)
    Assert-Ec ($d -le 32) "画面不是本次窗口内容（签名距离 $d）"

    # =========================================================================
    Write-Host "`n=== 6) 一份预算管整批：后面的目标不再重新领一份 ==="
    # =========================================================================
    # 同一个进程三扇同类窗口（--all 一次截三张）。预算只够第一步，于是三个目标一个都不该截：
    # 若每步/每目标重新领一份预算，这里就会截出三张图来。
    $tripClass = "ec-to-trip-$tag"
    $trip = Start-EcWindow -RunDir $run -Class $tripClass -Title "成批窗口 $tag" -Rect '200,600,620,880' `
        -Windows 3 -Seed 23 -MaxLifeSeconds 900
    $dir6 = Get-EcRunFile -RunDir $run -Name 'batch'
    New-Item -ItemType Directory -Force -Path $dir6 | Out-Null

    $r = Invoke-Ec @('--pid', [string]$trip.Pid, '--all', '--capture', 'dwm', '--yes',
                    '--out', (Join-Path $dir6 'batch_%i.png'), '--timeout-ms', '1')
    $o = Json-Of $r
    $codes6 = @(Codes $o.errors)
    Write-Host ("  批次：captured={0} errors={1} 文件={2}" -f $o.captured, ($codes6 -join ','),
                @(Get-ChildItem -LiteralPath $dir6 -File).Count)
    Assert-Ec ($r.Exit -eq 7) "批次超时的退出码不是 7：$($r.Exit)"
    Assert-Ec ($o.captured -eq 0 -and @(Get-ChildItem -LiteralPath $dir6 -File).Count -eq 0) `
        '预算只够第一步时仍然截出了图（每一步各领了一份预算）'
    Assert-Ec (@('match.timeout', 'capture.timeout') -contains $codes6[0]) "批次该报期限耗尽：$($codes6 -join ',')"
    Assert-Ec ($r.DurationMs -lt 4000) "整批没有按期限收口（$($r.DurationMs) ms）"
    Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) '批次之后留下了辅助进程'

    # 预算给够时同一批要真的截出三张：否则上面那条"没落地"就没意义
    $dir6b = Get-EcRunFile -RunDir $run -Name 'batch_ok'
    New-Item -ItemType Directory -Force -Path $dir6b | Out-Null
    $r = Invoke-Ec @('--pid', [string]$trip.Pid, '--all', '--capture', 'dwm', '--yes',
                    '--out', (Join-Path $dir6b 'ok_%i.png'))
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 3 -and @(Get-ChildItem -LiteralPath $dir6b -File).Count -eq 3) `
        "没有预算时这一批本来就该截三张（实际 captured=$($o.captured)）$($r.Stderr)"

    # =========================================================================
    Write-Host "`n=== 7) 辅助进程被外部结束：父命令 promptly 失败，不牵连目标 ==="
    # =========================================================================
    # 期限故意给大（9 秒），父进程还在等；这时由测试把辅助进程结束掉。父侧必须从"管道断了"
    # 这条路立刻脱身，而不是等满自己的期限。被结束的只可能是本工具自己起的那个进程 ——
    # 下面核对它的映像路径，绝不动目标应用。
    $probeKill = {
        param($p)
        foreach ($kid in @(Get-EcChildPids -ProcessId ([int]$p.Id))) {
            $proc = Get-Process -Id $kid -ErrorAction SilentlyContinue
            if (-not $proc) { continue }
            $path = ''
            try { $path = $proc.Path } catch { $path = '' }
            if ($path -ne $Exe) { continue }
            $script:EcKilledHelper = $kid
            try { $proc.Kill(); $proc.WaitForExit(5000) | Out-Null } catch { }
            return 'killed'
        }
        return ''
    }
    $script:EcKilledHelper = $null
    $p7 = Get-EcRunFile -RunDir $run -Name 'helper_killed.png'
    $r = Invoke-Ec @('--hwnd', $hPlain, '--capture', 'dwm', '--yes', '--out', $p7,
                    '--timeout-ms', '9000') -probe $probeKill -probeMs 30000
    $o = Json-Of $r
    $e = First-Error $o
    Assert-Ec ([bool]$script:EcKilledHelper) '没找到本次的辅助进程（隔离执行可能压根没启用）'
    Assert-Ec ($r.Exit -eq 7) "辅助进程被结束后退出码不是 7：$($r.Exit)"
    Assert-Ec ($e.code -eq 'capture.worker_failed') "该报辅助进程自己坏了，实际是 $($e.code)：$($e.message)"
    Assert-Ec ($e.stage -eq 'capture') "stage 该是 capture：$($e.stage)"
    Assert-Ec ($e.hint -match '\d+') "hint 里应当带着辅助进程最后的退出码：$($e.hint)"
    Assert-Ec (-not (Test-Path -LiteralPath $p7)) '辅助进程被结束后仍然落了文件'
    Assert-Ec ($r.DurationMs -lt 6000) "父进程等太久了（$($r.DurationMs) ms），断管这条路没起作用"
    Assert-Ec ([EcHarnessWin]::IsWindow($plain.Hwnd)) '目标窗口被人结束的辅助进程牵连掉了'
    Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) '上一轮的辅助进程留到了现在'

    # =========================================================================
    Write-Host "`n=== 8) 迟到的应答不会被后一次调用采用（每次交易各自独立） ==="
    # =========================================================================
    # 上一次调用留下的辅助进程可能"事后"才把东西送回来。判据不是"有没有报错"，而是紧随
    # 其后的那一次拿到的画面必须属于它自己那个目标（签名色不同），而不是上一次的残留。
    $other = Start-EcWindow -RunDir $run -Class "ec-to-late-$tag" -Title "迟到对照 $tag" `
        -Rect '1220,160,1600,500' -Seed 25 -MaxLifeSeconds 900
    $p8a = Get-EcRunFile -RunDir $run -Name 'late_discard.png'
    $script:EcKilledHelper = $null
    $r = Invoke-Ec @('--hwnd', $hStall, '--capture', 'dwm', '--yes', '--out', $p8a,
                    '--timeout-ms', '200') -probe $probeKill
    Assert-Ec ($r.Exit -eq 7) "先让上一次失败没收口（exit=$($r.Exit)）"
    Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) '上一次留下了辅助进程'

    $p8b = Get-EcRunFile -RunDir $run -Name 'late_next.png'
    $r = Invoke-Ec @('--class', $other.Class, '--capture', 'dwm', '--yes', '--out', $p8b)
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 1) "紧随其后的请求失败了：$($r.Stderr)"
    $stats = Get-EcImageStats -Path $p8b -Step 7
    $d = Get-EcColorDistance -A $stats.TopDominant -B (New-EcSignatureKey -Seed 25)
    Assert-Ec ($d -le 32) "后一次拿到的是别人的画面（与本次窗口签名距离 $d）"

    # =========================================================================
    Write-Host "`n=== 9) 正则：隔离求值 + 失控模式的稳定诊断 ==="
    # =========================================================================
    # (a) 普通模式在辅助进程里求值，命中列表原样交回父进程
    $r = Invoke-Ec @('--title-regex', '对照窗口', '--dry-run', '--out', 'x.png')
    $o = Json-Of $r
    $note = @($o.notes | Where-Object { $_.code -eq 'note.dry_run' })[0]
    Assert-Ec ($r.Exit -eq 0 -and $note) "正则命中判据没跑通（exit=$($r.Exit)）$($r.Stderr)"
    Assert-Ec ($note.value -match [regex]::Escape($hPlain)) "隔离求值把该命中的窗口漏了：$($note.value)"

    # (b) 嵌套量词这种失控模式：本机正则库的上限把它挡下来，必须给稳定码而不是"没有匹配"，
    #     也不许悄悄改成别的语法
    $long = Start-EcWindow -RunDir $run -Class "ec-to-rgxlong-$tag" -Title (('a' * 30) + '!') `
        -Rect '1180,600,1500,820' -Seed 24
    $r = Invoke-Ec @('--title-regex', '(a+)+$', '--dry-run', '--out', 'x.png')
    $o = Json-Of $r
    $e = First-Error $o
    Assert-Ec ($r.Exit -eq 1) "失控正则该按参数错退出 1，实际 $($r.Exit)：$($r.Stderr)"
    Assert-Ec ($e.code -eq 'cli.invalid_regex') "失控正则的码不对：$($e.code)"
    Assert-Ec ($e.stage -eq 'match' -and $e.option -eq '--title-regex') "定位字段不对：$($e.stage)/$($e.option)"
    Assert-Ec ($e.message -match '复杂度') "文案要指着回溯复杂度说：$($e.message)"
    Assert-Ec (@(Wait-EcIdleHelpers).Count -eq 0) '正则求值留下了辅助进程'
    Stop-EcWindow -Window $long

    # =========================================================================
    Write-Host "`n=== 10) 人工确认：单独计时、到点按拒绝、且不因隔离而免确认 ==="
    # =========================================================================
    # (a) --consent-timeout-ms 到点没人答：按拒绝处理，且码与"人答否"不同。
    #     这一条不需要谁去点 —— 框到点由本工具自己关掉，什么都没拍到，所以可以默认跑。
    #     期限故意取短（400 ms）：本条判据的前提是"没人回答"，而这条框是真弹在人身上的，
    #     人要是正好在同一瞬间点了"是/否"，前提就不成立了（那是工具的正确答案，不是失败）。
    #     所以有人碰过它就记 SKIP 并说明原因，绝不把它读成通过，也绝不代答"是"。
    $p10a = Get-EcRunFile -RunDir $run -Name 'consent_timeout.png'
    $r10a = Invoke-EcConsentShot -Exe $Exe -Arguments @(
        '--hwnd', $hPlain, '--capture', 'wgc', '--out', $p10a, '--consent-timeout-ms', '400')
    Assert-Ec $r10a.Dialog '设了 --consent-timeout-ms 却压根没弹框'
    $o = Json-Of $r10a
    $e = First-Error $o
    $someoneAnswered = ($r10a.Exit -eq 0 -or (@($o.captured)[0] -gt 0) -or ($e.code -eq 'capture.access_denied'))
    if ($someoneAnswered) {
        Skip-Ec '确认到点没人答时按拒绝处理（capture.consent_timeout）' `
            "这一次有人在 400 ms 之内碰了那条确认框（exit=$($r10a.Exit)，码=$($e.code)）：这条判据的前提`"没人回答`"不成立。测试侧不代答`"是`"，所以只能记未验证"
    } else {
        Assert-Ec ($r10a.Exit -eq 6) "确认超时该按拒绝退出 6：$($r10a.Exit)"
        Assert-Ec ($e.code -eq 'capture.consent_timeout') "确认超时的码不对：$($e.code)（绝不能按`"默认同意`"处理）"
        Assert-Ec ($e.stage -eq 'consent') "stage 该是 consent：$($e.stage)"
        Assert-Ec ($o.captured -eq 0 -and -not (Test-Path -LiteralPath $p10a)) '确认超时的这一次仍有产出'
    }

    # (b) 要往框上点一下才能判的两条：默认不跑，按约定不代点真实确认框。
    if ($Consent) {
        # 桌面路径 + --yes 仍然要弹框：隔离执行没有把"会拍到桌面的那条路"变成免确认。
        $p10b = Get-EcRunFile -RunDir $run -Name 'desktop_bitblt.png'
        $r10b = Invoke-EcConsentShot -Exe $Exe -Answer $IDNO -Arguments @(
            '--hwnd', $hPlain, '--capture', 'bitblt', '--yes', '--out', $p10b, '--timeout-ms', '6000')
        Assert-Ec $r10b.Dialog '桌面路径带 --yes 竟然没弹框（隔离执行变成了免确认就是这条判据）'
        $o = Json-Of $r10b
        Assert-Ec ($r10b.Exit -eq 6 -and $o.captured -eq 0) "答`"否`"之后仍有产出（exit=$($r10b.Exit)）"
        Assert-Ec ((First-Error $o).code -eq 'capture.access_denied') `
            "桌面路径答`"否`"的码该是 capture.access_denied：$((First-Error $o).code)"
        Assert-Ec (-not (Test-Path -LiteralPath $p10b)) '答"否"的那一次写出了文件'

        # 自动预算不把人等的时间算进去：期限 400 ms，2 秒之后才答"否"。
        # 如果这段时间占了自动预算，报出来的就是 capture.timeout，调用方会误读成"机器慢了"，
        # 而真实原因是没人回答。
        $p10c = Get-EcRunFile -RunDir $run -Name 'consent_vs_budget.png'
        $r10c = Invoke-EcConsentShot -Exe $Exe -Answer $IDNO -DialogWaitMs 20000 -TimeoutMs 120000 -Arguments @(
            '--hwnd', $hPlain, '--capture', 'wgc', '--out', $p10c, '--timeout-ms', '400')
        $o = Json-Of $r10c
        $e = First-Error $o
        Assert-Ec $r10c.Dialog '窗口路径不带 --yes 竟然没弹框'
        Assert-Ec ($e.code -eq 'capture.access_denied') `
            "人还在被问时就烧掉了自动预算（报成了 $($e.code)）"
        Assert-Ec ($r10c.Exit -eq 6) "该按拒绝退出 6：$($r10c.Exit)"
    } else {
        Skip-Ec '桌面路径带 --yes 仍弹框、以及"人慢慢答不烧自动预算"' `
            '这两条要往确认框上点一下；按约定不代点真实确认。加 -Consent 才跑（只代答"否"）'
    }

    # =========================================================================
    Write-Host "`n=== 11) 期限覆盖不到的系统调用（如实记未验证，不假装已强制） ==="
    # =========================================================================
    Skip-Ec '写文件与写标准输出开工之后的期限抢占' `
        '原子写与 WriteFile 没有可取消的中间点，本机也没有满盘/只读卷可用。实现只在开工之前判预算、完工之后核用时，这条边界写在 README 与 AGENTS.md'
    Skip-Ec 'WinRT 编码器内部不回应取消时的期限覆盖' `
        '取消是合作式的：编码器内部卡住时只能留下那条等待自行结束。代码已按剩余预算等待并请求取消，缺的是"让编码器卡住"的可注入手段'
} finally {
    # 故意堵住消息线程的那扇要先回到消息循环再关：否则 PostMessage(WM_CLOSE) 排在同一条队列
    # 后面没人处理，收尾就只能等 max-life。恢复不了也不报错 —— 下面按本次拥有的进程收尾。
    if ($stall) { try { [void](Wait-EcWindowResponsive $stall -waitMs 30000) } catch { } }
    Stop-EcOwnedWindows
    $leftover = @(Get-EcProcessesFromPath -Path $Exe)
    if ($leftover.Count) {
        Write-Host ("  !! 本次构建的产物仍在运行：PID {0}（脚本不收别人的进程，只报出来）" -f ($leftover -join ',')) -ForegroundColor DarkYellow
    }
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '截图任务超时与阻塞隔离')
