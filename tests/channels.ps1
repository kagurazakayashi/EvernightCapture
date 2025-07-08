<#
.SYNOPSIS
    真机通道对比测试：同一个窗口分别用 wgc / dwm / printwindow / bitblt / duplication / auto 截图，
    并做遮挡对照，检查每条通道拿到的画面是否符合它的能力。
.DESCRIPTION
    目标窗口与遮挡窗口都由 tests\helper\ec_window.cs 编出的 ecwindow.exe 建立，本测试握着
    它们的 PID 与 HWND：只截这两个窗口、只结束这两个进程，绝不按进程名去找目标或批量收尾，
    使用者本来开着的同名进程不会被牵连（同名邻居的完整判据在 tests\isolation.ps1）。
    两个窗口都置顶建立，于是"目标在最上层"与"目标被完全盖住"两种情形都不受桌面上其它窗口影响。

    判据不是"有没有报错"，而是画面内容：
      * 尺寸合理 + 颜色够多 + 主色对得上签名色  ->  真的抓到了本窗口的内容，而不是空帧或别人的画面
      * 遮挡测试里红色占比  ->  wgc / dwm / printwindow 应几乎无红（拿的是窗口自己的画面），
                              bitblt / duplication 应几乎全红（拿的是屏幕合成画面，只能看到遮挡物）
    通道清单是手写的，但会用 --help 的实际输出核对，漏改会直接失败。
    截图写到本次运行专属的临时目录，收尾时只删这一个目录。
.EXAMPLE
    .\tests\channels.ps1
    .\tests\channels.ps1 -Keep      # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$run = New-EcRunDir -Tag 'channels'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
$SEED = 6
Write-Host "本次临时目录：$($run.Path)"

