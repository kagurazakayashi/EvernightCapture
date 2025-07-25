<#
.SYNOPSIS
    真机测试：标准流与结构化结果的可靠性（stdout 只交付一张图、异常边界、诊断定位字段）。
.DESCRIPTION
    这一套只截本测试自己建立的窗口（ecwindow.exe），不走整屏路径，所以正常情况下不会弹确认框；
    唯一涉及整屏的那一节（多屏 + --monitor all 写 stdout）判据是"确认框根本不该出现"，
    它只在被拒的那一侧断言，绝不让测试替人回答授权。

    覆盖的事：
      1. 单个目标写 stdout：图片字节走 stdout、JSON 整份走 stderr，file 是 "-"，source 是真实通道
      2. 多个目标写 stdout：在确认与取帧之前就被拒（cli.stdout_multiple_targets + 退出码 1），
         显式 --out - 与"根本没给输出路径"两种都一样
      3. 判据是实际目标数而不是 --all：--all 只命中一个窗口时仍然允许 stdout
      4. 多屏 --monitor all + stdout 的拒绝发生在确认框之前（本机不足两块屏时记未验证）
      5. 诊断的定位字段：target / backend / stage / hresult / win32 来自真实那一步
         （bitblt 是桌面像素通道，默认只答"否"判拒绝那一半；加 -SimulateConsent 才走到取帧失败）
      6. 批次中途失败：前面成功的图保留，失败那条带着自己的目标标识
      7. 结果送不到约定通道时退出码是 8，且绝不把文字补写进已被图片占用的 stdout
      8. 省略 --out 与显式 --out - 给出同一套机器语义：成功、无匹配、歧义、非法参数、
         后端失败、被拒绝、写入断管这七个场景逐个对拍退出码与 code 及定位字段
         （旧实现会把隐式那条路上的一切失败换成 cli.missing_output + 退出码 1，已删除）
    注入不了的两项（后端真的抛出异常、进程被强杀在半途）如实记成未验证。
.EXAMPLE
    .\tests\streams.ps1
    .\tests\streams.ps1 -Keep      # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SimulateConsent,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$run = New-EcRunDir -Tag 'streams'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

$RECT_ON = '240,240,720,560'
$RECT_OFF = '-40000,-40000,-39500,-39600'   # 完全在虚拟屏幕之外：bitblt 必然拿不到可见像素

