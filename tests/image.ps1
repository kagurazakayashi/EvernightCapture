<#
.SYNOPSIS
    帧内存校验与单色质量判断的判据：离线层手工摆像素排布，真机层用自有单色窗口验"单色不改来路、不升级授权"。
.DESCRIPTION
    这个功能的两半各自要判的东西不同，所以分两层：

      1) 离线层：build\ecapture-image-tests.exe（源码 tests\image_state.cpp）——
         竖条纹、单行多色、横条纹、棋盘、只有 alpha 不同、只在旧采样点之外变化、带行末填充、
         纯色；以及空图、短缓冲区、过小/过大 stride、超大边长、stride*height 超过整帧上限、
         越界裁剪与 32 位相加绕回。这些像素排布**只能手工摆**，真机截图造不出来，
         而在测试里再抄一份算法当判据正是要修的错 —— 所以测的是生产函数本体
         （src/ImageOps.cpp 与 src/CaptureCommon.cpp 的那几道检查）。
      2) 真机层：本测试自建的单色窗口（ecwindow.exe --mode solid）。这次修的正是
         "画面单色被当成没合成出来"：旧实现里 dwm 判平之后会升级到"把宿主窗口盖到目标位置上
         拷屏幕"那条退路 —— 那读的是桌面像素，要另外问一次人。现在这条升级只跟着 API 失败走，
         单色只留一条 note.frame_uniform 质量提示。判据就是：带 --yes 的单色窗口截图
         **一张框都不许弹**，来路仍写 window，且图里有那条质量提示。

    测试一律用自建的窗口，只收尾自己起的那个进程；需要真人确认的桌面通道（bitblt / duplication）
    默认记 SKIP，不加 -SimulateConsent 就绝不去动桌面像素那条路。
.EXAMPLE
    .\tests\image.ps1
    .\tests\image.ps1 -SkipState            # 只跑真机那层
    .\tests\image.ps1 -SimulateConsent      # 加上它才代答"否"，验桌面通道在单色画面上照样要问人
    .\tests\image.ps1 -Keep                 # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$SimulateConsent,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-image-tests.exe'
$IDNO = 7

$run = New-EcRunDir -Tag 'image'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

function Json-Of($r) {
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $o = $null
    try { $o = $body | ConvertFrom-Json } catch { }
    return $o
}

function Note-By($o, [string]$code) {
    if (-not $o) { return $null }
    return @($o.notes | Where-Object { $_.code -eq $code })[0]
}

function Error-Codes($o) {
    if (-not $o -or $null -eq $o.errors) { return @() }
    return @($o.errors | ForEach-Object { $_.code })
}

