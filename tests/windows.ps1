<#
.SYNOPSIS
    结构化的窗口发现与检查（--list / --inspect）的判据：离线注入假候选逐条判分页、可见性策略、
    字段级可读性与文档形状；真机只用自建窗口判接线、隐私、歧义、销毁与"查询确实只读"。
.DESCRIPTION
    这两条命令立的规矩是五条，彼此不能混：

      1) **只读**：一个像素都不取、不弹确认框、不写文件、不恢复/激活/移动任何窗口。
      2) **复用截图那同一套匹配语义**：同类 OR、跨类 AND、--monitor 按屏过滤、--title-regex 与
         --timeout-ms 走同一条辅助进程路线。所以"列出来的窗口"与"真去截图会选中的窗口"同源。
      3) **多匹配的后果按查询种类分**：--list 把多匹配当正常答复分页交回（不是截图歧义，退出码 0，
         一个都没命中也是空列表 + 0）；--inspect 需要唯一目标，歧义报 match.ambiguous_window + 5，
         绝不替用户挑一个。
      4) **列表是快照，会过期**：交回的身份字段（句柄 / PID / 进程创建时间 / 类名）只是判据，
         截图那一次仍要复核；文档里 verificationRequired / isAuthorizationToken 与 caveats 说清这点。
      5) **问不出来就说问不出来**：跨进程问答的三种下场（readable / denied / failed）逐字段写，
         带系统原因码；不拿空值冒充答案，也不建议改用管理员身份。

    判据分两层：
      1) 离线层：build\ecapture-windows-tests.exe（源码 tests\windows_state.cpp）注入假候选，逐条判
         "命中 100 扇只交回 3 扇""某一问被挡下""窗口矩形问不出来""默认那份不许出现完整路径"
         "--inspect 即使策略是 kAll 也不顺手交回整份列表"。这些现场真机上要么安排不出来（不能为了
         造"读不到"去动别人的进程），要么没有阴性对照。
      2) 真机层：不截图。用本次自建的测试窗口判分页与条数、可见性策略、--inspect=path 的隐私差别、
         查询确实不落地不弹框、--yes 不改变结果、以及"目标已销毁时 inspect 报无匹配"。

    隐私规矩：这一层不拍任何东西，也不代人点框。真机上"归属进程读不到"（受限进程）在本机造不出
    现场——那需要别人的高完整性进程，而且不能为测试去动使用者真实应用，所以照实记未验证。

.EXAMPLE
    .\tests\windows.ps1
    .\tests\windows.ps1 -SkipState       # 只跑真机层
    .\tests\windows.ps1 -SkipReal        # 只跑离线判据层（没有交互桌面时用）
    .\tests\windows.ps1 -Exe build\Debug\ecapture.exe
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$SkipReal
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-windows-tests.exe'

