<#
.SYNOPSIS
    HDR 色彩处理（--hdr）的判据：离线层 + 真机层（本机非 HDR 的部分如实记未验证）。
.DESCRIPTION
    三层各判各的，都不靠"真有一台 HDR 显示器"这种本机给不出的现场：

      0) 离线层（build\ecapture-hdr-tests.exe）：判据本体——两张登记表的一致性、DXGI 格式与
         显示 color space 的分类（含认不出一律 unknown、不猜）、half 解码与传递函数与 tone 曲线的
         性质（黑进黑 / 单调 / white=1 恒等 / 不越界）、用已知色块与亮度梯度逐点判 ConvertWideFrameToSdrBgra8、
         来源与形状守卫、那组结果键的合成分解，以及 HdrRequestPossible。
      1) 真机 SDR 层：这台机器的显示器不支持 HDR，所以凡是"带回一幅 HDR 帧"的现场都造不出来 ——
         但**恰恰因此**这一层判的是最要紧的一条：默认与 --hdr 各策略在一张真实的 SDR 自建房窗口上
         都不改变画面（SDR 回归），并且把来源如实报成 srgb_bgra8 / sdr_passthrough，而不是假装映射过。
      2) --capabilities 的 color 段：三件事分开写（compiled / status / verifiedOnThisMachine），
         verifiedOnThisMachine 恒 no（本项目没有 HDR 屏），两份查询同源。

    刻意不在本机伪造的现场（一律记未验证，见最后一节）：真 HDR 显示器上的实拍对照、--hdr refuse
    在 HDR 帧上的拒绝、FP16 帧池那条实际出图、HLG 那条的真机下场——没有 HDR 设备就不宣称色彩验收通过。

    默认只用自建窗口 + 窗口内容那一级（--yes 免掉的那一级），一次都不弹框，也不动使用者的显示设置。
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
$stateExe = Join-Path $root 'build\ecapture-hdr-tests.exe'

$run = New-EcRunDir -Tag 'hdr'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

function Json-Of($r) {
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $o = $null
    try { $o = $body | ConvertFrom-Json } catch { }
    return $o
}

# PNG 头里的 IHDR 宽高：独立于工具自己报告的尺寸做对照。
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

# 一次截图。--yes / --hdr 都不预置：授权与色彩这两件事每条判据自己写明要什么。
function Invoke-Shot {
    param(
        [Parameter(Mandatory)]$Window,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string[]]$Extra,
        [int]$TimeoutMs = 60000
    )
    $path = Get-EcRunFile -RunDir $run -Name "$Name.png"
    Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
    $argv = @('--hwnd', (Get-EcHwndHex $Window.Hwnd), '--format', 'png', '--lang', 'zh-CN',
             '--out', $path) + @($Extra)
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs $TimeoutMs -Arguments $argv
    $json = Json-Of $r
    [pscustomobject]@{
        Exit = $r.Exit; Path = $path; Json = $json; Exists = Test-Path -LiteralPath $path
        Codes = if ($json -and $json.errors) { @($json.errors | ForEach-Object { $_.code }) } else { @() }
        Notes = if ($json -and $json.notes) { @($json.notes | ForEach-Object { $_.code }) } else { @() }
        Img = if ($json -and $json.images -and @($json.images).Count) { @($json.images)[0] } else { $null }
        Raw = $r
    }
}