# 单色窗口用的那个颜色：判据里比的是 ARGB 那串十六进制，四种语言的文案都带着它
$SOLID = '2E7D32'
$SOLID_ARGB = "0xFF$SOLID"

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：像素排布与形状判据（测生产函数本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '帧形状与单色判定的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-EcStateBinary -Root $root -Exe $stateExe)) {
            Assert-Ec $false "缺少离线判据程序 $stateExe：源码树里先跑一次 .\build.ps1；安装目录里它必须随包发出（说明打包的依赖闭包没兜住）"
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "帧判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        $failed = [int]$m.Groups[2].Value
        $passed = [int]$m.Groups[1].Value
        Assert-Ec ($failed -eq 0) "帧判据有失败项：$tail"
        Assert-Ec ($passed -ge 120) "帧判据通过数不对劲（$passed），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    # ---------- 自有窗口：一扇单色、一扇带签名色的多色 ----------
    $solidClass = "ec-img-solid-$tag"
    $solid = Start-EcWindow -RunDir $run -Class $solidClass -Mode solid -Color $SOLID `
        -Rect '200,200,520,440' -MaxLifeSeconds 600
    $solidHwnd = Get-EcHwndHex $solid.Hwnd
    $multiClass = "ec-img-multi-$tag"
    $multi = Start-EcWindow -RunDir $run -Class $multiClass -Title "帧判据窗口 $multiClass" -Seed 9 `
        -Rect '700,200,1020,440' -MaxLifeSeconds 600
    $multiHwnd = Get-EcHwndHex $multi.Hwnd
    Write-Host ("单色窗口 PID={0} HWND={1}   多色窗口 PID={2} HWND={3}" -f `
        $solid.Pid, $solidHwnd, $multi.Pid, $multiHwnd)

    function Invoke-WindowShot {
        param([string]$Channel, [string]$Name, [string]$Hwnd, [string[]]$Extra = @())
        $path = Get-EcRunFile -RunDir $run -Name "$Name.png"
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $argv = @('--hwnd', $Hwnd, '--capture', $Channel, '--yes', '--lang', 'zh-CN', '--out', $path) + $Extra
        $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -ExpectNoDialog
        [pscustomobject]@{
            Exit = $r.Exit; Dialog = $r.Dialog; Clicked = $r.Clicked
            Out = $r; Path = $path; Json = (Json-Of $r)
            Stats = if (Test-Path -LiteralPath $path) { Get-EcImageStats -Path $path -Step 3 } else { $null }
        }
    }

    # =========================================================================
    Write-Host "`n=== 1) 单色窗口 + --yes：窗口内容通道一张框都不许弹（旧实现在这里升级到桌面） ==="
    # =========================================================================
    foreach ($ch in @('wgc', 'dwm', 'printwindow')) {
        $r = Invoke-WindowShot -Channel $ch -Name "solid_$ch" -Hwnd $solidHwnd
        $img = if ($r.Json) { @($r.Json.images)[0] } else { $null }
        $note = Note-By $r.Json 'note.frame_uniform'
        Write-Host ("  {0,-12} exit={1} 弹框={2} path={3} scope={4} 颜色={5} 提示={6}" -f `
            $ch, $r.Exit, $r.Dialog, $img.path, $img.scope, $r.Stats.Colors, [bool]$note)

        Assert-Ec (-not $r.Dialog) `
            "$ch 带 --yes 截单色窗口弹了确认框：画面单色不是升级到桌面取图的理由（这一次读到的会是屏幕上那块区域）"
        Assert-Ec ($r.Exit -eq 0) "$ch 截单色窗口退出码 $($r.Exit)，应为 0：$($r.Out.Stderr)"
        Assert-Ec ($img -and $r.Json.captured -eq 1) "$ch 没交出图片"
        Assert-Ec ($img.scope -eq 'window') "$ch 的 scope 该是 window，实际 $($img.scope)"
        Assert-Ec ((Error-Codes $r.Json) -notcontains 'capture.frame_invalid') "$ch 把一帧合法的图判成了形状不合法"
        if ($ch -eq 'dwm') {
            # 这就是这次修的那条：判平之后不再走 dwm.screen
            Assert-Ec ($img.path -eq 'dwm.thumbnail') "dwm 的来路该是 dwm.thumbnail，实际 $($img.path)（升级到屏幕取图了？）"
        }
        Assert-Ec ([bool]$note) "$ch 的整幅单色图没留 note.frame_uniform 质量提示"
        Assert-Ec ($note.message -match [regex]::Escape($SOLID_ARGB)) `
            "质量提示里写的颜色不是 $SOLID_ARGB：$($note.message)"
        Assert-Ec ($note.backend -eq $ch -and $note.stage -eq 'capture') `
            "质量提示没带上通道与阶段：$($note.backend)/$($note.stage)"
        Assert-Ec ($note.target -eq $solidHwnd) "质量提示的 target 不是本次窗口：$($note.target)"
        Assert-Ec ($r.Stats.Colors -eq 1) "$ch 的图被判成单色，但图本身有 $($r.Stats.Colors) 种颜色（判据与画面互相打脸）"
    }

    # =========================================================================
    Write-Host "`n=== 2) 多色窗口：同一条通道不许冒出单色提示（阴性对照） ==="
    # =========================================================================
    $r = Invoke-WindowShot -Channel 'wgc' -Name 'multi_wgc' -Hwnd $multiHwnd
    $img = if ($r.Json) { @($r.Json.images)[0] } else { $null }
    $note = Note-By $r.Json 'note.frame_uniform'
    Write-Host ("  wgc 多色窗口 exit={0} 颜色={1} 提示={2}" -f $r.Exit, $r.Stats.Colors, [bool]$note)
    Assert-Ec ($r.Exit -eq 0 -and $r.Stats.Colors -ge 12) `
        "多色窗口没截成或画面不对劲（exit=$($r.Exit) 颜色=$($r.Stats.Colors)）"
    Assert-Ec (-not $note) "多色画面被误判成单色，留了一条 note.frame_uniform"
    $d = Get-EcColorDistance -A $r.Stats.TopDominant -B (New-EcSignatureKey -Seed 9)
    Assert-Ec ($d -le 32) "多色窗口的主色与签名色对不上（距离 $d），这一轮的画面不是本次窗口的"

    # =========================================================================
    Write-Host "`n=== 3) --quiet 抑制提示，但不抑制来路与覆盖范围 ==="
    # =========================================================================
    $r = Invoke-WindowShot -Channel 'wgc' -Name 'quiet_wgc' -Hwnd $solidHwnd -Extra @('--quiet')
    $img = if ($r.Json) { @($r.Json.images)[0] } else { $null }
    # 没有 notes 这个键时 $r.Json.notes 是 $null，而 @($null).Count 是 1 —— 先判键在不在
    $hasNotes = $r.Json -and $r.Json.PSObject.Properties.Name -contains 'notes'
    $noteCount = if ($hasNotes) { @($r.Json.notes).Count } else { 0 }
    Write-Host ("  quiet exit={0} notes={1} path={2} scope={3}" -f `
        $r.Exit, $noteCount, $img.path, $img.scope)
    Assert-Ec ($r.Exit -eq 0) "--quiet 那次截图失败（exit=$($r.Exit)）"
    Assert-Ec ($noteCount -eq 0) "--quiet 还留着 notes（$noteCount 条）"
    Assert-Ec ($img.path -and $img.scope -eq 'window') "--quiet 把 path/scope 这两个隐私判据一起藏了：$($img.path)/$($img.scope)"

    # =========================================================================
    Write-Host "`n=== 4) 桌面通道：单色画面照样要问人（--yes 不生效） ==="
    # =========================================================================
    if (-not $SimulateConsent) {
        Skip-Ec '桌面通道在单色画面上的授权判据' '默认不代答确认框（那等于替人同意拍到桌面像素）；要跑请加 -SimulateConsent'
    } else {
        foreach ($ch in @('bitblt')) {
            $path = Get-EcRunFile -RunDir $run -Name "solid_desktop_$ch.png"
            Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
            $argv = @('--hwnd', $solidHwnd, '--capture', $ch, '--yes', '--lang', 'zh-CN', '--out', $path)
            $r = Invoke-EcConsentShot -Exe $Exe -Arguments $argv -Answer $IDNO
            Assert-Ec $r.Dialog "$ch 是桌面像素通道，截单色窗口却没弹确认框"
            Assert-Ec ($r.Exit -eq 6) "$ch 答`"否`"的退出码 $($r.Exit)，应为 6"
            Assert-Ec (-not (Test-Path -LiteralPath $path)) "$ch 被拒绝之后还是把文件写出来了"
        }
    }

    # =========================================================================
    Write-Host "`n=== 5) 本机造不出来的判据（照实记录，不算通过） ==="
    # =========================================================================
    Skip-Ec '分配失败的注入（缓冲区resize那一步）' '生产代码里没有分配故障注入点；形状与上限的判据本身在离线层已经判过，超限那一条走的是分配之前的检查'
    Skip-Ec 'GPU 那一段的 Unmap 与设备丢失' '需要真纹理；离线层测的是同一道形状判据，取图通道留给 tests\channels.ps1'

    Write-Host "`n=== 收尾 ==="
    Stop-EcWindow -Window $solid
    Stop-EcWindow -Window $multi
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '帧校验与质量判断')
