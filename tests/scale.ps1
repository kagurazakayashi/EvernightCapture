<#
.SYNOPSIS
    截图等比缩小（--scale）的判据：离线层逐条判比例与映射，真机层用自建窗口核对交付尺寸、缩放映射与像素内容，并判"缩放不改变授权"。
.DESCRIPTION
    这条功能最容易做错的三件事，本测试一条一条钉住：

      1) **默认什么都不改**。不带 --scale 的那一次，交付尺寸必须与这条选项存在之前一模一样，而且结果里
         一个 scale 键都不许出现；带了 --scale 但三条天花板都比图宽的那一次（"天花板比图大"），
         也必须原样交付（scaleApplied=false，尺寸不变）——**不放大**是这条选项的第一条语义。
      2) **比例只减不增、顺序是"有效帧 -> 裁剪 -> 缩放 -> 编码"**。真机层拿 --roi 先造出一块横图与一块
         竖图，再配 --scale，判：交付尺寸等于按同一个比例算出来的尺寸；scaleFromWidth/Height 等于
         **裁之后**那张图的尺寸（不是整窗图）——这一条就是顺序的判据本体；裁与缩的映射闭合
         （交付 (0,0) 必须等于"只裁不缩"那张图的 (0,0)）。cropRect / cropScreenRect 不因缩放而改写。
      3) **缩放不改变采集风险等级**。桌面像素那几条即使只要 8 像素宽、即使给了 --yes，照样一定弹框问人：
         这一节**只看不点**（探测这个进程有没有弹出 #32770），所以 AI 不代答任何按钮，也没人需要在场；
         "没点头"用"文件不存在"判。

    判据分两层：
      1) 离线层：build\ecapture-image-tests.exe（源码 tests\image_state.cpp 的 TestScale）——把源尺寸与
         ScaleRequest 直接注进生产判据本体（src/ImageOps.cpp 的 ResolveScale / ScaleFrame）：默认不放大、
         三条天花板取最紧、向下取整、夹到至少 1 像素、夸张到 16384x16384 的面积上限（只做算术不分配）、
         以及最近邻映射逐点对拍、只缩不放、坏形状与带填充的帧。
      2) 真机层：自建 --bordered 窗口（WS_OVERLAPPEDWINDOW），走 --capture wgc 那条窗口内容路径，
         带 --yes 不弹确认框（这正是 --yes 唯一管得着的一级）。横图与竖图都用 --roi 从同一扇窗口里取出来，
         所以"交付尺寸"有一个独立的算式可对（缩之前那张图的尺寸由 --roi 自己定死）。

    本机判不了的两条一律记未验证，不伪造：
      * 单边超过 16384 的帧（"超大输入"的真机现场）—— 造不出一扇这么大的窗口，而且一帧过不了
        capture.frame_invalid 的根本到不了缩放这一层（缩放排在帧形状检查之后），所以"缩放绕不过单边上限"
        这条由离线层逐条判。
      * HDR 与缩放同时生效 —— 这台开发机的显示器不支持开启 HDR（与 hdr.ps1 同一处限制）。
      另外跨屏混合 DPI 在本机也不具备条件（只接了一块屏）。
.EXAMPLE
    .\tests\scale.ps1
    .\tests\scale.ps1 -SkipReal    # 只跑离线判据层
    .\tests\scale.ps1 -SkipState   # 只跑真机层
    .\tests\scale.ps1 -Keep        # 保留截图与临时目录以便人眼看
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
$stateExe = Join-Path $root 'build\ecapture-image-tests.exe'

$run = New-EcRunDir -Tag 'scale'
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

# 任意格式的尺寸（jpg / bmp 没有手写的头解析，用 System.Drawing 读；判的是"文件里真正写下的尺寸"，
# 不是 JSON 自己报的那一份）。
function Get-ImageSize {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    Add-Type -AssemblyName System.Drawing | Out-Null
    $bmp = New-Object System.Drawing.Bitmap($Path)
    try { return [pscustomobject]@{ Width = $bmp.Width; Height = $bmp.Height } }
    finally { $bmp.Dispose() }
}

