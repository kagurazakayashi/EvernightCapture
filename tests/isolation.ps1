<#
.SYNOPSIS
    真机资源隔离测试：证明桌面测试只截自己建的窗口、只收尾自己的对象，两轮并发互不干扰。
.DESCRIPTION
    这里截的图全部来自本测试自己建立的窗口（ecwindow.exe，内容是一整块签名色 + 颜色带），
    不涉及用户开着的任何应用，也不走整屏路径，所以可以无人值守跑。

    覆盖五件事：
      1. 同名但不由某一轮建立的邻居窗口一直活着，而且不会被选成截图目标
      2. 只给 --process ecwindow.exe 时工具报多匹配（退出码 5），而不是替调用方随便挑一个
      3. 用 --hwnd 精确命中本次建立的窗口，JSON 里的归属字段逐条对上
      4. 像素签名只对得上自己那一轮：并发两轮的图里不会出现对方的签名色
      5. 建立者硬退出（不走 finally）时，本次窗口因父进程看门狗而自己消失，邻居不受影响
.EXAMPLE
    .\tests\isolation.ps1
    .\tests\isolation.ps1 -Keep        # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [int]$Seed = 0,
    [int]$OtherSeed = 0,
    [string]$ResultFile = '',
    [string]$WorkerClass = '',
    [switch]$Abort,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
Initialize-EcHarness -Exe $Exe | Out-Null
Set-EcDpiAware

$SEED_A = 5
$SEED_B = 11

# ---------------------------------------------------------------------------
# 工作轮：一个进程就是一"轮"，它只认自己建的那些窗口
# ---------------------------------------------------------------------------
function Invoke-EcWorkerRound {
    param(
        [Parameter(Mandatory)][string]$Class,
        [Parameter(Mandatory)][int]$Seed,
        [Parameter(Mandatory)][int]$OtherSeed,
        [switch]$AbortRound
    )

    $run = New-EcRunDir -Tag "iso-$Seed"
    Write-Host "  本次临时目录：$($run.Path)"
    $code = 1
    try {
        $title = "隔离测试窗口 $Class"
        $window = Start-EcWindow -RunDir $run -Class $Class -Title $title -Seed $Seed -Rect '140,140,620,480'
        Write-Host ("  OWNPID={0} OWNHWND={1} CLASS={2}" -f `
                   $window.Pid, (Get-EcHwndHex $window.Hwnd), $Class)

        # 1) 精确命中：--hwnd 只有一个可能结果，归属字段必须全是自己的
        $shot = Get-EcRunFile -RunDir $run -Name 'own.png'
        $r = Invoke-Ecapture @('--hwnd', (Get-EcHwndHex $window.Hwnd), '--yes', '--out', $shot)
        $o = $null
        try { $o = $r.Stdout | ConvertFrom-Json } catch { }
        Assert-Ec ($r.Exit -eq 0 -and $o -and $o.captured -eq 1) "--hwnd 精确命中失败（exit=$($r.Exit)）"
        $img = @($o.images)[0]
        Assert-Ec ($img.hwnd -eq (Get-EcHwndHex $window.Hwnd)) "images[0].hwnd=$($img.hwnd)，应为本轮 HWND"
        Assert-Ec ($img.pid -eq $window.Pid) "images[0].pid=$($img.pid)，应为本轮 PID $($window.Pid)"
        Assert-Ec ($img.class -eq $Class) "images[0].class=$($img.class)，应为 $Class"
        Assert-Ec ($img.title -eq $title) "images[0].title=$($img.title)，应为 $title"

        # 2) 像素签名：这张图的颜色必须更像我自己的窗口，而不是另一轮的
        $stats = Get-EcImageStats -Path $shot -Step 4
        Assert-Ec ($stats -and $stats.Colors -ge 12) "画面颜色只有 $($stats.Colors) 种，可能没抓到内容"
        $mine = New-EcSignatureKey -Seed $Seed
        $theirs = New-EcSignatureKey -Seed $OtherSeed
        $dMine = Get-EcColorDistance -A $stats.TopDominant -B $mine
        $dTheirs = Get-EcColorDistance -A $stats.TopDominant -B $theirs
        Write-Host ("  主色 {0} 与本轮签名 {1} 距离 {2}，与另一轮 {3} 距离 {4}" -f `
                   $stats.TopDominant, $mine, $dMine, $theirs, $dTheirs) -ForegroundColor DarkGray
        Assert-Ec ($dMine -le 32) "画面主色离本轮签名太远（$dMine），像素可能来自别处"
        Assert-Ec ($dTheirs -gt $dMine) "画面主色同时像另一轮的签名（$dTheirs vs $dMine），两轮串了"

        # 3) 尺寸应当等于尺寸请求的物理矩形
        $rect = Get-EcWindowRect -Hwnd $window.Hwnd
        Assert-Ec ($stats.Width -eq ($rect.Right - $rect.Left) -and
                   $stats.Height -eq ($rect.Bottom - $rect.Top)) `
            "图 $($stats.Width)x$($stats.Height) 与窗口矩形对不上"

        # 4) 进程名 + 本轮类名这个组合必须只命中本轮，邻居在同名进程里也不算目标
        $r2 = Invoke-Ecapture @('--process', 'ecwindow.exe', '--class', $Class, '--yes', '--out',
                               (Get-EcRunFile -RunDir $run -Name 'own2.png'))
        $o2 = $null
        try { $o2 = $r2.Stdout | ConvertFrom-Json } catch { }
        Assert-Ec ($r2.Exit -eq 0 -and @($o2.images).Count -eq 1 -and @($o2.images)[0].pid -eq $window.Pid) `
            "--process + --class 没能只命中本轮（exit=$($r2.Exit)）"

        if ($AbortRound) {
            # 硬退出：不走 finally。窗口应当由父进程看门狗收掉，目录留在原地待编排者验证
            Write-Host '  现在跳过收尾直接退出'
            [Environment]::Exit(7)
        }

        # 5) 只收尾自己：停掉本次窗口后，本次进程没了；邻居由编排者核对
        Stop-EcWindow -Window $window
        Assert-Ec (-not (Test-EcProcessAlive -ProcessId $window.Pid)) "收尾后本次进程 $($window.Pid) 还活着"
        Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
        $code = Complete-EcSuite -Title "资源隔离（第 $Seed 轮）"
    } finally {
        Stop-EcOwnedWindows
        if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
    }
    return [pscustomobject]@{ Code = $code; RunDir = $run.Path }
}

