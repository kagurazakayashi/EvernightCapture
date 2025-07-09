<#
.SYNOPSIS
    真机测试：截图文件的保存与覆盖保护（路径规划、碰撞检测、原子写入、--no-overwrite）。
.DESCRIPTION
    这一套只截本测试自己建立的窗口（ecwindow.exe），不走整屏路径，所以不会弹确认框，可以无人值守跑。
    所有图都写到本次运行专属的临时目录，收尾只删这一个目录；并发那一节另起独立子进程，
    每个子进程也只往本次那份目录里写。

    覆盖的事：
      1. --no-overwrite 每条布尔写法落到真实写入上的结果，以及"目标已存在时旧内容一个字都不变"
      2. 整批输出名在取帧之前一次算好：任何两个目标撞名就整批报错，一张也不会落地
      3. %n 的清洗：非法字符、尾部点与空格、保留设备名、长度与 UTF-16 代理对边界
      4. %d / %t 一个批次只取一次时钟
      5. 写文件是原子的：目标要么保持原样要么整体换成新内容，失败只清自己的临时文件
      6. 多个进程同时对同一路径禁止覆盖写入：最多一个成功，其余 io.file_exists，胜出那张完整
      7. --out - 不参与路径展开，也不会在工作目录里凭空写出文件
      8. 本机注入不了的两项（短写、网络共享上的提交语义）如实记成未验证
.EXAMPLE
    .\tests\save.ps1
    .\tests\save.ps1 -Keep      # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$run = New-EcRunDir -Tag 'save'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

function New-ShotDir {
    <# 每一节用自己的子目录：上一节留下的图看起来和本节的成功一模一样。 #>
    param([Parameter(Mandatory)][string]$Name)

    $dir = Join-Path $run.Path $Name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
}

function Get-HashOf {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Get-JsonOf {
    param($Result)
    $body = if ($Result.Stdout.Trim()) { $Result.Stdout } else { $Result.Stderr }
    try { return $body | ConvertFrom-Json } catch { return $null }
}

function Get-Codes {
    param($List)
    if ($null -eq $List) { return @() }
    return @($List | ForEach-Object { $_.code })
}

function Get-Temps {
    <# 本次没清理掉的临时文件：FileSave 只认 ~<目标名>.ecapture-<pid>-… 这个形状。 #>
    param([string]$Dir)
    if (-not (Test-Path -LiteralPath $Dir)) { return @() }
    return @(Get-ChildItem -LiteralPath $Dir -File -Filter '~*.tmp')
}

function Test-CompleteImage {
    <# 「不是半成品」的判据：按容器自己声明的长度核对，与像素内容无关（指针之类不影响）。 #>
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 26) { return $false }
    if ($bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50) {
        # PNG：宽高要在 IHDR 里，结尾必须是 IEND 块
        $tail = [Text.Encoding]::ASCII.GetString($bytes[($bytes.Length - 8)..($bytes.Length - 1)])
        return ($tail -like '*IEND*' -and $bytes[19] -gt 0 -and $bytes[23] -gt 0)
    }
    if ($bytes[0] -eq 0x42 -and $bytes[1] -eq 0x4D) {
        $declared = [int]$bytes[2] -bor ([int]$bytes[3] -shl 8) -bor ([int]$bytes[4] -shl 16) -bor ([int]$bytes[5] -shl 24)
        return ($declared -eq $bytes.Length)
    }
    return $false
}

function Test-NoLoneSurrogate {
    <# 名字里不许出现落单的代理项码元：那是非法 UTF-16，写出去的路径各处表现都不一样。 #>
    param([string]$Text)

    for ($i = 0; $i -lt $Text.Length; $i++) {
        $c = $Text[$i]
        if ([char]::IsHighSurrogate($c)) {
            if ($i + 1 -ge $Text.Length) { return $false }
            if (-not [char]::IsLowSurrogate($Text[$i + 1])) { return $false }
            $i++
        } elseif ([char]::IsLowSurrogate($c)) {
            return $false
        }
    }
    return $true
}