function Get-PixelAt {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][int]$X,
        [Parameter(Mandatory)][int]$Y
    )
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    Add-Type -AssemblyName System.Drawing | Out-Null
    $bmp = New-Object System.Drawing.Bitmap($Path)
    try {
        if ($X -lt 0 -or $Y -lt 0 -or $X -ge $bmp.Width -or $Y -ge $bmp.Height) { return $null }
        $c = $bmp.GetPixel($X, $Y)
        return '{0},{1},{2}' -f $c.R, $c.G, $c.B
    } finally { $bmp.Dispose() }
}

function Get-ShotPath {
    param([Parameter(Mandatory)][string]$Name, [string]$Format = 'png')
    $ext = if ($Format -eq 'jpeg') { 'jpg' } else { $Format }
    return (Get-EcRunFile -RunDir $run -Name "$Name.$ext")
}

# 一次截图（argv 由调用方拼好，路径已经在里面）。判据对象形状与 crop.ps1 那份一致。
function Invoke-EcShot {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string[]]$Arguments,
        [int]$TimeoutMs = 60000
    )
    Remove-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs $TimeoutMs -Arguments $Arguments
    $json = Json-Of $r
    [pscustomobject]@{
        Exit = $r.Exit; Path = $Path; Json = $json; Exists = Test-Path -LiteralPath $Path
        Codes = if ($json -and $json.errors) { @($json.errors | ForEach-Object { $_.code }) } else { @() }
        Notes = if ($json -and $json.notes) { @($json.notes | ForEach-Object { $_.code }) } else { @() }
        Img = if ($json -and $json.images -and @($json.images).Count) { @($json.images)[0] } else { $null }
        Raw = $r
    }
}

# 窗口内容路径上的一次截图（--yes 管得着的那一级）。
function Invoke-WindowShot {
    param(
        [Parameter(Mandatory)]$Window,
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string[]]$Extra,
        [string]$Format = 'png',
        [int]$TimeoutMs = 60000
    )
    $path = Get-ShotPath -Name $Name -Format $Format
    $argv = @('--hwnd', (Get-EcHwndHex $Window.Hwnd), '--format', $Format, '--lang', 'zh-CN',
             '--out', $path) + @($Extra)
    return (Invoke-EcShot -Path $path -Arguments $argv -TimeoutMs $TimeoutMs)
}

