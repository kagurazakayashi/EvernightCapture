<#
.SYNOPSIS
    光标包含与排除（--cursor）的判据：离线层 + 真机层。
.DESCRIPTION
    三层各判各的，都不靠"把光标真的摆到窗口上"这种没法安排的现场：

      0) 离线层（build\ecapture-cursor-tests.exe）：判据本体——按路径登记的能力表、
         两份表的一致性、通道链按光标要求收窄（含 19041 那道门槛两侧）、版本问不出来时
         只按结构筛、以及结果里 requested/effective/basis 的合成。
      1) 源码级守卫：src/ 里出现任何"把指针形状合成进帧"或"事后抹掉光标"的 API 就红。
         这条判的是本工具的**做法**（不修补图像），不是某个系统的行为。
      2) 真机层：只用自建窗口，走 --yes 免掉的窗口内容那一级，一次都不弹框。
         判的是"那个开关真的被设进去、也真的读回来了"（API 层面的回执），以及画面本身
         仍然是本次那扇窗口（尺寸 + 签名色 + 颜色种数），即设置没有把取图弄坏。

    刻意不在本机伪造的现场（一律记未验证，见第 7 节）：像素级"图里看得见/看不见指针"
    （那要把光标停在目标窗口上，等于动使用者的鼠标）、桌面那两条通道要人点头的实截、
    低于 19041 的系统上开关问不到时的真机下场。
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
$stateExe = Join-Path $root 'build\ecapture-cursor-tests.exe'

$run = New-EcRunDir -Tag 'cursor'
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

# 一次截图。--yes / --cursor 都不预置：授权与光标这两件事每条判据自己写明要什么。
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
        NoteItems = if ($json -and $json.notes) { @($json.notes) } else { @() }
        Img = if ($json -and $json.images -and @($json.images).Count) { @($json.images)[0] } else { $null }
        Raw = $r
    }
}

