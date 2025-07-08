<#
.SYNOPSIS
    真机冒烟测试：起一个本测试自有的窗口，用 ECAPTURE 截它，校验产出的图片，然后只收尾自己起的窗口。
.DESCRIPTION
    目标窗口由 tests\helper\ec_window.cs 编出的 ecwindow.exe 建立：测试握着它的 PID 与 HWND，
    截图只针对这一个窗口，既不按进程名去找目标，结束时也不会去结束别人的应用。
    画面内容是一整块签名色 + 16 条颜色带，所以"尺寸对但全黑/全白"这种假成功在这里会被判失败。
    图片写到本次运行专属的临时目录，收尾时只删这一个目录。
    另外验证一次 `-o -`：图片字节走 stdout、JSON 走 stderr，两个通道不混流。
.EXAMPLE
    .\tests\smoke.ps1
    .\tests\smoke.ps1 -Format jpg
    .\tests\smoke.ps1 -Keep          # 保留截图与临时目录以便人眼看
#>
param(
    [string]$Exe,
    [string]$Format = 'png',
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$run = New-EcRunDir -Tag 'smoke'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
$class = "ec-smoke-$tag"
$title = "冒烟测试窗口 $class"
Write-Host "本次临时目录：$($run.Path)"

try {
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '160,160,640,500' -Seed 2
    Write-Host ("目标窗口 PID={0} HWND={1} 类={2}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd), $class)

    # ---------- 1) 写到文件 ----------
    Write-Host "`n=== 截到文件并校验图片 ==="
    $img = Get-EcRunFile -RunDir $run -Name "target.$Format"
    $r = Invoke-Ecapture -Arguments @('--pid', [string]$window.Pid, '--class', $class,
                                      '--format', $Format, '--out', $img)
    Assert-Ec ($r.Exit -eq 0) "截图退出码 $($r.Exit)，应为 0（stderr: $($r.Stderr)）"
    $o = $null
    try { $o = $r.Stdout | ConvertFrom-Json } catch { Assert-Ec $false "输出不是合法 JSON：$_" }
    Assert-Ec ($o -and $o.captured -eq 1) "captured=$($o.captured)，期望 1"
    Assert-Ec (Test-Path -LiteralPath $img) "文件没写出来：$img"
    $img0 = @($o.images)[0]
    Assert-Ec ($img0.pid -eq $window.Pid -and $img0.hwnd -eq (Get-EcHwndHex $window.Hwnd)) `
        "JSON 归属不是本次窗口：pid=$($img0.pid) hwnd=$($img0.hwnd)"
    Assert-Ec ($img0.class -eq $class -and $img0.title -eq $title) "JSON 的 class/title 与本次窗口不符"

    if ($Format -eq 'png') {
        $bytes = [IO.File]::ReadAllBytes($img)
        Assert-Ec ($bytes[0] -eq 0x89 -and $bytes[1] -eq 0x50) '不是合法 PNG 签名'
        $pngW = [int]$bytes[16] * 16777216 + [int]$bytes[17] * 65536 + [int]$bytes[18] * 256 + [int]$bytes[19]
        $pngH = [int]$bytes[20] * 16777216 + [int]$bytes[21] * 65536 + [int]$bytes[22] * 256 + [int]$bytes[23]
        Write-Host ("PNG 头解析: {0}x{1}   JSON 报告: {2}x{3}   {4} 字节" -f `
                   $pngW, $pngH, $img0.width, $img0.height, $bytes.Length) -ForegroundColor DarkGray
        Assert-Ec ($pngW -eq $img0.width -and $pngH -eq $img0.height) 'JSON 尺寸与图片实际尺寸不一致'
        Assert-Ec ($pngW -ge 50 -and $pngH -ge 50) "尺寸过小（$pngW x $pngH），窗口可能没被真正抓到"

        # 内容检查：尺寸对但全黑/全白同样是无有效帧，必须看像素
        $stats = Get-EcImageStats -Path $img -Step 7
        Write-Host ("采样 {0} 点：不同颜色 {1} 种，主色 {2}" -f `
                   $stats.Samples, $stats.Colors, $stats.TopDominant) -ForegroundColor DarkGray
        Assert-Ec ($stats.Colors -ge 12) "画面颜色只有 $($stats.Colors) 种，抓到的是空帧/纯色"
        $mine = New-EcSignatureKey -Seed 2
        $d = Get-EcColorDistance -A $stats.TopDominant -B $mine
        Assert-Ec ($d -le 32) "画面主色 $($stats.TopDominant) 离本次窗口的签名色 $mine 太远（$d）"
    } else {
        $bytes = [IO.File]::ReadAllBytes($img)
        Write-Host ("输出 {0} 字节，格式 {1}" -f $bytes.Length, $Format) -ForegroundColor DarkGray
        Assert-Ec ($bytes.Length -gt 1024) "输出只有 $($bytes.Length) 字节，可能是空图"
    }

    # ---------- 2) 写到标准输出：二进制不得被转码，JSON 必须整体改走 stderr ----------
    Write-Host "`n=== -o - ：图片字节占 stdout，JSON 走 stderr ==="
    $p = Invoke-EcProcess -FilePath $Exe -TimeoutMs 60000 -Arguments @(
        '--pid', [string]$window.Pid, '--class', $class, '--format', $Format, '-o', '-')
    $json = $null
    try { $json = $p.Stderr | ConvertFrom-Json } catch { }
    $b = $p.StdoutBytes
    Assert-Ec ($p.Exit -eq 0) "管道那一次退出码 $($p.Exit)，应为 0"
    Assert-Ec ($b.Length -gt 100) "stdout 只有 $($b.Length) 字节"
    Assert-Ec ($p.Stderr.Trim().StartsWith('{') -and $json) 'stderr 里没有合法 JSON'
    Assert-Ec ($json -and $json.captured -eq 1) "stderr 的 JSON captured=$($json.captured)，期望 1"
    if ($Format -eq 'png') {
        Assert-Ec ($b[0] -eq 0x89 -and $b[1] -eq 0x50 -and $b[2] -eq 0x4E -and $b[3] -eq 0x47) `
            'stdout 开头不是 PNG 签名（可能被转码了）'
        $pipe = Get-EcRunFile -RunDir $run -Name 'pipe.png'
        [IO.File]::WriteAllBytes($pipe, $b)
        $ps = Get-EcImageStats -Path $pipe -Step 7
        Assert-Ec ($ps -and $ps.Colors -ge 12) "管道里的图颜色只有 $(if($ps){$ps.Colors}else{'-'}) 种"
        $same = ($ps.Width -eq $img0.width -and $ps.Height -eq $img0.height)
        Assert-Ec $same "管道图 $($ps.Width)x$($ps.Height) 与文件图 $($img0.width)x$($img0.height) 不一致"
    }
    Write-Host ("  管道：stdout {0} 字节，stderr JSON captured={1}" -f $b.Length, $json.captured) -ForegroundColor DarkGray
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run -Quiet } else { Write-Host "  截图保留在 $($run.Path)" }
}

exit (Complete-EcSuite -Title '冒烟测试')