function Start-Shot {
    <# 一次截图。目标一律用 --hwnd 点名；工作目录设成输出目录，任何"以相对路径落地"的东西都跑不掉。 #>
    param(
        [string[]]$Hwnds,
        [string]$Out,
        [string[]]$Extra = @(),
        [switch]$All
    )

    $argv = @()
    foreach ($h in $Hwnds) { $argv += @('--hwnd', $h) }
    if ($All) { $argv += '--all' }
    if ($Extra) { $argv += $Extra }
    $argv += @('--out', $Out)
    # 工作目录设成输出目录，任何"以相对路径落地"的东西都跑不掉；目录本身不存在时不设，
    # 否则子进程连启动都会失败，判据就变成"起不来"而不是"写不进去了"。
    $workdir = if ($Out -eq '-') { $run.Path } else { Split-Path -Parent $Out }
    if ($workdir -and -not (Test-Path -LiteralPath $workdir)) { $workdir = $run.Path }
    return (Invoke-EcProcess -FilePath $Exe -Arguments $argv -TimeoutMs 60000 -WorkingDirectory $workdir)
}

$RECT_A = '120,120,560,440'
$RECT_B = '620,120,1060,440'
$RECT_BIG = '80,80,1500,940'      # 大窗口 -> 未压缩 BMP 有好几 MB，写入窗口才够长