function New-ShotDir {
    <# 每一节用自己的子目录：上一节留下的图看起来和本节的成功一模一样。 #>
    param([Parameter(Mandatory)][string]$Name)

    $dir = Join-Path $run.Path $Name
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
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

function Get-ErrorBy {
    param($Json, [string]$Code)
    if (-not $Json) { return $null }
    foreach ($e in @($Json.errors)) { if ($e -and $e.code -eq $Code) { return $e } }
    return $null
}

function Test-PngSingle {
    <# stdout 收到的该是"一张"完整 PNG：按块（chunk）走一遍，长度总和要正好等于收到的字节数，
       第一块是 IHDR、最后一块是 IEND。压缩数据里偶然出现 "PNG" / "IEND" 这几个字节是可能的，
       所以不能靠搜字符串判断"是不是拼了多张"。 #>
    param([byte[]]$Bytes)

    if ($Bytes.Length -lt 24) { return '空或太短' }
    if (-not ($Bytes[0] -eq 0x89 -and $Bytes[1] -eq 0x50 -and $Bytes[2] -eq 0x4E -and
              $Bytes[3] -eq 0x47)) { return '不是 PNG 签名' }
    $i = 8
    $first = ''
    $last = ''
    while ($i -lt $Bytes.Length) {
        if ($Bytes.Length - $i -lt 8) { return '块长度字段被截断' }
        $len = ([uint32]$Bytes[$i] -shl 24) -bor ([uint32]$Bytes[$i + 1] -shl 16) -bor
               ([uint32]$Bytes[$i + 2] -shl 8) -bor [uint32]$Bytes[$i + 3]
        $type = [Text.Encoding]::ASCII.GetString($Bytes[($i + 4)..($i + 7)])
        if (-not $first) { $first = $type }
        $last = $type
        $next = $i + 12 + $len      # 长度字段 + 类型 + 数据 + CRC
        if ($next -gt $Bytes.Length) { return '块声明的长度超过了实际字节数（图被截短）' }
        $i = $next
    }
    if ($i -ne $Bytes.Length) { return '块边界与总长度对不上' }
    if ($first -ne 'IHDR') { return "第一块不是 IHDR：$first" }
    if ($last -ne 'IEND') { return "最后一块不是 IEND：$last" }
    return ''
}

function Get-Field {
    <# 诊断项里某个可选字段在不在（ConvertFrom-Json 对缺失键给 $null，空串也算没写）。 #>
    param($Object, [string]$Name)
    if (-not $Object) { return $false }
    return ($Object.PSObject.Properties.Name -contains $Name -and
            -not [string]::IsNullOrEmpty([string]$Object.$Name))
}

function Get-MachineShape {
    <# 调用方据此分支的那一套：code 与全部定位字段 + 交付形状。
       images[].bytes 故意不参与 —— 两次截图之间光标可能落在窗口里、编码时间也不同，
       字节数会变，而那不是"隐式与显式 stdout 是否等价"这条判据要问的事。 #>
    param($Json)
    if (-not $Json) { return '<不是 JSON>' }
    $lines = @('captured={0}' -f $Json.captured, 'images={0}' -f @( $Json.images ).Count)
    foreach ($e in @($Json.errors)) {
        if ($null -eq $e) { continue }
        $lines += (@('err', $e.code, $e.option, $e.value, $e.target, $e.backend, $e.stage,
                     $e.hresult, $e.win32) -join '|')
    }
    foreach ($i in @($Json.images)) {
        if ($null -eq $i) { continue }
        $lines += (@('img', $i.file, $i.source, $i.path, $i.scope, $i.width, $i.height,
                     $i.hwnd) -join '|')
    }
    return ($lines -join "`n")
}

function Invoke-EcStdoutPair {
    <# 同一条命令跑两遍：一遍省略 --out（隐式 stdout），一遍显式 --out -。
       两遍各自独占一个空工作目录：上一次留下的图看起来和本遍的成功一模一样。
       两种写法的 output 都是 "-"，所以结果 JSON 一定在 stderr，这里只解析那一条流。 #>
    param([string]$Name, [string[]]$BaseArgs, [hashtable]$Extra = @{})

    $out = @{}
    foreach ($kind in @('implicit', 'explicit')) {
        $dir = New-ShotDir ('eq-{0}-{1}' -f $Name, $kind)
        $argv = if ($kind -eq 'explicit') { @($BaseArgs) + @('--out', '-') } else { @($BaseArgs) }
        if ($Extra.ContainsKey($kind)) { $argv = $argv + @($Extra[$kind]) }
        $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir -Arguments $argv
        $json = $null
        try { $json = $r.Stderr | ConvertFrom-Json } catch { }
        $out[$kind] = [pscustomobject]@{ Result = $r; Json = $json; Dir = $dir }
    }
    return $out
}

function Assert-EcPairEquivalent {
    <# 两条路必须给出同一套机器语义：退出码、code 与其定位字段、交付形状，
       以及"什么都没写进磁盘"（省略 --out 绝不等于"存个默认文件名"）。 #>
    param([string]$Name, $Pair, [int]$ExpectExit, [string[]]$ExpectCodes)

    foreach ($kind in @('implicit', 'explicit')) {
        $p = $Pair[$kind]
        Assert-Ec ($null -ne $p.Json) "$Name（$kind）的 stderr 不是 JSON：[$($p.Result.Stderr.Trim())]"
        Assert-Ec ($p.Result.Exit -eq $ExpectExit) `
            "$Name（$kind）退出码 $($p.Result.Exit)，期望 $ExpectExit"
        $codes = @(Get-Codes $p.Json.errors) -join ','
        Assert-Ec ($codes -eq ($ExpectCodes -join ',')) `
            "$Name（$kind）的 code 表是 [$codes]，期望 [$($ExpectCodes -join ',')]"
        Assert-Ec (-not ($codes -match 'cli\.missing_output')) `
            "$Name（$kind）又冒出 cli.missing_output：那条折叠已经删掉了"
        Assert-Ec (@(Get-ChildItem -LiteralPath $p.Dir -File -Recurse -Force).Count -eq 0) `
            "$Name（$kind）在工作目录里写出了文件"
    }
    $si = Get-MachineShape $Pair.implicit.Json
    $se = Get-MachineShape $Pair.explicit.Json
    Assert-Ec ($si -eq $se) `
        "$Name：隐式与显式 stdout 的机器语义不同 / 隐式 [$($si -replace '\r?\n', ' / ')] / 显式 [$($se -replace '\r?\n', ' / ')]"
}

try {
    # =========================================================================
    Write-Host "`n=== 1) 单个目标写 stdout：图走 stdout，JSON 整份走 stderr ==="
    # =========================================================================
    $dir1 = New-ShotDir 'single'
    $w1 = Start-EcWindow -RunDir $run -Class "ec-stream-a-$tag" -Title "标准流甲 $tag" `
                         -Rect $RECT_ON -Seed 21
    $r = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir1 -Arguments @(
        '--hwnd', (Get-EcHwndHex $w1.Hwnd), '--yes', '--out', '-')
    $o = Get-JsonOf @{ Stdout = ''; Stderr = $r.Stderr }
    Assert-Ec ($r.Exit -eq 0) "单目标 stdout 失败（exit=$($r.Exit) $($r.Stderr)）"
    Assert-Ec ($o -and $o.captured -eq 1) "captured=$($o.captured)，期望 1"
    $img = @($o.images)[0]
    Assert-Ec ($img.file -eq '-') "file 该是单横杠，实际 $($img.file)"
    Assert-Ec ($img.hwnd -eq (Get-EcHwndHex $w1.Hwnd)) '图的归属不是本次窗口'
    $bad = Test-PngSingle -Bytes $r.StdoutBytes
    Assert-Ec ($bad -eq '') "stdout 里的图片不是一张完整 PNG：$bad"
    Assert-Ec ($img.width -eq ([int]$r.StdoutBytes[16] * 16777216 + [int]$r.StdoutBytes[17] * 65536 +
                              [int]$r.StdoutBytes[18] * 256 + [int]$r.StdoutBytes[19])) `
        'JSON 的 width 与 PNG 头里的宽度不一致'
    Assert-Ec ($img.bytes -eq $r.StdoutBytes.Length) 'JSON 的 bytes 与 stdout 实际字节数不一致'
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir1 -File -Recurse).Count -eq 0) 'stdout 模式在工作目录里写出了文件'
    # source 是"真正出图的那条通道"，不是请求值
    Assert-Ec (Get-Field $img 'source') 'images[].source 没出现（该写明真实通道）'
    Assert-Ec (@('wgc', 'dwm', 'printwindow', 'bitblt', 'duplication') -contains $img.source) `
        "images[].source 不是已知通道：$($img.source)"
    Write-Host ("  真实通道 source={0}  {1}x{2}  {3} 字节" -f $img.source, $img.width, $img.height, $img.bytes) -ForegroundColor DarkGray

    # =========================================================================
    Write-Host "`n=== 2) 多目标写 stdout：取帧之前就整批被拒 ==="
    # =========================================================================
    $dir2 = New-ShotDir 'multi'
    $pair = Start-EcWindow -RunDir $run -Class "ec-stream-pair-$tag" -Title "标准流成对 $tag" `
                           -Rect $RECT_ON -Seed 22 -Windows 2
    $hwnds = @($pair.Hwnds | ForEach-Object { Get-EcHwndHex $_ })
    $argv = @('--hwnd', $hwnds[0], '--hwnd', $hwnds[1], '--all')

    # 2a) 显式 --out -
    $explicit = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir2 `
                                 -Arguments ($argv + @('--out', '-'))
    $oe = Get-JsonOf @{ Stdout = ''; Stderr = $explicit.Stderr }
    Assert-Ec ($explicit.Exit -eq 1) "多目标 stdout 该报参数错（exit=$($explicit.Exit)）"
    Assert-Ec ($oe -and (@(Get-Codes $oe.errors) -contains 'cli.stdout_multiple_targets')) `
        "code 不对：[$(if ($oe) { (Get-Codes $oe.errors) -join ',' } else { 'JSON 解析失败' })]"
    Assert-Ec ($oe -and $oe.captured -eq 0 -and @($oe.images).Count -eq 0) '被拒的那一次居然报了图'
    Assert-Ec ($explicit.StdoutBytes.Length -eq 0) '被拒的那一次往 stdout 写了东西'
    $multiErr = Get-ErrorBy $oe 'cli.stdout_multiple_targets'
    Assert-Ec (Get-Field $multiErr 'hint') '该有一条怎么改的 hint'

    # 2b) 根本没给输出路径（隐式 stdout）：报的必须是同一条真实原因。
    #     "缺少输出路径"从来不是这条失败的原因 —— 补上 --out 也治不了多个目标挤一条 stdout。
    $implicit = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir2 -Arguments $argv
    $oi = Get-JsonOf @{ Stdout = ''; Stderr = $implicit.Stderr }
    Assert-Ec ($implicit.Exit -eq 1) "隐式 stdout 的多目标该报参数错（exit=$($implicit.Exit)）"
    $codes = if ($oi) { @(Get-Codes $oi.errors) -join ',' } else { 'JSON 解析失败' }
    Assert-Ec ($oi -and (@(Get-Codes $oi.errors) -contains 'cli.stdout_multiple_targets')) `
        "隐式 stdout 报的不是那条真实原因：[$codes]"
    Assert-Ec ($oi -and -not (@(Get-Codes $oi.errors) -contains 'cli.missing_output')) `
        "新诊断不该和 cli.missing_output 同时出现：[$codes]"
    Assert-Ec ($implicit.StdoutBytes.Length -eq 0) '隐式 stdout 被拒时往 stdout 写了东西'
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir2 -File -Recurse).Count -eq 0) '被拒的那两次写出了文件'
    Stop-EcWindow -Window $pair

    # =========================================================================
    Write-Host "`n=== 3) 判据是实际目标数：--all 只命中一个时仍然放行 ==="
    # =========================================================================
    $dir3 = New-ShotDir 'all-single'
    $w3 = Start-EcWindow -RunDir $run -Class "ec-stream-single-$tag" -Title "标准流单扇 $tag" `
                         -Rect $RECT_ON -Seed 23
    $one = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir3 -Arguments @(
        '--class', "ec-stream-single-$tag", '--all', '--yes', '--out', '-')
    $oo = Get-JsonOf @{ Stdout = ''; Stderr = $one.Stderr }
    Assert-Ec ($one.Exit -eq 0 -and $oo -and $oo.captured -eq 1) `
        "--all 只命中一个目标时不该拒绝 stdout（exit=$($one.Exit) $($one.Stderr)）"
    Assert-Ec ((Test-PngSingle -Bytes $one.StdoutBytes) -eq '') '--all 单目标时 stdout 里的图不完整'
    Stop-EcWindow -Window $w3

    # =========================================================================
    Write-Host "`n=== 4) 多屏 + stdout：拒绝要发生在确认框之前 ==="
    # =========================================================================
    $monitorCount = 0
    try {
        # 屏幕数用工具自己的枚举（dry-run 的 value 里一块屏一段），测试侧不再算一套
        $probe = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -Arguments @(
            '--monitor', 'all', '--dry-run', '--out', '-')
        $po = Get-JsonOf @{ Stdout = ''; Stderr = $probe.Stderr }
        if ($po -and @($po.notes).Count -gt 0) {
            $monitorCount = @([string]@($po.notes)[0].value -split '\|').Count
        }
    } catch { $monitorCount = 0 }
    if ($monitorCount -lt 2) {
        Skip-Ec '本机不足两块屏幕，多屏写 stdout 的拒绝路径未验证'
    } else {
        $dir4 = New-ShotDir 'multi-screen'
        $script:EcDialogSeen = $false
        # 这一条只验证"被拒时不该弹框"：进程应当在取帧之前就返回，测试侧从不代答授权。
        $ms = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir4 `
                               -Arguments @('--monitor', 'all', '--out', '-') `
                               -Probe {
                                   param($p)
                                   if (Find-EcDialog -ProcessId $p.Id) {
                                       $script:EcDialogSeen = $true
                                       return $true
                                   }
                               }
        $om = Get-JsonOf @{ Stdout = ''; Stderr = $ms.Stderr }
        $codes4 = if ($om) { (Get-Codes $om.errors) -join ',' } else { 'JSON 解析失败' }
        Assert-Ec ($ms.Exit -eq 1 -and $om -and
                   (@(Get-Codes $om.errors) -contains 'cli.stdout_multiple_targets')) `
            "多屏写 stdout 该整批被拒（exit=$($ms.Exit) codes=[$codes4]）"
        Assert-Ec (-not $script:EcDialogSeen) '被拒的那一次弹出了整屏确认框（打扰了人）'
        Assert-Ec ($ms.StdoutBytes.Length -eq 0) '多屏被拒时往 stdout 写了东西'
        Assert-Ec (@(Get-ChildItem -LiteralPath $dir4 -File -Recurse).Count -eq 0) '多屏被拒时写出了文件'
    }

    # =========================================================================
    Write-Host "`n=== 5) 诊断定位字段：target / backend / stage / hresult / win32 ==="
    # =========================================================================
    $dir5 = New-ShotDir 'fields'
    # 5a) 屏外窗口 + bitblt：这条通道只能拷可见像素，本来必然失败且不需要注入。
    #     但 bitblt 取的是桌面像素，授权分级之后它一定先问人 —— 所以这一条分成两半：
    #       * 默认（不代人同意）：只答"否"，判"确认被拒时的诊断也带全套定位字段、零落地"；
    #       * 加 -SimulateConsent：答"是"，才走到取帧那一步判"后端失败的定位字段"。
    $woff = Start-EcWindow -RunDir $run -Class "ec-stream-off-$tag" -Title "标准流屏外 $tag" `
                           -Rect $RECT_OFF -Seed 24
    $offPath = Join-Path $dir5 'off.png'
    $offArgv = @('--hwnd', (Get-EcHwndHex $woff.Hwnd), '--capture', 'bitblt', '--out', $offPath)
    $off = Invoke-EcConsentShot -Exe $Exe -Arguments $offArgv -Answer $(if ($SimulateConsent) { 6 } else { 7 })
    Assert-Ec $off.Dialog 'bitblt 这种桌面像素通道没弹确认框'
    $oo5 = Get-JsonOf @{ Stdout = $off.Stdout; Stderr = $off.Stderr }
    $offErr = if ($oo5) { @($oo5.errors)[0] } else { $null }
    Assert-Ec ($offErr -and $offErr.target -eq (Get-EcHwndHex $woff.Hwnd)) `
        "target 该指向本次窗口：$($offErr.target)"
    Assert-Ec ($offErr.backend -eq 'bitblt') "backend 该是真实通道 bitblt：$($offErr.backend)"
    if (-not $SimulateConsent) {
        Assert-Ec ($off.Exit -eq 6) "答`"否`"的退出码 $($off.Exit)，应为 6"
        Assert-Ec ($offErr.code -eq 'capture.access_denied') `
            "code 该是 capture.access_denied：$($offErr.code)"
        Assert-Ec ($offErr.stage -eq 'consent') "stage 该是 consent：$($offErr.stage)"
        Assert-Ec ($offErr.value -eq 'bitblt.screen') "value 该是实际路径：$($offErr.value)"
        Skip-Ec '屏外窗口 + bitblt 的取帧失败定位字段（要人答"是"）' `
            '不代人同意桌面路径的确认框；加 -SimulateConsent 在无隐私桌面上跑'
    } else {
        Assert-Ec ($off.Exit -eq 7) "屏外窗口 + bitblt 该报截图失败（exit=$($off.Exit)）"
        Assert-Ec ($offErr.code -eq 'capture.failed') "code 该是 capture.failed：$($offErr.code)"
        Assert-Ec ($offErr.stage -eq 'capture') "stage 该是 capture：$($offErr.stage)"
    }
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir5 -File).Count -eq 0) '失败的那一次写出了文件'
    Stop-EcWindow -Window $woff

    # 5b) auto 的来路写的是真实通道，不是请求值 auto（屏外窗口上四条链的结果都算合法）
    $auto = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir5 -Arguments @(
        '--hwnd', (Get-EcHwndHex $w1.Hwnd), '--capture', 'auto', '--yes',
        '--out', (Join-Path $dir5 'auto.png'))
    $oa = Get-JsonOf $auto
    if ($auto.Exit -eq 0) {
        $asrc = @($oa.images)[0].source
        Assert-Ec ($asrc -and $asrc -ne 'auto') "images[].source 写成请求值了：$asrc"
        Assert-Ec (@('wgc', 'dwm', 'printwindow', 'bitblt') -contains $asrc) "source 不是已知通道：$asrc"
        $chan = Get-ErrorBy @{ errors = @($oa.notes) } 'note.capture_channel'
        if ($asrc -ne 'wgc') {
            Assert-Ec ($null -ne $chan) "回退到 $asrc 却没发 note.capture_channel"
            Assert-Ec ($chan.value -eq 'auto') '回退提示的 value 该是请求值 auto'
        }
    } else {
        $ae = @($oa.errors)[0]
        Assert-Ec ($ae.backend -and $ae.backend -ne 'auto') `
            "auto 全链失败时 backend 该列出真实试过的通道：$($ae.backend)"
        Assert-Ec ($ae.backend -match 'wgc') "backend 里没写出链首 wgc：$($ae.backend)"
    }

    # 5c) 一条真实的后端 HRESULT：WGC 拒掉任务栏窗口时，诊断要带它自己的码而不是占位值。
    #     本机要是允许截任务栏，这一条就判不了 —— 记未验证，并把那张图删掉。
    $tb = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -Arguments @(
        '--class', 'Shell_TrayWnd', '--capture', 'wgc', '--yes',
        '--out', (Join-Path $dir5 'tray.png'))
    $otb = Get-JsonOf $tb
    if ($tb.Exit -eq 0) {
        Remove-Item -LiteralPath (Join-Path $dir5 'tray.png') -Force -ErrorAction SilentlyContinue
        Skip-Ec '本机 WGC 允许截任务栏，拿不到真实后端 HRESULT 这一判据未验证'
    } else {
        $tbErr = @($otb.errors)[0]
        Assert-Ec (Get-Field $tbErr 'hresult') '后端失败没带 hresult 字段'
        Assert-Ec ($tbErr.hresult -match '^0x[0-9A-F]{8}$') "hresult 形状不对：$($tbErr.hresult)"
        Assert-Ec ($tbErr.hresult -ne '0x80004002') `
            'hresult 是 E_NOINTERFACE：这是旧实现写死的占位值，说明真实错误码又被换掉了'
        Assert-Ec ($tbErr.message -like ('*' + $tbErr.hresult + '*')) `
            'hresult 字段与 message 里引用的那个码不是同一个值'
        Assert-Ec ($tbErr.stage -eq 'capture' -and $tbErr.backend -eq 'wgc') '后端错误的 stage/backend 不对'
    }

    # =========================================================================
    Write-Host "`n=== 6) 批次中途失败：前面成功的图保留 ==="
    # =========================================================================
    $dir6 = New-ShotDir 'partial'
    $good = Start-EcWindow -RunDir $run -Class "ec-stream-good-$tag" -Title 'ecgood' -Rect $RECT_ON -Seed 25
    $block = Start-EcWindow -RunDir $run -Class "ec-stream-block-$tag" -Title 'ecblock' -Rect $RECT_ON -Seed 26
    # %n 用窗口标题当名字；先放一个同名"目录"在那里，第二张的提交必然失败（ERROR_ACCESS_DENIED）
    New-Item -ItemType Directory -Force -Path (Join-Path $dir6 'ecblock.png') | Out-Null
    $part = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -WorkingDirectory $dir6 -Arguments @(
        '--hwnd', (Get-EcHwndHex $good.Hwnd), '--hwnd', (Get-EcHwndHex $block.Hwnd),
        '--all', '--yes', '--out', (Join-Path $dir6 '%n.png'))
    $op = Get-JsonOf $part
    Assert-Ec ($part.Exit -eq 7) "部分成功该用截图失败码提示看 errors（exit=$($part.Exit)）"
    Assert-Ec ($op -and $op.captured -eq 1) "captured=$($op.captured)，期望 1"
    Assert-Ec (@(Get-Codes $op.errors) -contains 'io.write_failed') `
        "写入失败该报 io.write_failed：[$((Get-Codes $op.errors) -join ',')]"
    Assert-Ec (Test-Path -LiteralPath (Join-Path $dir6 'ecgood.png')) '那张能写的图没落地'
    $wErr = Get-ErrorBy $op 'io.write_failed'
    Assert-Ec ($wErr.stage -eq 'write') "stage 该是 write：$($wErr.stage)"
    Assert-Ec ($wErr.win32 -eq 5) "win32 该是 ERROR_ACCESS_DENIED(5)：$($wErr.win32)"
    Assert-Ec ($wErr.backend -ne 'auto') "backend 写成了请求值：$($wErr.backend)"
    $blockedHex = Get-EcHwndHex $block.Hwnd
    $goodHex = Get-EcHwndHex $good.Hwnd
    Assert-Ec (@($wErr.target -split ',') -contains $blockedHex -or $wErr.target -eq $blockedHex) `
        "target 该指向被挡住的那个目标 $blockedHex：$($wErr.target)"
    Assert-Ec (@(@($op.images) | ForEach-Object { $_.hwnd }) -notcontains $blockedHex) '被挡住的目标却报了图'
    Assert-Ec (@(Get-ChildItem -LiteralPath $dir6 -File -Filter '~*.tmp').Count -eq 0) '失败的那一次留下了临时文件'
    Assert-Ec (@(Get-ChildItem -LiteralPath (Join-Path $dir6 'ecblock.png')).Count -eq 0) `
        '那个挡住目标的目录里被写了东西（临时文件应当只写在目标目录，且失败后要清掉）'
    Stop-EcWindow -Window $good
    Stop-EcWindow -Window $block

    # =========================================================================
    Write-Host "`n=== 7) 结果送不到约定通道 => 退出码 8，且不把文字补进 stdout ==="
    # =========================================================================
    # 用一个批处理把某条流接到 CONIN$ 上：往那个句柄写会失败，正好用来验"约定通道写坏了"。
    # 正文按仓库规矩只用 ASCII 与大写 DOS 命令（见 AGENTS.md 的构建脚本一节）。
    $dir7 = New-ShotDir 'routing'
    $bat = Join-Path $dir7 'routing-probe.bat'
    $caseOut = Join-Path $dir7 'case-image-mode.txt'
    # 逗号比 + 绑定得更紧，所以拼接的那一行必须加括号（少一层括号会把一行批处理劈成三行）。
    # call "…" 而不是让行首就是引号：路径本身可能含空格。
    $lines = @(
        '@echo off',
        ('set "EXE=' + $Exe + '"'),
        'call "%EXE%" --version > CONIN$',
        'echo stdout_broken=%ERRORLEVEL%',
        'call "%EXE%" --version 2> CONIN$',
        'echo stderr_broken=%ERRORLEVEL%',
        ('call "%EXE%" --class Shell_TrayWnd --dry-run --out - > "' + $caseOut + '" 2> CONIN$'),
        'echo image_mode_stderr_broken=%ERRORLEVEL%'
    )
    Set-Content -LiteralPath $bat -Value $lines -Encoding ascii
    $batRun = Invoke-EcProcess -FilePath (Join-Path $env:SystemRoot 'System32\cmd.exe') `
                               -TimeoutMs 60000 -Arguments @('/d', '/c', $bat)
    $marker = @{}
    foreach ($l in (($batRun.Stdout -split "`r?`n") + ($batRun.Stderr -split "`r?`n"))) {
        if ($l -match '^(stdout_broken|stderr_broken|image_mode_stderr_broken)=(\d+)$') {
            $marker[$Matches[1]] = [int]$Matches[2]
        }
    }
    Assert-Ec $marker.ContainsKey('stdout_broken') "没拿到批处理的回显：[$($batRun.Stdout.Trim()) $($batRun.Stderr.Trim())]"
    Assert-Ec ($marker['stdout_broken'] -eq 8) `
        "约定通道（stdout）写坏时退出码该是 8，实际 $($marker['stdout_broken'])（旧实现会留成原来的码）"
    Assert-Ec ($marker['stderr_broken'] -eq 0) "stderr 写坏不影响 stdout 那次交付，实际 $($marker['stderr_broken'])"
    Assert-Ec ($marker['image_mode_stderr_broken'] -eq 8) `
        "图片模式（JSON 约定走 stderr）写坏时也该报 8，实际 $($marker['image_mode_stderr_broken'])"
    $txt = ''
    if (Test-Path -LiteralPath $caseOut) { $txt = '' + (Get-Content -LiteralPath $caseOut -Raw) }
    Assert-Ec ($txt.Trim() -eq '') "JSON 该留在 stderr，却被补写进了 stdout：$($txt.Trim())"

    # =========================================================================
    Write-Host "`n=== 8) 省略 --out 与显式 --out - 等价（真实错误、退出码、部分成功） ==="
    # =========================================================================
    # 0.4.0 之前的旧实现只要没给 --out，就把这条路上的一切失败换成一条 cli.missing_output +
    # 退出码 1，并清空 images 与 notes。现在两种写法走的是同一条代码路径：这里逐个场景对拍
    # "调用方据此分支"的那一套（code、定位字段、captured、交付形状），人读文字与 notes 不参与
    # （隐式那条发 note.output_defaulted_stdout，显式那条发 note.pipe_default_format）。
    $eqOk = Start-EcWindow -RunDir $run -Class "ec-stream-eq-$tag" -Title "标准流等价 $tag" `
                           -Rect $RECT_ON -Seed 31
    $eqHwnd = Get-EcHwndHex $eqOk.Hwnd

    # 8a) 成功：图必须是 stdout 里那一张完整 PNG，而且一个文件都不许落地
    $p = Invoke-EcStdoutPair -Name 'success' -BaseArgs @('--hwnd', $eqHwnd, '--yes')
    Assert-EcPairEquivalent -Name '成功' -Pair $p -ExpectExit 0 -ExpectCodes @()
    foreach ($kind in @('implicit', 'explicit')) {
        $bytes = $p[$kind].Result.StdoutBytes
        $bad = Test-PngSingle -Bytes $bytes
        Assert-Ec ($bad -eq '') "$kind 那条的 stdout 不是一张完整 PNG：$bad"
        $img = @($p[$kind].Json.images)[0]
        Assert-Ec ($img.bytes -eq $bytes.Length) `
            "$kind 那条的 images[].bytes($($img.bytes)) 与 stdout 实际字节数($($bytes.Length))不一致"
        Assert-Ec ($img.file -eq '-') "$kind 那条的 images[].file 不是 -：$($img.file)"
        Assert-Ec ($img.scope -eq 'window') "$kind 那条的 scope 不是 window：$($img.scope)"
    }
    Write-Host ("  成功那一对：隐式 {0} 字节 / 显式 {1} 字节，两次都是 {2}x{3}" -f `
        $p.implicit.Result.StdoutBytes.Length, $p.explicit.Result.StdoutBytes.Length,
        @($p.implicit.Json.images)[0].width, @($p.implicit.Json.images)[0].height) -ForegroundColor DarkGray

    # 8b) 无匹配：两个都没给路径的调用，报的都是"没有窗口满足条件"，退出码 4
    $p = Invoke-EcStdoutPair -Name 'no-match' -BaseArgs @('--class', "ec-no-such-class-$tag")
    Assert-EcPairEquivalent -Name '无匹配' -Pair $p -ExpectExit 4 -ExpectCodes @('match.no_window')

    # 8c) 歧义（同一进程两扇同标题窗口，不给消歧策略）：退出码 5，一条图都不出
    $eqPair = Start-EcWindow -RunDir $run -Class "ec-stream-eq2-$tag" -Title "标准流成双 $tag" `
                            -Rect $RECT_ON -Seed 32 -Windows 2
    $p = Invoke-EcStdoutPair -Name 'ambiguous' -BaseArgs @('--title', "标准流成双 $tag")
    Assert-EcPairEquivalent -Name '歧义' -Pair $p -ExpectExit 5 -ExpectCodes @('match.ambiguous_window')
    Stop-EcWindow -Window $eqPair

    # 8d) 非法参数：屏幕编号越界在解析/选择期就报错，两种写法都是 cli 之外的同一条码
    $p = Invoke-EcStdoutPair -Name 'bad-arg' -BaseArgs @('--monitor', '99')
    Assert-EcPairEquivalent -Name '非法参数' -Pair $p -ExpectExit 1 -ExpectCodes @('match.monitor_out_of_range')
    $p = Invoke-EcStdoutPair -Name 'bad-arg2' -BaseArgs @('--hwnd', 'zzz')
    Assert-EcPairEquivalent -Name '非法取值' -Pair $p -ExpectExit 1 -ExpectCodes @('cli.invalid_number')

    # 8e) 后端失败：任务栏那种"WGC 自己拒绝"的目标（窗口内容路径，带 --yes 不弹框）。
    #     本机允许截任务栏时这一档就造不出失败 —— 照实记未验证，不拿别的场景凑数。
    $p = Invoke-EcStdoutPair -Name 'backend' -BaseArgs @('--class', 'Shell_TrayWnd', '--capture', 'wgc', '--yes')
    if ($p.implicit.Result.Exit -eq 0) {
        Skip-Ec '本机 WGC 允许截任务栏，"后端失败"这一场景的显式/隐式等价无法现场判'
        Assert-EcPairEquivalent -Name '后端失败（本机变成成功）' -Pair $p -ExpectExit 0 -ExpectCodes @()
    } else {
        Assert-EcPairEquivalent -Name '后端失败' -Pair $p -ExpectExit 7 -ExpectCodes @('capture.failed')
        foreach ($kind in @('implicit', 'explicit')) {
            $e = @($p[$kind].Json.errors)[0]
            Assert-Ec (Get-Field $e 'hresult') "$kind 那条的后端失败没带 hresult"
            Assert-Ec ($e.hresult -ne '0x80004002') "$kind 那条又是 E_NOINTERFACE 顶掉了真实码"
        }
    }

    # 8f) 被拒绝：bitblt 是桌面像素通道，一定弹框；测试侧一律只答"否"（拒绝不拍到任何东西）
    $eqOff = Start-EcWindow -RunDir $run -Class "ec-stream-eq-off-$tag" -Title "标准流屏外 $tag" `
                           -Rect $RECT_OFF -Seed 33
    $refused = @{}
    foreach ($kind in @('implicit', 'explicit')) {
        $argv = @('--hwnd', (Get-EcHwndHex $eqOff.Hwnd), '--capture', 'bitblt')
        if ($kind -eq 'explicit') { $argv += @('--out', '-') }
        $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -Answer 7
        $j = $null
        try { $j = $r.Stderr | ConvertFrom-Json } catch { }
        $refused[$kind] = [pscustomobject]@{ Result = $r; Json = $j }
        Assert-Ec $r.Dialog "$kind 那条的桌面像素路径没弹确认框"
        Assert-Ec $r.Clicked "$kind 那条没点到`"否`""
        Assert-Ec ($r.StdoutBytes.Length -eq 0) "$kind 那条被拒时往 stdout 写了东西"
    }
    Assert-Ec ($refused.implicit.Result.Exit -eq 6 -and $refused.explicit.Result.Exit -eq 6) `
        "答`"否`"之后两种写法都该是退出码 6：隐式 $($refused.implicit.Result.Exit) / 显式 $($refused.explicit.Result.Exit)"
    Assert-Ec ((Get-MachineShape $refused.implicit.Json) -eq (Get-MachineShape $refused.explicit.Json)) `
        "被拒绝时隐式与显式 stdout 的机器语义不同 / 隐式 [$((Get-MachineShape $refused.implicit.Json) -replace '\r?\n', ' / ')] / 显式 [$((Get-MachineShape $refused.explicit.Json) -replace '\r?\n', ' / ')]"
    foreach ($kind in @('implicit', 'explicit')) {
        $codes = @(Get-Codes $refused[$kind].Json.errors) -join ','
        Assert-Ec ($codes -eq 'capture.access_denied') "$kind 那条被拒的 code 是 [$codes]"
        Assert-Ec (-not ($codes -match 'cli\.missing_output')) "$kind 那条被拒又塌成缺少输出路径"
        $e = @($refused[$kind].Json.errors)[0]
        Assert-Ec ($e.stage -eq 'consent' -and $e.value -eq 'bitblt.screen') `
            "$kind 那条的授权诊断该带 stage=consent 与实际路径名：[$($e.stage)/$($e.value)]"
    }
    Stop-EcWindow -Window $eqOff

    # 8g) 写入断管：stdout 这条道本身写坏了（把 cmd 的 stdout 接到 CONIN$ 上，往它写就失败）。
    #     两种写法都必须原样报 I/O 失败、退出码 8，而且都不把成功的图片写成文件。
    #     批处理正文按仓库规矩只有 ASCII，路径经 set 递进去。
    $dirPipe = New-ShotDir 'eq-pipe'
    $pipeBat = Join-Path $dirPipe 'pipe-probe.bat'
    $pipeLines = @(
        '@echo off',
        ('set "EXE=' + $Exe + '"'),
        ('set "H=' + $eqHwnd + '"'),
        'call "%EXE%" --hwnd %H% --yes > CONIN$ 2> "implicit.json"',
        'echo implicit_exit=%ERRORLEVEL%',
        'call "%EXE%" --hwnd %H% --yes --out - > CONIN$ 2> "explicit.json"',
        'echo explicit_exit=%ERRORLEVEL%'
    )
    Set-Content -LiteralPath $pipeBat -Value $pipeLines -Encoding ascii
    $pipeRun = Invoke-EcProcess -FilePath (Join-Path $env:SystemRoot 'System32\cmd.exe') `
                                -TimeoutMs 120000 -WorkingDirectory $dirPipe `
                                -Arguments @('/d', '/c', $pipeBat)
    $pipeExit = @{}
    foreach ($l in (($pipeRun.Stdout -split "`r?`n") + ($pipeRun.Stderr -split "`r?`n"))) {
        if ($l -match '^(implicit|explicit)_exit=(\d+)$') { $pipeExit[$Matches[1]] = [int]$Matches[2] }
    }
    Assert-Ec ($pipeExit.ContainsKey('implicit') -and $pipeExit.ContainsKey('explicit')) `
        "没拿到批处理的回显：[$($pipeRun.Stdout.Trim()) $($pipeRun.Stderr.Trim())]"
    $pipeJson = @{}
    foreach ($kind in @('implicit', 'explicit')) {
        $f = Join-Path $dirPipe ($kind + '.json')
        Assert-Ec (Test-Path -LiteralPath $f) "$kind 那条没有把结果写到 stderr（断管时结果该整份留在 stderr）"
        $body = '' + (Get-Content -LiteralPath $f -Raw)
        $j = $null
        try { $j = $body | ConvertFrom-Json } catch { }
        Assert-Ec ($null -ne $j) "$kind 那条 stderr 上的结果不是 JSON：[$($body.Trim())]"
        Assert-Ec ($pipeExit[$kind] -eq 8) "$kind 那条 stdout 写坏时退出码该是 8，实际 $($pipeExit[$kind])"
        $codes = @(Get-Codes $j.errors) -join ','
        Assert-Ec ($codes -eq 'io.write_failed') "$kind 那条断管该报 io.write_failed，实际 [$codes]"
        $e = @($j.errors)[0]
        Assert-Ec ($e.stage -eq 'stdout' -and $e.value -eq '-') `
            "$kind 那条的断管诊断该定位到 stdout：[$($e.stage)/$($e.value)]"
        Assert-Ec ($j.captured -eq 0 -and @($j.images).Count -eq 0) "$kind 那条字节没送到 stdout 却报了图"
        $pipeJson[$kind] = $j
    }
    Assert-Ec ((Get-MachineShape $pipeJson.implicit) -eq (Get-MachineShape $pipeJson.explicit)) `
        "断管时隐式与显式 stdout 的机器语义不同 / 隐式 [$((Get-MachineShape $pipeJson.implicit) -replace '\r?\n', ' / ')] / 显式 [$((Get-MachineShape $pipeJson.explicit) -replace '\r?\n', ' / ')]"
    # 隐式那条在断管时才说得出"给个 --out 就能绕开管道"；显式那条本来就选定了这条路，不补这句
    Assert-Ec (Get-Field @($pipeJson.implicit.errors)[0] 'hint') '隐式断管那条该带上"改成显式 --out"的 hint'
    Assert-Ec (-not (Get-Field @($pipeJson.explicit.errors)[0] 'hint')) '显式 --out - 那条不该出现同一句 hint'
    Assert-Ec (@(Get-ChildItem -LiteralPath $dirPipe -File -Filter '*.png').Count -eq 0) `
        '断管的两次里有一把成功的图片自动写成了文件'
    Stop-EcWindow -Window $eqOk

    # =========================================================================
    Write-Host "`n=== 9) 注入不了的两项（如实记未验证） ==="
    # =========================================================================
    Skip-Ec '后端真的抛出异常（而非返回失败）没有注入点：本机能稳定造出的异常路径已由第 5、6 节的返回失败覆盖'
    Skip-Ec '进程被强杀在写图与写结果之间时无法承诺 JSON 送达，这是文档里写明的边界'
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  产物保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '标准流与结构化结果')