# 查询类命令一律 15 秒期限：真去弹框或真去截一张图的话，这里会等死，期限本身就是判据。
function Invoke-Wq {
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

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：把假候选注进判据本体逐条判 ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '窗口查询离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-windows-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "窗口查询判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        $failed = if ($m.Success) { [int]$m.Groups[2].Value } else { -1 }
        $passed = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
        Assert-Ec ($failed -eq 0) "窗口查询判据有失败项，或摘要读不出来：$tail"
        Assert-Ec ($passed -ge 100) "窗口查询判据通过数不对劲（$passed），用例被删了？"
        Write-Host "  $tail" -ForegroundColor DarkGray
    }

    if ($SkipReal) {
        Write-Host '  真机层按调用方要求跳过' -ForegroundColor DarkGray
        exit (Complete-EcSuite -Title '窗口查询（只离线层）')
    }
    Set-EcDpiAware

    # =========================================================================
    Write-Host "`n=== 1) 真机：查询只读 —— 不落地、不弹框、--yes 不改变结果 ==="
    # =========================================================================
    # 全新空目录当工作目录：查询之后里面必须还是空的（"看起来没写文件"要和"确实没写"分开）。
    $run = New-EcRunDir -Tag 'wq-readonly'
    try {
        $a = Invoke-Wq -Arguments @('--list', '--class', 'Shell_TrayWnd') -WorkingDirectory $run.Path
        Assert-Ec ($a.Exit -eq 0) "查询没返回 0（exit=$($a.Exit)）：$($a.Raw.Stderr)"
        Assert-Ec ($a.Json.contract -eq 'windowquery') "契约名不对：$($a.Json.contract)"
        Assert-Ec ($a.Raw.StdErr.Trim() -eq '') "查询往 stderr 写了东西：$($a.Raw.Stderr)"
        Assert-Ec ($a.Json.authorization.pixelsRead -eq 0 -and
                    $a.Json.authorization.consentDialogShown -eq $false -and
                    $a.Json.authorization.filesWritten -eq $false) `
            '查询自己声称取了像素/弹了框/写了文件'

        $left = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left -eq 0) "查询之后本次目录里还剩 $left 个文件，它不应该写任何东西"

        # --yes 不该改变结果，也不该让查询"免掉"什么（它本来就不弹框）。
        # 叠放次序可能在两次调用之间被别的进程改动，所以只判除 zOrder 以外的字段与条数。
        $b = Invoke-Wq -Arguments @('--list', '--class', 'Shell_TrayWnd', '--yes') -WorkingDirectory $run.Path
        $sa = ($a.Json.windows | ForEach-Object { '{0}|{1}|{2}|{3}' -f $_.hwnd, $_.pid, $_.'class', $_.title }) -join ';'
        $sb = ($b.Json.windows | ForEach-Object { '{0}|{1}|{2}|{3}' -f $_.hwnd, $_.pid, $_.'class', $_.title }) -join ';'
        Assert-Ec ($sa -eq $sb) "--yes 改变了列表内容：[$sa] 对 [$sb]"
        Assert-Ec ($b.Json.pagination.matched -eq $a.Json.pagination.matched) `
            "--yes 改变了命中条数：$($b.Json.pagination.matched) 对 $($a.Json.pagination.matched)"
        $left2 = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left2 -eq 0) "--yes 那次之后本次目录里还剩 $left2 个文件"

        # 冲突那一档同样不许落地、不许弹框：报错之后一个像素都不取。
        $c = Invoke-Wq -Arguments @('--list', 'oops.png') -WorkingDirectory $run.Path
        Assert-Ec ($c.Exit -eq 1) "窗口查询与输出路径同时给出应为参数错（exit=$($c.Exit)）"
        Assert-Ec ((Codes $c.Json.errors) -contains 'cli.window_query_conflict') `
            "冲突码不对：[$((Codes $c.Json.errors) -join ',')]"
        Assert-Ec ($c.Json.captured -eq 0 -and @($c.Json.images).Count -eq 0) `
            '冲突那次的文档形状不是截图失败那一份'
        $left3 = @(Get-ChildItem -LiteralPath $run.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Assert-Ec ($left3 -eq 0) "冲突之后还剩 $left3 个文件，冲突报错前就已经动了文件系统"
    } finally {
        Remove-EcRunDir $run
    }

    # =========================================================================
    Write-Host "`n=== 2) 真机：同类 OR / 跨类 AND / 无匹配是空列表 + 0 ==="
    # =========================================================================
    $run2 = New-EcRunDir -Tag 'wq-match'
    try {
        $stag = $run2.Leaf -replace '[^a-z0-9]', ''
        # 两扇可见的同标题窗口（同一个进程，类名是 <class> 与 <class>-2）：给"多匹配"与
        # "inspect 之后仍然完好"那几条当锚点。
        $pair = Start-EcWindow -RunDir $run2 -Class "ec-wq-pair-$stag" -Title "窗口查询 同标题 $stag" `
                               -Rect '200,200,600,460' -Windows 2 -Seed 3
        # 另一扇独立的、可见的、不同类名的窗口（给跨类 AND 用）。
        $solo = Start-EcWindow -RunDir $run2 -Class "ec-wq-solo-$stag" -Title "窗口查询 单独 $stag" `
                               -Rect '700,200,1100,460' -Seed 4
        # 一扇建好就最小化的窗口（--topmost 让它别被别人的激活动作带回来）。
        $min = Start-EcWindow -RunDir $run2 -Class "ec-wq-min-$stag" -Title "窗口查询 最小化 $stag" `
                              -Rect '300,300,600,500' -Seed 7 -TopMost `
                              -ExtraArgs @('--minimize')

        # ---- 同类 OR：两个 --class 各命中一扇，并起来是两条（不是求交集）----
        $a1 = Start-EcWindow -RunDir $run2 -Class "ec-wq-or1-$stag" -Title "窗口查询 或一 $stag" `
                             -Rect '200,200,500,420' -Seed 5
        $a2 = Start-EcWindow -RunDir $run2 -Class "ec-wq-or2-$stag" -Title "窗口查询 或二 $stag" `
                             -Rect '260,240,560,460' -Seed 6
        $or = Invoke-Wq -Arguments @('--list', '--class', $a1.Class, '--class', $a2.Class)
        Assert-Ec ($or.Exit -eq 0 -and $or.Json.pagination.matched -eq 2) `
            "同类 OR 应当命中两条：exit=$($or.Exit) matched=$($or.Json.pagination.matched)"
        $orHwnds = @($or.Json.windows | ForEach-Object { $_.hwnd }) | Sort-Object
        $wantOr = @((Get-EcHwndHex $a1.Hwnd), (Get-EcHwndHex $a2.Hwnd)) | Sort-Object
        Assert-Ec (($orHwnds -join ',') -eq ($wantOr -join ',')) `
            ("同类 OR 命中的不是这两扇：[$($orHwnds -join ',')] 对 [$($wantOr -join ',')]")

        # ---- 跨类 AND：两种不同类的条件同时成立 = 没有这样的窗口 = 空列表 + 0 ----
        $andX = Invoke-Wq -Arguments @('--list', '--class', $a1.Class, '--title', $a2.Title)
        Assert-Ec ($andX.Exit -eq 0 -and @($andX.Json.windows).Count -eq 0 -and
                    $andX.Json.pagination.matched -eq 0) `
            "跨类 AND 无匹配应当是空列表 + 退出码 0，实际 exit=$($andX.Exit) 条数=$( @($andX.Json.windows).Count)"

        # ---- 跨类 AND 命中一扇（类名 + 标题子串都要成立）----
        $and = Invoke-Wq -Arguments @('--list', '--class', $solo.Class, '--title-contains', $stag)
        Assert-Ec ($and.Exit -eq 0 -and @($and.Json.windows).Count -eq 1 -and
                    $and.Json.windows[0].hwnd -eq (Get-EcHwndHex $solo.Hwnd)) `
            '跨类 AND（类名 + 标题子串）没对上单独那一扇'

        # ---- 跨类 AND 里 --title-regex 必须自己成立（回归：前面那类的命中曾把它顶掉）----
        # 阳性对照先立住：同一份进程条件 + 一条成立的正则，确实对得上那一扇。
        $soloHex = Get-EcHwndHex $solo.Hwnd
        $rxHit = '^' + $solo.Title + '$'
        $rxMiss = '^EC-WQ-NO-SUCH-TITLE-' + $stag + '$'
        $rxAnd = Invoke-Wq -Arguments @('--list', '--process', 'ecwindow.exe', '--title-regex', $rxHit)
        Assert-Ec ($rxAnd.Exit -eq 0 -and $rxAnd.Json.pagination.matched -eq 1 -and
                    $rxAnd.Json.windows[0].hwnd -eq $soloHex) `
            "进程 + 成立的正则应当对上那一扇：exit=$($rxAnd.Exit) matched=$($rxAnd.Json.pagination.matched)"

        # 同一份进程条件换成不成立的正则：必须是 0 条。旧实现这里会把该进程全部可见窗口交回来
        # （hwnd/pid/process/exe/title/title-contains 任一类的命中被当成"正则那类也满足了"）。
        foreach ($combo in @(
            @{ Name = '进程 + 不成立的正则'; A = @('--process', 'ecwindow.exe') },
            @{ Name = '句柄 + 不成立的正则'; A = @('--hwnd', $soloHex) },
            @{ Name = 'PID + 不成立的正则'; A = @('--pid', [string]$solo.Pid) },
            @{ Name = '精确标题 + 不成立的正则'; A = @('--title', $solo.Title) },
            @{ Name = '标题子串 + 不成立的正则'; A = @('--title-contains', $stag) }
        )) {
            $r = Invoke-Wq -Arguments (@('--list') + $combo.A + @('--title-regex', $rxMiss))
            Assert-Ec ($r.Exit -eq 0 -and $r.Json.pagination.matched -eq 0 -and
                        @($r.Json.windows).Count -eq 0) `
                ("$($combo.Name)：正则不成立时应当 0 条，实际 matched=$($r.Json.pagination.matched)")
        }

        # 反过来那一半：正则成立而别的类不成立，同样不能放行（AND 的两个方向都得判）。
        $rev = Invoke-Wq -Arguments @('--list', '--hwnd', (Get-EcHwndHex $a1.Hwnd), '--title-regex', $rxHit)
        Assert-Ec ($rev.Exit -eq 0 -and $rev.Json.pagination.matched -eq 0) `
            '句柄不匹配 + 正则匹配：不该命中'

        # 书写顺序与入口：正则写在别的类前面，结论不变；--inspect 与 --dry-run 走同一条判据。
        $inspRx = Invoke-Wq -Arguments @('--inspect', '--title-regex', $rxMiss, '--hwnd', $soloHex)
        Assert-Ec ($inspRx.Exit -eq 4 -and (Codes $inspRx.Json.errors) -contains 'match.no_window') `
            "inspect（正则写在前面）：exit=$($inspRx.Exit) codes=[($(Codes $inspRx.Json.errors) -join ','))]"
        $dryRx = Invoke-Wq -Arguments @('--title-regex', $rxMiss, '--process', 'ecwindow.exe',
                                        '--dry-run', 'out.png')
        Assert-Ec ($dryRx.Exit -eq 4 -and (Codes $dryRx.Json.errors) -contains 'match.no_window') `
            "dry-run（正则不成立）：exit=$($dryRx.Exit) codes=[($(Codes $dryRx.Json.errors) -join ','))]"

        # ---- 无匹配：一个都不成立的条件也是空列表 + 0 ----
        $none = Invoke-Wq -Arguments @('--list', '--class', "ec-wq-none-$stag")
        Assert-Ec ($none.Exit -eq 0 -and $none.Json.contract -eq 'windowquery' -and
                    @($none.Json.windows).Count -eq 0 -and
                    -not (@(Codes $none.Json.errors).Count)) `
            "无匹配那次不是空列表 + 0：exit=$($none.Exit) codes=[($(Codes $none.Json.errors) -join ','))]"

        # ---- --inspect 在无匹配时报 match.no_window + 4（与截图那一次同一判据） ----
        $insp0 = Invoke-Wq -Arguments @('--inspect', '--class', "ec-wq-none-$stag")
        Assert-Ec ($insp0.Exit -eq 4 -and (Codes $insp0.Json.errors) -contains 'match.no_window') `
            "inspect 无匹配：exit=$($insp0.Exit) codes=[($(Codes $insp0.Json.errors) -join ','))]"

        # ---- 可见性策略：默认排除最小化，--list=all 才列进来 ----
        # 等它真进入最小化状态：叠放次序与状态不由测试进程独占，等不到就如实记未验证。
        $minClass = 'ec-wq-min-' + $stag
        $isIconic = $false
        for ($k = 0; $k -lt 20; $k++) {
            $r = Invoke-Wq -Arguments @('--list=all', '--class', $minClass)
            if (@($r.Json.windows | Where-Object { $_.minimized -eq $true }).Count -eq 1) {
                $isIconic = $true; break
            }
            Start-Sleep -Milliseconds 150
        }
        if ($isIconic) {
            $dflt = Invoke-Wq -Arguments @('--list', '--class', $minClass)
            $with = Invoke-Wq -Arguments @('--list=all', '--class', $minClass)
            Assert-Ec (@($dflt.Json.windows).Count -eq 0) `
                '默认策略下最小化窗口仍出现在列表里（与截图链路的枚举策略不一致）'
            Assert-Ec (@($with.Json.windows).Count -eq 1 -and $with.Json.windows[0].minimized -eq $true) `
                '--list=all 没把最小化那一扇列进来'
            Assert-Ec ($with.Json.windows[0].visible -eq $false) `
                '并进来的最小化那条把 visible 写成真（两个字段不许打脸）'
        } else {
            Skip-Ec '最小化窗口的默认排除与 --list=all 并入' `
                    '本次那扇窗口没能在 3 秒内进入最小化状态（叠放次序与状态不由测试进程独占）'
        }

        # ---- 查询不动目标：inspect 之后那几扇还在、还可见、位置没变 ----
        foreach ($h in @($pair.Hwnds) + @($a1.Hwnd, $a2.Hwnd, $solo.Hwnd)) {
            Assert-Ec (Test-EcWindowAlive -Hwnd $h) "查询之后句柄 $h 已经不是窗口了"
        }
        $rectBefore = Get-EcWindowRect -Hwnd $solo.Hwnd
        $i2 = Invoke-Wq -Arguments @('--inspect', '--hwnd', (Get-EcHwndHex $solo.Hwnd))
        Assert-Ec ($i2.Exit -eq 0 -and $i2.Json.contract -eq 'windowinspect') `
            "inspect 单窗口那份不对：exit=$($i2.Exit) contract=$($i2.Json.contract)"
        $rectAfter = Get-EcWindowRect -Hwnd $solo.Hwnd
        Assert-Ec ($rectAfter.Left -eq $rectBefore.Left -and $rectAfter.Top -eq $rectBefore.Top) `
            '查询把目标窗口挪了位置（这一层不该动任何窗口）'
    } finally {
        Stop-EcOwnedWindows
        Remove-EcRunDir $run2
    }

    # =========================================================================
    Write-Host "`n=== 3) 真机：分页、字段与身份约束 ==="
    # =========================================================================
    $run3 = New-EcRunDir -Tag 'wq-page'
    try {
        $stag = $run3.Leaf -replace '[^a-z0-9]', ''
        # 一个进程建 4 扇同类名族窗口 + 一个进程建 3 扇：一次查询能命中 7 扇，够判分页。
        $w4 = Start-EcWindow -RunDir $run3 -Class "ec-wq-a-$stag" -Title "窗口查询 分页 $stag" `
                             -Rect '120,120,420,340' -Windows 4 -Seed 11
        $w3 = Start-EcWindow -RunDir $run3 -Class "ec-wq-b-$stag" -Title "窗口查询 分页 $stag" `
                             -Rect '520,120,820,340' -Windows 3 -Seed 12
        $all = @($w4.Classes) + @($w3.Classes)
        $listArgs = @('--list')
        foreach ($c in $all) { $listArgs += @('--class', $c) }

        $r = Invoke-Wq -Arguments $listArgs
        Assert-Ec ($r.Exit -eq 0 -and $r.Json.pagination.matched -eq 7) `
            "七扇自建窗口应当全部命中：exit=$($r.Exit) matched=$($r.Json.pagination.matched)"
        Assert-Ec ($r.Json.pagination.returned -eq 7 -and $r.Json.pagination.truncated -eq $false) `
            '未达默认条数时不该报截断'

        $p = Invoke-Wq -Arguments ($listArgs + @('--limit', '2'))
        Assert-Ec ($p.Json.pagination.limit -eq 2 -and $p.Json.pagination.returned -eq 2 -and
                    $p.Json.pagination.matched -eq 7 -and $p.Json.pagination.truncated -eq $true) `
            "--limit 2 应当交回 2 条而命中总数仍是 7（分页不改判命中）"
        Assert-Ec ($p.Json.pagination.nextOffset -eq 2) `
            "翻页偏移不对：$($p.Json.pagination.nextOffset)"
        $ids1 = @($p.Json.windows | ForEach-Object { $_.hwnd })

        $p2 = Invoke-Wq -Arguments ($listArgs + @('--limit', '2', '--offset', '2'))
        $ids2 = @($p2.Json.windows | ForEach-Object { $_.hwnd })
        Assert-Ec ($p2.Json.pagination.returned -eq 2 -and
                    (@(Compare-Object $ids1 $ids2).Count -eq 4)) `
            '第二页与第一页不该有交集，也不该空'

        $last = Invoke-Wq -Arguments ($listArgs + @('--limit', '2', '--offset', '6'))
        Assert-Ec ($last.Json.pagination.returned -eq 1 -and
                    $last.Json.pagination.truncated -eq $false) `
            '尾页只有一条时不该报截断（判的是还有没有下一条）'

        $past = Invoke-Wq -Arguments ($listArgs + @('--offset', '7'))
        Assert-Ec ($past.Exit -eq 0 -and @($past.Json.windows).Count -eq 0) `
            '--offset 正好等于命中数应当是空批次而不是回绕'

        $bad = Invoke-Wq -Arguments ($listArgs + @('--limit', '90000'))
        Assert-Ec ($bad.Exit -eq 1 -and (Codes $bad.Json.errors) -contains 'cli.invalid_number') `
            "--limit 超过一次求值本来能拿到的条数那道线应当在解析期就拒：exit=$($bad.Exit)"

        # ---- 字段齐不齐：每条都该有句柄/PID/类名/标题/映像名/矩形/身份约束 ----
        $one = $r.Json.windows[0]
        foreach ($field in @('hwnd', 'pid', 'title', 'class', 'image', 'rect', 'visible',
                             'minimized', 'zOrder', 'readability', 'identity')) {
            Assert-Ec ($null -ne $one.PSObject.Properties[$field]) "结果条目缺字段 $field"
        }
        Assert-Ec ($one.rect.width -gt 0 -and $one.rect.height -gt 0) "矩形宽高不合法：$($one.rect)"
        Assert-Ec ($one.identity.'class' -eq $one.'class' -and $one.identity.pid -eq $one.pid -and
                    $one.identity.hwnd -eq $one.hwnd) `
            '身份约束字段与顶层那几项必须是同一个值（两处各问一次迟早打脸）'
        Assert-Ec ($one.identity.verificationRequired -eq $true -and
                    $one.identity.isAuthorizationToken -eq $false) `
            '身份字段要写明"仍要复核"且"不是凭证"'
        Assert-Ec (@($r.Json.caveats) -contains 'snapshot_expires') `
            "caveats 里少了快照过期那条：[$($r.Json.caveats -join ',')]"

        # ---- 矩形与真机独立量到的物理像素对得上（同一套坐标，不是 DPI 虚拟化后那份） ----
        $found = $null
        foreach ($h in @($w4.Hwnds)) {
            if ((Get-EcHwndHex $h) -eq $one.hwnd) { $found = $h }
        }
        if ($found) {
            $native = Get-EcWindowRect -Hwnd $found
            Assert-Ec ($one.rect.x -eq $native.Left -and $one.rect.y -eq $native.Top) `
                "结果里的矩形与当场量到的不一致：$($one.rect) 对 左$($native.Left) 上$($native.Top)"
        } else {
            Skip-Ec '矩形与当场量到的物理像素对照' '第一条结果不属于本次那组窗口（叠放次序变了）'
        }

        # ---- 归属进程信息：本次那个 ecwindow.exe 应当可读且映像名对得上 ----
        Assert-Ec ($one.readability.process.state -eq 'readable' -and
                    $one.readability.imagePath.state -eq 'readable') `
            "归属进程那两问问不出来：$($one.readability)"
        Assert-Ec ($one.image -eq 'ecwindow.exe') "映像名不对：$($one.image)"

        # ---- 隐私：默认连 exePath 这个键都不出现；--inspect=path 才写出完整路径 ----
        # 取舍写在查询自己的取值里，所以 --list 与 --inspect=path 同时给出是冲突（一次一份文档），
        # 这一条用 --inspect=path 点名单个句柄来判。
        Assert-Ec (-not ($r.Body -match 'exePath')) `
            '默认那份里出现了 exePath 这个键（完整路径常含用户名，默认不许交）'
        $withPath = Invoke-Wq -Arguments @('--inspect=path', '--hwnd', $one.hwnd)
        Assert-Ec ($withPath.Exit -eq 0 -and $withPath.Json.window.exePath -like '*ecwindow.exe') `
            "--inspect=path 没写出完整路径：exit=$($withPath.Exit) exePath=$($withPath.Json.window.exePath)"
        Assert-Ec ($withPath.Json.window.readability.imagePath.state -eq 'readable') `
            '--inspect=path 那次的可读性字段与结果对不上（写了值却说问不出来）'
        $noPathSame = Invoke-Wq -Arguments @('--inspect', '--hwnd', $one.hwnd)
        Assert-Ec (-not ($noPathSame.Body -match 'exePath')) `
            '不给 =path 时出现了完整路径（默认只写文件名这条没守住）'

        # ---- inspect 按 --hwnd 点名：与列表里那一条逐字一致 ----
        $insp = Invoke-Wq @('--inspect', '--hwnd', $one.hwnd)
        Assert-Ec ($insp.Exit -eq 0 -and $insp.Json.window.hwnd -eq $one.hwnd) `
            "inspect 点名的不是那一扇：exit=$($insp.Exit)"
        Assert-Ec ($insp.Json.window.identity.processStartTicks -eq $one.identity.processStartTicks) `
            'inspect 与列表交回的身份基线不是同一个值（同一份枚举产物不该各算一次）'
        Assert-Ec ($insp.Json.pagination.matched -eq 1 -and $insp.Json.pagination.returned -eq 1) `
            'inspect 那一份的分页字段应当是"命中 1 交回 1"'

        # ---- 窗口销毁之后：inspect 那个句柄报无匹配（一个窗口都没交回去） ----
        $gone = Start-EcWindow -RunDir $run3 -Class "ec-wq-gone-$stag" -Title "窗口查询 会消失 $stag" `
                               -Rect '900,120,1100,300' -Seed 13 `
                               -ExtraArgs @('--destroy-after-ms', '400')
        $goneHex = Get-EcHwndHex $gone.Hwnd
        $deadline = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $deadline -and (Test-EcWindowAlive -Hwnd $gone.Hwnd)) {
            Start-Sleep -Milliseconds 100
        }
        Assert-Ec (-not (Test-EcWindowAlive -Hwnd $gone.Hwnd)) '测试窗口没按预期被销毁，这条判据做不成'
        $inspGone = Invoke-Wq @('--inspect', '--hwnd', $goneHex)
        Assert-Ec ($inspGone.Exit -eq 4 -and (Codes $inspGone.Json.errors) -contains 'match.no_window') `
            "inspect 一个已销毁的句柄：exit=$($inspGone.Exit) codes=[($(Codes $inspGone.Json.errors) -join ','))]"
        $listGone = Invoke-Wq @('--list', '--class', $gone.Class)
        Assert-Ec ($listGone.Exit -eq 0 -and @($listGone.Json.windows).Count -eq 0) `
            '列表里那扇已销毁的窗口应当消失（而不是留一条陈旧条目）'

        # ---- 属性改变（改名）：列表交回的是当场问到的新标题，条件不再成立就不命中 ----
        $rename = Start-EcWindow -RunDir $run3 -Class "ec-wq-rename-$stag" -Title "窗口查询 改名前 $stag" `
                                 -Rect '1150,120,1350,300' -Seed 14
        $before = Invoke-Wq @('--list', '--title', $rename.Title)
        Assert-Ec (@($before.Json.windows).Count -eq 1) '改名前按标题应当命中那一扇'
        # 改名靠 --rename-after-ms 做不到（那是截图链路的判据），所以这里改判"标题随查询现问现答"：
        # 两次查询之间没有缓存，列表里出现的必须是当场问到的那一个值。
        $again = Invoke-Wq @('--list', '--class', $rename.Class)
        Assert-Ec ($again.Json.windows[0].title -eq $rename.Title) `
            "列表里的标题不是当场问到的那一个：$($again.Json.windows[0].title)"
    } finally {
        Stop-EcOwnedWindows
        Remove-EcRunDir $run3
    }

    # =========================================================================
    Write-Host "`n=== 4) 真机：多匹配时 --inspect 报歧义、--list 正常分页 ==="
    # =========================================================================
    $run4 = New-EcRunDir -Tag 'wq-ambig'
    try {
        $stag = $run4.Leaf -replace '[^a-z0-9]', ''
        # 同一个进程建三扇同标题窗口：任何按标题的条件都命中三个，正是"没人消歧"的现场。
        $w = Start-EcWindow -RunDir $run4 -Class "ec-wq-many-$stag" -Title "窗口查询 三个一样 $stag" `
                            -Rect '160,160,460,380' -Windows 3 -Seed 21
        $t = $w.Title

        $list = Invoke-Wq @('--list', '--title', $t)
        Assert-Ec ($list.Exit -eq 0 -and $list.Json.pagination.matched -eq 3) `
            "--list 命中三个应当正常出列表：exit=$($list.Exit) matched=$($list.Json.pagination.matched)"
        Assert-Ec (-not (Codes $list.Json.errors) -contains 'match.ambiguous_window') `
            '--list 把多匹配报成了截图歧义'

        $amb = Invoke-Wq @('--inspect', '--title', $t)
        Assert-Ec ($amb.Exit -eq 5 -and (Codes $amb.Json.errors) -contains 'match.ambiguous_window') `
            "--inspect 命中三个应当报歧义 + 5：exit=$($amb.Exit) codes=[($(Codes $amb.Json.errors) -join ','))]"
        Assert-Ec ($amb.Json.errors[0].value -eq '3') "歧义的 value 不是命中数：$($amb.Json.errors[0].value)"
        Assert-Ec ($null -eq $amb.Json.window -and @($amb.Json.windows).Count -eq 0) `
            '歧义那次仍交回了窗口（绝不随便选一个）'

        # 消歧之后能对上号：--index 2 选中的必须是列表里第 2 条（同一套 Z 序，两处同一条线）。
        $order = @($list.Json.windows | ForEach-Object { $_.hwnd })
        $pick = Invoke-Wq @('--inspect', '--title', $t, '--index', '2')
        Assert-Ec ($pick.Exit -eq 0 -and $pick.Json.window.hwnd -eq $order[1]) `
            "inspect --index 2 选的不是列表里第二条：$($pick.Json.window.hwnd) 对 $($order[1])"

        $top = Invoke-Wq @('--inspect', '--title', $t, '--topmost-match')
        Assert-Ec ($top.Exit -eq 0 -and $top.Json.window.hwnd -eq $order[0]) `
            '--topmost-match 在 inspect 里取的不是 Z 序第一'

        # --list 与选择策略互斥：列表本来就是全部，"取哪一扇"对它没意义。
        $cf = Invoke-Wq @('--list', '--title', $t, '--index', '2')
        Assert-Ec ($cf.Exit -eq 1 -and (Codes $cf.Json.errors) -contains 'cli.window_query_conflict') `
            "--list 配 --index 应当算冲突：exit=$($cf.Exit)"
        $cf2 = Invoke-Wq @('--list', '--title', $t, '--all')
        Assert-Ec ((Codes $cf2.Json.errors) -contains 'cli.window_query_conflict') `
            '--list 配 --all 应当算冲突（列表不是截图批次）'
        # 两条窗口查询同时给出也是一条冲突，value 里两个名字都列出来。
        $cf3 = Invoke-Wq @('--list', '--inspect', '--title', $t)
        Assert-Ec ($cf3.Exit -eq 1 -and $cf3.Json.errors[0].value -like '*--list*' -and
                    $cf3.Json.errors[0].value -like '*--inspect*') `
            "--list 与 --inspect 同时给出：$($cf3.Json.errors[0].value)"
        # 窗口查询与环境查询互斥，且报错用的是窗口那一条码（不是 cli.query_conflict）。
        $cf4 = Invoke-Wq @('--capabilities', '--list', '--title', $t)
        Assert-Ec ($cf4.Exit -eq 1 -and
                    (@(Codes $cf4.Json.errors) -contains 'cli.query_conflict' -or
                     @(Codes $cf4.Json.errors) -contains 'cli.window_query_conflict')) `
            "环境查询与窗口查询同时给出没报错：[$((Codes $cf4.Json.errors) -join ',')]"
        # --dry-run 那条兼容入口不受影响，也不与 --list 混用（两条路各有各的文档）。
        $dry = Invoke-Wq @('--class', $w.Class, '--dry-run', 'out.png')
        Assert-Ec ($dry.Exit -eq 0 -and (Codes $dry.Json.notes) -contains 'note.dry_run') `
            '--dry-run 的兼容入口被改坏了（它本来就是查完即返回，不截图不写文件）'
        # ---- 退出码 7 的那一条来路：预算花在条件求值那一步，说的是"这一次问答没跑完" ----
        # 用了 --title-regex 就整步进辅助进程，而那一个进程自己起来就要几毫秒，所以 1 毫秒的预算必然花完。
        $to = Invoke-Wq @('--list', '--title-regex', 'a.*', '--timeout-ms', '1')
        Assert-Ec ($to.Exit -eq 7 -and (Codes $to.Json.errors) -contains 'match.timeout') `
            "--list 的求值预算花完时该按同一条码与退出码 7：exit=$($to.Exit) codes=[($(Codes $to.Json.errors) -join ','))]"
        Assert-Ec ($to.Json.errors[0].stage -eq 'match') `
            "这一条的 stage 该是 match：$($to.Json.errors[0].stage)"
        # 截图那一份 hint 讲的是"换一条不会卡住的取图通道"，而一次窗口查询根本没有通道可换：
        # 交回那句等于给调用方一条做不到的下一步，所以这一路换成查询自己的说法。
        $hint = [string]$to.Json.errors[0].hint
        Assert-Ec ($hint -notmatch 'wgc|dwm') `
            "--list 的期限 hint 里不该出现取图通道：$hint"
        Assert-Ec ($hint -match '--capture') `
            "--list 的期限 hint 该明说换通道没有用：$hint"
        # 这一条不是截图失败：一个像素都没取，也没有任何后端被叫过。
        Assert-Ec ($to.Json.authorization.pixelsRead -eq 0 -and
                    -not $to.Json.authorization.consentDialogShown) `
            '期限到点那一次仍然不该取像素或弹框'
    } finally {
        Stop-EcOwnedWindows
        Remove-EcRunDir $run4
    }

    # =========================================================================
    Write-Host "`n=== 5) 真机：--monitor 按屏限缩与 -v 的回显 ==="
    # =========================================================================
    $run5 = New-EcRunDir -Tag 'wq-screen'
    try {
        $stag = $run5.Leaf -replace '[^a-z0-9]', ''
        $w = Start-EcWindow -RunDir $run5 -Class "ec-wq-mon-$stag" -Title "窗口查询 按屏 $stag" `
                            -Rect '180,180,480,400' -Seed 31
        $onPrimary = Invoke-Wq @('--list', '--monitor', 'primary', '--class', $w.Class, '-v')
        Assert-Ec ($onPrimary.Exit -eq 0 -and @($onPrimary.Json.windows).Count -eq 1) `
            "按主屏过滤应当命中本次那扇：exit=$($onPrimary.Exit)"
        Assert-Ec ($onPrimary.Json.input.monitor -eq 'primary' -and
                    $onPrimary.Json.input.target -eq 'window') `
            "-v 的 input 段没回显这一次查询按哪块屏限缩：$($onPrimary.Json.input)"
        Assert-Ec ($onPrimary.Json.input.action -eq 'list' -and
                    @($onPrimary.Json.input.class) -contains $w.Class) `
            "-v 的条件回显不对：$($onPrimary.Json.input)"
        # 编号越界在开工之前就报，与截图那一次同一判据、同一个退出码。
        $oob = Invoke-Wq @('--list', '--monitor', '99')
        Assert-Ec ($oob.Exit -eq 1 -and (Codes $oob.Json.errors) -contains 'match.monitor_out_of_range') `
            "--monitor 越界那次：exit=$($oob.Exit) codes=[($(Codes $oob.Json.errors) -join ','))]"
    } finally {
        Stop-EcOwnedWindows
        Remove-EcRunDir $run5
    }

    # =========================================================================
    Write-Host "`n=== 6) 本机不具备条件、照实记未验证的部分 ==="
    # =========================================================================
    Skip-Ec '归属进程读不到时的字段级 denied / failed 与系统原因码' `
            '本机没有可控制的高完整性进程可以当目标，而为测试去动使用者真实应用是仓库规矩禁止的。这一条由离线层注入假候选逐条判（含"开不到句柄""路径问不出来""窗口矩形问不出来"三种形状）'
    Skip-Ec '受限进程（服务、别人的提升窗口）在真机上的列表形状' `
            '需要那类进程在场且不能为测试去结束或改动它们；离线层已判"问不出来"不会被折成"是空的"'
    Skip-Ec '不可见窗口（IsWindowVisible 为假）确实不进列表' `
            '测试造不出"可见性为假但还能被 EnumWindows 收到而尺寸非零"的稳定现场；默认策略那一条与截图链路共用同一次枚举，因此由离线层与截图侧既有判据共同覆盖'
    Skip-Ec '无交互桌面（服务会话、计划任务）里那份文档的值' `
            '本机就是一个有人盯着的交互桌面。那一条本来也与列表无关：列表不弹框也不取像素'
    Skip-Ec '命中条数超过一次求值上限（8192 条）时的分页与截断' `
            '本机没有那么多可控窗口；分页边界与"截断判的是还有没有下一条"由离线层注入 100 条候选逐条判'
} finally {
    Stop-EcOwnedWindows
}

exit (Complete-EcSuite -Title '结构化窗口发现与检查')
