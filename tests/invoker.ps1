<#
.SYNOPSIS
    离线验证共享测试进程调用器（tests\harness.psm1）：argv 引号规则、双流并发、二进制、超时。
.DESCRIPTION
    一律不截图，也不碰桌面上任何别人的窗口：被测对象是调用器自己，
    假后端是 tests\helper\ec_window.cs 编出来的 ecwindow.exe（args / streams / hang 三种模式）。
    覆盖这些坑：
      1. 空参数、中文、空格、制表符、引号、路径里的普通与结尾反斜杠
      2. stdout / stderr 同时大量输出（先读完一条再读另一条的老写法会在这里死锁）
      3. 二进制 stdout 不被文本转码改坏
      4. 子进程卡死：到期只结束本次拥有的进程树（含孙进程），并保留部分输出
      5. 唯一临时目录：两轮并发各有各的目录，删除只认自己那一个
      6. 探测回调（-Probe）：整屏确认框那类"跑到一半要点一下"的测试靠它，不能把读取堵住
    最后用 -Worker 起两轮本子脚本并发跑，验证互不干扰。
.EXAMPLE
    .\tests\invoker.ps1
    pwsh -NoProfile -File .\tests\invoker.ps1        # PowerShell 7 走 ArgumentList 那条分支
    .\tests\invoker.ps1 -Worker 1                   # 只跑一轮（供并发检查内部调用）