try {
    $solo = Start-EcWindow -RunDir $run -Class "ec-save-solo-$tag" `
             -Title "保存测试单窗 $tag" -Rect $RECT_A -Seed 3
    $soloHex = Get-EcHwndHex $solo.Hwnd
    Write-Host ("通用目标 PID={0} HWND={1}" -f $solo.Pid, $soloHex)

    # =========================================================================
    Write-Host "`n=== 1) --no-overwrite 的布尔写法，落到真实写入上 ==="
    # =========================================================================
    $dir1 = New-ShotDir 'overwrite'
    $t1 = Join-Path $dir1 'one.png'
    $r = Start-Shot -Hwnds @($soloHex) -Out $t1
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 1) "首次写入失败（exit=$($r.Exit) $($r.Stdout)$($r.Stderr)）"
    Assert-Ec (Test-Path -LiteralPath $t1) "首次写入没建出文件：$t1"
    Assert-Ec (@(Get-Temps $dir1).Count -eq 0) '首次写入后目录里还留着本次的临时文件'
    $first = Get-HashOf $t1
    $firstWrite = (Get-Item -LiteralPath $t1).LastWriteTime

    foreach ($on in @('--no-overwrite', '--no-overwrite=true', '--no-overwrite=1',
                      '--no-overwrite=yes', '--no-overwrite=y', '--no-overwrite=on')) {
        $r = Start-Shot -Hwnds @($soloHex) -Out $t1 -Extra @($on)
        $o = Get-JsonOf $r
        Assert-Ec ($r.Exit -eq 8 -and ((Get-Codes $o.errors) -contains 'io.file_exists')) `
            "$on 撞上已存在的目标该报 io.file_exists + 退出码 8（exit=$($r.Exit) codes=[$((Get-Codes $o.errors) -join ',')]）"
        Assert-Ec ($o.captured -eq 0) "$on 那一次不该算成功（captured=$($o.captured)）"
        Assert-Ec ((Get-HashOf $t1) -eq $first) "$on 那一次旧文件内容被改动了"
        Assert-Ec (@(Get-Temps $dir1).Count -eq 0) "$on 那一次留下了没清理的临时文件"
    }
    Assert-Ec ((Get-Item -LiteralPath $t1).LastWriteTime -eq $firstWrite) '被拒绝的覆盖改动了目标文件的修改时间'

    foreach ($off in @('--no-overwrite=false', '--no-overwrite=0', '--no-overwrite=no',
                       '--no-overwrite=n', '--no-overwrite=off')) {
        # 同一扇窗口的 PNG 每次都一样，所以先塞一段哨兵，否则"覆盖成功"根本看不出来
        [IO.File]::WriteAllBytes($t1, [Text.Encoding]::ASCII.GetBytes('sentinel-' * 400))
        $before = Get-HashOf $t1
        $r = Start-Shot -Hwnds @($soloHex) -Out $t1 -Extra @($off)
        Assert-Ec ($r.Exit -eq 0) "$off 该取消禁令并覆盖成功（exit=$($r.Exit) $($r.Stdout)）"
        Assert-Ec (Test-CompleteImage $t1) "$off 之后目标不是一张完整图"
        Assert-Ec ((Get-HashOf $t1) -ne $before) "$off 那一次声称成功却没真的换内容"
        Assert-Ec (@(Get-Temps $dir1).Count -eq 0) "$off 那一次留下了没清理的临时文件"
    }

    # 重复给出：最后一个生效（顺序语义与其余选项一致）
    $r = Start-Shot -Hwnds @($soloHex) -Out $t1 -Extra @('--no-overwrite', '--no-overwrite=false')
    Assert-Ec ($r.Exit -eq 0) "--no-overwrite --no-overwrite=false 应以最后一个为准（exit=$($r.Exit)）"
    $r = Start-Shot -Hwnds @($soloHex) -Out $t1 -Extra @('--no-overwrite=false', '--no-overwrite')
    Assert-Ec ($r.Exit -eq 8) "--no-overwrite=false --no-overwrite 应以最后一个为准（exit=$($r.Exit)）"
    $r = Start-Shot -Hwnds @($soloHex) -Out $t1 -Extra @('--no-overwrite=true', '--no-overwrite=false')
    Assert-Ec ($r.Exit -eq 0) "--no-overwrite=true --no-overwrite=false 应以最后一个为准（exit=$($r.Exit)）"

    # 非法取值：在解析期就拒绝，不留到写文件那一步
    $r = Start-Shot -Hwnds @($soloHex) -Out $t1 -Extra @('--no-overwrite=maybe')
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 1 -and ((Get-Codes $o.errors) -contains 'cli.switch_takes_no_value')) `
        "--no-overwrite=maybe 该在解析期被拒（exit=$($r.Exit)）"
    Assert-Ec ((Get-HashOf $t1) -ne '') '--no-overwrite=maybe 那一次目标被动过'

    # =========================================================================
    Write-Host "`n=== 2) 原子提交：失败只毁本次的临时文件，目标保持原样 ==="
    # =========================================================================
    $dir2 = New-ShotDir 'atomic'
    $held = Join-Path $dir2 'held.png'
    [IO.File]::WriteAllBytes($held, [Text.Encoding]::ASCII.GetBytes('k' * 4096))
    $heldHash = Get-HashOf $held

    # (a) 目标被别的进程读着（不给写/删）-> 提交失败，旧内容一字不改
    $locker = [IO.File]::Open($held, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $r = Start-Shot -Hwnds @($soloHex) -Out $held
        $o = Get-JsonOf $r
        Assert-Ec ($r.Exit -eq 8) "目标被占着时该报写文件失败（exit=$($r.Exit)）"
        Assert-Ec (((Get-Codes $o.errors) -contains 'io.write_failed') -or
                   ((Get-Codes $o.errors) -contains 'io.file_exists')) `
            "目标被占着时的 code 不对：[$((Get-Codes $o.errors) -join ',')]"
        Assert-Ec ((Get-HashOf $held) -eq $heldHash) '提交失败把旧文件改了或清空了'
        Assert-Ec (@(Get-Temps $dir2).Count -eq 0) '提交失败后留下了本次的临时文件'
    } finally { $locker.Dispose() }

    # 占用解除后同一目标名要还能写成功（证明上一次的失败没把名字卡住）
    $r = Start-Shot -Hwnds @($soloHex) -Out $held
    Assert-Ec ($r.Exit -eq 0 -and (Test-CompleteImage $held)) "解除占用后同一目标名写不成功（exit=$($r.Exit)）"
    Assert-Ec ((Get-HashOf $held) -ne $heldHash) '解除占用后那一次没真的覆盖'

    # (b) 目标名是一个已存在的目录 -> 失败，目录还在
    $asDir = Join-Path $dir2 'isdirectory.png'
    New-Item -ItemType Directory -Path $asDir | Out-Null
    $r = Start-Shot -Hwnds @($soloHex) -Out $asDir
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 8 -and ((Get-Codes $o.errors) -contains 'io.write_failed')) `
        "目标名是个目录时该报 io.write_failed（exit=$($r.Exit)）"
    Assert-Ec (Test-Path -LiteralPath $asDir) '目标目录被顺手删掉了'
    Assert-Ec (@(Get-Temps $dir2).Count -eq 0) '目标是个目录时留下了本次的临时文件'

    # (c) 输出目录不存在 -> 报目录不存在，且不替调用方建目录
    $missing = Join-Path $dir2 'no-such-dir\x.png'
    $r = Start-Shot -Hwnds @($soloHex) -Out $missing
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 8 -and ((Get-Codes $o.errors) -contains 'io.write_failed')) `
        "目录不存在时该报 io.write_failed（exit=$($r.Exit)）"
    Assert-Ec (@($o.errors)[0].hint -match '目录') "目录不存在时的 hint 没提到目录：[$(@($o.errors)[0].hint)]"
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $dir2 'no-such-dir'))) '工具替调用方建了目录'

    # (d) 写到一半被硬杀：目标要么是旧内容要么是完整新图，绝不会是半成品
    $killDir = New-ShotDir 'interrupt'
    $killTarget = Join-Path $killDir 'old-content.bmp'
    $oldText = [Text.Encoding]::ASCII.GetBytes('old-' * 512)
    [IO.File]::WriteAllBytes($killTarget, $oldText)
    $oldHash = Get-HashOf $killTarget
    $big = Start-EcWindow -RunDir $run -Class "ec-save-big-$tag" -Title "大窗口 $tag" -Rect $RECT_BIG -Seed 4
    $bigHex = Get-EcHwndHex $big.Hwnd
    Write-Host ("  大窗口 PID={0} HWND={1}（未压缩 BMP 才有够长的写入窗口）" -f $big.Pid, $bigHex)

    $sawTempFile = $false
    $sawKilledBeforeCommit = $false
    foreach ($delay in 200, 280, 340, 400, 460, 520, 600, 700) {
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 30000 -ProbeTimeoutMs 12000 `
             -WorkingDirectory $killDir `
             -Arguments @('--hwnd', $bigHex, '--format', 'bmp', '--out', $killTarget) `
             -Probe {
                param($p)
                if ($watch.ElapsedMilliseconds -lt $delay) { return $false }
                [void](Stop-EcProcessTree -Process $p)
                return $true
             }
        $now = Get-HashOf $killTarget
        $temps = @(Get-Temps $killDir)
        foreach ($t in $temps) {
            Assert-Ec ($t.Name -like '~old-content.bmp.ecapture-*.tmp') "残留的临时文件名不对劲：$($t.Name)"
            $sawTempFile = $true
            Remove-Item -LiteralPath $t.FullName -Force
        }
        if ($now -eq $oldHash) {
            if ($r.Exit -ne 0) { $sawKilledBeforeCommit = $true }
        } else {
            Assert-Ec (Test-CompleteImage $killTarget) "第 ${delay}ms 硬杀之后目标成了半成品"
        }
    }
    Assert-Ec (Test-CompleteImage $killTarget) '最后一轮之后目标不是完整图'
    if ($sawKilledBeforeCommit -and $sawTempFile) {
        Write-Host '  硬杀确实落在过"临时文件已建、尚未提交"之间' -ForegroundColor DarkGray
    } else {
        Skip-Ec '硬杀落在写入窗口之内（临时文件已在、目标仍是旧内容）' `
            -Reason "八个时间点上都没撞上写入那几毫秒（sawTemp=$sawTempFile sawKilled=$sawKilledBeforeCommit）；半成品判据本身每次都验了"
    }

    # (e) 本机注入不了的两项，如实记未验证
    Skip-Ec '短写（WriteFile 报成功但字节数不足）的注入' `
        -Reason '外部没有可注入点，要造得靠满盘/只读卷或测试钩子；本机没有，代码里的长度核对未实测'
    Skip-Ec '网络共享上的提交语义（MOVEFILE_REPLACE_EXISTING 由服务端决定）' `
        -Reason '本机没有可用的 SMB 测试共享，不拿它当已验证'

    # =========================================================================
    Write-Host "`n=== 3) 并发禁止覆盖：最多一个成功 ==="
    # =========================================================================
    $dir3 = New-ShotDir 'concurrent'
    $race = Join-Path $dir3 'race.png'
    $racers = @()
    $n = 0
    foreach ($n in 1..5) {
        $p = Start-Process -FilePath $Exe `
             -ArgumentList (Format-EcWindowsArgv -Arguments @('--hwnd', $soloHex, '--no-overwrite', '--out', $race)) `
             -NoNewWindow -PassThru -WorkingDirectory $dir3 `
             -RedirectStandardOutput (Join-Path $dir3 "log-$n.out") `
             -RedirectStandardError (Join-Path $dir3 "log-$n.err")
        $racers += [pscustomobject]@{ Proc = $p; Id = [int]$p.Id; Log = "log-$n" }
    }
    Write-Host ("  同时起了 {0} 个 ECAPTURE" -f $racers.Count)
    foreach ($w in $racers) {
        $deadline = (Get-Date).AddSeconds(90)
        while (-not $w.Proc.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
        Assert-Ec $w.Proc.HasExited "并发子进程 $($w.Id) 90 秒还没结束"
    }
    $wins = 0; $refused = 0; $strange = 0
    foreach ($w in $racers) {
        # Start-Process -PassThru 给的进程对象要先正经等一下，ExitCode 才读得到
        try { [void]$w.Proc.WaitForExit() } catch { }
        $code = $null
        try { $code = $w.Proc.ExitCode } catch { }
        $blob = ''
        foreach ($suffix in @('out', 'err')) {
            $log = Join-Path $dir3 "$($w.Log).$suffix"
            if (Test-Path -LiteralPath $log) { $blob += (Get-Content -LiteralPath $log -Raw) }
        }
        $json = $null
        try { $json = $blob | ConvertFrom-Json } catch { }
        if ($null -eq $code) {
            # 读不到退出码时按 JSON 判（退出码本身的判据在第 1 节的单进程用例里）
            if ($json -and @($json.images).Count -eq 1) { $code = 0 }
            elseif ($blob -match 'io\.file_exists') { $code = 8 }
            else { $code = -1 }
        }
        if ($code -eq 0) {
            $wins++
            if (-not $json -or @($json.images).Count -ne 1) { $strange++ }
        } elseif ($code -eq 8 -and $blob -match 'io\.file_exists') {
            $refused++
        } else {
            $strange++
            Write-Host ("  意外的并发结果 pid=$($w.Id) exit=$code 输出=$($blob.Trim())") -ForegroundColor DarkGray
        }
    }
    Assert-Ec ($strange -eq 0) "并发里有 $strange 个进程给出了预料之外的结果"
    Assert-Ec ($wins -le 1) "并发禁止覆盖成功了 $wins 个，最多只能 1 个"
    Assert-Ec (($wins + $refused) -eq $racers.Count) '并发里有进程既没成功也没报 io.file_exists'
    Assert-Ec ($wins -eq 1) "并发里一个都没成功（拒绝 $refused 个）"
    Assert-Ec (Test-CompleteImage $race) '并发胜出的那张不是完整图'
    Assert-Ec (@(Get-Temps $dir3).Count -eq 0) '并发结束后留下了临时文件'
    $works = @(Get-ChildItem -LiteralPath $dir3 -File | Where-Object { $_.Name -notlike 'log-*' })
    Assert-Ec ($works.Count -eq 1 -and $works[0].Name -eq 'race.png') `
        "并发那节的目录里多出了别的东西：[$($works.Name -join ' ')]"

    # =========================================================================
    Write-Host "`n=== 4) 整批输出名先规划：撞名就整批不截 ==="
    # =========================================================================
    $dir4 = New-ShotDir 'collision'
    # 同一进程的两扇窗口：同 PID、同标题 —— %p / %n 的撞名只有这么造才验得到
    $pair = Start-EcWindow -RunDir $run -Class "ec-save-pair-$tag" -Title "成对窗口 $tag" `
             -Rect $RECT_B -Seed 5 -Windows 2
    $pairHexes = @($pair.Hwnds | ForEach-Object { Get-EcHwndHex $_ })
    Assert-Ec ($pairHexes.Count -eq 2) "没造出同一进程的两扇窗口：[$($pairHexes -join ' ')]"
    Write-Host ("  同进程两扇 PID={0} HWND={1}" -f $pair.Pid, ($pairHexes -join ' '))

    function Assert-Collision {
        param([string]$Label, [string[]]$Hwnds, [string]$FileName)

        $template = Join-Path $dir4 $FileName
        $r = Start-Shot -Hwnds $Hwnds -Out $template -All
        $o = Get-JsonOf $r
        Assert-Ec ($r.Exit -eq 8 -and ((Get-Codes $o.errors) -contains 'io.output_collision')) `
            "${Label}：该报 io.output_collision + 退出码 8（exit=$($r.Exit) codes=[$((Get-Codes $o.errors) -join ',')]）"
        Assert-Ec ($o.captured -eq 0 -and @($o.images).Count -eq 0) "${Label}：撞名还给出了成功的图"
        $err = @($o.errors)[0]
        Assert-Ec ($err.option -eq '--out') "${Label}：错误的 option 不是 --out（$($err.option)）"
        Assert-Ec ($err.hint -and $err.hint -notmatch '\?io\.|%[1-9]') "${Label}：hint 没渲染好 [$($err.hint)]"
        Assert-Ec ($err.value -eq $template) "${Label}：value 该回显模板 [$($err.value)]"
        Assert-Ec (@(Get-ChildItem -LiteralPath $dir4 -File).Count -eq 0) "${Label}：撞名那一次居然往目录里落了文件"
    }

    Assert-Collision '%d 同一天' $pairHexes 'day-%d.png'
    Assert-Collision '%t 同一秒' $pairHexes 'sec-%t.png'
    Assert-Collision '%p 同进程' $pairHexes 'pid-%p.png'
    Assert-Collision '%n 同标题' $pairHexes 'name-%n.png'
    Assert-Collision '%% 与未知 %x' $pairHexes 'pct-%%-and-%q.png'

    # 不同进程、标题只差大小写：NTFS 上这两个名字就是同一个文件
    $caseA = Start-EcWindow -RunDir $run -Class "ec-save-caseA-$tag" -Title "EC-Case 大小写 $tag" -Rect $RECT_A -Seed 6
    $caseB = Start-EcWindow -RunDir $run -Class "ec-save-caseB-$tag" -Title "ec-case 大小写 $tag" -Rect $RECT_B -Seed 7
    Assert-Collision '%n 只差大小写' @((Get-EcHwndHex $caseA.Hwnd), (Get-EcHwndHex $caseB.Hwnd)) 'case-%n.png'

    # 清洗之后撞在一起：反斜杠与冒号都换成 _
    $sanA = Start-EcWindow -RunDir $run -Class "ec-save-sanA-$tag" -Title "EC\San:Test $tag" -Rect $RECT_A -Seed 8
    $sanB = Start-EcWindow -RunDir $run -Class "ec-save-sanB-$tag" -Title "EC_San_Test $tag" -Rect $RECT_B -Seed 9
    Assert-Collision '%n 清洗后相同' @((Get-EcHwndHex $sanA.Hwnd), (Get-EcHwndHex $sanB.Hwnd)) 'san-%n.png'

    # 截断之后撞在一起：前 80 个码元完全相同，后面的差异被截掉了
    $shared = ('甲' * 80)          # 80 个码元，正好顶到 %n 的上限，后面的差异会被截掉
    $truncA = Start-EcWindow -RunDir $run -Class "ec-save-truncA-$tag" -Title ($shared + 'AAA') -Rect $RECT_A -Seed 10
    $truncB = Start-EcWindow -RunDir $run -Class "ec-save-truncB-$tag" -Title ($shared + 'BBB') -Rect $RECT_B -Seed 11
    Assert-Collision '%n 截断后相同' @((Get-EcHwndHex $truncA.Hwnd), (Get-EcHwndHex $truncB.Hwnd)) 'trunc-%n.png'
    foreach ($w in @($caseA, $caseB, $sanA, $sanB, $truncA, $truncB)) { Stop-EcWindow -Window $w }

    # 该分开的必须分开：%h 与 %i 各自唯一
    foreach ($template in @('uniq-%h.png', 'uniq-%i.png')) {
        $r = Start-Shot -Hwnds $pairHexes -Out (Join-Path $dir4 $template) -All
        $o = Get-JsonOf $r
        Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 2) "${template} 该截出两张（exit=$($r.Exit) captured=$($o.captured)）"
        $names = @($o.images | ForEach-Object { $_.file })
        Assert-Ec (@($names | Sort-Object -Unique).Count -eq 2) "${template} 两张回的路径没分开：[$($names -join ' ')]"
        foreach ($f in $names) {
            Assert-Ec (Test-Path -LiteralPath $f) "${template} 回显的路径不在磁盘上：$f"
            Assert-Ec (Test-CompleteImage $f) "${template} 的回显路径不是完整图：$f"
        }
    }

    # 无占位符仍按原约定追加 _序号
    $r = Start-Shot -Hwnds $pairHexes -Out (Join-Path $dir4 'plain.png') -All
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 2) "无占位符时该按原约定追加序号（exit=$($r.Exit)）"
    Assert-Ec ((Get-Codes $o.notes) -contains 'note.all_without_placeholder') '--all 无占位符没发 note.all_without_placeholder'
    Assert-Ec ((Test-Path -LiteralPath (Join-Path $dir4 'plain_1.png')) -and
               (Test-Path -LiteralPath (Join-Path $dir4 'plain_2.png'))) 'plain_1.png / plain_2.png 没都写出来'
    Assert-Ec (@(Get-Temps $dir4).Count -eq 0) '规划那一节留下了临时文件'

    # 批次时钟只取一次：同批两张的 %d-%t 段必须完全相同
    $clock = Start-Shot -Hwnds $pairHexes -Out (Join-Path $dir4 'clock-%d-%t-%i.png') -All
    $clockJson = Get-JsonOf $clock
    Assert-Ec ($clock.Exit -eq 0 -and $clockJson.captured -eq 2) "批次时钟那一次没截成两张（exit=$($clock.Exit)）"
    $stamps = @(@($clockJson.images) | ForEach-Object { [IO.Path]::GetFileName($_.file) })
    Assert-Ec ($stamps.Count -eq 2) "时钟那一次没拿到两个文件名：[$($stamps -join ' ')]"
    $tokens = @($stamps | ForEach-Object { ($_ -replace '^clock-(\d{8}-\d{6})-\d+\.png$', '$1') })
    Assert-Ec ((@($tokens | Sort-Object -Unique).Count) -eq 1) "同一批用了两个时钟：[$($stamps -join ' ')]"
    Assert-Ec ($tokens[0] -match '^\d{8}-\d{6}$') "时钟段的形状不对：$($tokens[0])"
    Stop-EcWindow -Window $pair

    # =========================================================================
    Write-Host "`n=== 5) %n 的形状：保留设备名、尾部点空格、长度与代理对 ==="
    # =========================================================================
    $dir5 = New-ShotDir 'naming'
    $win = Start-EcWindow -RunDir $run -Class "ec-save-con-$tag" -Title 'CON' -Rect $RECT_A -Seed 12
    $r = Start-Shot -Hwnds @((Get-EcHwndHex $win.Hwnd)) -Out (Join-Path $dir5 '%n.png')
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 1) "标题是 CON 时该正常落地（exit=$($r.Exit)）"
    $made = [IO.Path]::GetFileName(@($o.images)[0].file)
    Assert-Ec ($made -eq '_CON.png') "%n 撞上保留设备名该加前缀，实际是 $made"
    Assert-Ec (Test-Path -LiteralPath (Join-Path $dir5 '_CON.png')) '_CON.png 没写出来'
    Stop-EcWindow -Window $win

    $trail = Start-EcWindow -RunDir $run -Class "ec-save-trail-$tag" -Title 'EC-尾点与空格...   ' -Rect $RECT_A -Seed 13
    $r = Start-Shot -Hwnds @((Get-EcHwndHex $trail.Hwnd)) -Out (Join-Path $dir5 'tail-%n.png')
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 0) "尾部点/空格那一次没落地（exit=$($r.Exit) $($r.Stdout)）"
    $made = [IO.Path]::GetFileName(@($o.images)[0].file)
    Assert-Ec ($made -notmatch '[\. ]+\.png$') "清洗后名字尾部还留着点或空格：$made"
    Assert-Ec ($made -like 'tail-EC-*png') "名字对不上：$made"
    Stop-EcWindow -Window $trail

    # 第 80 个码元正好落在代理对里：截断不许把一个字符劈成两半
    $emoji = [string][char]0xD83D + [string][char]0xDE00
    $surr = Start-EcWindow -RunDir $run -Class "ec-save-surr-$tag" -Title (('A' * 79) + $emoji + $emoji + 'tail') `
             -Rect $RECT_A -Seed 14
    $r = Start-Shot -Hwnds @((Get-EcHwndHex $surr.Hwnd)) -Out (Join-Path $dir5 'surr-%n.png')
    $o = Get-JsonOf $r
    Assert-Ec ($r.Exit -eq 0) "代理对边界那一次没落地（exit=$($r.Exit) $($r.Stdout)）"
    $surrPath = @($o.images)[0].file
    $made = [IO.Path]::GetFileName($surrPath)
    Assert-Ec (Test-NoLoneSurrogate $made) "回显的名字里有落单的代理项码元：$made"
    $part = $made -replace '^surr-' -replace '\.png$'   # 只看 %n 那一截
    Assert-Ec ($part.Length -le 80) "%n 没按长度截断：$($part.Length) 个码元"
    Assert-Ec ($part -eq ('A' * 79)) "截断点该停在代理对之前，实际是：$part"
    Assert-Ec (Test-Path -LiteralPath $surrPath) "回显路径与磁盘上的对不上：$surrPath"
    Stop-EcWindow -Window $surr

    # 回显路径 = 实际写入路径：绝对、存在、字节数与 JSON 一致
    $dir6 = New-ShotDir 'echo'
    $r = Start-Shot -Hwnds @($soloHex) -Out (Join-Path $dir6 'echo-%h.png')
    $o = Get-JsonOf $r
    $img = @($o.images)[0]
    Assert-Ec ($r.Exit -eq 0) "回显那一次没成功（exit=$($r.Exit)）"
    Assert-Ec ([IO.Path]::IsPathRooted($img.file)) "file 不是绝对路径：$($img.file)"
    Assert-Ec (Test-Path -LiteralPath $img.file) "file 指向的文件不存在：$($img.file)"
    Assert-Ec ((Get-Item -LiteralPath $img.file).Length -eq $img.bytes) 'JSON 的 bytes 与实际文件大小不符'
    Assert-Ec (@(Get-Temps $dir6).Count -eq 0) '回显那一次留下了临时文件'

    # =========================================================================
    Write-Host "`n=== 6) --out - 不参与路径展开 ==="
    # =========================================================================
    $dir7 = New-ShotDir 'stdout'
    $pair3 = Start-EcWindow -RunDir $run -Class "ec-save-pipe-$tag" -Title "管道成对 $tag" -Rect $RECT_B -Seed 15 -Windows 2
    $pipe = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir7 -Arguments @(
        '--hwnd', (Get-EcHwndHex $pair3.Hwnds[0]), '--hwnd', (Get-EcHwndHex $pair3.Hwnds[1]),
        '--all', '--out', '-')
    $pj = Get-JsonOf @{ Stdout = ''; Stderr = $pipe.Stderr }
    Assert-Ec ($pipe.Exit -eq 0) "--all 写标准输出失败（exit=$($pipe.Exit) $($pipe.Stderr)）"
    Assert-Ec ($pj -and $pj.captured -eq 2) "标准输出那一次 captured=$($pj.captured)，期望 2"
    $files = @(@($pj.images) | ForEach-Object { $_.file } | Sort-Object -Unique)
    $msg = '标准输出时 file 该是单横杠：[' + ($files -join ' ') + ']'
    Assert-Ec ($files.Count -eq 1 -and $files[0] -eq '-') $msg
    Assert-Ec ($pipe.StdoutBytes.Length -gt 200) 'stdout 里没有两份图片字节'
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir7 -File).Count -eq 0) '--out - 在工作目录里写出了文件'
    Stop-EcWindow -Window $pair3
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '文件保存与覆盖保护')