# 那三个键在不在、各是什么，写成一份好读的对照（缺席也是一种答案）。
function Get-CursorFields($Img) {
    if (-not $Img) { return $null }
    $names = @($Img.PSObject.Properties.Name)
    [pscustomobject]@{
        HasRequested = $names -contains 'cursorRequested'
        HasEffective = $names -contains 'cursorEffective'
        HasBasis = $names -contains 'cursorBasis'
        Requested = $Img.cursorRequested
        Effective = $Img.cursorEffective
        Basis = $Img.cursorBasis
    }
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：光标能力表、链收窄与三键合成（测生产函数本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '光标的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-cursor-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "光标判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "光标判据有失败项：$tail"
        # 这条下限是防"判据被删了还绿"：登记表两份一致性、链收窄、三键合成、解析层直接调用
        # 那四大批加起来本就有 80 条以上。
        Assert-Ec ([int]$m.Groups[1].Value -ge 80) "光标判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    # =========================================================================
    Write-Host "`n=== 1) 源码级守卫：本工具不合成指针形状，也不抹除光标 ==="
    # =========================================================================
    # "排除"这件事在各条路径上说的都是**来源**（那张登记表），不是"把已经画进去的东西修掉"。
    # 桌面复制那条的指针形状本来是独立元数据（GetFramePointerShape），取了就得自己画进帧里——
    # 那正是这次明确不要的做法，所以这里用源码级判据钉住：出现任何一个就红。
    # 判的是**调用形状**（`->Name(` / `Name(`），不是名字本身：源码与文档里要写清"我们不做哪几件事"，
    # 那些说明文字提到 API 名字是应该的，这里不能把它判成违规。
    $forbidden = @(
        @{ Name = '取指针形状元数据（要自己画进帧里才用得上）'; Pat = '->\s*GetFramePointerShape\s*\(' },
        @{ Name = '取指针形状列表'; Pat = '->\s*GetPointerShapes?\s*\(' },
        @{ Name = '把光标画进 DC / 帧里'; Pat = '(?<![A-Za-z_])DrawIcon(Ex)?\s*\(' },
        @{ Name = '为了截图去动使用者的鼠标'; Pat = '(?<![A-Za-z_])Set(Physical)?CursorPos\s*\(' }
    )
    $srcFiles = Get-ChildItem -LiteralPath (Join-Path $root 'src') -File |
        Where-Object { $_.Extension -in '.cpp', '.h' }
    Assert-Ec ($srcFiles.Count -ge 30) "src 只扫到 $($srcFiles.Count) 个文件，这一节的判据没意义"
    $hits = @()
    foreach ($f in $srcFiles) {
        $text = [IO.File]::ReadAllText($f.FullName)
        foreach ($b in $forbidden) {
            if ($text -match $b.Pat) { $hits += ("{0}:{1}" -f $f.Name, $b.Name) }
        }
    }
    Assert-Ec ($hits.Count -eq 0) ("源码里出现了不该有的指针修补/合成调用：{0}" -f ($hits -join ', '))
    Write-Host '  src/ 里没有任何指针形状合成或抹除光标的调用' -ForegroundColor DarkGray

    # 唯一那条真设进去的开关必须真的在 CaptureWgc 里被用到（否则"支持则实际设置"就落空了）。
    $wgcText = [IO.File]::ReadAllText((Join-Path $root 'src\CaptureWgc.cpp'))
    Assert-Ec ($wgcText -match 'put_IsCursorCaptureEnabled' -and
               $wgcText -match 'get_IsCursorCaptureEnabled') `
        'CaptureWgc.cpp 里没有真的设进去再读回那个开关（put/get IsCursorCaptureEnabled）'
    # 而且排在 StartCapture 之前：开始采集之后再设，这一帧要不要画已经定了。
    $idxPut = $wgcText.IndexOf('put_IsCursorCaptureEnabled')
    $idxStart = $wgcText.IndexOf('session.StartCapture()')
    Assert-Ec ($idxPut -ge 0 -and $idxStart -ge 0 -and $idxPut -lt $idxStart) `
        '光标开关的设置没有排在 StartCapture 之前（那等于对已经开拍的那一帧说话）'

    if ($SkipReal) {
        Skip-Ec '真机光标设置、读回与画面判据' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'   # 只跑离线层时直接进 finally 收尾
    }

    # =========================================================================
    Write-Host "`n=== 2) 真机 wgc：明确要求时真的设进去、也真的读回来 ==="
    # =========================================================================
    $class = "ec-cursor-$tag"
    $title = "光标测试窗口 $class"
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '200,200,700,500' -Seed 11
    Write-Host ("目标窗口 PID={0} HWND={1} 类={2}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd), $class)
    $mine = New-EcSignatureKey -Seed 11
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
    Assert-Ec ($wW -eq 500 -and $wH -eq 300) "窗口矩形不是 500x300：${wW}x${wH}"

    # 三种要求各截一张：--yes 走窗口内容那一级（本来就不该弹框，第 6 节另判"框真没弹"）。
    $cases = @(
        @{ Name = 'exclude'; Extra = @('--capture', 'wgc', '--cursor', 'exclude', '--yes');
           WantMode = 'exclude'; WantEffective = 'exclude'; WantBasis = 'wgc_session_property_set' },
        @{ Name = 'include'; Extra = @('--capture', 'wgc', '--cursor', 'include', '--yes');
           WantMode = 'include'; WantEffective = 'include'; WantBasis = 'wgc_session_property_set' },
        @{ Name = 'default'; Extra = @('--capture', 'wgc', '--cursor', 'default', '--yes');
           WantMode = 'default'; WantEffective = $null; WantBasis = 'wgc_session_property_read' }
    )
    $defaultReadBack = $null
    foreach ($c in $cases) {
        $r = Invoke-Shot -Window $window -Name ("wgc-{0}" -f $c.Name) -Extra $c.Extra
        Assert-Ec ($r.Exit -eq 0) "$($c.Name)：退出码 $($r.Exit)，应为 0：$($r.Raw.Stderr)"
        Assert-Ec ($r.Exists) "$($c.Name)：没落地"
        Assert-Ec ($r.Img -and $r.Img.path -eq 'wgc') "$($c.Name)：来路该是 wgc，实际 $($r.Img.path)"
        Assert-Ec ($r.Img.scope -eq 'window') "$($c.Name)：scope 该是 window，实际 $($r.Img.scope)"
        Assert-Ec ($r.Codes.Count -eq 0) "$($c.Name)：不该有错误：$($r.Codes -join ',')"

        $f = Get-CursorFields $r.Img
        Assert-Ec ($f.HasRequested -and $f.HasEffective -and $f.HasBasis) `
            "$($c.Name)：三个键没齐（requested=$($f.HasRequested) effective=$($f.HasEffective) basis=$($f.HasBasis)）"
        Assert-Ec ($f.Requested -eq $c.WantMode) "$($c.Name)：cursorRequested 是 $($f.Requested)，应为 $($c.WantMode)"
        Assert-Ec ($f.Basis -eq $c.WantBasis) "$($c.Name)：cursorBasis 是 $($f.Basis)，应为 $($c.WantBasis)"
        if ($null -ne $c.WantEffective) {
            Assert-Ec ($f.Effective -eq $c.WantEffective) `
                "$($c.Name)：cursorEffective 是 $($f.Effective)，应为 $($c.WantEffective)（设过之后读回来不是那一件事时这一张根本不该落地）"
        } else {
            # default 那一路读回来的是什么就报什么，绝不折成任何一种；本机那个默认值记在输出里。
            $defaultReadBack = $f.Effective
            Assert-Ec ($f.Effective -in @('include', 'exclude', 'unverified')) `
                "default：effective 写的是 $($f.Effective)，只能是这三个机器名之一"
        }

        # 设置这件事不能把取图弄坏：尺寸跟着窗口、画面确实是本次那扇窗口。
        $png = Get-PngSize -Path $r.Path
        Assert-Ec ($png -and $png.Width -eq $wW -and $png.Height -eq $wH) `
            "$($c.Name)：PNG $($png.Width)x$($png.Height) 与窗口 ${wW}x${wH} 不一致"
        Assert-Ec ($r.Img.width -eq $wW -and $r.Img.height -eq $wH) `
            "$($c.Name)：JSON 报告 $($r.Img.width)x$($r.Img.height) 与窗口不一致"
        $stats = Get-EcImageStats -Path $r.Path -Step 3
        Assert-Ec ($stats.Colors -ge 12) "$($c.Name)：画面只有 $($stats.Colors) 种颜色，可能是空帧"
        $d = Get-EcColorDistance -A $stats.TopDominant -B $mine
        Assert-Ec ($d -le 32) "$($c.Name)：主色 $($stats.TopDominant) 离签名色 $mine 太远（$d），这一帧不是本窗口的"
        Write-Host ("  {0}: effective={1} basis={2} 尺寸={3}x{4}" -f $c.Name, $f.Effective, $f.Basis,
                   $png.Width, $png.Height) -ForegroundColor DarkGray
    }
    Write-Host ("  本机这条会话的默认光标状态（--cursor default 读回来的）：{0}" -f $defaultReadBack) `
        -ForegroundColor DarkGray

    # =========================================================================
    Write-Host "`n=== 3) 没写 --cursor：结果里那三个键一个都不出现（兼容） ==="
    # =========================================================================
    $plain = Invoke-Shot -Window $window -Name 'no-cursor' -Extra @('--capture', 'wgc', '--yes')
    Assert-Ec ($plain.Exit -eq 0 -and $plain.Exists) "基线截图失败：$($plain.Raw.Stderr)"
    $pf = Get-CursorFields $plain.Img
    Assert-Ec (-not $pf.HasRequested -and -not $pf.HasEffective -and -not $pf.HasBasis) `
        "没写 --cursor 时结果里出现了光标那三个键（requested=$($pf.Requested)），默认值不该改变输出形状"
    # 画面判据与基线一致：这条选项没改动窗口内容那条路的任何东西。
    $pstats = Get-EcImageStats -Path $plain.Path -Step 3
    Assert-Ec ($pstats.Colors -ge 12) "基线画面只有 $($pstats.Colors) 种颜色"
    Assert-Ec ((Get-EcColorDistance -A $pstats.TopDominant -B $mine) -le 32) '基线主色不是本次窗口的'

    # --quiet 只去掉 notes，那三个键是内容判据，不许被抑制（与 path/scope/cropRect 同一条规矩）。
    $quiet = Invoke-Shot -Window $window -Name 'quiet-exclude' -Extra @(
        '--capture', 'wgc', '--cursor', 'exclude', '--yes', '--quiet')
    Assert-Ec ($quiet.Exit -eq 0) "--quiet 那一路退出码 $($quiet.Exit)"
    Assert-Ec ($quiet.Json.PSObject.Properties.Name -notcontains 'notes') '--quiet 没去掉 notes'
    $qf = Get-CursorFields $quiet.Img
    Assert-Ec ($qf.HasRequested -and $qf.HasEffective -and $qf.HasBasis) `
        '--quiet 把光标那三个键也藏了（它是内容判据，不是提示）'
    Assert-Ec ($qf.Requested -eq 'exclude' -and $qf.Basis -eq 'wgc_session_property_set') `
        "--quiet 那一路的三个键内容与不抑制时不同：$($qf.Requested)/$($qf.Effective)/$($qf.Basis)"

    # =========================================================================
    Write-Host "`n=== 4) auto + include：链真的收窄，画面仍出自兑现得了的那一条 ==="
    # =========================================================================
    $auto = Invoke-Shot -Window $window -Name 'auto-include' -Extra @(
        '--capture', 'auto', '--cursor', 'include', '--yes', '--verbose')
    Assert-Ec ($auto.Exit -eq 0) "auto + include 退出码 $($auto.Exit)：$($auto.Raw.Stderr)"
    Assert-Ec ($auto.Img.source -eq 'wgc') "auto + include 实际出图那条是 $($auto.Img.source)，只可能是 wgc"
    Assert-Ec ($auto.Img.path -eq 'wgc') "来路写成 $($auto.Img.path)，说明走了一条没有光标开关的路径"
    $chain = @($auto.Json.input.captureChain)
    Assert-Ec ((($chain) -join ',') -eq 'wgc') "-v 回显的 captureChain 是 '$($chain -join ',')'，收窄后应只剩 wgc"
    $skips = @($auto.NoteItems | Where-Object { $_.code -eq 'note.cursor_channel_skipped' })
    Assert-Ec ($skips.Count -eq 3) "摘掉三条通道应有三条 note，实际 $($skips.Count)"
    $skippedBackends = @($skips | ForEach-Object { $_.backend }) | Sort-Object
    Assert-Ec ((($skippedBackends) -join ',') -eq 'bitblt,dwm,printwindow' `
        -or (($skippedBackends) -join ',') -like '*bitblt*') `
        "note 里列的被摘除通道不对劲：$($skippedBackends -join ',')"
    # 收窄不是"换了一条高风险后端"：这一张的像素来源仍然是窗口内容（--yes 管得着的那一级）。
    Assert-Ec ($auto.Img.scope -eq 'window') "scope 是 $($auto.Img.scope)，要求光标不该把窗口内容路径换成桌面路径"
    $af = Get-CursorFields $auto.Img
    Assert-Ec ($af.Requested -eq 'include' -and $af.Effective -eq 'include') `
        "auto + include 那一路的三键不对劲：$($af.Requested)/$($af.Effective)/$($af.Basis)"

    # exclude 不收窄链（那几条本来就没有光标），但结果里 basis 要说清是哪一件事。
    $autoEx = Invoke-Shot -Window $window -Name 'auto-exclude' -Extra @(
        '--capture', 'auto', '--cursor', 'exclude', '--yes', '--verbose')
    Assert-Ec ($autoEx.Exit -eq 0) "auto + exclude 退出码 $($autoEx.Exit)"
    Assert-Ec ((@($autoEx.Json.input.captureChain) -join ',') -eq 'wgc,dwm,printwindow,bitblt' `
        -and @($autoEx.Json.input.captureChain).Count -eq 4) `
        "exclude 不该收窄链：$(@($autoEx.Json.input.captureChain) -join ',')"
    $aef = Get-CursorFields $autoEx.Img
    Assert-Ec ($aef.Requested -eq 'exclude') "cursorRequested 是 $($aef.Requested)"
    # 实际出图那条是 wgc 时 basis 是"设过"；万一回退到没有开关的那条，basis 必须换成来源那一条，
    # 而不是继续写"设过"。两种都合法，但必须与 source 自相一致。
    if ($autoEx.Img.source -eq 'wgc') {
        Assert-Ec ($aef.Basis -eq 'wgc_session_property_set') "wgc 出图时 basis 是 $($aef.Basis)"
    } else {
        Assert-Ec ($aef.Basis -eq 'path_excludes_cursor') `
            "出图那条是 $($autoEx.Img.source)，basis 却说 $($aef.Basis)（该写来源那件事）"
    }
    Write-Host ("  auto+exclude 实际出图：{0}，basis：{1}" -f $autoEx.Img.source, $aef.Basis) -ForegroundColor DarkGray

    # =========================================================================
    Write-Host "`n=== 5) 做不到的那条：解析期就拒，一个像素都不取、也不弹框 ==="
    # =========================================================================
    foreach ($m in @('printwindow', 'bitblt', 'duplication', 'dwm')) {
        $path = Get-EcRunFile -RunDir $run -Name ("refuse-{0}.png" -f $m)
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $argv = @('--hwnd', (Get-EcHwndHex $window.Hwnd), '--format', 'png', '--lang', 'zh-CN',
                 '--out', $path, '--capture', $m, '--cursor', 'include', '--yes')
        # 只探测不代答：这一条根本不该弹框（连"要不要截"都没问到），所以 ExpectNoDialog。
        $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -TimeoutMs 20000 `
                                  -ExpectNoDialog -DialogWaitMs 2500
        $json = Json-Of $r
        $codes = if ($json -and $json.errors) { @($json.errors | ForEach-Object { $_.code }) } else { @() }
        Assert-Ec ($r.Exit -eq 1) "include 配 $m 退出码 $($r.Exit)，应为 1（参数用法错，不是截图失败）"
        Assert-Ec ($codes -contains 'capture.cursor_unsupported') `
            "include 配 $m 的 code 是 '$($codes -join ',')'，应为 capture.cursor_unsupported"
        Assert-Ec ($codes -notcontains 'capture.cursor_unverifiable') `
            "结构上就做不到的一条报成了无法核实（$($codes -join ',')）"
        Assert-Ec (-not (Test-Path -LiteralPath $path)) "include 配 $m：被拒的这一次居然落地了"
        Assert-Ec (-not $r.Dialog) "include 配 $m：一条注定做不到的请求先去打扰人一次（弹了确认框）"
        if ($json -and @($json.images).Count -gt 0) { Assert-Ec $false "被拒的这一次交回了图片" }
        $e = @($json.errors | Where-Object { $_.code -eq 'capture.cursor_unsupported' })[0]
        Assert-Ec ($e.option -eq '--cursor' -and $e.value -eq 'include') `
            "那条错误的 option/value 不对劲：$($e.option)/$($e.value)"
        Write-Host ("  {0}: exit=1 code=capture.cursor_unsupported 落地={1} 弹框={2}" -f $m,
                   (Test-Path -LiteralPath $path), $r.Dialog) -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 6) 要求光标这件事不改变授权（隐私那条一条都没松） ==="
    # =========================================================================
    # 窗口内容那条：不带 --yes 时照样弹框问人（--cursor 不是第二道免确认的口子）。
    $askWindow = Invoke-EcConsentShot -Exe $Exe -TimeoutMs 30000 -Answer 7 -Arguments @(
        '--hwnd', (Get-EcHwndHex $window.Hwnd), '--format', 'png', '--lang', 'zh-CN',
        '--out', (Get-EcRunFile -RunDir $run -Name 'ask-window.png'),
        '--capture', 'wgc', '--cursor', 'exclude')
    Assert-Ec $askWindow.Dialog 'wgc 不带 --yes 时要弹框问人（只给 --cursor 不该免掉它）'
    Assert-Ec (-not (Test-Path -LiteralPath (Get-EcRunFile -RunDir $run -Name 'ask-window.png'))) `
        '答"否"之后这张图居然落地了'

    # 桌面像素那两条：即使 --cursor exclude（"要的正是没有光标的画面"）、也给了 --yes，
    # 一定弹框，且没点头什么都不落地。这条判的是"排除光标"没被当成降低风险的理由。
    # 这里只探测不代答（AI 不替人点任何一个按钮），所以到点由测试一侧结束那次等待：
    # 判据是"框真弹出来了 + 没落地 + 没返回成功"，"答否"那一条码留给 consent.ps1。
    foreach ($m in @('bitblt', 'duplication')) {
        $path = Get-EcRunFile -RunDir $run -Name ("ask-desktop-{0}.png" -f $m)
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $r = Invoke-EcConsentShot -Exe $Exe -TimeoutMs 9000 -DialogWaitMs 4000 -Arguments @(
            '--hwnd', (Get-EcHwndHex $window.Hwnd), '--format', 'png', '--lang', 'zh-CN',
            '--out', $path, '--capture', $m, '--cursor', 'exclude', '--yes')
        Assert-Ec $r.Dialog "桌面那条通道（$m）带着 --cursor exclude 与 --yes 却没弹框问人"
        Assert-Ec (-not (Test-Path -LiteralPath $path)) "没人点头，$m 这一张居然落地了"
        Assert-Ec ($r.Exit -ne 0) "$m：没经过任何确认就返回了成功"
        Write-Host ("  {0}: 弹框={1} exit={2} 落地={3}" -f $m, $r.Dialog, $r.Exit,
                   (Test-Path -LiteralPath $path)) -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 7) 只读查询不受影响，也不给第二条免确认的口子 ==="
    # =========================================================================
    $cap = Invoke-EcProcess -FilePath $Exe -TimeoutMs 20000 -Arguments @('--capabilities', '--lang', 'zh-CN')
    Assert-Ec ($cap.Exit -eq 0) "--capabilities 退出码 $($cap.Exit)"
    $capJson = $cap.Stdout | ConvertFrom-Json
    $cur = $capJson.cursor
    Assert-Ec ($cur -and $cur.default -eq 'default') "能力报告里没有 cursor 段或默认值不对：$($cur.default)"
    Assert-Ec (@($cur.values) -join ',' -eq 'default,include,exclude' `
        -and @($cur.values).Count -eq 3) "cursor.values 不对劲：$(@($cur.values) -join ',')"
    $paths = @($cur.paths)
    Assert-Ec ($paths.Count -ge 9) "cursor.paths 只列了 $($paths.Count) 条（登记表里应有九条内部路径）"
    $settable = @($paths | Where-Object { $_.capability -eq 'settable' } | ForEach-Object { $_.path })
    Assert-Ec ((($settable | Sort-Object) -join ',') -eq 'screen.wgc,wgc' `
        -or ($settable -contains 'wgc')) "有开关的那两条应是 wgc / screen.wgc，实际：$($settable -join ',')"
    $noInclude = @($paths | Where-Object { $_.capability -eq 'excludes_cursor' -and $_.include -ne 'no' })
    Assert-Ec ($noInclude.Count -eq 0) `
        "没有开关的那几条里有人把 include 说成了做得到：$(($noInclude | ForEach-Object { $_.path }) -join ',')"
    Assert-Ec ($cur.pointerShapeCompositing -eq 'never' -and $cur.pixelRetouching -eq 'never') `
        ('能力报告没写清不合成指针形状也不抹除光标这两条：{0}/{1}' -f $cur.pointerShapeCompositing, $cur.pixelRetouching)
    Assert-Ec (@($capJson.caveats) -contains 'cursor_effective_is_a_setting_not_a_pixel_check') `
        'caveats 里少了那条边界说明（effective 说的是设置与来源，不是像素）'
    # 两份查询同源：--diagnostics 也带同一段（除段落取舍外字段全同）。
    $diag = Invoke-EcProcess -FilePath $Exe -TimeoutMs 20000 -Arguments @('--diagnostics', '--lang', 'zh-CN')
    $diagJson = $diag.Stdout | ConvertFrom-Json
    Assert-Ec (($diagJson.cursor | ConvertTo-Json -Depth 6 -Compress) -eq
               ($cur | ConvertTo-Json -Depth 6 -Compress)) `
        '--capabilities 与 --diagnostics 的 cursor 段不一致（同一份判据不该有两个答案）'
    # 屏幕查询那一份里没有 cursor 段与截图字段：这一条选项没有把它变成"免确认点名一块屏"。
    $screens = Invoke-EcProcess -FilePath $Exe -TimeoutMs 20000 -Arguments @('--screens', '--lang', 'zh-CN')
    $screensJson = $screens.Stdout | ConvertFrom-Json
    Assert-Ec ($screens.Exit -eq 0 -and -not ($screensJson.PSObject.Properties.Name -contains 'cursor')) `
        '屏幕查询那份里出现了 cursor 段（它与环境查询同族，但不该带这段）'

    # =========================================================================
    Write-Host "`n=== 8) 本机造不出来的判据（照实记录，不算通过） ==="
    # =========================================================================
    Skip-Ec '像素级"图里看得见 / 看不见指针"' '那要把光标停在目标窗口上再截（等于动使用者的鼠标），而且本 SDK 的会话接口没有 IsCursorVisible 那个只读属性；这一层只判到 API 回执（设过并读回）与画面自相一致，边界写在 --capabilities 的 caveats 里'
    Skip-Ec '低于 19041 的系统上那个开关问不到（capture.cursor_unverifiable / env.cursor_unsupported 的真机现场）' '这台开发机降级不了，也不该为测试去降级；那条门槛两侧的下场由离线层注入假版本逐条判（tests\cursor_state.cpp 的第 3、4、5 节）'
    Skip-Ec 'duplication 交回的指针形状与桌面帧不同这一现场（真机截一张带光标的桌面）' '它要人在确认框上点头，而测试一侧绝不代答"是"；这里判到的是"本工具从不取那份元数据"（第 1 节源码级守卫）与登记表把它的原因写成 pointer_shape_is_separate_metadata（第 0、7 节）'
    Skip-Ec '远程桌面 / 基本显示驱动会话里屏幕 DC 与桌面复制的光标表现' '本机不是那种会话，也没有第二台机器可试；那条路本来就先被 capture.monitor_changed / cap.dup.* 那一组码挡住，与光标这件事无关'

    Stop-EcOwnedWindows
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} catch {
    if ("$_" -ne '__SKIP_REAL__') { throw }
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '光标包含与排除（--cursor）')