#>
param(
    [string]$Exe,
    [int]$Worker = 0,
    [string]$ResultFile  # 并发检查用的落盘路径：Start-Process -PassThru 拿不到 ExitCode，自己写一个文件最可靠
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
Initialize-EcHarness -Exe $Exe -AllowMissing | Out-Null

$run = New-EcRunDir -Tag $(if ($Worker) { "invoke-w$Worker" } else { 'invoke' })
Write-Host ("  本次临时目录：{0}" -f $run.Path)
$suite = '调用器'
if ($Worker) { $suite = "调用器（并发第 $Worker 轮）" }
Reset-EcSuite
$script:EcWorkerDirs = @()

try {
    $helper = Build-EcHelper -RunDir $run
    Assert-Ec (Test-Path -LiteralPath $helper) "测试窗口程序编出来了：$helper"
    Write-Host ("  运行时传参方式：{0}（PowerShell {1}）" -f (Get-EcArgumentListMode), $PSVersionTable.PSVersion) -ForegroundColor DarkGray

    # ---------- 1) 引号规则：函数级断言（不依赖子进程） ----------
    Write-Host "`n=== Format-EcWindowsArgv 的期望输出 ==="
    $quoteCases = @(
        @{ In = ''; Want = '""' },
        @{ In = 'plain'; Want = 'plain' },
        @{ In = 'has space'; Want = '"has space"' },
        @{ In = "tab`there"; Want = '"tab	here"' },
        @{ In = 'C:\dir\'; Want = 'C:\dir\' },
        @{ In = 'a b\'; Want = '"a b\\"' },
        @{ In = 'x"y'; Want = '"x\"y"' },
        @{ In = 'a\"b'; Want = '"a\\\"b"' },
        @{ In = 'dir\"'; Want = '"dir\\\""' },
        @{ In = '\\\\'; Want = '\\\\' },
        @{ In = 'C:\Program Files\App\shots\'; Want = '"C:\Program Files\App\shots\\"' },
        @{ In = 'no "q" and \trail\'; Want = '"no \"q\" and \trail\\"' },
        @{ In = '无期迷途'; Want = '无期迷途' }
    )
    foreach ($c in $quoteCases) {
        $got = Format-EcWindowsArgv -Arguments @($c.In)
        Assert-Ec ($got -ceq $c.Want) "引号规则：[$($c.In)] -> $got，期望 $($c.Want)"
    }
    $joined = Format-EcWindowsArgv -Arguments @('a b', 'c\d\')
    Assert-Ec ($joined -ceq '"a b" c\d\') "多个参数以空格相连：$joined"

    # ---------- 2) argv 往返：真正起进程，看子进程解析到什么 ----------
    Write-Host "`n=== 子进程实际解析到的参数（长度前缀协议，不用分隔符） ==="
    $payload = @(
        '', 'plain', 'has  space', '无期迷途 主线 12-3', 'C:\Program Files\App\shots\',
        'quote"dou', 'a\"b', '\\server\share\x', 'x""y', "new`nline", "tab`tend",
        '-abc', '--title=x', '--', '-', 'a b "c" d', 'C:\dir\"qn'
    )
    $r = Invoke-EcProcess -FilePath $helper -TimeoutMs 30000 `
        -Arguments (@('--mode', 'args', '--stderr-text', '参数回显示例 一二三', '--') + $payload)
    Assert-Ec ($r.Exit -eq 0) "args 模式退出码 $($r.Exit)，应为 0" -Quiet
    Assert-Ec (-not $r.TimedOut) 'args 模式不该超时'
    $back = Parse-EcArgEcho -Bytes $r.StdoutBytes
    Assert-Ec ($back.Count -eq $payload.Count) "回吐了 $($back.Count) 条，应为 $($payload.Count) 条"
    $mismatch = @()
    for ($i = 0; $i -lt [Math]::Min($back.Count, $payload.Count); $i++) {
        if ($back[$i] -cne $payload[$i]) {
            $mismatch += ("#{0} [{1}] vs [{2}]" -f $i, $back[$i], $payload[$i])
        }
    }
    Assert-Ec ($mismatch.Count -eq 0) ("argv 往返不一致：" + ($mismatch -join ' ; '))
    Assert-Ec ($r.Stderr.Trim() -eq '参数回显示例 一二三') "stderr 的中文解码：[$($r.Stderr.Trim())]"
    Assert-Ec ($r.ArgumentMode -eq (Get-EcArgumentListMode)) '调用器报告的传参方式与实际一致'

    # ---------- 3) 双流同时大量输出 + 二进制完整性 ----------
    Write-Host "`n=== 两条流同时各写 6 MB（老写法会在这里互相等死） ==="
    $big = 6189056      # 6 MB 出头，远超管道缓冲，且不是 251 的整数倍
    $r = Invoke-EcProcess -FilePath $helper -TimeoutMs 120000 `
        -Arguments @('--mode', 'streams', '--stdout-bytes', [string]$big, '--stderr-bytes', [string]$big)
    Assert-Ec ($r.Exit -eq 0) "streams 模式退出码 $($r.Exit)，应为 0" -Quiet
    Assert-Ec (-not $r.TimedOut) '双流大量输出没有超时（超时说明有一条流没被并发消费）'
    Assert-Ec ($r.StdoutBytes.Length -eq $big) "stdout 字节数 $($r.StdoutBytes.Length)，应为 $big"
    Assert-Ec ($r.StderrBytes.Length -eq $big) "stderr 字节数 $($r.StderrBytes.Length)，应为 $big"
    Assert-Ec (Test-EcBytePattern -Bytes $r.StdoutBytes -Total $big) 'stdout 的二进制模式被改坏了'
    Assert-Ec (Test-EcBytePattern -Bytes $r.StderrBytes -Total $big) 'stderr 的二进制模式被改坏了'
    # 只要字节完整就行：二进制里含 NUL / CR / LF，解码成文本必然丢信息，所以另要一条 -NoTextDecode
    $raw = Invoke-EcProcess -FilePath $helper -TimeoutMs 30000 -NoTextDecode `
        -Arguments @('--mode', 'streams', '--stdout-bytes', '4096', '--stderr-bytes', '1024')
    Assert-Ec ($raw.StdoutBytes.Length -eq 4096 -and $raw.Stdout -eq '') `
        "-NoTextDecode 下应给出完整字节但不解码文本（bytes=$($raw.StdoutBytes.Length) text=$($raw.Stdout.Length)）"
    Write-Host ("  用时 {0} ms；两流各 {1} 字节" -f $r.DurationMs, $big) -ForegroundColor DarkGray

    # ---------- 4) 卡死的子进程树 ----------
    Write-Host "`n=== 子进程卡死：到期只结束本次拥有的进程树，并保留部分输出 ==="
    $r = Invoke-EcProcess -FilePath $helper -TimeoutMs 2500 `
        -Arguments @('--mode', 'hang', '--seconds', '120', '--spawn', '2')
    Assert-Ec $r.TimedOut '卡死的子进程没有被超时中断'
    Assert-Ec ($r.Exit -ne 0) "超时后退出码 $($r.Exit)，应非 0" -Quiet
    Assert-Ec ($r.KilledBy -ne 'none') "收尾方式报告为 none（实际用了 $($r.KilledBy)）" -Quiet
    Assert-Ec ($r.DurationMs -ge 2500 -and $r.DurationMs -lt 20000) "超时用时 $($r.DurationMs) ms，应贴近期限"
    Assert-Ec ($r.Stdout -match 'hang-prepared children=(\d+) (\d+)') `
        "超时后没保留部分 stdout（内容：$($r.Stdout.Trim())）"
    $kids = @()
    if ($r.Stdout -match 'hang-prepared children=(\d+) (\d+)') { $kids = @([int]$Matches[1], [int]$Matches[2]) }
    Assert-Ec ($kids.Count -eq 2) "没拿到两个孙进程 PID：[$($r.Stdout.Trim())]"
    Start-Sleep -Seconds 2
    foreach ($kid in $kids) {
        Assert-Ec (-not (Test-EcProcessAlive -ProcessId $kid)) "孙进程 $kid 还活着，进程树没杀干净"
    }
    Show-EcProcessDiag -Result $r -Label 'hang' -MaxChars 200

    # ---------- 5) 起进程失败要能报告，而不是抛出来 ----------
    Write-Host "`n=== 起不存在的程序 ==="
    $r = Invoke-EcProcess -FilePath (Get-EcRunFile -RunDir $run -Name 'no-such-exe.exe') -Arguments @('a')
    Assert-Ec ($r.StartError -and $r.Exit -eq -1) "缺可执行文件时没给出 StartError（exit=$($r.Exit)）"

    # ---------- 6) 探测回调：不阻塞读流，能提前结束 ----------
    Write-Host "`n=== -Probe 回调（整屏确认框那类测试要用它） ==="
    $script:EcProbeRounds = 0
    $r = Invoke-EcProcess -FilePath $helper -TimeoutMs 30000 -ProbeTimeoutMs 1200 -ProbeIntervalMs 50 `
        -Arguments @('--mode', 'streams', '--stdout-bytes', '400000', '--stderr-bytes', '400000') `
        -Probe {
            param($p)
            $script:EcProbeRounds++
            return $false
        }
    Assert-Ec ($script:EcProbeRounds -ge 2) "探测回调只跑了 $($script:EcProbeRounds) 轮，应按间隔一直跑到时限"
    Assert-Ec ($r.StdoutBytes.Length -eq 400000) "探测期间没把 stdout 读干净（$($r.StdoutBytes.Length) 字节）"
    $stopEarly = Invoke-EcProcess -FilePath $helper -TimeoutMs 30000 -ProbeTimeoutMs 8000 `
        -Arguments @('--mode', 'args', '--', 'x') -Probe { param($p) return $true }
    Assert-Ec ($stopEarly.Exit -eq 0) '探测回调返回 true 后应立即停止，不影响正常读取'

    # ---------- 7) 唯一临时目录与只删自己 ----------
    Write-Host "`n=== 唯一临时目录 ==="
    $a = New-EcRunDir -Tag 'dirA'
    $b = New-EcRunDir -Tag 'dirB'
    try {
        Assert-Ec ($a.Path -ne $b.Path) "两次建目录得到同一路径：$($a.Path)"
        Assert-Ec (Test-EcOwnRunDir $a) "自己建的目录形状不被认可：$($a.Leaf)"
        Set-Content -LiteralPath (Get-EcRunFile $a 'mine.txt') -Value 'x' -Encoding ASCII
        Remove-EcRunDir $a
        Assert-Ec (-not (Test-Path -LiteralPath $a.Path)) '删除自己的目录没生效'
        Assert-Ec (Test-Path -LiteralPath $b.Path) '删一个目录把另一个也带走了'

        # 形状不对的目录必须拒删（拿一个确实存在但不是本次建立的路径来试）
        $foreign = [pscustomobject]@{ Path = $env:TEMP; Root = (Split-Path -Parent $env:TEMP); Leaf = (Split-Path -Leaf $env:TEMP) }
        Remove-EcRunDir $foreign
        Assert-Ec (Test-Path -LiteralPath $env:TEMP) '不是本次建立的目录被删掉了'
    } finally {
        Remove-EcRunDir $b -Quiet
    }

    # ---------- 8) 并发两轮 ----------
    if (-not $Worker) {
        Write-Host "`n=== 并发两轮：各起一个子进程跑同一份脚本 ==="
        $engine = (Get-Process -Id $PID).Path
        if (-not $engine) { $engine = 'powershell.exe' }
        $workers = @()
        foreach ($n in 1, 2) {
            $childArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                           (Join-Path $PSScriptRoot 'invoker.ps1'), '-Worker', [string]$n)
            if ($Exe) { $childArgs += @('-Exe', $Exe) }
            # 两条流各自落到本次临时目录里的文件：管道不读就必然互相等死，文件不会
            $out = Get-EcRunFile -RunDir $run -Name "worker$n.log"
            $err = Get-EcRunFile -RunDir $run -Name "worker$n.err"
            $exit = Get-EcRunFile -RunDir $run -Name "worker$n.exit"
            $childArgs += @('-ResultFile', $exit)
            $p = Start-Process -FilePath $engine -ArgumentList (Format-EcWindowsArgv -Arguments $childArgs) `
                    -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
            $workers += [pscustomobject]@{ N = $n; Proc = $p; Out = $out; Err = $err; ExitFile = $exit }
        }
        foreach ($w2 in $workers) {
            $deadline = (Get-Date).AddSeconds(300)
            while (-not $w2.Proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
            Assert-Ec ($w2.Proc.HasExited) "并发第 $($w2.N) 轮跑了 300 秒还没结束（已被视为失败）"
            if (-not $w2.Proc.HasExited) { Stop-EcProcessTree -Process $w2.Proc | Out-Null; continue }
            $text = (Get-Content -LiteralPath $w2.Out -Raw -ErrorAction SilentlyContinue) +
                    (Get-Content -LiteralPath $w2.Err -Raw -ErrorAction SilentlyContinue)
            $code = (Get-Content -LiteralPath $w2.ExitFile -Raw -ErrorAction SilentlyContinue).Trim()
            Assert-Ec ($code -eq '0') `
                "并发第 $($w2.N) 轮自报退出码 [$code]，应为 0（日志 $($w2.Out)）"
            Assert-Ec ($text -match '全部通过') "并发第 $($w2.N) 轮的输出里没有通过汇总"
            Assert-Ec ($text -notmatch '  FAIL  ') "并发第 $($w2.N) 轮有 FAIL，见 $($w2.Out)"
            $mine = @([regex]::Matches($text, 'run-invoke-w\d+-\d{8}-\d{6}-\d+-[0-9a-f]{8}')) |
                ForEach-Object { $_.Value } | Sort-Object -Unique
            Assert-Ec ($mine.Count -ge 1) "并发第 $($w2.N) 轮没报告自己的临时目录"
            $script:EcWorkerDirs += $mine
        }
        $uniq = @($script:EcWorkerDirs | Sort-Object -Unique)
        Assert-Ec ($uniq.Count -ge 2) "两轮并发共用了同一个临时目录：[$($uniq -join ' ')]"
        Write-Host ("  两轮各自的临时目录：{0}" -f ($uniq -join ', ')) -ForegroundColor DarkGray
    }

    $code = Complete-EcSuite -Title $suite
    if ($ResultFile) { Set-Content -LiteralPath $ResultFile -Value ([string]$code) -Encoding ASCII }
    exit $code
} finally {
    Stop-EcOwnedWindows
    Remove-EcRunDir $run -Quiet
}