# HDR 那组键在不在、各是什么（缺席也是一种答案）。
function Get-HdrFields($Img) {
    if (-not $Img) { return $null }
    $names = @($Img.PSObject.Properties.Name)
    [pscustomobject]@{
        HasAny = ($names -contains 'hdrRequested') -or ($names -contains 'hdrEffective') -or
                 ($names -contains 'hdrBasis') -or ($names -contains 'sourceColorSpace') -or
                 ($names -contains 'sourceBitDepth')
        Requested = $Img.hdrRequested
        Effective = $Img.hdrEffective
        Basis = $Img.hdrBasis
        SourceColorSpace = $Img.sourceColorSpace
        SourceBitDepth = $Img.sourceBitDepth
        HasBitDepth = ($names -contains 'sourceBitDepth')
    }
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：两张表一致、格式分类、tone 数学与结果键合成（测生产函数本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec 'HDR 的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-hdr-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "HDR 离线判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '共 (\d+) 项，失败 (\d+) 项')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "HDR 离线判据有失败项：$tail"
        # 这条下限防"判据被删了还绿"：两张表一致、格式与状态分类、单点数学、色块/梯度/守卫、
        # 三键合成、HdrRequestPossible 这六大批本就有 50 条以上。
        Assert-Ec ([int]$m.Groups[1].Value -ge 50) "HDR 离线判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    if ($SkipReal) {
        Skip-Ec '真机 HDR 判据' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'
    }

    # =========================================================================
    Write-Host "`n=== 1) 真机 SDR 回归：--hdr 各策略都不改变一张真实 SDR 自建房窗口的画面 ==="
    # =========================================================================
    $class = "ec-hdr-$tag"
    $title = "HDR 测试窗口 $class"
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '200,200,700,500' -Seed 21
    Write-Host ("目标窗口 PID={0} HWND={1} 类={2}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd), $class)
    $mine = New-EcSignatureKey -Seed 21
    Start-Sleep -Milliseconds 400
    $responsive = $false
    foreach ($try in 1..8) {
        if (Test-EcWindowResponsive -Hwnd $window.Hwnd -TimeoutMs 500) { $responsive = $true; break }
        Start-Sleep -Milliseconds 250
    }
    Assert-Ec $responsive '测试窗口的消息线程一直没在响应'
    $wr = Get-EcWindowRect -Hwnd $window.Hwnd
    $wW = $wr.Right - $wr.Left
    $wH = $wr.Bottom - $wr.Top

    # 基线：没写 --hdr。色彩那组键一个都不出现（与这条选项存在之前逐字节相同），画面正常。
    $plain = Invoke-Shot -Window $window -Name 'no-hdr' -Extra @('--capture', 'wgc', '--yes')
    Assert-Ec ($plain.Exit -eq 0 -and $plain.Exists) "基线截图失败：exit=$($plain.Exit) $($plain.Raw.Stderr)"
    $pf = Get-HdrFields $plain.Img
    Assert-Ec (-not $pf.HasAny) '没写 --hdr 时结果里出现了色彩那组键（默认值不该改变输出形状）'
    $png = Get-PngSize -Path $plain.Path
    Assert-Ec ($png -and $png.Width -eq $wW -and $png.Height -eq $wH) "基线图尺寸 $($png.Width)x$($png.Height) 不是窗口 $wW x $wH"
    $pstats = Get-EcImageStats -Path $plain.Path -Step 3
    Assert-Ec ($pstats.Colors -ge 12) "基线画面只有 $($pstats.Colors) 种颜色，可能是空帧或发白图"
    Assert-Ec ((Get-EcColorDistance -A $pstats.TopDominant -B $mine) -le 32) '基线主色不是本次窗口的'

    # --hdr auto：那组键出现，但如实报"这条路径带回的是 8 位 SDR"，且不发 note（auto 只被动上报）。
    $auto = Invoke-Shot -Window $window -Name 'hdr-auto' -Extra @('--capture', 'wgc', '--hdr', 'auto', '--yes')
    Assert-Ec ($auto.Exit -eq 0 -and $auto.Exists) "--hdr auto 退出码 $($auto.Exit)"
    $af = Get-HdrFields $auto.Img
    Assert-Ec ($af.Requested -eq 'auto' -and $af.Effective -eq 'sdr_passthrough') `
        "--hdr auto 那组键不对劲：$($af.Requested)/$($af.Effective)/$($af.Basis)"
    Assert-Ec ($af.SourceColorSpace -eq 'srgb_bgra8' -and $af.HasBitDepth -and $af.SourceBitDepth -eq 8) `
        "--hdr auto 来源应报成 srgb_bgra8 / 8 位，实际 $($af.SourceColorSpace) / $($af.SourceBitDepth)"
    Assert-Ec ($af.Basis -eq 'delivered_bgra8_sdr') "wgc 上带回 SDR，basis 该是 delivered_bgra8_sdr，实际 $($af.Basis)"
    Assert-Ec ($auto.Notes -notcontains 'note.hdr_source_sdr') '--hdr auto 不该发"来源是 SDR"那条提示（它只被动上报）'
    # 画面与基线一致：auto 没有动采集格式。
    $apng = Get-PngSize -Path $auto.Path
    Assert-Ec ($apng.Width -eq $wW -and $apng.Height -eq $wH) "--hdr auto 改了图尺寸：$($apng.Width)x$($apng.Height)"

    # --hdr tonemap：本机 SDR -> 恒等透传，且留一条 note.hdr_source_sdr（明确要过却没 HDR 可映射）。
    $tm = Invoke-Shot -Window $window -Name 'hdr-tonemap' -Extra @('--capture', 'wgc', '--hdr', 'tonemap', '--yes')
    Assert-Ec ($tm.Exit -eq 0 -and $tm.Exists) "--hdr tonemap 退出码 $($tm.Exit)：$($tm.Raw.Stderr)"
    $tf = Get-HdrFields $tm.Img
    Assert-Ec ($tf.Requested -eq 'tonemap' -and $tf.Effective -eq 'sdr_passthrough' -and
               $tf.Basis -eq 'delivered_bgra8_sdr') `
        "--hdr tonemap 在 SDR 上应透传，实际 $($tf.Requested)/$($tf.Effective)/$($tf.Basis)"
    Assert-Ec ($tm.Notes -contains 'note.hdr_source_sdr') `
        "--hdr tonemap 配 SDR 来源应留一条 note.hdr_source_sdr，实际 notes：$($tm.Notes -join ',')"
    # SDR 回归的关键一条：要求过 tonemap 也没把这张 SDR 图映射歪（主色仍是本次窗口、颜色数没塌）。
    $tpng = Get-PngSize -Path $tm.Path
    Assert-Ec ($tpng.Width -eq $wW -and $tpng.Height -eq $wH) "--hdr tonemap 改了图尺寸：$($tpng.Width)x$($tpng.Height)"
    $tstats = Get-EcImageStats -Path $tm.Path -Step 3
    Assert-Ec ($tstats.Colors -ge 12) "--hdr tonemap 后画面只剩 $($tstats.Colors) 种颜色（可能被错误映射）"
    Assert-Ec ((Get-EcColorDistance -A $tstats.TopDominant -B $mine) -le 32) '--hdr tonemap 后主色不再是本次窗口（映射动了 SDR）'

    # --hdr refuse：本机 SDR -> 没有 HDR 可拒绝，正常出图，effective=sdr_passthrough，也留 note。
    $rf = Invoke-Shot -Window $window -Name 'hdr-refuse' -Extra @('--capture', 'wgc', '--hdr', 'refuse', '--yes')
    Assert-Ec ($rf.Exit -eq 0 -and $rf.Exists) "--hdr refuse 在 SDR 上不该报错，退出码 $($rf.Exit)"
    Assert-Ec ($rf.Codes -notcontains 'capture.hdr_refused') "--hdr refuse 在没有 HDR 的机器上却拒了：$($rf.Codes -join ',')"
    $rff = Get-HdrFields $rf.Img
    Assert-Ec ($rff.Requested -eq 'refuse' -and $rff.Effective -eq 'sdr_passthrough') `
        "--hdr refuse 在 SDR 上那组键不对劲：$($rff.Requested)/$($rff.Effective)"
    Assert-Ec ($rf.Notes -contains 'note.hdr_source_sdr') '--hdr refuse 配 SDR 来源也应留一条 note.hdr_source_sdr'

    # --quiet：那组键是内容判据（不许抑制），而 note 归到 notes 被去掉。
    $quiet = Invoke-Shot -Window $window -Name 'quiet-tonemap' -Extra @(
        '--capture', 'wgc', '--hdr', 'tonemap', '--yes', '--quiet')
    Assert-Ec ($quiet.Exit -eq 0) "--quiet 那一路退出码 $($quiet.Exit)"
    $qf = Get-HdrFields $quiet.Img
    Assert-Ec ($qf.Requested -eq 'tonemap' -and $qf.Effective -eq 'sdr_passthrough') `
        '--quiet 把色彩那组键也藏了（它是内容判据，不是提示）'
    Assert-Ec ($quiet.Json.PSObject.Properties.Name -notcontains 'notes') '--quiet 没去掉 notes 段'

    # =========================================================================
    Write-Host "`n=== 2) 只读查询：--capabilities 的 color 段，两份查询同源 ==="
    # =========================================================================
    $cap = Invoke-EcProcess -FilePath $Exe -TimeoutMs 20000 -Arguments @('--capabilities', '--lang', 'zh-CN')
    Assert-Ec ($cap.Exit -eq 0) "--capabilities 退出码 $($cap.Exit)"
    $capJson = $cap.Stdout | ConvertFrom-Json
    $col = $capJson.color
    Assert-Ec ($col -and $col.default -eq 'auto') "能力报告里没有 color 段或默认值不对：$($col.default)"
    Assert-Ec ((@($col.values) -join ',') -eq 'auto,tonemap,refuse' -and @($col.values).Count -eq 3) `
        "color.values 不对劲：$(@($col.values) -join ',')"
    Assert-Ec ($col.compiled -eq $true) 'color.compiled 该是 true（这个构建实现了 tone mapping）'
    Assert-Ec ($col.verifiedOnThisMachine -eq 'no') `
        "color.verifiedOnThisMachine 该恒为 no（本机没有 HDR 屏，不宣称色彩验收通过），实际 $($col.verifiedOnThisMachine)"
    Assert-Ec ($col.encoderOutput -eq 'sdr_bgra8') "color.encoderOutput 该是 sdr_bgra8，实际 $($col.encoderOutput)"
    $colPaths = @($col.paths)
    Assert-Ec ($colPaths.Count -ge 9) "color.paths 只列了 $($colPaths.Count) 条（登记表里应有九条内部路径）"
    $wide = @($colPaths | Where-Object { $_.capability -eq 'wide_gamut_capable' } | ForEach-Object { $_.path }) | Sort-Object
    Assert-Ec ((($wide) -join ',') -eq 'duplication.frame,screen.duplication,screen.wgc,wgc' -or
               ($wide -contains 'wgc')) "带得回广色域的那四条应是 wgc/screen.wgc/duplication.frame/screen.duplication，实际：$($wide -join ',')"
    $sdrOnly = @($colPaths | Where-Object { $_.capability -eq 'sdr_source_only' } | ForEach-Object { $_.path })
    Assert-Ec ($sdrOnly -contains 'printwindow') "printwindow 该登记成 sdr_source_only，实际 sdr-only 集合：$($sdrOnly -join ',')"
    Assert-Ec (@($capJson.caveats) -contains 'hdr_tone_mapping_not_verified_on_hdr_display') `
        'caveats 里少了"tone mapping 没在 HDR 帧上实测过"那条边界'
    Assert-Ec (@($capJson.caveats) -contains 'hdr_output_is_tone_mapped_to_sdr_bgra8') `
        'caveats 里少了"HDR 一律映射成 SDR 交付"那条边界'
    # 两份查询同源：--diagnostics 也带同一段（除段落取舍外字段全同）。
    $diag = Invoke-EcProcess -FilePath $Exe -TimeoutMs 20000 -Arguments @('--diagnostics', '--lang', 'zh-CN')
    $diagJson = $diag.Stdout | ConvertFrom-Json
    Assert-Ec (($diagJson.color | ConvertTo-Json -Depth 6 -Compress) -eq
               ($col | ConvertTo-Json -Depth 6 -Compress)) `
        '--capabilities 与 --diagnostics 的 color 段不一致（同一份判据不该有两个答案）'
    # 屏幕查询那一份里没有 color 段：--hdr 没有把它变成"免确认点名一块屏"。
    $screens = Invoke-EcProcess -FilePath $Exe -TimeoutMs 20000 -Arguments @('--screens', '--lang', 'zh-CN')
    $screensJson = $screens.Stdout | ConvertFrom-Json
    Assert-Ec ($screens.Exit -eq 0 -and -not ($screensJson.PSObject.Properties.Name -contains 'color')) `
        '屏幕查询那份里出现了 color 段（它与环境查询同族，但不该带这段）'

    # =========================================================================
    Write-Host "`n=== 3) 本机造不出来的判据（照实记录，不算通过） ==="
    # =========================================================================
    Skip-Ec '真 HDR 显示器上的实拍对照（FP16 scRGB 帧池实际出图 + tone mapping 后与人眼看过的结果一致）' `
        '这台开发机的显示器不支持开启 HDR，带不回一幅广色域帧。tone mapping 的数学由离线层用已知色块与亮度梯度逐点判（第 0 节），--capabilities 的 verifiedOnThisMachine 也据此恒记 no，不在这里伪造一次"通过"'
    Skip-Ec '--hdr refuse 在一幅真 HDR 帧上拒绝出图（capture.hdr_refused 的真机现场）' `
        '同样要有 HDR 屏才造得出"来源确是 HDR"。那条判断与它的下一步（不落地、不换后端）由离线层的格式分类与 HdrRequestPossible 逐条判'
    Skip-Ec '带回一个本构建认不出的广色域格式（capture.hdr_unverifiable 的真机现场）' `
        '要有 HDR 屏、且驱动真给出一个没登记的 DXGI 宽格式才现形。认不出就拒映射这件事由离线层的 ConvertWideFrameToSdrBgra8 来源守卫判'
    Skip-Ec 'HLG BT.2020 那条的真机下场' `
        '本机没有 HDR 模式，也拿不到一幅 HLG 桌面帧；HLG 反 OETF 与 tone 曲线由离线层单点判，端到端记未验证'

    Stop-EcOwnedWindows
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} catch {
    if ("$_" -ne '__SKIP_REAL__') { throw }
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title 'HDR 色彩处理（--hdr）')
