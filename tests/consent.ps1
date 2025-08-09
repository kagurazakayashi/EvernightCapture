<#
.SYNOPSIS
    截图授权与 --yes 的回归判据：离线状态机 + 真机侧"弹不弹框"。
.DESCRIPTION
    这个功能的分支很多（哪条路径要问、--yes 能免掉哪一部分、拒绝之后停在哪里、目标或屏幕
    拓扑一变授权就作废），全部靠真机点框跑不完，也绝不该为测试在发布版里留旁路。所以分两层：

      1) 离线层：build\ecapture-consent-tests.exe（源码 tests\consent_state.cpp）用注入的
         假应答器与假屏幕布局把 ConsentGate 整台状态机跑完 —— 不需要桌面，也不需要人。
      2) 真机层：目标一律是本测试自建的窗口（tests\helper\ec_window.cs），只收尾自己起的进程。
         凡是要弹框的用例，本脚本一律代答"否"（IDNO）：拒绝不会拍到任何东西，所以可以自动跑，
         同时把"必须弹框"与"拒绝之后的契约"一起判掉（第 8 节的超时判据是"什么都不答"，
         让框自己到点收尾 —— 那正是它判的东西）。绝代人点"是" —— 那等于替人同意把桌面上
         其它窗口拍进图里。要跑桌面路径的画面判据，请在无隐私的专用桌面上用
         tests\channels.ps1 -SimulateConsent 或 tests\window_shot.bat（由人自己点）。
         "没有可交互桌面"那种情况在本机（当前登录的交互会话）造不出来，如实记未验证，
         它的行为由离线层判据覆盖。

    判据不是"有没有报错"：弹没弹框用本次进程的 #32770 窗口核对，落地文件数、captured、
    诊断的 code / stage / target / value 与图片的 path / scope / rect 都要对得上。
    跑的时候屏幕上会短暂闪过确认框，请让它自己答"否"，人不要点。
.EXAMPLE
    .\tests\consent.ps1
    .\tests\consent.ps1 -SkipState      # 只跑真机层
    .\tests\consent.ps1 -Keep           # 保留临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-consent-tests.exe'
$IDNO = 7

$run = New-EcRunDir -Tag 'consent'
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

