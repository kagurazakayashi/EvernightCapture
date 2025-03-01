<#
.SYNOPSIS
    真机冒烟测试：起一个记事本窗口，用 ECAPTURE 截它，校验产出的图片，然后关掉记事本。
.DESCRIPTION
    会短暂在你的桌面上开一个记事本窗口（结束或失败时自动关闭）。截图只针对该窗口，不抓整屏。
    输出图片写到临时目录，不留在这台机器上。
.EXAMPLE
    .\tests\smoke.ps1
    .\tests\smoke.ps1 -Keep
#>
param(
    [string]$Exe,
    [string]$Format = 'png',
    [switch]$Keep           # 保留截图文件以便人眼看
)

$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
if (-not $Exe) { $Exe = Join-Path $root 'build\ecapture.exe' }
if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe（先运行 .\build.ps1）" }
$Exe = (Get-Item -LiteralPath $Exe).FullName

$outDir = Join-Path ([IO.Path]::GetTempPath()) 'ecapture-smoke'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$stamp = Get-Date -Format 'HHmmss'
$img = Join-Path $outDir "notepad_$stamp.$Format"

$notepad = $null
try {
    $notepad = Start-Process notepad.exe -PassThru
    Start-Sleep -Milliseconds 900      # 等窗口建好并出内容
    Write-Host "记事本 PID=$($notepad.Id)"

    $json = & $Exe --pid $notepad.Id --format $Format $img 2>&1 | Out-String
    $code = $LASTEXITCODE
    Write-Host "ECAPTURE 退出码 = $code"
    Write-Host $json.Trim()

    $o = $json | ConvertFrom-Json
    if ($code -ne 0) { throw "截图失败：退出码 $code" }
    if ($o.captured -ne 1) { throw "captured=$($o.captured)，期望 1" }
    if (-not (Test-Path $img)) { throw "文件没写出来：$img" }

    $bytes = [IO.File]::ReadAllBytes($img)
    $w = $o.images[0].width; $h = $o.images[0].height
    if ($Format -eq 'png') {
        if ($bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50) { throw '不是合法 PNG 签名' }
        $pngW = [int]$bytes[16] * 16777216 + [int]$bytes[17] * 65536 + [int]$bytes[18] * 256 + [int]$bytes[19]
        $pngH = [int]$bytes[20] * 16777216 + [int]$bytes[21] * 65536 + [int]$bytes[22] * 256 + [int]$bytes[23]
        Write-Host ("PNG 头解析: {0}x{1}   JSON 报告: {2}x{3}   {4} 字节" -f $pngW, $pngH, $w, $h, $bytes.Length)
        if ($pngW -ne $w -or $pngH -ne $h) { throw 'JSON 尺寸与图片实际尺寸不一致' }
        if ($w -lt 50 -or $h -lt 50) { throw "尺寸过小（$w x $h），窗口可能没被真正抓到" }

        # 内容检查：尺寸对但全黑/全白同样是无有效帧，必须看像素
        Add-Type -AssemblyName System.Drawing
        $bmp = [Drawing.Bitmap]::new($img)
        try {
            $colors = @{}
            $black = 0; $white = 0; $other = 0; $samples = 0
            for ($yy = 0; $yy -lt $bmp.Height; $yy += 7) {
                for ($xx = 0; $xx -lt $bmp.Width; $xx += 7) {
                    $c = $bmp.GetPixel($xx, $yy)
                    $samples++
                    $colors["$($c.R),$($c.G),$($c.B)"] = 1
                    if ($c.R -lt 8 -and $c.G -lt 8 -and $c.B -lt 8) { $black++ }
                    elseif ($c.R -gt 247 -and $c.G -gt 247 -and $c.B -gt 247) { $white++ }
                    else { $other++ }
                }
            }
            Write-Host ("像素采样 {0} 点：不同颜色 {1} 种，黑 {2} 白 {3} 其他 {4}" -f `
                       $samples, $colors.Count, $black, $white, $other)
            if ($colors.Count -lt 3) { throw '画面颜色过少，抓到的是空帧/黑屏' }
            if ($samples -gt 0 -and ($black -eq $samples -or $white -eq $samples)) {
                throw '画面纯色，抓到的是空帧'
            }
        } finally {
            $bmp.Dispose()
        }
    } else {
        Write-Host ("输出 {0} 字节，格式 {1}" -f $bytes.Length, $Format)
    }

    # 同一窗口再截一张到标准输出，验证 JSON 走 stderr 而图片走 stdout
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.Arguments = "--pid $($notepad.Id) --format $Format -o -"
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $pipeOut = $p.StandardOutput.BaseStream
    $mem = New-Object IO.MemoryStream
    $pipeOut.CopyTo($mem)
    $pipeErr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    $env1 = $pipeErr | ConvertFrom-Json
    Write-Host ("管道：stdout {0} 字节，stderr JSON captured={1}" -f $mem.Length, $env1.captured)
    if ($mem.Length -lt 100 -or $env1.captured -ne 1) { throw '管道输出不符合契约' }

    Write-Host "`n冒烟测试通过" -ForegroundColor Green
    if ($Keep) { Write-Host "截图保留在 $img" }
} finally {
    if ($notepad -and -not $notepad.HasExited) {
        Stop-Process -Id $notepad.Id -Force -ErrorAction SilentlyContinue
        Write-Host '已关闭记事本'
    }
    if (-not $Keep) { Remove-Item $img -ErrorAction SilentlyContinue }
}
