<#
.SYNOPSIS
    窗口内部裁剪（--roi / --client-area）的判据：离线层逐条注入几何，真机层用自建的带边框窗口核对裁剪矩形、坐标映射与像素内容。
.DESCRIPTION
    这个功能最容易说清楚也最容易做错的四件事，本测试一条一条钉住：

      1) **坐标系**。裁剪矩形说的是交付图像自己的像素坐标（左上角 = (0,0)），不是桌面绝对坐标。
         真机层拿三条独立问答对照：GetWindowRect（授权矩形那一条）、DWMWA_EXTENDED_FRAME_BOUNDS
         （用户看到的、也就是通道交付的那一圈）、GetClientRect + ClientToScreen（客户区那一块）。
         于是 cropRect / cropScreenRect / fullWidth / fullHeight 与最终 width / height 必须同时闭合，
         而不是"尺寸看着像"。
      2) **越界只能拒绝**。不往里挪、不裁到边上为止、也不退回整窗交出；而且取帧之前就看得出
         放不下时根本不弹框、不写文件（match.roi_out_of_range），取到帧之后才发现的也一样不落地
         （capture.roi_invalid）。resize 那条现场就是靠 Set-EcWindowRect 真的把窗口改小来判。
      3) **裁剪不改变授权**。用的是桌面像素的那几条，即使 --roi 只要一小块、即使给了 --yes，
         照样一定要问人：这一节**只看不点**（探测这个进程有没有弹出 #32770），所以 AI 不代答
         任何按钮，也没人需要在场；"没点头"用"文件不存在"判，而"答否 / 到点按拒绝"那两条码
         不在这里断言（见第 8 节的未验证记录）。
      4) **像素内容**。裁剪图的 (0,0) 必须等于整窗图的 (x,y)，右下角同理 —— 尺寸对但取自别处
         这种假成功，只有逐点比对才挡得住。

    判据分两层：
      1) 离线层：build\ecapture-crop-tests.exe（源码 tests\crop_state.cpp）——把交付图像的尺寸、
         它的屏幕矩形问没问到、客户区那块矩形问没问到，直接注进生产判据本体（src/CropGeometry.cpp）：
         贴边、越界一条边、零宽高、相加绕回、上限、图像原点核实不出来、负坐标的屏、
         以及"问不出来"与"放不下"各归哪一条码。
      2) 真机层：自建 --bordered 窗口（WS_OVERLAPPEDWINDOW，标题栏 + 可拖动边框）。
         为什么要带边框：WS_POPUP 那种窗口，窗口矩形、客户区矩形、DWM 可见边框矩形三者重合，
         --client-area 的偏移与 cropRect 的位置在那种窗口上判不出差别（永远是 0,0）。
         走 --capture wgc 那条窗口内容路径，带 --yes 不弹确认框（这正是 --yes 唯一管得着的一级）。

    本机判不了的两条一律记未验证，不伪造：
      * 跨屏混合 DPI（同一扇窗口挪到缩放比不同的另一块屏上）—— 这台机器只接了一块屏。
        真机层只能现场判"单位是物理像素、不按 DPI 缩放"这一条：交付的整窗尺寸必须等于
        独立量到的可见矩形像素数，而同一条 --roi 在那种缩放下取的像素数不变。
      * 目标在"取帧之前那条预检通过之后、帧交回来之前"这一瞬间被改小 —— 要的就是这个竞态
        窗口本身，时间点安排不出来，所以那条由离线层的 ResolveWindowCrop 逐条判。
.EXAMPLE
    .\tests\crop.ps1
    .\tests\crop.ps1 -SkipReal          # 只跑离线判据层
    .\tests\crop.ps1 -SkipState         # 只跑真机层
    .\tests\crop.ps1 -Keep              # 保留截图与临时目录以便人眼看
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
$stateExe = Join-Path $root 'build\ecapture-crop-tests.exe'

$run = New-EcRunDir -Tag 'crop'
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

# 一次截图。--yes 与 --consent-timeout-ms 都不预置：授权那几条判据要自己控制给不给。
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

