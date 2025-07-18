<#
.SYNOPSIS
    WGC 动态尺寸与帧池生命周期的判据：离线层逐条注入几何形状，真机层用可控测试窗口连续缩放核对交付尺寸与边缘。
.DESCRIPTION
    这次修的是"每帧只看 item.Size() 那一份初始尺寸、整张纹理复制、忽略 frame.ContentSize()"：
    窗口在取帧这一刻被缩小，交回的纹理仍是帧池当初那份较大的尺寸，多出来的边缘是没定义的像素，
    却被当成整幅画面交出去；窗口被放大超过帧池，那一帧本身就残缺，也被当成完整尺寸宣称。

    判据分两层：
      1) 离线层：build\ecapture-wgc-tests.exe（源码 tests\wgc_state.cpp）——把"内容尺寸 vs 纹理尺寸
         vs 帧池 vs 采集项"这几种关系直接注入生产判据本体（src/WgcGeometry.cpp）：
         内容小于 / 等于 / 大于纹理、采集项超出帧池要重建、重建超预算放弃、重建次数到上限放弃，
         以及"任何判为复制的决定，复制矩形都在纹理之内且非零"这条不读未定义边缘的硬约束。
         这些形状真机上要么难稳定复现（要窗口恰好在取帧那一瞬被缩放），要么拿不到阴性对照。
      2) 真机层：本测试自建的窗口（ecwindow.exe）在三个确定尺寸上各截一次，核对交付的 PNG 尺寸
         恰好等于窗口当下请求的矩形、边缘是真实内容（顶部签名色 + 颜色带），JSON 报告与图片一致。
         再跑一段连续缩放的健壮性循环：每一次都必须截成功（不崩、不交形状不合法的帧），
         尺寸落在用过的目标尺寸之内、绝不为 0、绝不超过最大那一份（旧实现可能把较大的旧帧
         连边缘一起当成整幅交出来）。

    "同一会话内内容严格小于纹理""取帧中途窗口被放大触发重建"这两条只能在单帧会话内、且时间点上
    恰好命中才有现场，真机上无法稳定复现，一律由离线层判；本层如实记未验证，不伪造。
    测试一律用自建的窗口，只收尾自己起的那个进程；wgc 走窗口内容路径，带 --yes 不弹确认框。
.EXAMPLE
    .\tests\wgc.ps1
    .\tests\wgc.ps1 -SkipState        # 只跑真机那层
    .\tests\wgc.ps1 -SkipReal         # 只跑离线判据层（没有交互桌面时用）
    .\tests\wgc.ps1 -Keep             # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$SkipReal,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-wgc-tests.exe'

$run = New-EcRunDir -Tag 'wgc'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

function Json-Of($r) {
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $o = $null
    try { $o = $body | ConvertFrom-Json } catch { }
    return $o
}

# PNG 头里的 IHDR 宽高（大端 32 位）：不依赖任何库，独立于工具自己报告的尺寸做对照。
function Get-PngSize {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $b = [IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 24) { return $null }
    if (-not ($b[0] -eq 0x89 -and $b[1] -eq 0x50 -and $b[2] -eq 0x4E -and $b[3] -eq 0x47)) { return $null }
    $w = [int]$b[16] * 16777216 + [int]$b[17] * 65536 + [int]$b[18] * 256 + [int]$b[19]
    $h = [int]$b[20] * 16777216 + [int]$b[21] * 65536 + [int]$b[22] * 256 + [int]$b[23]
    return [pscustomobject]@{ Width = $w; Height = $h }
}