# 一条图里该不该有 scale 那组键（只有写过 --scale 的才有）。
function Assert-ScaleKeysAbsent($Img, [string]$Who) {
    foreach ($k in @('scaleMethod', 'scaleApplied', 'scaleFromWidth', 'scaleFromHeight')) {
        Assert-Ec (-not $Img.PSObject.Properties.Name.Contains($k)) "$Who 结果里不该出现 $k"
    }
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：比例与映射判据（测生产函数本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '等比缩小的离线判据（ResolveScale / ScaleFrame）' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-image-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "离线判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "离线判据有失败项：$tail"
        Assert-Ec ([int]$m.Groups[1].Value -ge 180) "离线判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    if ($SkipReal) {
        Skip-Ec '真机：交付尺寸、缩放映射与像素内容（自建窗口）' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'
    }

    # =========================================================================
    Write-Host "`n=== 1) 默认不改变交付尺寸：不带 --scale，与"天花板比图大"两次都要原样 ==="
    # =========================================================================
    $class = "ec-scale-$tag"
    $title = "缩放测试窗口 $class"
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '200,200,700,500' `
                             -Bordered -Seed 5
    Write-Host ("窗口 PID={0} HWND={1}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd))
    Start-Sleep -Milliseconds 400
    $responsive = $false
    foreach ($try in 1..8) {
        if (Test-EcWindowResponsive -Hwnd $window.Hwnd -TimeoutMs 500) { $responsive = $true; break }
        Start-Sleep -Milliseconds 250
    }
    Assert-Ec $responsive '测试窗口的消息线程一直没在响应'

    $ext = Get-EcExtendedFrameBounds -Hwnd $window.Hwnd
    Assert-Ec ($null -ne $ext) 'DWMWA_EXTENDED_FRAME_BOUNDS 问不到，这一节没有参照物'
    $full = Invoke-WindowShot -Window $window -Name 'full' -Extra @('--capture', 'wgc', '--yes')
    Assert-Ec ($full.Exit -eq 0) "整窗截图失败（exit=$($full.Exit)）：$($full.Codes -join ',')"
    $fullW = $full.Img.width; $fullH = $full.Img.height
    Assert-Ec ($fullW -eq ($ext.Right - $ext.Left) -and $fullH -eq ($ext.Bottom - $ext.Top)) `
        ("交付的整窗尺寸不等于独立量到的可见矩形：图 {0}×{1}，可见矩形 {2}×{3}" -f `
         $fullW, $fullH, ($ext.Right - $ext.Left), ($ext.Bottom - $ext.Top))
    Assert-ScaleKeysAbsent $full.Img '没写 --scale 的那一次'
    $fullPng = Get-PngSize -Path $full.Path
    Assert-Ec ($fullPng -and $fullPng.Width -eq $fullW -and $fullPng.Height -eq $fullH) `
        "写出来的 PNG 尺寸与 JSON 不一致：$($fullPng.Width)×$($fullPng.Height)"

    # 天花板全都比图大：这是"不放大"那条语义的真机现场（尺寸与内容都必须原样）
    $above = Invoke-WindowShot -Window $window -Name 'ceiling-above' -Extra @(
        '--capture', 'wgc', '--yes',
        '--scale', 'max-width=16000,max-height=16000,max-pixels=256000000')
    Assert-Ec ($above.Exit -eq 0) "天花板比图大的那次截图失败（exit=$($above.Exit)）：$($above.Codes -join ',')"
    Assert-Ec ($above.Img.scaleApplied -eq $false) '天花板比图大时 scaleApplied 必须是 false（不放大）'
    Assert-Ec ($above.Img.scaleMethod -eq 'nearest') "scaleMethod 应该写 nearest：$($above.Img.scaleMethod)"
    Assert-Ec ($above.Img.width -eq $fullW -and $above.Img.height -eq $fullH) `
        ("天花板比图大时交付尺寸不该变：{0}×{1} 对 {2}×{3}" -f $above.Img.width, $above.Img.height, $fullW, $fullH)
    Assert-Ec ($above.Img.scaleFromWidth -eq $fullW -and $above.Img.scaleFromHeight -eq $fullH) `
        'scaleFrom* 应该说"缩之前"就是这张整窗图'
    $abovePng = Get-PngSize -Path $above.Path
    Assert-Ec ($abovePng -and $abovePng.Width -eq $fullW -and $abovePng.Height -eq $fullH) `
        '天花板比图大时写出来的 PNG 尺寸不该变'
    # 像素也没被搬走：同几个采样点必须与不缩放那次一致（"尺寸对但内容错位"这种假成功只有逐点比才挡得住）
    foreach ($pt in @(@(0, 0), @([int]($fullW / 2), [int]($fullH / 2)), @(($fullW - 1), ($fullH - 1)))) {
        $a = Get-PixelAt -Path $above.Path -X $pt[0] -Y $pt[1]
        $b = Get-PixelAt -Path $full.Path -X $pt[0] -Y $pt[1]
        Assert-Ec ($a -and $b -and $a -eq $b) "天花板比图大时 ($($pt[0]),$($pt[1])) 的像素被改动了：$a vs $b"
    }

    # =========================================================================
    Write-Host "`n=== 2) 横图与竖图：比例只减不增，scaleFrom* 是"裁之后"那张图 ==="
    # =========================================================================
    # 横块 200x60 从 (30,30) 起：限宽 100 -> 交付 100x30（高按同一个比例 0.5）
    $hw = 200; $hh = 60; $hx = 30; $hy = 30
    $wide = Invoke-WindowShot -Window $window -Name 'wide' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "$hx,$hy,$hw,$hh", '--scale', 'max-width=100')
    Assert-Ec ($wide.Exit -eq 0) "横图缩放失败（exit=$($wide.Exit)）：$($wide.Codes -join ',')"
    $wi = $wide.Img
    Assert-Ec ($wi.width -eq 100 -and $wi.height -eq 30) `
        ("横图 200x60 限宽 100 应该交 100x30，实际 {0}×{1}" -f $wi.width, $wi.height)
    Assert-Ec ($wi.scaleApplied -eq $true) '真的缩小了 scaleApplied 该是 true'
    Assert-Ec ($wi.scaleMethod -eq 'nearest') '插值策略只该有 nearest 一种'
    Assert-Ec ($wi.scaleFromWidth -eq $hw -and $wi.scaleFromHeight -eq $hh) `
        ("scaleFrom* 必须是裁之后那张图的尺寸（{0}x{1}），实际 {2}×{3}" -f `
         $hw, $hh, $wi.scaleFromWidth, $wi.scaleFromHeight)
    Assert-Ec ($wi.cropMode -eq 'roi' -and $wi.cropRect.x -eq $hx -and $wi.cropRect.y -eq $hy -and
               $wi.cropRect.width -eq $hw -and $wi.cropRect.height -eq $hh) `
        "缩放不该改写 cropRect：$($wi.cropRect | ConvertTo-Json -Compress)"
    Assert-Ec ($null -ne $wi.cropScreenRect -and $wi.cropScreenRect.x -eq ($ext.Left + $hx) -and
               $wi.cropScreenRect.y -eq ($ext.Top + $hy)) `
        ("缩放不该改写坐标映射：cropScreenRect={0}，期望 {1},{2}" -f `
         ($wi.cropScreenRect | ConvertTo-Json -Compress), ($ext.Left + $hx), ($ext.Top + $hy))
    $widePng = Get-PngSize -Path $wide.Path
    Assert-Ec ($widePng -and $widePng.Width -eq 100 -and $widePng.Height -eq 30) `
        "写出来的 PNG 尺寸与 JSON 不一致：$($widePng.Width)×$($widePng.Height)"

    # 竖块 60x200 从 (30,20) 起：限高 100 -> 交付 30x100（宽按同一个比例 0.5）
    $tw = 60; $th = 200; $tx = 30; $ty = 20
    $tall = Invoke-WindowShot -Window $window -Name 'tall' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "$tx,$ty,$tw,$th", '--scale', 'max-height=100')
    Assert-Ec ($tall.Exit -eq 0) "竖图缩放失败（exit=$($tall.Exit)）：$($tall.Codes -join ',')"
    $ti = $tall.Img
    Assert-Ec ($ti.width -eq 30 -and $ti.height -eq 100) `
        ("竖图 60x200 限高 100 应该交 30x100，实际 {0}×{1}" -f $ti.width, $ti.height)
    Assert-Ec ($ti.scaleFromWidth -eq $tw -and $ti.scaleFromHeight -eq $th) `
        "竖图那条的 scaleFrom* 也要是裁之后的尺寸"
    $tallPng = Get-PngSize -Path $tall.Path
    Assert-Ec ($tallPng -and $tallPng.Width -eq 30 -and $tallPng.Height -eq 100) `
        "竖图写出来的 PNG 尺寸与 JSON 不一致：$($tallPng.Width)×$($tallPng.Height)"

    # =========================================================================
    Write-Host "`n=== 3) 单边限制、面积天花板与比例舍入（与 --roi 组合，映射闭合） ==="
    # =========================================================================
    # 只裁剪、不缩放的一张：作为映射对照物（两次取的是同一块，内容应当一致）
    $roiOnly = Invoke-WindowShot -Window $window -Name 'roi-only' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "$hx,$hy,$hw,$hh")
    Assert-Ec ($roiOnly.Exit -eq 0 -and $roiOnly.Img.width -eq $hw -and $roiOnly.Img.height -eq $hh) `
        "--roi 不缩放那次没拿到 200x60：exit=$($roiOnly.Exit)"
    Assert-ScaleKeysAbsent $roiOnly.Img '只裁剪不缩放的那一次'

    # 最近邻映射：(0,0)->(0,0)，(99,29)->(floor(99*200/100)=198, floor(29*60/30)=58)
    foreach ($pair in @(@(@(0, 0), @(0, 0)), @(@(1, 1), @(2, 2)), @(@(99, 29), @(198, 58)))) {
        $a = Get-PixelAt -Path $wide.Path -X $pair[0][0] -Y $pair[0][1]
        $b = Get-PixelAt -Path $roiOnly.Path -X $pair[1][0] -Y $pair[1][1]
        Assert-Ec ($a -and $b -and $a -eq $b) `
            ("缩放图的 ({0},{1}) 应该等于原图 ({2},{3}) 的像素：{4} vs {5}" -f `
             $pair[0][0], $pair[0][1], $pair[1][0], $pair[1][1], $a, $b)
    }

    # 面积天花板：同一块 200x60 = 12000 像素，限 3000 -> 比例 0.5 -> 100x30（与限宽 100 同解）
    $byPixels = Invoke-WindowShot -Window $window -Name 'by-pixels' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "$hx,$hy,$hw,$hh", '--scale', 'max-pixels=3000')
    Assert-Ec ($byPixels.Exit -eq 0) "面积天花板那次失败（exit=$($byPixels.Exit)）：$($byPixels.Codes -join ',')"
    Assert-Ec ($byPixels.Img.width -eq 100 -and $byPixels.Img.height -eq 30) `
        ("200x60 限 3000 像素应该交 100x30，实际 {0}×{1}" -f $byPixels.Img.width, $byPixels.Img.height)
    Assert-Ec (($byPixels.Img.width * $byPixels.Img.height) -le 3000) '缩完之后的总像素数越过了天花板'

    # 比例舍入（向下取整）：100x33 限宽 33 -> 33x10（floor(33*33/100) = 10，不是 11）
    $round = Invoke-WindowShot -Window $window -Name 'round' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', '10,10,100,33', '--scale', 'max-width=33')
    Assert-Ec ($round.Exit -eq 0) "比例舍入那次失败：$($round.Codes -join ',')"
    Assert-Ec ($round.Img.width -eq 33 -and $round.Img.height -eq 10) `
        ("100x33 限宽 33 应该向下取整成 33x10，实际 {0}×{1}" -f $round.Img.width, $round.Img.height)

    # 三条天花板一起给：最紧的那一条说了算（限高 20 最紧：100x33 -> 60x20）
    $three = Invoke-WindowShot -Window $window -Name 'three' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', '10,10,100,33',
        '--scale', 'max-width=90,max-height=20,max-pixels=100000')
    Assert-Ec ($three.Exit -eq 0) "三条天花板那次失败：$($three.Codes -join ',')"
    Assert-Ec ($three.Img.width -eq 60 -and $three.Img.height -eq 20) `
        ("100x33 同时限宽 90 / 限高 20 应该交 60x20，实际 {0}×{1}" -f $three.Img.width, $three.Img.height)
    Assert-Ec (($three.Img.width * $three.Img.height) -le 100000) '三条一起给时越过了面积天花板'

    # =========================================================================
    Write-Host "`n=== 4) 极小图：3x2 缩到 1x1（再缩也要留 1 像素） ==="
    # =========================================================================
    $tinyOnly = Invoke-WindowShot -Window $window -Name 'tiny-only' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', '5,5,3,2')
    Assert-Ec ($tinyOnly.Exit -eq 0 -and $tinyOnly.Img.width -eq 3 -and $tinyOnly.Img.height -eq 2) `
        "3x2 的 --roi 没成：exit=$($tinyOnly.Exit)"
    $tiny = Invoke-WindowShot -Window $window -Name 'tiny' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', '5,5,3,2', '--scale', 'max-width=1')
    Assert-Ec ($tiny.Exit -eq 0) "极小图缩放失败（exit=$($tiny.Exit)）：$($tiny.Codes -join ',')"
    Assert-Ec ($tiny.Img.width -eq 1 -and $tiny.Img.height -eq 1) `
        ("3x2 限宽 1 应该交 1x1，实际 {0}×{1}" -f $tiny.Img.width, $tiny.Img.height)
    Assert-Ec ($tiny.Img.scaleFromWidth -eq 3 -and $tiny.Img.scaleFromHeight -eq 2) `
        '极小图的 scaleFrom* 也要写对'
    $tinyPng = Get-PngSize -Path $tiny.Path
    Assert-Ec ($tinyPng -and $tinyPng.Width -eq 1 -and $tinyPng.Height -eq 1) `
        "1x1 那张 PNG 的 IHDR 不对：$($tinyPng.Width)×$($tinyPng.Height)"
    $t0 = Get-PixelAt -Path $tiny.Path -X 0 -Y 0
    $o0 = Get-PixelAt -Path $tinyOnly.Path -X 0 -Y 0
    Assert-Ec ($t0 -and $o0 -and $t0 -eq $o0) "1x1 那个像素应该取自原图 (0,0)：$t0 vs $o0"
    # 3x1（单像素高）同理：限宽 1 -> 1x1
    $line = Invoke-WindowShot -Window $window -Name 'line' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', '5,5,3,1', '--scale', 'max-width=1')
    Assert-Ec ($line.Exit -eq 0 -and $line.Img.width -eq 1 -and $line.Img.height -eq 1) `
        ("3x1 限宽 1 应该交 1x1（实际 exit={0}，{1}×{2}）" -f $line.Exit, $line.Img.width, $line.Img.height)

    # =========================================================================
    Write-Host "`n=== 5) 与不同编码格式组合：尺寸在文件里真的写下来了 ==="
    # =========================================================================
    foreach ($fmt in @('png', 'bmp', 'jpeg')) {
        $s = Invoke-WindowShot -Window $window -Name "fmt-$fmt" -Format $fmt -Extra @(
            '--capture', 'wgc', '--yes', '--roi', "$hx,$hy,$hw,$hh", '--scale', 'max-width=50')
        Assert-Ec ($s.Exit -eq 0) "$fmt 那次缩放失败（exit=$($s.Exit)）：$($s.Codes -join ',')"
        Assert-Ec ($s.Img.width -eq 50 -and $s.Img.height -eq 15) `
            ("$fmt 那条 200x60 限宽 50 应该交 50x15，实际 {0}×{1}" -f $s.Img.width, $s.Img.height)
        $sz = Get-ImageSize -Path $s.Path
        Assert-Ec ($sz -and $sz.Width -eq 50 -and $sz.Height -eq 15) `
            ("$fmt 文件里写下的尺寸不是 50x15：{0}×{1}" -f $sz.Width, $sz.Height)
        if ($fmt -eq 'png') {
            $hdr = Get-PngSize -Path $s.Path
            Assert-Ec ($hdr -and $hdr.Width -eq 50 -and $hdr.Height -eq 15) 'PNG 的 IHDR 与 JSON 不一致'
        }
    }

    # =========================================================================
    Write-Host "`n=== 6) 缩放不改变采集风险等级：桌面像素 + 极小 --scale 照样一定要问人 ==="
    # =========================================================================
    # 这一节**只看不点**：AI 不代答任何按钮，人也未必在场。判据是"框真弹出来了"，
    # "没人点头所以什么都没落地"用文件不存在来判；进程由测试一侧结束（它会一直等在那里），
    # 所以"答否 / 到点按拒绝"那两条码不在这里断言 —— 那是 consent.ps1 与 timeout.ps1 的活。
    $deskCases = @(
        @{ Name = 'desk-bitblt-scale-roi'; Extra = @('--capture', 'bitblt', '--roi', '5,5,200,60', '--scale', 'max-width=8') },
        @{ Name = 'desk-dup-scale';        Extra = @('--capture', 'duplication', '--scale', 'max-width=8') }
    )
    foreach ($c in $deskCases) {
        $path = Get-ShotPath -Name $c.Name
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $argv = @('--hwnd', (Get-EcHwndHex $window.Hwnd), '--format', 'png', '--lang', 'zh-CN',
                 '--out', $path) + @($c.Extra) + @('--yes')
        $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -TimeoutMs 6000 -DialogWaitMs 3000
        Assert-Ec $r.Dialog ("桌面像素那条即使只要 8 像素宽、也给了 --yes，仍必须弹框问人（$($c.Name)）")
        Assert-Ec (-not (Test-Path -LiteralPath $path)) '没人点头时，这张缩过的图不该落地'
        Assert-Ec ($r.Exit -ne 0) '没人点头时不该返回成功（exit 应为非 0）'
        Write-Host ("  {0}: 弹框={1} exit={2} 落地={3}" -f $c.Name, $r.Dialog, $r.Exit,
                   (Test-Path -LiteralPath $path)) -ForegroundColor DarkGray
    }
    # 对照：同样给 --yes，窗口内容那条路径一次都不弹框就出图（上面那两条不是"谁都弹"）。
    $winOk = Invoke-WindowShot -Window $window -Name 'win-content' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "$hx,$hy,$hw,$hh", '--scale', 'max-width=100')
    Assert-Ec ($winOk.Exit -eq 0 -and $winOk.Img.width -eq 100) `
        "窗口内容路径带 --yes 不该被打扰：exit=$($winOk.Exit)"
    Assert-Ec (-not ($winOk.Codes | Where-Object { $_ -like 'capture.consent*' })) `
        '窗口内容路径带 --yes 时不该出现授权那一条码'

    # =========================================================================
    Write-Host "`n=== 7) 标准流与 --quiet / -v：缩放那几个键是判据，藏不掉 ==="
    # =========================================================================
    $quiet = Invoke-WindowShot -Window $window -Name 'quiet' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "$hx,$hy,$hw,$hh", '--scale', 'max-width=100', '--quiet')
    Assert-Ec ($quiet.Exit -eq 0) "--quiet 那次截图失败：exit=$($quiet.Exit)"
    $qi = $quiet.Img
    Assert-Ec ($qi.scaleApplied -eq $true -and $qi.width -eq 100 -and $qi.height -eq 30 -and
               $qi.scaleFromWidth -eq $hw -and $null -ne $qi.cropScreenRect) `
        '--quiet 也必须留下缩放的定位字段（这几个是判据，不该被省掉）'
    Assert-Ec ($quiet.Notes.Count -eq 0) "--quiet 之后不该留着 notes（实际：$($quiet.Notes -join ',')）"

    $verbose = Invoke-WindowShot -Window $window -Name 'verbose' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "$hx,$hy,$hw,$hh",
        '--scale', 'max-width=120,max-height=40,max-pixels=9999', '--verbose')
    Assert-Ec ($verbose.Exit -eq 0) "-v 那次截图失败：exit=$($verbose.Exit)"
    $sc = $verbose.Json.input.scale
    Assert-Ec ($sc.maxWidth -eq 120 -and $sc.maxHeight -eq 40 -and $sc.maxPixels -eq 9999) `
        "-v 的 input.scale 应当回显规范化后的请求（实际：$($sc | ConvertTo-Json -Compress)）"

    # 省略 --out（= --out -）：图走 stdout，尺寸照样是缩过的那一份
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -Arguments @(
        '--hwnd', (Get-EcHwndHex $window.Hwnd), '--capture', 'wgc', '--yes',
        '--roi', "$hx,$hy,$hw,$hh", '--scale', 'max-width=40', '--format', 'png', '--lang', 'zh-CN')
    Assert-Ec ($r.Exit -eq 0) "省略 --out 时缩放照样能成：exit=$($r.Exit)"
    $pipeJson = $null
    try { $pipeJson = $r.Stderr | ConvertFrom-Json } catch { }
    Assert-Ec ($pipeJson -and @($pipeJson.images).Count -eq 1) '省略 --out 时结果 JSON 应当走约定流（stderr）'
    $pi = @($pipeJson.images)[0]
    Assert-Ec ($pi.width -eq 40 -and $pi.scaleApplied -eq $true) '省略 --out 时那一份元数据也应当带上缩放的定位键'
    $bytes = $r.StdoutBytes
    Assert-Ec ($bytes.Length -ge 24 -and $bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50) `
        '省略 --out 时 stdout 上应当是一张 PNG（缩过的那张）'
    $pipeW = [int]$bytes[16] * 16777216 + [int]$bytes[17] * 65536 + [int]$bytes[18] * 256 + [int]$bytes[19]
    $pipeH = [int]$bytes[20] * 16777216 + [int]$bytes[21] * 65536 + [int]$bytes[22] * 256 + [int]$bytes[23]
    Assert-Ec ($pipeW -eq 40 -and $pipeH -eq 12) "标准输出上那张图的尺寸不是缩过的：$pipeW×$pipeH"

    # =========================================================================
    Write-Host "`n=== 8) 一批里每张都按同一条 --scale 缩（两扇窗口） ==="
    # =========================================================================
    $pairTitle = "缩放成对窗口 ec-pairs-$tag"
    $pair = Start-EcWindow -RunDir $run -Class "ec-scp-$tag" -Title $pairTitle `
                           -Rect '120,520,520,720' -Windows 2 -Seed 6
    Start-Sleep -Milliseconds 400
    $tmpl = Get-EcRunFile -RunDir $run -Name 'batch_%i.png'
    foreach ($k in 1, 2) {
        Remove-Item -LiteralPath (Get-EcRunFile -RunDir $run -Name "batch_$k.png") -ErrorAction SilentlyContinue
    }
    $br = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -Arguments @(
        '--title', $pairTitle, '--all', '--capture', 'wgc', '--yes', '--lang', 'zh-CN',
        '--scale', 'max-width=100', '--out', $tmpl)
    $bjson = Json-Of $br
    Assert-Ec ($br.Exit -eq 0 -and $bjson -and @($bjson.images).Count -eq 2) `
        "两扇窗口各缩一张应该成，实际 exit=$($br.Exit)：$($bjson.errors | ConvertTo-Json -Compress)"
    $n = 0
    foreach ($k in 1, 2) {
        $p = Get-EcRunFile -RunDir $run -Name "batch_$k.png"
        Assert-Ec (Test-Path -LiteralPath $p) "batch_$k.png 应当写出来（实际没有）"
        $sz = if (Test-Path -LiteralPath $p) { Get-PngSize -Path $p } else { $null }
        Assert-Ec ($sz -and $sz.Width -eq 100) "batch_$k.png 的尺寸不是限宽 100 之后的：$($sz.Width)"
        foreach ($im in @($bjson.images)) {
            if ($im.width -eq 100 -and $im.scaleApplied -eq $true -and $im.scaleFromWidth -gt 100) { $n++ }
        }
    }
    Assert-Ec ($n -ge 2) '批里每一张的结果都应当写上"缩过"（至少两张）'

    # =========================================================================
    Write-Host "`n=== 9) 本机造不出来的判据（照实记录，不算通过） ==="
    # =========================================================================
    Skip-Ec '单边超过 16384 的帧（"超大输入"的真机现场）' '造不出一扇这么大的窗口；而且一帧过不了 capture.frame_invalid 的到不了缩放这一层（缩放排在帧形状检查与 --roi 越界判据之后），"缩放绕不过单边上限"由离线层 ResolveScale 逐条判（含 16384x16384 的面积天花板）'
    Skip-Ec 'HDR 与缩放同时生效（广色域帧缩完之后的色彩）' '这台开发机的显示器不支持开启 HDR（与 hdr.ps1 同一处限制）；离线层的缩放只搬像素、不碰色彩，映射与内插不随色彩空间而变'
    Skip-Ec '跨屏混合 DPI 下缩放的交付像素数不变' '这台机器只接了一块屏，没有第二种缩放比的屏可挪；"单位是物理像素、不按 DPI 缩放"由离线层与 --roi 那一条共同钉住（缩放只是从既有像素里取，不做任何 DPI 换算）'
    Skip-Ec '答案 / --consent-timeout-ms 到点按拒绝时缩放那两级都不落地' '第 6 节判到的是"框一定弹、没点头就不落地"；测试一侧不代答任何按钮，那两条码留给 consent.ps1 / timeout.ps1 在有人守着屏幕时判'

    Stop-EcOwnedWindows
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} catch {
    if ("$_" -ne '__SKIP_REAL__') { throw }
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '截图等比缩小（--scale）')