# 逐点比对：证明"这块像素确实出自那个位置"，而不是只看尺寸对不对。
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

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：裁剪几何判据（测生产函数本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '裁剪几何的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-crop-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "裁剪几何判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "裁剪几何判据有失败项：$tail"
        Assert-Ec ([int]$m.Groups[1].Value -ge 50) "裁剪几何判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    if ($SkipReal) {
        Skip-Ec '真机裁剪矩形、坐标映射与像素内容对照' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'   # 只跑离线层时直接进 finally 收尾
    }

    # =========================================================================
    Write-Host "`n=== 1) --roi：裁剪矩形、坐标映射与像素内容（三条独立问答对照） ==="
    # =========================================================================
    $class = "ec-crop-$tag"
    $title = "裁剪测试窗口 $class"
    # 带边框的窗口：窗口矩形 500×300，客户区与 DWM 可见矩形都在它里面，三者各不相同。
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '200,200,700,500' `
                             -Bordered -Seed 5
    Write-Host ("窗口 PID={0} HWND={1}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd))
    Start-Sleep -Milliseconds 400
    # 消息线程偶尔在画第一帧（探针默认只等 200 ms），所以给几次机会而不是第一次没回话就算失败
    $responsive = $false
    foreach ($try in 1..8) {
        if (Test-EcWindowResponsive -Hwnd $window.Hwnd -TimeoutMs 500) { $responsive = $true; break }
        Start-Sleep -Milliseconds 250
    }
    Assert-Ec $responsive '测试窗口的消息线程一直没在响应'

    # 三条独立的问答：授权矩形 = GetWindowRect；交付的那一圈 = DWM 可见矩形；客户区 = 自己算。
    $wr = Get-EcWindowRect -Hwnd $window.Hwnd
    Assert-Ec ($wr -and ($wr.Right - $wr.Left) -eq 500) "窗口矩形量出来不是 500 宽：$($wr | Out-String)"
    $ext = Get-EcExtendedFrameBounds -Hwnd $window.Hwnd
    Assert-Ec ($null -ne $ext) 'DWMWA_EXTENDED_FRAME_BOUNDS 问不到，这一节的对照没有参照物'
    $cl = Get-EcClientScreenRect -Hwnd $window.Hwnd
    Assert-Ec ($null -ne $cl -and $cl.Width -gt 0 -and $cl.Height -gt 0) '客户区问不到，同上'
    Write-Host ("  独立问答：窗口 {0}×{1} @({2},{3})；可见 {4}×{5} @({6},{7})；客户区 {8}×{9} @({10},{11})" -f `
               ($wr.Right - $wr.Left), ($wr.Bottom - $wr.Top), $wr.Left, $wr.Top,
               ($ext.Right - $ext.Left), ($ext.Bottom - $ext.Top), $ext.Left, $ext.Top,
               $cl.Width, $cl.Height, $cl.Left, $cl.Top) -ForegroundColor DarkGray

    # 整窗一张作为像素参照物（不裁剪）
    $full = Invoke-Shot -Window $window -Name 'full' -Extra @('--capture', 'wgc', '--yes')
    Assert-Ec ($full.Exit -eq 0) "整窗截图失败（exit=$($full.Exit)）：$($full.Codes -join ',')"
    Assert-Ec ($full.Img.width -eq ($ext.Right - $ext.Left) -and
               $full.Img.height -eq ($ext.Bottom - $ext.Top)) `
        ("交付的整窗尺寸不等于独立量到的可见矩形（图 {0}×{1}，可见矩形 {2}×{3}）—— 坐标系判据没有参照物" -f `
         $full.Img.width, $full.Img.height, ($ext.Right - $ext.Left), ($ext.Bottom - $ext.Top))
    Assert-Ec (-not $full.Img.cropMode) '没给裁剪请求时不该出现 cropMode'
    Assert-Ec ($full.Img.rect.x -eq $wr.Left -and $full.Img.rect.width -eq ($wr.Right - $wr.Left)) `
        'images[].rect 不是授权那一条 GetWindowRect（它比可见矩形宽是预期方向，但两处不能打脸）'

    $roiX = 20; $roiY = 40; $roiW = 120; $roiH = 80
    $roi = Invoke-Shot -Window $window -Name 'roi' -Extra @('--capture', 'wgc', '--yes',
                                                            '--roi', "$roiX,$roiY,$roiW,$roiH")
    Assert-Ec ($roi.Exit -eq 0) "--roi 截图失败（exit=$($roi.Exit)）：$($roi.Json.errors | ConvertTo-Json -Compress)"
    $ri = $roi.Img
    Assert-Ec ($ri.cropMode -eq 'roi') "cropMode 应该是 roi，实际 $($ri.cropMode)"
    Assert-Ec ($ri.width -eq $roiW -and $ri.height -eq $roiH) "最终尺寸不是请求的那块：$($ri.width)×$($ri.height)"
    Assert-Ec ($ri.cropRect.x -eq $roiX -and $ri.cropRect.y -eq $roiY -and
               $ri.cropRect.width -eq $roiW -and $ri.cropRect.height -eq $roiH) `
        "cropRect 要原样回显请求：$($ri.cropRect | ConvertTo-Json -Compress)"
    Assert-Ec ($ri.fullWidth -eq $full.Img.width -and $ri.fullHeight -eq $full.Img.height) `
        "fullWidth/fullHeight 应该等于裁之前那张整窗图：$($ri.fullWidth)×$($ri.fullHeight)"
    $pngRoi = Get-PngSize -Path $roi.Path
    Assert-Ec ($pngRoi -and $pngRoi.Width -eq $roiW -and $pngRoi.Height -eq $roiH) `
        "写出来的 PNG 尺寸与 JSON 不一致：$($pngRoi.Width)×$($pngRoi.Height)"
    Assert-Ec ($ri.scope -eq 'window' -and $ri.path -eq 'wgc') `
        "窗口内容路径的来路两项该是 wgc/window：$($ri.path) / $($ri.scope)"

    # 坐标映射：cropScreenRect 必须等于"独立量到的可见矩形原点 + 图像坐标偏移"，
    # 而这一层闭合之后，倒推回来的原点必须正好是那条独立问答的答案。
    Assert-Ec ($null -ne $ri.cropScreenRect) '图像原点本该核实得出来（交付尺寸 == 可见矩形），cropScreenRect 却没写'
    Assert-Ec (-not ($roi.Notes -contains 'note.crop_mapping_unavailable')) `
        '这一条路上不该出现"映射问不出来"的提示'
    Assert-Ec ($ri.cropScreenRect.x -eq ($ext.Left + $roiX) -and
               $ri.cropScreenRect.y -eq ($ext.Top + $roiY)) `
        ("cropScreenRect 不等于可见矩形原点 + 偏移：{0},{1} 对 {2},{3}" -f `
         $ri.cropScreenRect.x, $ri.cropScreenRect.y, ($ext.Left + $roiX), ($ext.Top + $roiY))
    Assert-Ec ($ri.cropScreenRect.x - $ri.cropRect.x -eq $ext.Left -and
               $ri.cropScreenRect.y - $ri.cropRect.y -eq $ext.Top) `
        '映射不自反：由 cropScreenRect 减 cropRect 倒推不出图像原点'
    Assert-Ec ($ri.cropScreenRect.x -ge $wr.Left -and
               ($ri.cropScreenRect.x + $roiW) -le $wr.Right -and
               ($ri.cropScreenRect.y + $roiH) -le $wr.Bottom) `
        '裁剪矩形落到授权矩形（GetWindowRect）之外了 —— 那正是这条功能禁止的事'

    # 像素内容：左上与右下两个对应点必须与整窗图同位同色（只看尺寸会放过"取自别处"）
    $a00 = Get-PixelAt -Path $roi.Path -X 0 -Y 0
    $b00 = Get-PixelAt -Path $full.Path -X $roiX -Y $roiY
    Assert-Ec ($a00 -and $b00 -and $a00 -eq $b00) "裁剪图左上角与整窗图同位不同色：$a00 vs $b00"
    $aEnd = Get-PixelAt -Path $roi.Path -X ($roiW - 1) -Y ($roiH - 1)
    $bEnd = Get-PixelAt -Path $full.Path -X ($roiX + $roiW - 1) -Y ($roiY + $roiH - 1)
    Assert-Ec ($aEnd -and $bEnd -and $aEnd -eq $bEnd) "裁剪图右下角与整窗图同位不同色：$aEnd vs $bEnd"
    $adjacent = Get-PixelAt -Path $full.Path -X ($roiX + $roiW) -Y ($roiY + $roiH - 1)
    Write-Host ("  采样点：裁剪(0,0)={0} 整窗({1},{2})={3}；裁剪右下={4} 整窗右下={5}；图像外一格={6}" -f `
               $a00, $roiX, $roiY, $b00, $aEnd, $bEnd, $adjacent) -ForegroundColor DarkGray

    # =========================================================================
    Write-Host "`n=== 2) --client-area：去掉的就是标题栏与边框那一圈 ==="
    # =========================================================================
    $ca = Invoke-Shot -Window $window -Name 'client' -Extra @('--capture', 'wgc', '--yes', '--client-area')
    Assert-Ec ($ca.Exit -eq 0) "--client-area 截图失败（exit=$($ca.Exit)）：$($ca.Json.errors | ConvertTo-Json -Compress)"
    $ci = $ca.Img
    Assert-Ec ($ci.cropMode -eq 'client-area') "cropMode 应该是 client-area，实际 $($ci.cropMode)"
    Assert-Ec ($ci.width -eq $cl.Width -and $ci.height -eq $cl.Height) `
        ("交付尺寸不等于独立量到的客户区：图 {0}×{1}，客户区 {2}×{3}" -f `
         $ci.width, $ci.height, $cl.Width, $cl.Height)
    Assert-Ec ($null -ne $ci.cropScreenRect -and $ci.cropScreenRect.x -eq $cl.Left -and
               $ci.cropScreenRect.y -eq $cl.Top) `
        ("cropScreenRect 不等于客户区自己的屏幕矩形：{0} vs {1},{2}" -f `
         ($ci.cropScreenRect | ConvertTo-Json -Compress), $cl.Left, $cl.Top)
    Assert-Ec ($ci.cropRect.x -gt 0 -or $ci.cropRect.y -gt 0) `
        'cropRect 的偏移是 (0,0)：等于"客户区与整窗一样大"，边框根本没被去掉'
    Assert-Ec ($ci.cropRect.x -eq ($cl.Left - $ext.Left) -and $ci.cropRect.y -eq ($cl.Top - $ext.Top)) `
        'cropRect 的偏移不等于"客户区原点相对可见矩形原点的偏移"'
    Assert-Ec ($ci.cropRect.width -lt $ci.fullWidth -and $ci.cropRect.height -lt $ci.fullHeight) `
        '--client-area 交回的尺寸与整窗一样大：那一圈边框根本没去掉'
    Assert-Ec ($ci.fullWidth -eq $full.Img.width -and $ci.fullHeight -eq $full.Img.height) `
        'fullWidth/fullHeight 应该还是那张整窗图的尺寸'
    # 像素对照：客户区图的左上角必须等于整窗图在 (cl.Left-ext.Left, cl.Top-ext.Top) 那一个像素
    $offX = $cl.Left - $ext.Left; $offY = $cl.Top - $ext.Top
    $c00 = Get-PixelAt -Path $ca.Path -X 0 -Y 0
    $f00 = Get-PixelAt -Path $full.Path -X $offX -Y $offY
    Assert-Ec ($c00 -and $f00 -and $c00 -eq $f00) "客户区图左上角与整窗图同位不同色：$c00 vs $f00"
    # 边框那一条边界要判到"之外一格"：客户区之上那一行属于标题栏，交付图里不该有它
    $titleRow = Get-PixelAt -Path $full.Path -X $offX -Y ($offY - 1)
    Assert-Ec ($null -ne $titleRow) '整窗图里客户区之上那一行问不到，边框边界这条判据没有参照物'
    Write-Host ("  边框：客户区在图像里从 ({0},{1}) 起；客户区左上像素={2}；它上面一行（标题栏）={3}" -f `
               $offX, $offY, $c00, $titleRow) -ForegroundColor DarkGray

    # =========================================================================
    Write-Host "`n=== 2b) 各条窗口内容通道交回的是同一块裁剪（--yes 都不弹框） ==="
    # =========================================================================
    # 判的是"同一份请求在四条窗口内容路线上给出同一块矩形与同一个映射"，而不是只测 wgc 那一条：
    # 裁剪的判据本来只读帧的尺寸 + 一条独立的窗口矩形问答，所以它不该跟着通道换形状。
    # 这四条都带 --yes：全是窗口内容路径（scope=window），一次框都不该弹。
    foreach ($cap in @('wgc', 'dwm', 'printwindow', 'auto')) {
        $roiShot = Invoke-Shot -Window $window -Name "cap-$cap-roi" -Extra @(
            '--capture', $cap, '--yes', '--roi', '15,25,80,50')
        Assert-Ec ($roiShot.Exit -eq 0) "$cap 那条路裁剪失败：exit=$($roiShot.Exit)，$($roiShot.Codes -join ',')"
        Assert-Ec ($roiShot.Img.width -eq 80 -and $roiShot.Img.height -eq 50 -and
                   $roiShot.Img.cropRect.x -eq 15 -and $roiShot.Img.cropRect.y -eq 25) `
            "$cap 交回的裁剪不是请求的那块：$($roiShot.Img | ConvertTo-Json -Compress)"
        Assert-Ec ($roiShot.Img.scope -eq 'window' -and -not ($roiShot.Img.path -like '*.screen')) `
            "$cap 带 --yes 这条裁剪路径本该一直待在窗口内容那一级：path=$($roiShot.Img.path) scope=$($roiShot.Img.scope)"
        Assert-Ec ($roiShot.Img.fullWidth -eq $full.Img.width -and
                   $roiShot.Img.fullHeight -eq $full.Img.height) `
            ("{0} 交付的整窗尺寸与 wgc 那份不一致（{1}×{2} 对 {3}×{4}）" -f `
             $cap, $roiShot.Img.fullWidth, $roiShot.Img.fullHeight, $full.Img.width, $full.Img.height)
        Assert-Ec ($null -ne $roiShot.Img.cropScreenRect -and
                   $roiShot.Img.cropScreenRect.width -eq 80 -and
                   $roiShot.Img.cropScreenRect.x -ge $roiShot.Img.rect.x) `
            "$cap 的 cropScreenRect 没能与请求对上，或落到授权矩形之外了"
        $caShot = Invoke-Shot -Window $window -Name "cap-$cap-client" -Extra @(
            '--capture', $cap, '--yes', '--client-area')
        Assert-Ec ($caShot.Exit -eq 0) "$cap 那条路 --client-area 失败：exit=$($caShot.Exit)，$($caShot.Codes -join ',')"
        Assert-Ec ($caShot.Img.width -eq $cl.Width -and $caShot.Img.height -eq $cl.Height) `
            ("{0} 交回的客户区尺寸不等于独立量到的那块：{1}×{2} 对 {3}×{4}" -f `
             $cap, $caShot.Img.width, $caShot.Img.height, $cl.Width, $cl.Height)
        Assert-Ec ($null -ne $caShot.Img.cropScreenRect -and $caShot.Img.cropScreenRect.x -eq $cl.Left -and
                   $caShot.Img.cropScreenRect.y -eq $cl.Top) `
            "$cap 的 cropScreenRect 与独立量到的客户区屏幕矩形不一致"
    }

    # =========================================================================
    Write-Host "`n=== 3) 越界与空矩形：拒绝、不落地、而且不去打扰人 ==="
    # =========================================================================
    $imgW = $full.Img.width; $imgH = $full.Img.height
    $over = Invoke-Shot -Window $window -Name 'over' -Extra @('--capture', 'wgc', '--yes',
                                                             '--roi', "0,0,$($imgW + 1),$imgH")
    Assert-Ec ($over.Exit -eq 1) "越界一条像素的 --roi 应该返回 1，实际 $($over.Exit)：$($over.Codes -join ',')"
    Assert-Ec ($over.Codes -contains 'match.roi_out_of_range') "越界那条码应该是 match.roi_out_of_range：$($over.Codes -join ',')"
    Assert-Ec (-not $over.Exists) '越界的请求居然把文件写出来了'
    $diag = @($over.Json.errors)[0]
    Assert-Ec ($diag.option -eq '--roi' -and $diag.stage -eq 'match') `
        "越界诊断该带 option=--roi 与 stage=match：$($diag | ConvertTo-Json -Compress)"
    Assert-Ec ($diag.value -eq "0,0,$($imgW + 1),$imgH") "value 要原样回显用户写的那一条：$($diag.value)"
    Assert-Ec ($diag.target -eq (Get-EcHwndHex $window.Hwnd)) "target 要能对上号：$($diag.target)"
    Assert-Ec (@($over.Json.images).Count -eq 0) '被拒的批次里还是有图交出来'

    # 右下正好贴边是合法的（判据是"含左上、不含右下"），四个方向各越界一条都要被挡住。
    $edge = Invoke-Shot -Window $window -Name 'edge' -Extra @('--capture', 'wgc', '--yes',
                                                             '--roi', "0,0,$imgW,$imgH")
    Assert-Ec ($edge.Exit -eq 0 -and $edge.Img.width -eq $imgW -and $edge.Img.height -eq $imgH) `
        "右下正好贴边的裁剪是合法的（实际 exit=$($edge.Exit)）"
    $badCases = @("$imgW,0,10,10", "0,$imgH,10,10", "$($imgW - 5),0,10,10", "0,$($imgH - 5),10,10")
    $n = 0
    foreach ($bad in $badCases) {
        $n++
        $r = Invoke-Shot -Window $window -Name "bad-$n" -Extra @('--capture', 'wgc', '--yes', '--roi', $bad)
        Assert-Ec ($r.Exit -eq 1 -and $r.Codes -contains 'match.roi_out_of_range') `
            "这条越界写法没被挡住：--roi $bad（exit=$($r.Exit)，$($r.Codes -join ',')）"
        Assert-Ec (-not $r.Exists) "--roi $bad 越界 yet 写了文件"
    }

    # 桌面像素那条路也照同一越界判据挡在弹框之前：判据是"根本没弹出确认框"（-ExpectNoDialog
    # 探测到 #32770 就算失败），所以这条不需要任何人回答，也不会把人晾在那里。
    $overPath = Get-EcRunFile -RunDir $run -Name 'over-desktop.png'
    Remove-Item -LiteralPath $overPath -ErrorAction SilentlyContinue
    $preCheck = Invoke-EcConsentShot -Exe $Exe -ExpectNoDialog -TimeoutMs 20000 -Arguments @(
        '--hwnd', (Get-EcHwndHex $window.Hwnd), '--format', 'png', '--lang', 'zh-CN',
        '--out', $overPath, '--capture', 'bitblt', '--yes', '--roi', "$($imgW + 50),0,10,10")
    $preBody = if ($preCheck.Stdout.Trim()) { $preCheck.Stdout } else { $preCheck.Stderr }
    $preJson = $null
    try { $preJson = $preBody | ConvertFrom-Json } catch { }
    Assert-Ec (-not $preCheck.Dialog) `
        '越界的请求居然走到了人工确认 —— "预检排在弹框之前"那条没成立'
    Assert-Ec ($preCheck.Exit -eq 1) "越界应该返回 1，实际 $($preCheck.Exit)"
    $preCodes = if ($preJson -and $preJson.errors) { @($preJson.errors | ForEach-Object { $_.code }) } else { @() }
    Assert-Ec ($preCodes -contains 'match.roi_out_of_range') "越界那条码没送出来：$($preCodes -join ',')"
    Assert-Ec (-not (Test-Path -LiteralPath $overPath)) '越界 + 桌面通道 居然后落地了'

    # =========================================================================
    Write-Host "`n=== 4) 目标改了尺寸：同一份 --roi 从放得下变成放不下 ==="
    # =========================================================================
    $smallRect = '200,200,420,340'
    [void](Set-EcWindowRect -Window $window -Rect $smallRect)
    Start-Sleep -Milliseconds 300
    $ext2 = Get-EcExtendedFrameBounds -Hwnd $window.Hwnd
    Assert-Ec ($ext2 -and ($ext2.Right - $ext2.Left) -lt $imgW) '改小之后可见矩形没变小，这一节没有现场'
    $afterResize = Invoke-Shot -Window $window -Name 'after-resize' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', "0,0,$imgW,$imgH")
    Assert-Ec ($afterResize.Exit -eq 1 -and $afterResize.Codes -contains 'match.roi_out_of_range') `
        ("窗口改小之后同一条 --roi 必须被挡掉（exit={0}，codes={1}）" -f `
         $afterResize.Exit, ($afterResize.Codes -join ','))
    Assert-Ec (-not $afterResize.Exists) '目标已经改小了，这条请求居中还写了文件'
    # 阴性对照：按新尺寸重新给一条落在窗口内的 --roi，必须照常成功（不是"一有裁剪就失败"）
    $newW = $ext2.Right - $ext2.Left; $newH = $ext2.Bottom - $ext2.Top
    $refit = Invoke-Shot -Window $window -Name 'refit' -Extra @('--capture', 'wgc', '--yes',
                                                               '--roi', '5,5,50,40')
    Assert-Ec ($refit.Exit -eq 0 -and $refit.Img.width -eq 50 -and $refit.Img.height -eq 40) `
        ("重新落在新窗口之内的那条 --roi 应该成功（exit={0}，codes={1}）" -f `
         $refit.Exit, ($refit.Codes -join ','))
    Assert-Ec ($refit.Img.cropScreenRect.x -eq ($ext2.Left + 5) -and
               $refit.Img.cropScreenRect.y -eq ($ext2.Top + 5)) `
        '改小之后坐标映射要跟着可见矩形的新原点走（不是留在旧坐标上）'
    Assert-Ec ($refit.Img.fullWidth -eq $newW -and $refit.Img.fullHeight -eq $newH) `
        "fullWidth/fullHeight 应该是改小之后那张整窗图的尺寸：$($refit.Img.fullWidth)×$($refit.Img.fullHeight)"
    # 新的 --client-area 也要跟着新几何走（客户区是自己量的，不是缓存下来的）
    $cl2 = Get-EcClientScreenRect -Hwnd $window.Hwnd
    $ca2 = Invoke-Shot -Window $window -Name 'client-after-resize' -Extra @(
        '--capture', 'wgc', '--yes', '--client-area')
    Assert-Ec ($ca2.Exit -eq 0 -and $ca2.Img.width -eq $cl2.Width -and
               $ca2.Img.height -eq $cl2.Height) `
        ("改小之后 --client-area 交回的尺寸要等于新的客户区：图 {0}×{1}，客户区 {2}×{3}" -f `
         $ca2.Img.width, $ca2.Img.height, $cl2.Width, $cl2.Height)

    # =========================================================================
    Write-Host "`n=== 5) 裁剪不改变授权：桌面像素 + 极小 ROI 照样一定要问人 ==="
    # =========================================================================
    # 这一节**只看不点**：AI 不代答任何按钮，人也未必在场。判据是"框真弹出来了"，
    # "没人点头所以什么都没落地"用文件不存在来判；进程由测试一侧结束（它会一直等在那里），
    # 所以"答否 / 到点按拒绝"那两条码不在这里断言 —— 那是 consent.ps1 与 timeout.ps1 的活。
    # 为什么不用 --consent-timeout-ms 让框自己到点关闭：这台机器上那条框到点没被自己关掉
    # （拿本次改动之前那份构建也试过同样的一次，一样等在那里），所以这条判据不依赖它。
    $deskCases = @(
        @{ Name = 'desk-bitblt-roi';    Extra = @('--capture', 'bitblt', '--roi', '5,5,20,20') },
        @{ Name = 'desk-bitblt-client'; Extra = @('--capture', 'bitblt', '--client-area') },
        @{ Name = 'desk-dup-roi';       Extra = @('--capture', 'duplication', '--roi', '0,0,8,8') }
    )
    foreach ($c in $deskCases) {
        $path = Get-EcRunFile -RunDir $run -Name "$($c.Name).png"
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $argv = @('--hwnd', (Get-EcHwndHex $window.Hwnd), '--format', 'png', '--lang', 'zh-CN',
                 '--out', $path) + @($c.Extra) + @('--yes')
        $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -TimeoutMs 6000 -DialogWaitMs 3000
        Assert-Ec $r.Dialog ("桌面像素那条即使 --roi 只要一小块、也给了 --yes，仍必须弹框问人（$($c.Name)）")
        Assert-Ec (-not (Test-Path -LiteralPath $path)) '没人点头，这张图居然落地了'
        Assert-Ec ($r.Exit -ne 0) '没经过任何确认就返回了成功'
        Write-Host ("  {0}: 弹框={1} exit={2} 落地={3}" -f $c.Name, $r.Dialog, $r.Exit,
                   (Test-Path -LiteralPath $path)) -ForegroundColor DarkGray
    }
    # 对照：同样给 --yes，窗口内容那条路径一次都不弹框就出图（上面那三条不是"谁都弹"）。
    $winContent = Invoke-Shot -Window $window -Name 'window-content' -Extra @(
        '--capture', 'wgc', '--yes', '--roi', '4,4,30,20')
    Assert-Ec ($winContent.Exit -eq 0) "窗口内容路径带 --yes 不该被打扰：exit=$($winContent.Exit)"
    Assert-Ec (-not ($winContent.Codes | Where-Object { $_ -like 'capture.consent*' })) `
        '窗口内容路径带 --yes 时不该出现授权那一条码'

    # =========================================================================
    Write-Host "`n=== 6) 标准流与 --quiet：裁剪字段是定位判据，藏不掉 ==="
    # =========================================================================
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -Arguments @(
        '--hwnd', (Get-EcHwndHex $window.Hwnd), '--capture', 'wgc', '--yes',
        '--roi', '3,3,40,30', '--format', 'png', '--lang', 'zh-CN')
    $pipeJson = $null
    try { $pipeJson = $r.Stderr | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $pipeJson -and @($pipeJson.images).Count -eq 1) `
        "省略 --out（= --out -）时裁剪照样能成：exit=$($r.Exit)"
    $bytes = $r.StdoutBytes
    Assert-Ec ($bytes.Length -ge 24 -and $bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50) `
        'stdout 上收到的不是一张 PNG（裁剪后的图）'
    $pipeW = [int]$bytes[16] * 16777216 + [int]$bytes[17] * 65536 + [int]$bytes[18] * 256 + [int]$bytes[19]
    $pipeH = [int]$bytes[20] * 16777216 + [int]$bytes[21] * 65536 + [int]$bytes[22] * 256 + [int]$bytes[23]
    Assert-Ec ($pipeW -eq 40 -and $pipeH -eq 30) "标准输出上那张图的尺寸不是请求的裁剪：$pipeW×$pipeH"
    $pi = @($pipeJson.images)[0]
    Assert-Ec ($pi.cropRect.width -eq 40 -and $pi.cropScreenRect.width -eq 40) `
        'JSON 里那两项与流上的字节不一致'

    $quiet = Invoke-Shot -Window $window -Name 'quiet' -Extra @('--capture', 'wgc', '--yes',
                                                                '--roi', '2,2,25,15', '--quiet')
    Assert-Ec ($quiet.Exit -eq 0) "--quiet 那次截图失败：exit=$($quiet.Exit)"
    $qi = $quiet.Img
    Assert-Ec ($qi.cropMode -eq 'roi' -and $qi.cropRect.width -eq 25 -and $null -ne $qi.cropScreenRect -and
               $qi.fullWidth -gt 0) '--quiet 把裁剪的定位字段一起藏掉了（这几个是判据）'
    Assert-Ec ($quiet.Notes.Count -eq 0) "--quiet 之后还留着 notes：$($quiet.Notes -join ',')"

    $verbose = Invoke-Shot -Window $window -Name 'verbose' -Extra @('--capture', 'wgc', '--yes',
                                                                   '--roi', '7,9,33,21', '--verbose')
    Assert-Ec ($verbose.Exit -eq 0) "-v 那次截图失败：exit=$($verbose.Exit)"
    $crop = $verbose.Json.input.crop
    Assert-Ec ($crop.mode -eq 'roi' -and $crop.x -eq 7 -and $crop.y -eq 9 -and
               $crop.width -eq 33 -and $crop.height -eq 21) `
        "-v 的 input.crop 没有回显规范化后的请求：$($crop | ConvertTo-Json -Compress)"

    # =========================================================================
    Write-Host "`n=== 7) 一批里有一扇放不下就整批不开工（两扇不同尺寸的窗口） ==="
    # =========================================================================
    # 同一标题、两扇不同尺寸的自有窗口：那条 --roi 落在大的一扇之内、却超出小的一扇。
    # 混尺寸的批次只能这么造（--windows N 建的那几扇尺寸相同），所以这里另起一个进程。
    $pairTitle = "裁剪成对窗口 ec-pair-$tag"
    $big = Start-EcWindow -RunDir $run -Class "ec-pa-$tag" -Title $pairTitle -Rect '120,120,620,420' -Bordered -Seed 7
    $small = Start-EcWindow -RunDir $run -Class "ec-pb-$tag" -Title $pairTitle -Rect '700,120,860,260' -Bordered -Seed 8
    Start-Sleep -Milliseconds 400
    $extBig = Get-EcExtendedFrameBounds -Hwnd $big.Hwnd
    $extSmall = Get-EcExtendedFrameBounds -Hwnd $small.Hwnd
    Assert-Ec ($extBig -and $extSmall -and ($extBig.Right - $extBig.Left) -gt ($extSmall.Right - $extSmall.Left)) `
        '两扇窗口的可见矩形没能拉开差距，这一节没有现场'
    $roiBoth = '{0},{1},{2},{3}' -f 0, 0, ($extBig.Right - $extBig.Left), ($extBig.Bottom - $extBig.Top)
    $tmpl = Get-EcRunFile -RunDir $run -Name 'pair_%i.png'
    foreach ($k in 1, 2) { Remove-Item -LiteralPath (Get-EcRunFile -RunDir $run -Name "pair_$k.png") -ErrorAction SilentlyContinue }
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -Arguments @(
        '--title', $pairTitle, '--all', '--capture', 'wgc', '--yes', '--lang', 'zh-CN',
        '--roi', $roiBoth, '--out', $tmpl)
    $json = Json-Of $r
    Assert-Ec ($r.Exit -eq 1) "混尺寸的一批里有一扇放不下这条 --roi，应该整批返回 1，实际 $($r.Exit)"
    Assert-Ec (@($json.errors | ForEach-Object { $_.code }) -contains 'match.roi_out_of_range') `
        '整批拒绝时那条码要照实送出来'
    Assert-Ec ($json.captured -eq 0 -and @($json.images).Count -eq 0) '被挡掉的批次里居还是有图交出来'
    foreach ($k in 1, 2) {
        Assert-Ec (-not (Test-Path -LiteralPath (Get-EcRunFile -RunDir $run -Name "pair_$k.png"))) `
            "整批不开工，pair_$k.png 却写出来了"
    }
    # 那条诊断要点名是哪一扇放不下（target 与 images[].hwnd 同形）
    $who = @($json.errors | Where-Object { $_.code -eq 'match.roi_out_of_range' } | Select-Object -First 1)
    $tags = @((Get-EcHwndHex $big.Hwnd), (Get-EcHwndHex $small.Hwnd))
    Assert-Ec ($who.target -in $tags) ("target 要能对上号，实际 {0}（本次两扇：{1}）" -f $who.target, ($tags -join ' / '))

    # =========================================================================
    Write-Host "`n=== 8) 本机造不出来的判据（照实记录，不算通过） ==="
    # =========================================================================
    Skip-Ec '跨屏混合 DPI 下同一份 --roi 取的像素数不变' '这台机器只接了一块屏，没有第二种缩放比的屏可挪；这一层只能现场判"交付尺寸等于独立量到的可见矩形像素数"（见第 1 节），DIP→物理像素那条换算由调用方负责'
    Skip-Ec '预检通过之后、帧交回来之前那一瞬窗口被改小（capture.roi_invalid 的现场）' '要的就是这个竞态窗口本身，时间点安排不出来；这条由离线层 ResolveWindowCrop 逐条判（同一份请求在较小的图像上必须 kOutOfRange，且不退回整窗交出）'
    Skip-Ec '桌面像素 + 极小 ROI 时"答否 / --consent-timeout-ms 到点按拒绝"那两条码' '第 5 节判到的是"框一定弹、没点头就不落地"；这台机器上那条框到点没有被自己关掉（拿本次改动之前的构建试过同样一次，也一样等在那里），而测试一侧不代答任何按钮，所以那两条码留给 consent.ps1 / timeout.ps1 在有人守着屏幕时判'

    Stop-EcOwnedWindows
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} catch {
    if ("$_" -ne '__SKIP_REAL__') { throw }
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '窗口内部裁剪（--roi / --client-area）')