# 用 wgc 通道截本次窗口一张：--yes 免掉窗口内容那一级确认（桌面像素路径与此无关）。
function Invoke-WgcShot {
    param([Parameter(Mandatory)]$Window, [Parameter(Mandatory)][string]$Name)
    $path = Get-EcRunFile -RunDir $run -Name "$Name.png"
    Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 30000 -Arguments @(
        '--hwnd', (Get-EcHwndHex $Window.Hwnd), '--capture', 'wgc', '--yes',
        '--format', 'png', '--lang', 'zh-CN', '--out', $path)
    $json = Json-Of $r
    $img = if ($json) { @($json.images)[0] } else { $null }
    $err = if ($json -and $json.errors) { @($json.errors | ForEach-Object { $_.code }) } else { @() }
    [pscustomobject]@{
        Exit = $r.Exit; Path = $path; Json = $json; Img = $img; ErrCodes = $err
        Err = $r.Stderr
        Png = Get-PngSize -Path $path
    }
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：几何判据与重建闸门（测生产函数本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec 'WGC 几何与重建的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-wgc-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "WGC 几何判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "WGC 几何判据有失败项：$tail"
        Assert-Ec ([int]$m.Groups[1].Value -ge 20) "WGC 几何判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    if ($SkipReal) {
        Skip-Ec '真机连续缩放核对交付尺寸与边缘' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'   # 只跑离线层时直接进 finally 收尾
    }

    # =========================================================================
    Write-Host "`n=== 1) 真机三个确定尺寸：交付 PNG 尺寸跟着窗口走、边缘是真实内容 ==="
    # =========================================================================
    $class = "ec-wgc-$tag"
    $title = "WGC 缩放窗口 $class"
    # 从一份中等尺寸起步；WS_POPUP 没有边框，请求矩形就是采集项当下的尺寸。
    $w = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '200,200,700,500' -Seed 7
    Write-Host ("目标窗口 PID={0} HWND={1} 类={2}" -f $w.Pid, (Get-EcHwndHex $w.Hwnd), $class)
    $mine = New-EcSignatureKey -Seed 7

    # 名字，L,T,R,B（宽x高 = R-L, B-T）
    $sizes = @(
        @{ N = 'start';  R = '200,200,700,500';  W = 500; H = 300 },   # 500x300
        @{ N = 'small';  R = '260,260,520,400';  W = 260; H = 140 },   # 260x140（缩小）
        @{ N = 'big';    R = '120,120,860,620';  W = 740; H = 500 }    # 740x500（放大超过前两份）
    )
    foreach ($s in $sizes) {
        [void](Set-EcWindowRect -Window $w -Rect $s.R)
        Start-Sleep -Milliseconds 150   # 等 DWM 把新尺寸合成出来，别拍到还在改的旧帧
        $r = Invoke-WgcShot -Window $w -Name $s.N
        Write-Host ("  {0,-6} {1}x{2} 期望 {3}x{4} exit={5} 弹框=N/A path={6}" -f `
            $s.N, $r.Png.Width, $r.Png.Height, $s.W, $s.H, $r.Exit, $r.Img.path) -ForegroundColor DarkGray

        Assert-Ec ($r.Exit -eq 0) "$($s.N) 截图退出码 $($r.Exit)，应为 0：$($r.Err)"
        Assert-Ec ($r.ErrCodes -notcontains 'capture.frame_invalid') `
            "$($s.N) 把一帧合法的图判成了形状不合法：$($r.ErrCodes -join ',')"
        Assert-Ec ($r.Img -and $r.Json.captured -eq 1) "$($s.N) 没交出图片"
        Assert-Ec ($r.Img.path -eq 'wgc') "$($s.N) 的来路该是 wgc，实际 $($r.Img.path)"
        Assert-Ec ($r.Img.scope -eq 'window') "$($s.N) 的 scope 该是 window，实际 $($r.Img.scope)"
        # 核心：交付尺寸 = 窗口当下尺寸。旧实现在窗口缩小时会按纹理（帧池旧尺寸）整张复制，
        # 尺寸偏大且多出一圈没定义的边缘。
        Assert-Ec ($r.Png -and $r.Png.Width -eq $s.W -and $r.Png.Height -eq $s.H) `
            "$($s.N) 交付 PNG $($r.Png.Width)x$($r.Png.Height)，应为 $($s.W)x$($s.H)（尺寸没跟着窗口走）"
        Assert-Ec ($r.Img.width -eq $s.W -and $r.Img.height -eq $s.H) `
            "$($s.N) JSON 报告 $($r.Img.width)x$($r.Img.height) 与窗口尺寸 $($s.W)x$($s.H) 不一致"

        # 边缘判据：图有真实内容（颜色种数够），且主色是这次窗口的签名色 —— 不是没定义边缘的杂色、
        # 也不是别的窗口。
        $stats = Get-EcImageStats -Path $r.Path -Step 3
        Assert-Ec ($stats.Colors -ge 12) "$($s.N) 画面只有 $($stats.Colors) 种颜色，可能是空帧/未定义边缘"
        $d = Get-EcColorDistance -A $stats.TopDominant -B $mine
        Assert-Ec ($d -le 32) "$($s.N) 主色 $($stats.TopDominant) 离本次签名色 $mine 太远（$d），这一帧不是本窗口的"
    }

    # =========================================================================
    Write-Host "`n=== 2) 真机连续缩放健壮性循环：每一次都要截成功、尺寸不越界 ==="
    # =========================================================================
    $maxW = 0; $maxH = 0
    foreach ($s in $sizes) { if ($s.W -gt $maxW) { $maxW = $s.W }; if ($s.H -gt $maxH) { $maxH = $s.H } }
    $rounds = 8
    $okCount = 0
    $shapes = @('300,300,480,380', '240,240,760,560', '160,160,600,420', '260,260,360,300')
    for ($i = 0; $i -lt $rounds; $i++) {
        $rect = $shapes[$i % $shapes.Count]
        [void](Set-EcWindowRect -Window $w -Rect $rect)
        Start-Sleep -Milliseconds 120
        $r = Invoke-WgcShot -Window $w -Name ("stress_{0}" -f $i)
        $p = $r.Path.Split(',')   # 只为读一下当前请求矩形
        $rp = $rect.Split(',') | ForEach-Object { [int]$_ }
        $wantW = $rp[2] - $rp[0]; $wantH = $rp[3] - $rp[1]
        Assert-Ec ($r.Exit -eq 0) "缩放循环第 $i 次截图失败（exit=$($r.Exit)）：$($r.Err)"
        Assert-Ec ($r.Png -and $r.Png.Width -gt 0 -and $r.Png.Height -gt 0) `
            "缩放循环第 $i 次交付了 0 尺寸或坏 PNG 头"
        # 绝不把较大的旧帧连边缘当成整幅交出来：交付尺寸不能超过本次窗口尺寸之外（这里窗口是静止的，
        # 应严格等于；给 8px 余量吸收改尺寸与采样之间的抖动）。
        Assert-Ec ([Math]::Abs($r.Png.Width - $wantW) -le 8 -and [Math]::Abs($r.Png.Height - $wantH) -le 8) `
            "缩放循环第 $i 次交付 $($r.Png.Width)x$($r.Png.Height)，与本次窗口 ${wantW}x${wantH} 差太多（可能读到未定义边缘）"
        if ($r.Exit -eq 0) { $okCount++ }
    }
    Assert-Ec ($okCount -eq $rounds) "缩放循环只有 $okCount/$rounds 次成功"
    Write-Host "  $rounds 次连续缩放全部截成功，尺寸跟随窗口" -ForegroundColor DarkGray

    # =========================================================================
    Write-Host "`n=== 3) 目标窗口消失：句柄还在但画面没了 -> capture.window_gone（不崩、不交空图） ==="
    # =========================================================================
    $gone = Start-EcWindow -RunDir $run -Class "ec-wgc-gone-$tag" -Title "will close" -Rect '200,200,520,420' -Seed 3
    $goneHwnd = Get-EcHwndHex $gone.Hwnd
    Stop-EcWindow -Window $gone
    $path = Get-EcRunFile -RunDir $run -Name 'gone.png'
    Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 30000 -Arguments @(
        '--hwnd', $goneHwnd, '--capture', 'wgc', '--yes', '--lang', 'zh-CN', '--out', $path)
    $j = Json-Of $r
    $codes = @()
    if ($j -and $j.errors) { $codes = @($j.errors | ForEach-Object { $_.code }) }
    Write-Host ("  关闭后的窗口 exit={0} codes={1}" -f $r.Exit, ($codes -join ',')) -ForegroundColor DarkGray
    # 关掉的窗口枚举阶段就被挡（match.no_window）或取帧阶段报 window_gone；两种都不该崩、都不该写出图。
    Assert-Ec ($r.Exit -ne 0) "目标已经不在了却返回成功（exit=$($r.Exit)）"
    Assert-Ec (-not (Test-Path -LiteralPath $path)) "目标没了还是把文件写出来了"
    Assert-Ec ($codes -notcontains 'capture.worker_failed') "关掉的目标被报成工具自身故障：$($codes -join ',')"

    # =========================================================================
    Write-Host "`n=== 4) 本机造不出来的判据（照实记录，不算通过） ==="
    # =========================================================================
    Skip-Ec '同一会话内 ContentSize 严格小于纹理（读不到未定义边缘）' '要窗口恰好在 StartCapture 与 TryGetNextFrame 之间缩小才有现场，真机不可稳定复现；由离线层 DecideWgcFrame 判（内容小于纹理 => 只复制内容矩形）'
    Skip-Ec '取帧中途窗口放大触发帧池重建' '同样要卡在单帧会话的时间点上；重建判定与预算/次数闸门由离线层 DecideWgcRecreate 判，真机只能证到"稳定尺寸下交付尺寸=窗口尺寸"'

    Write-Host "`n=== 收尾 ==="
    Stop-EcOwnedWindows
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} catch {
    if ("$_" -ne '__SKIP_REAL__') { throw }
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title 'WGC 动态尺寸与帧池生命周期')