if ($WorkerClass) {
    Reset-EcSuite
    $outcome = Invoke-EcWorkerRound -Class $WorkerClass -Seed $Seed -OtherSeed $OtherSeed -AbortRound:$Abort
    if ($ResultFile) {
        Set-Content -LiteralPath $ResultFile -Value ("{0}`t{1}" -f $outcome.Code, $outcome.RunDir) -Encoding ASCII
    }
    exit $outcome.Code
}

# ---------------------------------------------------------------------------
# 编排者：起邻居、并发跑两轮、再验异常退出的收尾
# ---------------------------------------------------------------------------
Reset-EcSuite
$orch = New-EcRunDir -Tag 'iso-orch'
$tag = ($orch.Leaf -replace '[^a-z0-9]', '')
$engine = (Get-Process -Id $PID).Path
if (-not $engine) { $engine = 'powershell.exe' }
Write-Host "编排者临时目录：$($orch.Path)"

function Start-EcWorkerProcess {
    param([string]$Class, [int]$Seed, [int]$OtherSeed, [switch]$AbortSwitch)

    $log = Get-EcRunFile -RunDir $orch -Name "$Class.log"
    $err = Get-EcRunFile -RunDir $orch -Name "$Class.err"
    $res = Get-EcRunFile -RunDir $orch -Name "$Class.exit"
    $childArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                   (Join-Path $PSScriptRoot 'isolation.ps1'),
                   '-WorkerClass', $Class, '-Seed', [string]$Seed, '-OtherSeed', [string]$OtherSeed,
                   '-ResultFile', $res)
    if ($Exe) { $childArgs += @('-Exe', $Exe) }
    if ($AbortSwitch) { $childArgs += '-Abort' }
    # 两条流各自落到文件里：管道不读就必然互相等死，文件不会
    $p = Start-Process -FilePath $engine -ArgumentList (Format-EcWindowsArgv -Arguments $childArgs) `
            -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError $err
    return [pscustomobject]@{ Proc = $p; Class = $Class; Log = $log; Err = $err; Exit = $res }
}

$neighbours = @()
try {
    # 邻居：与测试程序同名、但某一轮不认识的两个窗口。它们代表用户本来开着的同类应用
    # —— 某轮既不能把它们当目标，也不能在结束时顺手关掉它们。
    foreach ($n in 1, 2) {
        $neighbours += Start-EcWindow -RunDir $orch -Class "ec-neighbour$n-$tag" `
            -Title "邻居窗口 $n（不属于任何一轮）" -Seed (2 + $n) `
            -Rect $(if ($n -eq 1) { '700,140,1180,480' } else { '140,560,620,900' })
    }
    foreach ($n in $neighbours) { Write-Host ("邻居 PID={0} 类={1}" -f $n.Pid, $n.Class) }

    # 两轮的签名色必须离得足够远，否则"更像我"这条判据没有区分力
    $sep = Get-EcColorDistance -A (New-EcSignatureKey -Seed $SEED_A) -B (New-EcSignatureKey -Seed $SEED_B)
    Assert-Ec ($sep -ge 64) "两轮签名色相距只有 $sep，判据没区分力（应 >= 64）"

    Write-Host "`n=== 并发两轮 ==="
    $workers = @(
        (Start-EcWorkerProcess -Class "ec-a-$tag" -Seed $SEED_A -OtherSeed $SEED_B),
        (Start-EcWorkerProcess -Class "ec-b-$tag" -Seed $SEED_B -OtherSeed $SEED_A)
    )

    # 邻居已经给出两个同名窗口：只给进程名就必须报多匹配，而不是随便挑一个
    $amb = Invoke-Ecapture @('--process', 'ecwindow.exe', '--dry-run', '--out',
                            (Get-EcRunFile -RunDir $orch -Name 'amb.png'))
    $ambJson = $amb.Stdout + $amb.Stderr
    Assert-Ec ($amb.Exit -eq 5) "只给 --process ecwindow.exe 的退出码 $($amb.Exit)，应为 5（多匹配）"
    Assert-Ec ($ambJson -match 'match\.ambiguous_window') "多匹配没报 match.ambiguous_window"
    foreach ($n in $neighbours) {
        Assert-Ec ($ambJson -match [regex]::Escape((Get-EcHwndHex $n.Hwnd))) "多匹配的提示里没列出邻居 $($n.Class)"
    }
    Assert-Ec ($ambJson -match 'ecwindow\.exe') '多匹配的提示里没给出同名进程名'
    Assert-Ec ($ambJson -notmatch 'capture\.|io\.') '多匹配阶段不该出现截图或写文件失败'

    $roundDirs = @()
    foreach ($w in $workers) {
        $deadline = (Get-Date).AddSeconds(240)
        while (-not $w.Proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
        Assert-Ec $w.Proc.HasExited "并发一轮 $($w.Class) 跑了 240 秒还没结束"
        if (-not $w.Proc.HasExited) { Stop-EcProcessTree -Process $w.Proc | Out-Null; continue }
        $text = (Get-Content -LiteralPath $w.Log -Raw -ErrorAction SilentlyContinue) +
                (Get-Content -LiteralPath $w.Err -Raw -ErrorAction SilentlyContinue)
        Write-Host ($text.Trim() -replace '(?m)^', '    ')
        $line = ''
        if (Test-Path -LiteralPath $w.Exit) { $line = (Get-Content -LiteralPath $w.Exit -Raw).Trim() }
        Assert-Ec ($line -match '^0\t') "$($w.Class) 自报结果 [$line]，应为 0<TAB>目录"
        Assert-Ec ($text -notmatch '  FAIL  ') "$($w.Class) 那一轮有 FAIL"
        if ($line -match "`t(\S+)$") { $roundDirs += $Matches[1] }
    }
    Assert-Ec ($roundDirs.Count -eq 2 -and (@($roundDirs | Sort-Object -Unique).Count -eq 2)) `
        "两轮没各自建临时目录：[$($roundDirs -join ' ')]"

    foreach ($n in $neighbours) {
        Assert-Ec (Test-EcProcessAlive -ProcessId $n.Pid) "邻居 $($n.Pid) 在两轮并发之间被结束了"
    }

    # 异常退出的一轮：窗口应由父进程看门狗收掉，邻居不受影响
    Write-Host "`n=== 一轮硬退出：本次窗口必须自己消失，邻居不受影响 ==="
    $abortClass = "ec-x-$tag"
    $abortRun = Invoke-EcProcess -FilePath $engine -TimeoutMs 240000 -Arguments (
        @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'isolation.ps1'),
          '-WorkerClass', $abortClass, '-Seed', [string]$SEED_A, '-OtherSeed', [string]$SEED_B,
          '-Abort') + $(if ($Exe) { @('-Exe', $Exe) } else { @() }))
    Assert-Ec ($abortRun.Exit -eq 7) "硬退出那轮退出码 $($abortRun.Exit)，应为 7"
    $abortText = $abortRun.Stdout + $abortRun.Stderr
    Write-Host ($abortText.Trim() -replace '(?m)^', '    ') -ForegroundColor DarkGray
    Assert-Ec ($abortText -notmatch '  FAIL  ') '硬退出那一轮在退出之前就出现了 FAIL'

    $ownPid = 0
    if ($abortText -match 'OWNPID=(\d+)') { $ownPid = [int]$Matches[1] }
    Assert-Ec ($ownPid -gt 0) '没从输出里拿到那轮的窗口进程 PID'
    $gone = $false
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-EcProcessAlive -ProcessId $ownPid)) { $gone = $true; break }
        Start-Sleep -Milliseconds 250
    }
    Assert-Ec $gone "建立者已退出，但它建的窗口进程 $ownPid 还活着（看门狗没起作用）"
    foreach ($n in $neighbours) {
        Assert-Ec (Test-EcProcessAlive -ProcessId $n.Pid) "邻居 $($n.Pid) 被异常退出那一轮结束了"
    }
    # 注意用 -eq [IntPtr]::Zero 判断：IntPtr 是引用类型，-not 对它永远为假
    Assert-Ec ((Find-EcWindow -ProcessId $ownPid -Class $abortClass) -eq [IntPtr]::Zero) '那轮的窗口还在屏幕上'

    # 那一轮没来得及删自己的临时目录：形状仍应被认作"测试自建"，由编排者代为删除
    $orphanDir = ''
    if ($abortText -match '本次临时目录：(\S+)') { $orphanDir = $Matches[1] }
    if ($orphanDir -and (Test-Path -LiteralPath $orphanDir)) {
        $fake = [pscustomobject]@{
            Path = $orphanDir
            Root = Split-Path -Parent $orphanDir
            Leaf = Split-Path -Leaf $orphanDir
        }
        Assert-Ec (Test-EcOwnRunDir $fake) "遗留目录 $orphanDir 不被认作测试自建目录"
        Remove-EcRunDir $fake -Quiet
        Assert-Ec (-not (Test-Path -LiteralPath $orphanDir)) '代为清理遗留目录失败'
    } else {
        Skip-Ec '没能从输出里定位那轮遗留的临时目录'
    }
} finally {
    foreach ($n in $neighbours) {
        Stop-EcWindow -Window $n
        Write-Host ("邻居 $($n.Pid) 收尾：alive={0}" -f (Test-EcProcessAlive -ProcessId $n.Pid)) -ForegroundColor DarkGray
    }
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $orch -Quiet } else { Write-Host "  编排者目录保留 $($orch.Path)" }
}

exit (Complete-EcSuite -Title '资源隔离')