function Code-By($o, [string]$code) {
    return @($o.errors | Where-Object { $_.code -eq $code })[0]
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：授权状态机（假应答器 + 假屏幕布局） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '授权状态机离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-consent-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "状态机判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        $failed = if ($m.Success) { [int]$m.Groups[2].Value } else { -1 }
        $passed = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
        Assert-Ec ($failed -eq 0) "状态机有失败项，或摘要读不出来：$tail"
        Assert-Ec ($passed -ge 80) "状态机通过数不对劲（$passed），判据被删了？"
        Write-Host "  $tail" -ForegroundColor DarkGray
    }

    # 目标窗口：本测试自有、带签名色，所以"截到了"这件事可以用像素判据核对
    $class = "ec-consent-$tag"
    $title = "授权测试窗口 $class"
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '180,180,660,520' -Seed 11 -MaxLifeSeconds 900
    $hwnd = Get-EcHwndHex $window.Hwnd
    Write-Host ("目标窗口 PID={0} HWND={1}" -f $window.Pid, $hwnd)

    # 同一进程两扇同标题窗口（类名分别是 <class>、<class>-2，标题相同）：
    # "多匹配"、"多目标写 stdout"与"%n 撞名"这三条判据都靠它。
    $pairClass = "ec-consent2-$tag"
    $pairTitle = "授权同名对 $tag"
    $pair = Start-EcWindow -RunDir $run -Class $pairClass -Title $pairTitle -Rect '700,180,1000,420' -Windows 2 -Seed 12 -MaxLifeSeconds 900
    $pair0 = Get-EcHwndHex $pair.Hwnds[0]
    $pair1 = Get-EcHwndHex $pair.Hwnds[1]

    # =========================================================================
    Write-Host "`n=== 1) 可靠窗口路径 + --yes：不弹框，来路要写清是窗口内容 ==="
    # =========================================================================
    $p1 = Get-EcRunFile -RunDir $run -Name 'yes_window.png'
    $a1 = @('--hwnd', $hwnd, '--capture', 'wgc', '--yes', '--out', $p1)
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $a1 -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) '带 --yes 的窗口内容截图弹了确认框（这一级本来能免掉）'
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o -and $o.captured -eq 1) "没截成（exit=$($r.Exit)）$($r.Stderr)"
    $img = @($o.images)[0]
    Assert-Ec ($img.source -eq 'wgc' -and $img.path -eq 'wgc') "来路不对：$($img.source)/$($img.path)"
    Assert-Ec ($img.scope -eq 'window') "scope 该是 window：$($img.scope)"
    Assert-Ec ($img.rect.width -gt 100 -and $img.rect.height -gt 100) "rect 不对劲：$($img.rect | ConvertTo-Json -Compress)"
    $stats = Get-EcImageStats -Path $p1 -Step 7
    $d = Get-EcColorDistance -A $stats.TopDominant -B (New-EcSignatureKey -Seed 11)
    Assert-Ec ($stats.Colors -ge 12 -and $d -le 32) "画面不是本次窗口内容（颜色 $($stats.Colors)，签名距离 $d）"

    $p1b = Get-EcRunFile -RunDir $run -Name 'yes_printwindow.png'
    $a1b = @('--hwnd', $hwnd, '--capture', 'printwindow', '--yes', '--out', $p1b)
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $a1b -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) 'printwindow 带 --yes 仍弹了框'
    $imgB = @((Json-Of $r).images)[0]
    Assert-Ec ($r.Exit -eq 0 -and $imgB.scope -eq 'window') "printwindow 的来路没报成窗口内容（exit=$($r.Exit)）"
    Assert-Ec ($imgB.path -eq 'printwindow') "printwindow 的 path 不对：$($imgB.path)"

    # =========================================================================
    Write-Host "`n=== 2) 同样的窗口不带 --yes：一定要问，答`"否`"就什么都不做 ==="
    # =========================================================================
    $p2 = Get-EcRunFile -RunDir $run -Name 'ask_window.png'
    Remove-Item -LiteralPath $p2 -ErrorAction SilentlyContinue
    $a2 = @('--hwnd', $hwnd, '--capture', 'wgc', '--lang', 'zh-CN', '--out', $p2)
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $a2 -Answer $IDNO
    Assert-Ec $r.Dialog '窗口内容路径没带 --yes 时没弹确认框（真实截图默认要问一次）'
    Assert-Ec $r.Clicked '确认框上没点到"否"（按钮找不到、框已关，或有人先点了一步）'
    Assert-Ec ($r.Exit -eq 6) "答`"否`"的退出码 $($r.Exit)，应为 6"
    $o = Json-Of $r
    Assert-Ec ($o -and (Codes $o.errors) -contains 'capture.access_denied') "答`"否`"的诊断码不对：[$((Codes $o.errors) -join ',')]"
    $denied = Code-By $o 'capture.access_denied'
    Assert-Ec ($denied.stage -eq 'consent') "stage 该是 consent：$($denied.stage)"
    Assert-Ec ($denied.value -eq 'wgc') "value 该是实际路径 wgc：$($denied.value)"
    Assert-Ec ($denied.backend -eq 'wgc') "backend 该是通道名：$($denied.backend)"
    Assert-Ec ($denied.target -eq $hwnd) "target 该是本次窗口：$($denied.target)"
    Assert-Ec ($o.captured -eq 0 -and @($o.images).Count -eq 0) '答"否"却报了图'
    Assert-Ec (-not (Test-Path -LiteralPath $p2)) '答"否"却写出了文件'

    $p2q = Get-EcRunFile -RunDir $run -Name 'ask_quiet.png'
    $a2q = @('--hwnd', $hwnd, '--quiet', '--out', $p2q)
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $a2q -Answer $IDNO
    Assert-Ec $r.Dialog '--quiet 跳过了确认框'
    $o = Json-Of $r
    Assert-Ec ($o -and (Codes $o.errors) -contains 'capture.access_denied') '--quiet 把拒绝也一起藏了'
    Assert-Ec ($null -eq $o.notes) '--quiet 之下不该再有 notes（errors 必须留着）'

    # =========================================================================
    Write-Host "`n=== 3) 没给输出路径时被拒绝：报的是人答了否 ==="
    # =========================================================================
    # 这条很重要：AI 调用方读到"缺少输出路径"会去补一个 --out 再截一次，而真正的原因是没人同意。
    # 省略 --out 与显式 --out - 走的是同一条路，所以这里不需要任何特例。
    $a3 = @('--hwnd', $hwnd, '-v')
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $a3 -Answer $IDNO
    Assert-Ec $r.Dialog '隐式 stdout 的窗口截图没弹确认框'
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 6) "退出码 $($r.Exit)，应为 6（拒绝）而不是被折叠成 1"
    Assert-Ec ($o -and (Codes $o.errors) -contains 'capture.access_denied') "拒绝的诊断被折叠了：[$((Codes $o.errors) -join ',')]"
    Assert-Ec (-not ((Codes $o.errors) -contains 'cli.missing_output')) '拒绝的同时又报缺少输出路径'
    Assert-Ec ($o.input.yes -eq $false) "-v 的 input.yes 该回显 false：$($o.input.yes)"

    # =========================================================================
    Write-Host "`n=== 4) 会拍到桌面的路径：带 --yes 也一定要问，答`"否`"零落地 ==="
    # =========================================================================
    $argvWin1 = @('--capture', 'bitblt')
    $argvWin2 = @('--capture', 'duplication')
    $argvScr0 = @('--monitor', '1')
    $argvScr1 = @('--monitor', '1', '--capture', 'bitblt')
    $argvScr2 = @('--monitor', '1', '--capture', 'duplication')
    $desktopCases = @()
    # ExpectPath 是诊断 value 里该出现的实际路径名（桌面那几条各有各的名字，不许含糊）
    $c1 = @{ Name = 'bitblt 截窗口（拷屏幕像素）'; Arg = $argvWin1; Screen = $false; ExpectPath = 'bitblt.screen' }
    $c2 = @{ Name = 'duplication 截窗口（桌面帧裁剪）'; Arg = $argvWin2; Screen = $false; ExpectPath = 'duplication.frame' }
    $c3 = @{ Name = '整屏 wgc'; Arg = $argvScr0; Screen = $true; ExpectPath = 'screen.wgc' }
    $c4 = @{ Name = '整屏 bitblt'; Arg = $argvScr1; Screen = $true; ExpectPath = 'screen.bitblt' }
    $c5 = @{ Name = '整屏 duplication'; Arg = $argvScr2; Screen = $true; ExpectPath = 'screen.duplication' }
    $desktopCases = @($c1, $c2, $c3, $c4, $c5)
    $n = 0
    foreach ($c in $desktopCases) {
        $n++
        $out = Get-EcRunFile -RunDir $run -Name ("desktop_$n.png")
        if ($c.Screen) {
            $argv = @('--yes') + $c.Arg + @('--out', $out)
        } else {
            $argv = @('--hwnd', $hwnd, '--yes') + $c.Arg + @('--out', $out)
        }
        $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -Answer $IDNO
        Assert-Ec $r.Dialog "$($c.Name)：带 --yes 就没弹确认框（--yes 越权批掉了桌面路径）"
        Assert-Ec $r.Clicked "$($c.Name)：这一次测试没点到`"否`"（有人先答了一步？这条判据要看的就是拒绝之后不落地）"
        $o = Json-Of $r
        Assert-Ec ($r.Exit -eq 6) "$($c.Name)：退出码 $($r.Exit)，未取得同意应为 6"
        Assert-Ec ($o -and (Codes $o.errors) -contains 'capture.access_denied') "$($c.Name)：没报 capture.access_denied：[$((Codes $o.errors) -join ',')]"
        $d2 = Code-By $o 'capture.access_denied'
        Assert-Ec ($d2.stage -eq 'consent') "$($c.Name)：stage 该是 consent：$($d2.stage)"
        Assert-Ec ($d2.value -like $c.ExpectPath) "$($c.Name)：value 该是实际路径，写的是 $($d2.value)"
        Assert-Ec (-not (Test-Path -LiteralPath $out)) "$($c.Name)：框被答`"否`"却写出了文件"
        Write-Host ("  {0,-32} 弹框=是，答否 => 没有落地" -f $c.Name) -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 5) 只读与预检失败一律不弹框（--yes 不是`"没条件就截桌面`"） ==="
    # =========================================================================
    $argvNoCond = @('--yes')
    $argvNoMatch = @('--class', "ec-none-$tag", '--yes', '--out', 'x.png')
    $argvBadValue = @('--class', $class, '--capture', 'magnification', '--yes', '--out', 'x.png')
    $argvOutOfRange = @('--monitor', '99', '--yes', '--out', 'x.png')
    $argvDryRun = @('--monitor', '1', '--dry-run', '--yes', '--out', 'x.png')
    $argvStdoutMulti = @('--title', $pairTitle, '--all', '--yes', '--out', '-')
    $quietCases = @()
    $quietCases += @{ Name = '不给任何条件（只有 --yes）'; A = $argvNoCond; Exit = 2 }
    $quietCases += @{ Name = '无匹配窗口'; A = $argvNoMatch; Exit = 4 }
    $quietCases += @{ Name = '取值非法'; A = $argvBadValue; Exit = 1 }
    $quietCases += @{ Name = '屏幕编号越界'; A = $argvOutOfRange; Exit = 1 }
    $quietCases += @{ Name = '--dry-run 带 --yes 与 --monitor'; A = $argvDryRun; Exit = 0 }
    $quietCases += @{ Name = '多目标写 stdout'; A = $argvStdoutMulti; Exit = 1 }
    foreach ($c in $quietCases) {
        $r = Invoke-EcConsentShot -Exe $Exe -Arguments $c.A -ExpectNoDialog
        Assert-Ec (-not $r.Dialog) "$($c.Name) 弹了确认框（不取帧的调用打扰了人）"
        Assert-Ec ($r.Exit -eq $c.Exit) "$($c.Name) 退出码 $($r.Exit)，应为 $($c.Exit)"
        Write-Host ("  {0,-38} 不弹框，exit={1}" -f $c.Name, $r.Exit) -ForegroundColor DarkGray
    }

    # 整批输出名撞车是同一类：%n 用同标题的两个目标算出同一个名字 -> 问人之前就报 io.output_collision
    $collideOut = Get-EcRunFile -RunDir $run -Name 'collide_%n.png'
    $argvCollide = @('--hwnd', $pair0, '--hwnd', $pair1, '--all', '--yes', '--out', $collideOut)
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argvCollide -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) '整批输出名撞车时弹了确认框（注定存不下来的一批不该先问人）'
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 8 -and (Codes $o.errors) -contains 'io.output_collision') "撞名没报 io.output_collision（exit=$($r.Exit)）：[$((Codes $o.errors) -join ',')]"
    Assert-Ec (@(Get-ChildItem -LiteralPath $run.Path -Filter 'collide_*').Count -eq 0) '撞名的一批写出了文件'

    # 多匹配没消歧：报 match.ambiguous_window，同样不许先弹框
    $argvAmbiguous = @('--class', $pairClass, '--class', "$pairClass-2", '--yes', '--out', (Get-EcRunFile -RunDir $run -Name 'amb.png'))
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argvAmbiguous -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) '多匹配消歧失败时弹了确认框'
    Assert-Ec ($r.Exit -eq 5) "多匹配退出码 $($r.Exit)，应为 5"

    # =========================================================================
    Write-Host "`n=== 6) 一次确认覆盖一批目标；答`"否`"整批零新增、旧结果保留 ==="
    # =========================================================================
    $dir6 = Join-Path $run.Path 'batch'
    New-Item -ItemType Directory -Force -Path $dir6 | Out-Null
    $argvBatch = @('--hwnd', $hwnd, '--hwnd', $pair0, '--all', '--yes', '--out', (Join-Path $dir6 '%n.png'))
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argvBatch -ExpectNoDialog
    Assert-Ec (-not $r.Dialog) '带 --yes 的两个目标批次弹了框（窗口内容路径应一次都不问）'
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 2) "批次截图失败（exit=$($r.Exit)）$($r.Stderr)"
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir6 -File).Count -eq 2) '两个目标该各写一张图'
    foreach ($im in @($o.images)) {
        Assert-Ec ($im.scope -eq 'window' -and $im.path -eq 'wgc') "批次里有一张来路不是窗口内容：$($im.path)/$($im.scope)"
    }

    # 同一批不带 --yes：只问一次；答"否" => 一张都不截，而上一批写好的图保留
    $dir7 = Join-Path $run.Path 'batch_refused'
    New-Item -ItemType Directory -Force -Path $dir7 | Out-Null
    $argvRefuse = @('--hwnd', $hwnd, '--hwnd', $pair0, '--all', '--out', (Join-Path $dir7 '%n.png'))
    $before = @(Get-ChildItem -LiteralPath $dir6 -File).Count
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argvRefuse -Answer $IDNO
    Assert-Ec $r.Dialog '两个目标一批也要问一次：这里根本没弹框'
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 6 -and $o.captured -eq 0) "答`"否`"之后仍有产出（exit=$($r.Exit) captured=$($o.captured)）"
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir7 -File -ErrorAction SilentlyContinue).Count -eq 0) '答"否"的那一批写出了文件'
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir6 -File).Count -eq $before) '这一批的拒绝动了上一批的文件'

    # =========================================================================
    Write-Host "`n=== 7) 本机造不出来的部分（如实记未验证） ==="
    # =========================================================================
    $skipNoDesktop = '没有可交互桌面时：窗口路径带 --yes 仍可截图、桌面路径只能拒绝'
    $whyNoDesktop = '本机是当前登录的交互会话，造不出服务会话；两种码由离线层判据覆盖'
    $skipYesClick = '代点"是"之后的桌面路径画面判据（含 DWM 内部屏幕退路的升级）'
    $whyYesClick = '代人同意会拍到桌面上其它窗口；请在无隐私专用桌面上跑 channels.ps1 -SimulateConsent 与 window_shot.bat'
    $skipAutoEsc = 'auto 链在真机上升级到 bitblt 时的第二次确认'
    $whyAutoEsc = '需要 wgc / dwm / printwindow 同时失败的窗口，本机造不出来；升级由离线层判据覆盖'
    Skip-Ec $skipNoDesktop $whyNoDesktop
    Skip-Ec $skipYesClick $whyYesClick
    Skip-Ec $skipAutoEsc $whyAutoEsc

    # =========================================================================
    Write-Host "`n=== 8) 关框收尾与 auto 链的拒绝传播（真机框） ==="
    # =========================================================================
    # (a) auto 链到点没人答：框必须自己关得掉（本机实测头一发 WM_CLOSE 会被无视，
    #     收尾靠的是升级链），且码保持 capture.consent_timeout —— 不许换后端重跑，
    #     更不许最后统一吞成 capture.failed（那会把"没人同意"说成"机器不行"）。
    $p8a = Get-EcRunFile -RunDir $run -Name 'auto_consent_timeout.png'
    Remove-Item -LiteralPath $p8a -ErrorAction SilentlyContinue
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments @(
        '--hwnd', $hwnd, '--out', $p8a, '--consent-timeout-ms', '900')
    Assert-Ec $r.Dialog 'auto 路径设了 --consent-timeout-ms 却压根没弹框'
    $o = Json-Of $r
    $codes8a = (Codes $o.errors) -join ','
    if ($r.Exit -eq 0 -or $codes8a -eq 'capture.access_denied') {
        Skip-Ec 'auto 链确认超时：码保持 + 不换后端' `
            "这一次有人在 900 ms 内碰了确认框（exit=$($r.Exit)，码=[$codes8a]）：前提`"没人回答`"不成立，测试侧不代答`"是`""
    } else {
        Assert-Ec ($r.Exit -eq 6) "auto 链确认超时的退出码 $($r.Exit)，应为 6（零交付按拒绝）"
        Assert-Ec ($codes8a -eq 'capture.consent_timeout') `
            "auto 链确认超时只该报 capture.consent_timeout：[$codes8a]（绝不吞成 capture.failed）"
        Assert-Ec ($null -eq $o.notes) '确认超时不该有 capture_channel 提示：后端一条都没试过'
        Assert-Ec ($o.captured -eq 0 -and -not (Test-Path -LiteralPath $p8a)) '确认超时的这一次仍有产出'
    }
    # (b) auto 路径答"否"：拒绝同为终局，整份 errors 就一条 access_denied
    $p8b = Get-EcRunFile -RunDir $run -Name 'auto_consent_denied.png'
    Remove-Item -LiteralPath $p8b -ErrorAction SilentlyContinue
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments @('--hwnd', $hwnd, '--out', $p8b) -Answer $IDNO
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 6 -and ((Codes $o.errors) -join ',') -eq 'capture.access_denied') `
        "auto 答`"否`"应退出 6 且只报 capture.access_denied（exit=$($r.Exit)）：[$((Codes $o.errors) -join ',')]"
    Assert-Ec (-not (Test-Path -LiteralPath $p8b)) 'auto 路径被取消却写出了文件'
    # (c) 一批两个目标、问一次答"否"：零交付按拒绝退出 6；判定器拒后整批停住，
    #     所以 errors 恰好一条（第二个目标根本没被处理），文件一个不落。
    $dir8 = Join-Path $run.Path 'refused_batch'
    New-Item -ItemType Directory -Force -Path $dir8 | Out-Null
    $argvRefuse2 = @('--hwnd', $hwnd, '--hwnd', $pair0, '--all', '--out', (Join-Path $dir8 'batch_%n.png'))
    $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argvRefuse2 -Answer $IDNO
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 6 -and $o.captured -eq 0) `
        "整批被拒（零交付）该退出 6：exit=$($r.Exit) captured=$($o.captured)"
    $codes8c = @(Codes $o.errors)
    Assert-Ec ($codes8c.Count -eq 1 -and $codes8c[0] -eq 'capture.access_denied') `
        "拒绝后整批应当就此收住（恰好一条 access_denied）：[$(($codes8c -join ','))]"
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir8 -Filter 'batch_*' -ErrorAction SilentlyContinue).Count -eq 0) `
        '被拒的一批写出了文件'
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '截图授权与 --yes')
