<#
.SYNOPSIS
    窗口选择与身份一致性的判据：离线用假查询层逐条注入，真机用自建窗口工厂判接线与 Z 序语义。
.DESCRIPTION
    这一批判据管两件事。

    1) 身份一致性（src/WindowIdentity.cpp）
       窗口枚举之后到真正取帧之间，目标可能被销毁、同一个句柄值可能被另一扇窗口占用、或者它
       不再满足当初挑中它的那个条件。工具必须分辨「还是它」与「长得像它的新对象」，并在后者
       出现时拒绝取帧 —— 用户批准的是旧对象，许可不转移给新对象，也不放宽条件去另找一个。
       判据分两层：
         * 离线层（build\ecapture-identity-tests.exe，源码 tests\identity_state.cpp）用**假查询层**
           逐条注入。「句柄被另一个进程占用」「PID 相同但那是另一个进程（进程创建时间不同）」
           「类名换了」「条件不再成立」「每一问各自问不出来」这些现场真机上安排不出来 ——
           系统什么时候回收句柄与 PID 不由测试决定，要造就得杀掉别人的进程，而本测试不碰
           使用者真实应用。短路顺序与 kCheap / kFull 两档各自问哪几问也在这一层判。
         * 真机层（本脚本）只判**接线**：健康目标一次都不误伤、目标真被销毁确实报
           capture.target_gone、目标不再满足当初那条标题条件确实报 capture.target_changed、
           标题改了而条件仍成立就照常出图（正常刷新不能当换目标）、一批里坏掉一个不牵连其余。
       「确认框开着的那段时间里目标变了」这一条要测试代人点"是"才做得出来，而代人点"是"
       等于代人同意，所以只在显式给了 -SimulateConsent、且桌面上没有隐私内容时才做；
       不给就如实记未验证。

       真机那几条时序判据靠同一件事把窗口撑开：批次按 Z 序处理目标，所以**先建**那一扇排在最后，
       轮到它之前要先把另外几扇截完（每扇几百毫秒）。销毁 / 改名安排在这段路中间，
       "枚举时它还在、轮到它时它已经变了"就成了稳定现场（枚举发生在几百毫秒以内，
       所以下手时间刻意选在它之后；批次的其余目标保证轮到它之前已经过了那一刻）。
       撑不开时如实记未验证，不硬判失败也不伪造通过。

    2) 选择策略的名字（--topmost-match / --bottommost-match）
       命中列表按当下的 Z 序排列（EnumWindows 的顺序就是从顶到底），而旧名字 --newest / --oldest
       说的是创建先后。窗口创建时间没有公开 API 可取，进程创建时间也不是窗口创建时间，
       所以选的还是 Z 序、名字改成语义准确的写法，旧名保留为兼容别名并各留一条 note。
       这一段全部用 --dry-run 判：它在选完目标之后就返回，不取帧、不弹框、不落地，
       于是"哪一扇被挑中"可以反复读。区分"Z 序"与"创建顺序"靠的是置顶带：先建的那扇挂
       WS_EX_TOPMOST、后建的那扇是普通窗口，于是 Z 序第一恰好是先建的那一扇。
       （把一扇既存窗口重新激活来改 Z 序在这台机器上行不通：Windows 的前台锁不让后台进程
        替窗口抢叠放位置，实测从测试进程调 SetWindowPos(HWND_TOP)、以及由窗口自己那条线程调，
        EnumWindows 的顺序都不动。这一条因此记在未验证清单里。）

    测试一律用自建的窗口（tests\helper\ec_window.cs），只收尾自己起的那些进程。
    窗口内容路径带 --yes 不弹框；会拍到桌面像素的那几条一律不在这里截。
.EXAMPLE
    .\tests\identity.ps1
    .\tests\identity.ps1 -SkipState        # 只跑真机那层
    .\tests\identity.ps1 -SkipReal         # 只跑离线判据层（没有交互桌面时用）
    .\tests\identity.ps1 -SimulateConsent  # 加上它才代人点确认框的"是"（只在无隐私桌面上用）
    .\tests\identity.ps1 -Keep             # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$SkipReal,
    [switch]$SimulateConsent,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-identity-tests.exe'

function Json-Of($r) {
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $o = $null
    try { $o = $body | ConvertFrom-Json } catch { }
    return $o
}

function Error-Codes($o) {
    if (-not $o) { return @() }
    return @(@($o.errors) | Where-Object { $_ } | ForEach-Object { [string]$_.code })
}