try {
    # ---------- 目标窗口：本测试自有，类名带本次运行的盐，别的轮次撞不上 ----------
    $targetClass = "ec-chan-$tag"
    $targetTitle = "通道测试窗口 $targetClass"
    $target = Start-EcWindow -RunDir $run -Class $targetClass -Title $targetTitle -Seed $SEED `
        -Rect '200,200,680,540' -TopMost -MaxLifeSeconds 600
    Write-Host ("目标窗口 PID={0} HWND={1} 类={2}" -f $target.Pid, (Get-EcHwndHex $target.Hwnd), $targetClass)

    # 支持的通道清单（与 src/CliOptions.cpp 的 kCaptureValues 手工保持一致；
    # 下面那步会用 --help 的实际输出核对，漏改就在这里失败）
    $channels = @('wgc', 'dwm', 'printwindow', 'bitblt', 'duplication', 'auto')
    $advertised = ((& $Exe --help) | Select-String -Pattern '--capture, -C' | ForEach-Object { $_.Line }) `
        | ForEach-Object { [regex]::Matches($_, '(?<=\s|^)(wgc|dwm|printwindow|bitblt|duplication|auto)(?=[\s/(])') } |
        ForEach-Object { $_.Value } | Select-Object -Unique
    Write-Host ("--help 声明的通道：{0}" -f ($advertised -join ', '))
    Assert-Ec (@(Compare-Object $channels $advertised).Count -eq 0) `
        "测试清单与 --help 不一致：测试=$($channels -join ',') 帮助=$($advertised -join ',')"

    # 每条通道都截一次：归属必须是本次窗口，画面必须有本次窗口的内容
    function Invoke-ChannelShot {
        param([string]$Channel, [string]$Name)

        $path = Get-EcRunFile -RunDir $run -Name "$Name.png"
        Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue
        $r = Invoke-Ecapture -Arguments @('--hwnd', (Get-EcHwndHex $target.Hwnd), '--capture', $Channel,
                                         '--out', $path)
        $o = $null
        try { $o = $r.Stdout | ConvertFrom-Json } catch { }
        $img = @($o.images)[0]
        [pscustomobject]@{
            Exit = $r.Exit; Json = ($r.Stdout + $r.Stderr); Path = $path; Out = $img
            Info = Get-EcImageStats -Path $path -Step 5
        }
    }

    Write-Host "`n=== 未遮挡：每条通道都要截到窗口内容 ==="
    foreach ($ch in $channels) {
        $r = Invoke-ChannelShot -Channel $ch -Name "open_$ch"
        $m = $r.Info
        $d = if ($m) { Get-EcColorDistance -A $m.TopDominant -B (New-EcSignatureKey -Seed $SEED) } else { 9999 }
        Write-Host ("  {0,-14} exit={1} {2}x{3} colors={4} red={5} 签名距离={6} bytes={7}" -f `
                   $ch, $r.Exit, $m.Width, $m.Height, $m.Colors, $m.RedRatio, $d, $m.Bytes)
        Assert-Ec ($r.Exit -eq 0) "$ch 通道退出码 $($r.Exit)，应为 0"
        Assert-Ec ($m -and $m.Width -ge 200 -and $m.Height -ge 150) "$ch 通道画面过小：$($m.Width)x$($m.Height)"
        Assert-Ec ($m -and $m.Colors -ge 12) "$ch 通道颜色过少（$($m.Colors)），可能是空帧"
        Assert-Ec ($d -le 32) "$ch 通道主色 $($m.TopDominant) 与本次窗口签名色对不上（距离 $d）"
        Assert-Ec ($r.Out.hwnd -eq (Get-EcHwndHex $target.Hwnd) -and $r.Out.pid -eq $target.Pid) `
            "$ch 通道 JSON 归属不是本次窗口：$($r.Out.hwnd)/$($r.Out.pid)"
    }

    # 遮挡：在本窗口上压一个纯红不透明窗口。它必须是独立进程并自己泵消息，
    # 否则父脚本忙着截图时它不会重绘，屏幕上会出现"半红半原图"的假象。
    Write-Host "`n=== 放上纯红遮挡窗口（独立进程、自己泵消息） ==="
    $rect = Get-EcWindowRect -Hwnd $target.Hwnd
    $coverRect = '{0},{1},{2},{3}' -f ($rect.Left - 8), ($rect.Top - 8), ($rect.Right + 8), ($rect.Bottom + 8)
    $coverClass = "ec-cover-$tag"
    $cover = Start-EcWindow -RunDir $run -Class $coverClass -Mode solid -Rect $coverRect `
        -Color 'FF0000' -TopMost -MaxLifeSeconds 600
    Write-Host ("遮挡窗口 PID={0} 矩形={1}" -f $cover.Pid, $coverRect)
    Start-Sleep -Milliseconds 1200

    $seeThrough = @('wgc', 'dwm', 'printwindow')   # 拿的是窗口自己的画面，不该看到红色
    $screenOnly = @('bitblt', 'duplication')      # 拿的是屏幕合成画面，应该全是遮挡物
    $grouped = (($seeThrough + $screenOnly + 'auto') | Sort-Object) -join ','
    Assert-Ec ($grouped -eq (($channels | Sort-Object) -join ',')) `
        "遮挡分组没覆盖全部通道：$grouped"
    foreach ($ch in ($seeThrough + $screenOnly + 'auto')) {
        $r = Invoke-ChannelShot -Channel $ch -Name "covered_$ch"
        $m = $r.Info
        Write-Host ("  {0,-14} exit={1} {2}x{3} colors={4} red={5}" -f `
                   $ch, $r.Exit, $m.Width, $m.Height, $m.Colors, $m.RedRatio)
        Assert-Ec ($r.Exit -eq 0) "$ch 通道遮挡时退出码 $($r.Exit)，应为 0"
        Assert-Ec ($r.Out.pid -eq $target.Pid) "$ch 通道遮挡时截到了别的进程 $($r.Out.pid) 的窗口"
        if ($ch -in $seeThrough) {
            Assert-Ec ($m.RedRatio -lt 0.15) "$ch 应能截到被遮挡窗口的内容，实际红色占比 $($m.RedRatio)"
            $d = Get-EcColorDistance -A $m.TopDominant -B (New-EcSignatureKey -Seed $SEED)
            Assert-Ec ($d -le 32) "$ch 截到内容了但主色不是本次窗口的签名色（距离 $d）"
        } elseif ($ch -eq 'auto') {
            Assert-Ec ($m.RedRatio -lt 0.15) "auto 首选 wgc，不该退化成屏幕取图（红色占比 $($m.RedRatio)）"
        } else {
            Assert-Ec ($m.RedRatio -gt 0.6) "$ch 只能取屏幕画面，红色占比应远高于其它通道，实际 $($m.RedRatio)"
        }
    }

    # 撤掉遮挡：目标必须重新可截（证明上面那组的红色不是"整幅本来就红"）
    Write-Host "`n=== 撤掉遮挡：wgc 再截一次应当仍是本窗口内容 ==="
    Stop-EcWindow -Window $cover
    Start-Sleep -Milliseconds 800
    $back = Invoke-ChannelShot -Channel 'wgc' -Name 'uncovered_wgc'
    Assert-Ec ($back.Exit -eq 0 -and $back.Info.RedRatio -lt 0.15) `
        "撤掉遮挡后仍拍到红色（red=$($back.Info.RedRatio)）"
    $cover = $null

    # 已删除的方案必须在解析期就拒绝，不能留一个"能传但截不出东西"的取值
    Write-Host "`n=== 已删除的取值必须被拒绝 ==="
    foreach ($gone in 'magnification') {
        $r = Invoke-ChannelShot -Channel $gone -Name "removed_$gone"
        Write-Host ("  {0,-14} exit={1} -> {2}" -f $gone, $r.Exit, ($r.Json -replace '\s+', ' ').Trim())
        Assert-Ec ($r.Exit -eq 1) "$gone 退出码 $($r.Exit)，应为 1（解析期拒绝）"
        Assert-Ec ($r.Json -match 'cli\.unknown_capture_method') "$gone 应报 cli.unknown_capture_method"
        Assert-Ec (-not (Test-Path -LiteralPath $r.Path)) "$gone 不该写出文件"
    }

    # 输出名没有扩展名时工具会补一个（nofmt -> nofmt.png），改了文件名就必须发 note
    Write-Host "`n=== 补扩展名不能静默改文件名 ==="
    $noext = Get-EcRunFile -RunDir $run -Name 'nofmt'
    Remove-Item -LiteralPath $noext, "$noext.png" -ErrorAction SilentlyContinue
    $r = Invoke-Ecapture -Arguments @('--hwnd', (Get-EcHwndHex $target.Hwnd), '--out', $noext)
    $json = $r.Stdout + $r.Stderr
    Assert-Ec ((Test-Path -LiteralPath "$noext.png") -and ($json -match 'output_extension_appended')) `
        '没写出 nofmt.png 或没发 note.output_extension_appended'
    Assert-Ec (-not (Test-Path -LiteralPath $noext)) '不该留下没有扩展名的同名文件'
    Write-Host ("  nofmt.png={0} note={1} 原始名残留={2}" -f (Test-Path -LiteralPath "$noext.png"),
                [bool]($json -match 'output_extension_appended'), (Test-Path -LiteralPath $noext))

    # 收尾之后本次窗口应当都不在了
    Write-Host "`n=== 收尾只清本次对象 ==="
    Stop-EcWindow -Window $target
    Assert-Ec (-not (Test-EcProcessAlive -ProcessId $target.Pid)) '本次目标窗口进程没收干净'
    Assert-Ec ((Get-EcOwnedWindowCount) -eq 0) '本次登记清单没清空'
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '通道测试')