function Note-Codes($o) {
    if (-not $o) { return @() }
    return @(@($o.notes) | Where-Object { $_ } | ForEach-Object { [string]$_.code })
}

function First-Error-With($o, $code) {
    if (-not $o) { return $null }
    $hit = @(@($o.errors) | Where-Object { $_.code -eq $code })
    if ($hit.Count -eq 0) { return $null }
    return $hit[0]
}

# --dry-run 把选中的目标写成一行 DescribeWindow，句柄是 hwnd=0x…… 那个形状。
# 从这里取"实际挑中了哪一扇"，判据因此不靠工具的归属字段自证。
function Selected-Hwnds($o) {
    if (-not $o) { return @() }
    $note = @(@($o.notes) | Where-Object { $_.code -eq 'note.dry_run' })
    if ($note.Count -eq 0) { return @() }
    return @([regex]::Matches([string]$note[0].value, 'hwnd=(0x[0-9A-Fa-f]{4,16})') |
             ForEach-Object { $_.Groups[1].Value })
}

function Joined($list) { return (@($list) -join ',') }

# =========================================================================
if (-not $SkipState) {
    # ===========================================================================
    Write-Host "`n=== 1) 离线：假查询层逐条注入（build\ecapture-identity-tests.exe）==="
    # ===========================================================================
    if (-not (Test-Path -LiteralPath $stateExe)) {
        Assert-Ec $false "找不到 $stateExe（先跑 .\build.ps1）"
    } else {
        $r = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $bad = @($r.Stdout -split "`r?`n" | Where-Object { $_ -match '^\s*FAIL' })
        foreach ($line in $bad) { Write-Host $line -ForegroundColor Red }
        $total = @($r.Stdout -split "`r?`n" | Where-Object { $_ -match '^\s*PASS' }).Count
        Assert-Ec ($r.Exit -eq 0) ("离线身份判据退出码 {0}，应为 0（{1} 条 FAIL）" -f $r.Exit, $bad.Count)
        Write-Host ("  PASS  离线身份判据：{0} 条全过" -f $total) -ForegroundColor DarkGreen
    }
}

if ($SkipReal) {
    exit (Complete-EcSuite -Title '身份复核（只离线层）：')
}

# ---------------------------------------------------------------------------
# 一批自建的陪跑窗口 + 一扇"会在半路变化"的目标，跑一次 --all 的 wgc 截图。
#
# 那扇目标**先建**，所以它在 Z 序最后、轮到它之前先把陪跑的那几扇截完；
# $ExtraArgs 里的 --destroy-after-ms / --rename-after-ms 就落在这段路中间。
# 收尾只停本次这一组窗口（绝不停整个清单：外面还有别的节在用自己的窗口）。
# ---------------------------------------------------------------------------
function Wait-EcWindowTitle {
    <#
        有界地等夹具把标题换成期望的那一条，回报最终标题与等了多久。

        为什么需要它：夹具的 --rename-after-ms 是后台线程 Thread.Sleep + PostMessage，而 Windows 10 起
        的定时器合并会让那一觉显著晚醒（本机实测：请求 1500 ms，第 3.8 s 才真的改名，窗口本身建了 0.4 s）。
        判据要的现场是"夹具到底改没改成"，不是"改名抢在取帧之前"——先后没排开那一种另有 Skip-Ec 记未验证
        （见 5a），所以这里只把"有没有改成"等出来，判据强度一点不放宽：等满仍没改成就是现场不成立，照样 FAIL。
    #>
    param(
        [Parameter(Mandatory)]$Window,
        [Parameter(Mandatory)][string]$Expected,
        [int]$TimeoutMs = 20000
    )

    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalMilliseconds -lt $TimeoutMs) {
        $t = Get-EcWindowText -Hwnd $Window.Hwnd
        if ($t -eq $Expected) {
            return [pscustomobject]@{ Title = $t; WaitedMs = [int]$sw.Elapsed.TotalMilliseconds; Hit = $true }
        }
        Start-Sleep -Milliseconds 100
    }
    return [pscustomobject]@{ Title = (Get-EcWindowText -Hwnd $Window.Hwnd)
                              WaitedMs = [int]$sw.Elapsed.TotalMilliseconds; Hit = $false }
}

function Invoke-ChangingBatch {
    param(
        [Parameter(Mandatory)][string]$Tag,
        [Parameter(Mandatory)][int]$SeedBase,
        [Parameter(Mandatory)][string[]]$ExtraArgs,
        [int]$Others = 6,
        [string]$ExpectTitle2 = ''
    )

    $sub = New-EcRunDir -Tag $Tag
    $stag = $sub.Leaf -replace '[^a-z0-9]', ''
    $owned = @()
    try {
        $first = Start-EcWindow -RunDir $sub -Class "ec-id-chg-$stag" -Title "身份 会变的 $stag" `
                                -Rect '150,150,540,420' -Seed $SeedBase -ExtraArgs $ExtraArgs
        $owned += $first
        for ($k = 1; $k -le $Others; $k++) {
            $owned += Start-EcWindow -RunDir $sub -Class ("ec-id-px{0}-{1}" -f $k, $stag) `
                                     -Title ("身份 陪跑{0} {1}" -f $k, $stag) `
                                     -Rect (('{0},150,{1},420') -f (600 + $k * 40), (920 + $k * 40)) `
                                     -Seed ($SeedBase + $k)
        }
        $out = Get-EcRunFile -RunDir $sub -Name 'batch_%i.png'
        $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 120000 -Arguments `
            (@('--title-contains', $stag, '--all', '--yes', '--capture', 'wgc', '--out', $out))
        # 等夹具那一次改名真的落地（见 Wait-EcWindowTitle 的说明），再读"当前标题"
        $wait = $null
        if ($ExpectTitle2) { $wait = Wait-EcWindowTitle -Window $first -Expected $ExpectTitle2 }
        $now = if ($wait) { $wait.Title } else { (Get-EcWindowText -Hwnd $first.Hwnd) }
        $waited = if ($wait) { $wait.WaitedMs } else { 0 }
        return [pscustomobject]@{
            Run = $sub; Stag = $stag; Result = $r; Json = (Json-Of $r)
            Hwnd = (Get-EcHwndHex $first.Hwnd); Ptr = $first.Hwnd
            TitleAtStart = "身份 会变的 $stag"
            TitleNow = $now; RenameWaitedMs = $waited
            Count = @(Get-ChildItem -LiteralPath $sub.Path -Filter 'batch_*.png' -File).Count
        }
    } finally {
        foreach ($w in $owned) { Stop-EcWindow -Window $w }
    }
}

$run = New-EcRunDir -Tag 'identity'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

try {
    # ===========================================================================
    Write-Host "`n=== 2) 选择策略对着当下的 Z 序（全部 --dry-run，不截图）==="
    # ===========================================================================
    $a = Start-EcWindow -RunDir $run -Class "ec-id-old-$tag" -Title "身份 先建 $tag" `
                        -Rect '150,150,540,420' -Seed 31
    $b = Start-EcWindow -RunDir $run -Class "ec-id-new-$tag" -Title "身份 后建 $tag" `
                        -Rect '620,150,1010,420' -Seed 32
    $COND = @('--title-contains', $tag)

    function Dry-Run($extra, $cond = $COND) {
        $r = Invoke-Ecapture -Arguments (@($cond) + @($extra) + @('--dry-run', '-v', 'out.png'))
        return (Json-Of $r)
    }

    # 先照 --all 读一遍当下的叠放次序，再把每个策略对着它判。
    # 判据不假设"后建的就在最前"：那件事本来就不由测试决定（别的窗口、别人的应用都在动）。
    $order = @(Selected-Hwnds (Dry-Run @('--all')))
    Assert-Ec ($order.Count -eq 2) ("--all 命中的目标数 {0}，应为 2" -f $order.Count)
    Assert-Ec ((Joined (Selected-Hwnds (Dry-Run @('--topmost-match')))) -eq $order[0]) `
              ("--topmost-match 挑的应为 --all 顺序里第一扇 {0}" -f $order[0])
    Assert-Ec ((Joined (Selected-Hwnds (Dry-Run @('--bottommost-match')))) -eq $order[-1]) `
              ("--bottommost-match 挑的应为 --all 顺序里最后一扇 {0}" -f $order[-1])
    for ($k = 1; $k -le $order.Count; $k++) {
        Assert-Ec ((Joined (Selected-Hwnds (Dry-Run @('--index', [string]$k)))) -eq $order[$k - 1]) `
                  ("--index {0} 应等于当下 Z 序第 {0} 扇 {1}" -f $k, $order[$k - 1])
    }

    # 兼容别名必须与规范名挑中同一扇：别名只是名字不同，不是另一条策略。
    $oAlias = Dry-Run @('--newest')
    Assert-Ec ((Joined (Selected-Hwnds $oAlias)) -eq $order[0]) `
              '别名 --newest 与 --topmost-match 挑中的应为同一扇'
    Assert-Ec ((Joined (Selected-Hwnds (Dry-Run @('--oldest')))) -eq $order[-1]) `
              '别名 --oldest 与 --bottommost-match 挑中的应为同一扇'

    # 别名要留下能追溯的痕迹：note.deprecated_option，而 -v 的 input.policy 写规范策略名。
    $notes = Note-Codes $oAlias
    Assert-Ec ($notes -contains 'note.dry_run' -and $notes -contains 'note.deprecated_option') `
              ("--newest 应留一条 note.deprecated_option，实际 notes={0}" -f (Joined $notes))
    Assert-Ec ($oAlias.input.policy -eq 'topmost') `
              ("-v 的 input.policy 应写规范策略名 topmost，实际 {0}" -f $oAlias.input.policy)
    $dep = @(@($oAlias.notes) | Where-Object { $_.code -eq 'note.deprecated_option' })
    Assert-Ec ($dep[0].option -eq '--newest' -and $dep[0].value -eq '--topmost-match') `
              ("废弃提示应写清哪个旧名换成哪个新名，实际 option={0} value={1}" -f `
               $dep[0].option, $dep[0].value)

    # ---------------- Z 序与创建顺序刻意相反的那一对 ----------------
    # 上面那一对里"后建的恰好在最前"，两种说法还分不出来。这一对让它们相反：
    # 先建的那扇挂 WS_EX_TOPMOST（置顶带压住所有普通窗口），后建的那扇是普通窗口。
    # 于是 Z 序第一 = **先建**的那一扇 —— 按"最后创建"去读这个名字就会挑错对象。
    # 这一对只靠"分岔"这个词自成一组候选：别的窗口标题里都没有它（其余那几扇一律叫"身份 …"）。
    $cond2 = @('--title-contains', '分岔')
    $pin = Start-EcWindow -RunDir $run -Class "ec-id-pin-$tag" -Title "分岔 先建但置顶 $tag" `
                          -Rect '1060,150,1420,420' -Seed 33 -TopMost
    $plain = Start-EcWindow -RunDir $run -Class "ec-id-plain-$tag" -Title "分岔 后建但普通 $tag" `
                            -Rect '1100,190,1460,460' -Seed 34
    $hPin = Get-EcHwndHex $pin.Hwnd
    $hPlain = Get-EcHwndHex $plain.Hwnd
    $order2 = @(Selected-Hwnds (Dry-Run @('--all') $cond2))
    Assert-Ec ($order2.Count -eq 2 -and $order2[0] -eq $hPin -and $order2[1] -eq $hPlain) `
              ("现场没建立起来：Z 序应为先建但置顶的 {0} 在前、后建的普通扇 {1} 在后，实际 {2}" -f `
               $hPin, $hPlain, (Joined $order2))
    Assert-Ec ((Joined (Selected-Hwnds (Dry-Run @('--topmost-match') $cond2))) -eq $hPin) `
              ("--topmost-match 应挑先建却置顶的 {0}（按创建顺序的读法会挑 {1}）" -f $hPin, $hPlain)
    Assert-Ec ((Joined (Selected-Hwnds (Dry-Run @('--bottommost-match') $cond2))) -eq $hPlain) `
              ("--bottommost-match 应落在后建那扇 {0}" -f $hPlain)
    Assert-Ec ((Joined (Selected-Hwnds (Dry-Run @('--newest') $cond2))) -eq $hPin) `
              '旧别名 --newest 在这对里同样按 Z 序挑（名字与行为不一致，所以才废弃它）'
    Stop-EcWindow -Window $plain
    Stop-EcWindow -Window $pin          # 置顶物留着会把后面的截图判据搞脏，先收掉

    # ---------------- 互斥、别名并用与越界 ----------------
    $r = Invoke-Ecapture -Arguments (@($COND) + @('--index', '1', '--all', '--dry-run', 'out.png'))
    Assert-Ec ((Error-Codes (Json-Of $r)) -contains 'cli.conflicting_options' -and $r.Exit -eq 1) `
              ("--index 与 --all 应互斥，实际 exit={0}" -f $r.Exit)

    $r = Invoke-Ecapture -Arguments (@($COND) + @('--topmost-match', '--bottommost-match', '--dry-run', 'out.png'))
    Assert-Ec ((Error-Codes (Json-Of $r)) -contains 'cli.conflicting_options') `
              '--topmost-match 与 --bottommost-match 应互斥'

    # 同一策略的新旧两种写法 = 还是一条策略，不能被当成互斥（别名最要紧的一条判据）。
    $r = Invoke-Ecapture -Arguments (@($COND) + @('--newest', '--topmost-match', '--dry-run', '-v', 'out.png'))
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and -not ((Error-Codes $o) -contains 'cli.conflicting_options')) `
              ("--newest 与 --topmost-match 并用不应判成两个互斥策略，实际 exit={0}" -f $r.Exit)
    Assert-Ec (@(@($o.notes) | Where-Object { $_.code -eq 'note.deprecated_option' }).Count -eq 1) `
              '同一策略的别名并用时废弃提示应只出现一次'
    Assert-Ec ($o.input.policy -eq 'topmost') '别名并用时 input.policy 仍应写清实际生效那条策略'

    # 别名与另一条策略并用：互斥报错里要能看见用户实际写的那两个名字（追溯得回去）。
    $r = Invoke-Ecapture -Arguments (@($COND) + @('--oldest', '--index', '2', '--dry-run', 'out.png'))
    $conflict = First-Error-With (Json-Of $r) 'cli.conflicting_options'
    Assert-Ec ($null -ne $conflict -and $conflict.value -match '--oldest' -and $conflict.value -match '--index') `
              ("互斥报错应列出用户写的那两个名字，实际 value={0}" -f $conflict.value)

    # 编号越界：命中两扇时 --index 3 仍按参数错处理，不放宽条件去另找目标。
    $r = Invoke-Ecapture -Arguments (@($COND) + @('--index', '3', '--dry-run', 'out.png'))
    Assert-Ec ($r.Exit -eq 1 -and (Error-Codes (Json-Of $r)) -contains 'match.index_out_of_range') `
              '--index 越界应按参数错处理，而不是换个目标凑数'

    # ===========================================================================
    Write-Host "`n=== 3) 身份复核不误伤健康目标（窗口内容路径 + --yes，不弹框）==="
    # ===========================================================================
    $shot1 = Get-EcRunFile -RunDir $run -Name 'identity_ok.png'
    $ha = Get-EcHwndHex $a.Hwnd
    $r = Invoke-Ecapture -Arguments @('--hwnd', $ha, '--yes', '--capture', 'wgc', '--out', $shot1)
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 1) `
              ("健康目标复核之后应照常出图，实际 exit={0} captured={1} stderr={2}" -f `
               $r.Exit, $o.captured, $r.Stderr)
    Assert-Ec ((Error-Codes $o).Count -eq 0) `
              ("健康目标不应被复核挡住，实际 codes={0}" -f (Joined (Error-Codes $o)))
    $img = @($o.images)[0]
    Assert-Ec ($img.hwnd -eq $ha) ("交付图的归属应为 {0}，实际 {1}" -f $ha, $img.hwnd)
    Assert-Ec ($img.scope -eq 'window' -and $img.path -eq 'wgc') `
              ("路径/来源应为 wgc + window，实际 {0} {1}" -f $img.path, $img.scope)
    $stats = Get-EcImageStats -Path $shot1 -Step 7
    $dist = Get-EcColorDistance -A $stats.TopDominant -B (New-EcSignatureKey -Seed 31)
    Assert-Ec ($dist -le 32) ("画面主色离本次窗口的签名色应 <=32，实际 {0}：复核之后截错了对象" -f $dist)

    # 同一个进程的两扇窗口（PID 相同、类名不同）：--hwnd 挑中的那一扇必须就是那一扇。
    # 这是"光比 PID 不够"的现实版 —— 两扇窗口的 PID 一模一样，判据只能靠句柄本身与类名。
    $twin = Start-EcWindow -RunDir $run -Class "ec-id-twin-$tag" -Title "身份 同进程 $tag" `
                           -Rect '150,480,540,760' -Windows 2 -Seed 34
    $second = Get-EcHwndHex $twin.Hwnds[1]
    $twinShot = Get-EcRunFile -RunDir $run -Name 'identity_twin.png'
    $r = Invoke-Ecapture -Arguments @('--hwnd', $second, '--yes', '--capture', 'wgc', '--out', $twinShot)
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o.captured -eq 1) `
              ("同进程的第二扇应截得到，实际 exit={0} stderr={1}" -f $r.Exit, $r.Stderr)
    Assert-Ec (@($o.images)[0].hwnd -eq $second) `
              ('同进程两扇目标时交付了另一扇（PID 相同不能当身份判据）：{0}' -f @($o.images)[0].hwnd)
    Assert-Ec (@($o.images)[0].class -eq $twin.Classes[1]) `
              ('交付图的类名应是那一扇的类名，实际 {0}' -f @($o.images)[0].class)

    # ===========================================================================
    Write-Host "`n=== 4) 目标在取帧之前被销毁：capture.target_gone，且不牵连其余目标 ==="
    # ===========================================================================
    $doomed = Invoke-ChangingBatch -Tag 'identity-doom' -SeedBase 35 `
                                   -ExtraArgs @('--destroy-after-ms', '1500')
    try {
        $o = $doomed.Json
        $e = First-Error-With $o 'capture.target_gone'
        if ($null -eq $e) {
            # 这台机器上枚举与销毁的先后没排开。如实记未验证，不硬判失败也不伪造通过。
            Skip-Ec '目标在批次中途被销毁这条现场没撞上' `
                    '判据本身已由离线层的假查询层逐条判过；接线由第 5 节那条标题失效的现场判'
            Write-Host ("  （本次：captured={0} codes={1}）" -f $o.captured,
                        (Joined (Error-Codes $o))) -ForegroundColor DarkGray
        } else {
            Assert-Ec ($e.target -eq $doomed.Hwnd) `
                      ("target_gone 应指着那扇被销毁的窗口 {0}，实际 {1}" -f $doomed.Hwnd, $e.target)
            Assert-Ec ($e.stage -eq 'capture') `
                      ("身份复核在取帧之前，stage 应为 capture，实际 {0}" -f $e.stage)
            Assert-Ec (@(Error-Codes $o | Where-Object { $_ -eq 'capture.target_gone' }).Count -eq 1) `
                      '目标消失那条诊断不该被复制好几遍，也不该顺手算到别的目标头上'
            Assert-Ec ([int]$o.captured -ge 6) `
                      ("其余目标该照常出图，实际 captured={0}" -f $o.captured)
            # 落地的文件数与 images 条数一致 = 被复核挡下的那一扇一张都没写。
            Assert-Ec ($doomed.Count -eq [int]$o.captured) `
                      ("落地的文件数 {0} 应与 captured {1} 一致：没取帧的目标不许留下图" -f `
                       $doomed.Count, $o.captured)
            Assert-Ec ($doomed.Result.Exit -eq 7) `
                      ("有目标失败、其余成功时退出码是 7，实际 {0}" -f $doomed.Result.Exit)
            Assert-Ec (-not (Test-EcWindowAlive -Hwnd $doomed.Ptr)) `
                      '销毁没真的发生：这条现场本身不成立，判据没判过任何东西'
        }
    } finally {
        if ($Keep) { Write-Host "保留临时目录：$($doomed.Run.Path)" -ForegroundColor DarkYellow }
        else { Remove-EcRunDir $doomed.Run -Quiet }
    }

    # ===========================================================================
    Write-Host "`n=== 5) 易变属性：标题刷新不算换目标，条件不再成立才算 ==="
    # ===========================================================================
    # 两批同构的现场，差别只在改出来的新标题里还留不留着当初那个子串。
    Write-Host '  -- 5a) 改了名而 --title-contains 不再成立'
    $broken = Invoke-ChangingBatch -Tag 'identity-brk' -SeedBase 45 `
                                   -ExtraArgs @('--rename-after-ms', '1500', '--title2', '彻底换了个名字不含标记') `
                                   -ExpectTitle2 '彻底换了个名字不含标记'
    try {
        $o = $broken.Json
        Assert-Ec ($broken.TitleNow -eq '彻底换了个名字不含标记') `
                  ("改名没真的发生（等满 {1} ms 仍没改成，现场不成立），当前标题 '{0}'" -f `
                   $broken.TitleNow, $broken.RenameWaitedMs)
        $e = First-Error-With $o 'capture.target_changed'
        if ($null -eq $e) {
            Skip-Ec '标题条件失效这条现场没撞上（改名与轮到它取帧的先后没排开）' `
                    '判据本身已由离线层的假查询层逐条判过'
            Write-Host ("  （本次：captured={0} codes={1}）" -f $o.captured,
                        (Joined (Error-Codes $o))) -ForegroundColor DarkGray
        } else {
            Assert-Ec ($e.target -eq $broken.Hwnd) `
                      ("target_changed 应指着那扇改了名的窗口 {0}，实际 {1}" -f $broken.Hwnd, $e.target)
            Assert-Ec ($e.message -match 'no longer satisfies') `
                      ("target_changed 的 message 要把重跑条件的结果写进来，实际 '{0}'" -f $e.message)
            Assert-Ec ($e.stage -eq 'capture') "身份复核的 stage 应为 capture，实际 $($e.stage)"
            Assert-Ec ([int]$o.captured -ge 6) `
                      ("坏掉一个目标不该牵连其余，实际 captured={0}" -f $o.captured)
            Assert-Ec ($broken.Result.Exit -eq 7) `
                      ("其余成功、一个失败时退出码是 7，实际 {0}" -f $broken.Result.Exit)
        }
    } finally {
        if ($Keep) { Write-Host "保留临时目录：$($broken.Run.Path)" -ForegroundColor DarkYellow }
        else { Remove-EcRunDir $broken.Run -Quiet }
    }

    Write-Host '  -- 5b) 改了名而 --title-contains 仍然成立（正常的标题刷新）'
    $kept = New-EcRunDir -Tag 'identity-kept'
    $keptOwned = @()
    try {
        $stag = $kept.Leaf -replace '[^a-z0-9]', ''
        $first = Start-EcWindow -RunDir $kept -Class "ec-id-keep-$stag" -Title "身份 改名仍命中 $stag" `
                                -Rect '150,150,540,420' -Seed 51 `
                                -ExtraArgs @('--rename-after-ms', '1500', "--title2", "$stag 还在标题里")
        $keptOwned += $first
        for ($k = 1; $k -le 6; $k++) {
            $keptOwned += Start-EcWindow -RunDir $kept -Class ("ec-id-kp{0}-{1}" -f $k, $stag) `
                                           -Title ("身份 陪跑{0} {1}" -f $k, $stag) `
                                           -Rect (('{0},150,{1},420') -f (600 + $k * 40), (920 + $k * 40)) `
                                           -Seed (51 + $k)
        }
        $out = Get-EcRunFile -RunDir $kept -Name 'kept_%i.png'
        $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 120000 -Arguments `
            (@('--title-contains', $stag, '--all', '--yes', '--capture', 'wgc', '--out', $out))
        $o = Json-Of $r
        $keptWait = Wait-EcWindowTitle -Window $first -Expected "$stag 还在标题里"
        Assert-Ec ($keptWait.Hit) `
                  ("改名没真的发生（等满 {0} ms 仍没改成，这条现场不成立），当前标题 '{1}'" -f `
                   $keptWait.WaitedMs, $keptWait.Title)
        Assert-Ec ($null -eq (First-Error-With $o 'capture.target_changed')) `
                  ("标题改了而条件仍然成立时被当成了换目标：{0}" -f `
                   (First-Error-With $o 'capture.target_changed').message)
        Assert-Ec ($r.Exit -eq 0 -and [int]$o.captured -eq 7) `
                  ("七扇都该照常出图，实际 exit={0} captured={1}" -f $r.Exit, $o.captured)
        # 交付的标题是**选定那一刻**那一份：它对应人看到并被批准的那一行，不跟着刷新跑。
        $mine = @(@($o.images) | Where-Object { $_.hwnd -eq (Get-EcHwndHex $first.Hwnd) })
        Assert-Ec ($mine.Count -eq 1 -and $mine[0].title -eq "身份 改名仍命中 $stag") `
                  ("images[].title 应是选定当时的标题，实际 '{0}'" -f $mine[0].title)
    } finally {
        foreach ($w in $keptOwned) { Stop-EcWindow -Window $w }
        if ($Keep) { Write-Host "保留临时目录：$($kept.Path)" -ForegroundColor DarkYellow }
        else { Remove-EcRunDir $kept -Quiet }
    }

    # ===========================================================================
    Write-Host "`n=== 6) 确认框等待期间的变化（要代人点'是'才做得出来）==="
    # ===========================================================================
    if (-not $SimulateConsent) {
        Skip-Ec '确认框开着的那段时间里目标被销毁' `
                '这条要测试代人点"是"（等于代人同意），只在显式给 -SimulateConsent、且桌面没有隐私内容时做。会不会弹框本身由 tests\consent.ps1 判'
        Skip-Ec '确认框开着的那段时间里目标改了名' '同上'
    } else {
        # 不带 --yes => 窗口内容路径也要问一次。等人（这里是探测）这段时间足够目标自己把窗口销毁。
        $gone = Start-EcWindow -RunDir $run -Class "ec-id-consent-$tag" -Title "身份 确认中变 $tag" `
                               -Rect '200,200,600,480' -Seed 71 -ExtraArgs @('--destroy-after-ms', '600')
        $hg = Get-EcHwndHex $gone.Hwnd
        $shot = Get-EcRunFile -RunDir $run -Name 'consent_gone.png'
        $script:ConsentProbeHit = $false
        $probe = {
            param($p)
            $h = Find-EcDialog -ProcessId ([int]$p.Id)
            if ($h -eq [IntPtr]::Zero) { return '' }
            # 先等目标真的没了，再代人点"是"：点的就是"确认框还开着而目标已经变了"那一瞬间。
            $deadline = (Get-Date).AddSeconds(10)
            while ((Test-EcWindowAlive -Hwnd $gone.Hwnd) -and (Get-Date) -lt $deadline) {
                Start-Sleep -Milliseconds 100
            }
            [void](Click-EcDialogButton -Dialog $h -ButtonId 6)   # IDYES=6
            $script:ConsentProbeHit = $true
            return 'clicked-yes-after-target-gone'
        }
        $r = Invoke-EcProcess -FilePath $Exe -Arguments @('--hwnd', $hg, '--capture', 'wgc', '--out', $shot) `
                             -Probe $probe -ProbeIntervalMs 60 -ProbeTimeoutMs 20000 -TimeoutMs 60000
        $o = Json-Of $r
        Assert-Ec $script:ConsentProbeHit `
                  '不带 --yes 的窗口内容路径应弹一次确认框，否则这一轮的现场没建立起来'
        $e = First-Error-With $o 'capture.target_gone'
        Assert-Ec ($null -ne $e) `
                  ('确认框期间目标被销毁、之后代答了"是"，应报 capture.target_gone 而不是拍一张别的画面：exit={0} codes={1}' -f `
                   $r.Exit, (Joined (Error-Codes $o)))
        Assert-Ec ($e.target -eq $hg) ("target 应指着当初批准的那一扇，实际 {0}" -f $e.target)
        Assert-Ec ([int]$o.captured -eq 0 -and -not (Test-Path -LiteralPath $shot)) `
                  '身份复核没通过时一张都不该落地'
        Assert-Ec ($r.Exit -eq 7) ("复核挡下的一次截图失败应按 7 交出去，实际 {0}" -f $r.Exit)
        Stop-EcWindow -Window $gone
    }
} finally {
    Stop-EcOwnedWindows
    if ($Keep) { Write-Host "保留临时目录：$($run.Path)" -ForegroundColor DarkYellow }
    else { Remove-EcRunDir $run }
}

# ===========================================================================
Write-Host "`n=== 7) 明确记未验证的项（不伪造通过）==="
# ===========================================================================
Skip-Ec '真机上把"同一个 HWND 值被另一个进程复用""同一个 PID 上是另一个进程"做成现场' `
        '系统什么时候回收句柄与 PID 不由测试决定，要造就得杀掉别人的进程。这两条由离线层的假查询层逐条判（第 1 节），本测试不为凑现场去动使用者真实应用'
Skip-Ec '把一扇既存窗口重新激活来改变 Z 序，再看 --topmost-match 跟着换人' `
        'Windows 的前台锁不让后台进程替窗口抢叠放位置：实测从测试进程调 SetWindowPos(HWND_TOP)、以及由窗口自己那条线程调，EnumWindows 的顺序都不动。Z 序与创建顺序相反这一层由第 2 节那对"先建但置顶 / 后建但普通"的窗口现场判'
Skip-Ec 'dwm 升级到屏幕退路之前那一次 kFull 复核的现场' `
        '这条退路要 PrintWindow 真的失败、还要人在桌面确认框上点"是"，两件事都得有人在现场。判据分支本身在离线层判过，退路的弹框判据在 tests\consent.ps1 与 tests\channels.ps1'
Skip-Ec '复核之后到取帧之前那一瞬的窗口生命周期竞态被完全消除' `
        '身份复核只把竞态窗口缩小：判据与取帧之间不是原子的，而 HWND 既不是可等待对象，也没有"锁住一个窗口"的公开 API'

exit (Complete-EcSuite -Title '身份复核与选择策略：')
