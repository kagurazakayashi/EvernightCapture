<#
.SYNOPSIS
    交付那一步（写文件 / 写标准输出）与"期限合规"分开记账的回归判据：离线编排层 + 真机那条被堵住的管道。
.DESCRIPTION
    这一套要钉住的是同一件事的两头：**图到底有没有交出去**，与**这一批有没有在预算内结束**。
    过去只有前一头被记下来 —— 一次跨过时限的慢盘写、或者一段把管道挤满之后才排干的标准输出，
    会以"完全成功"的姿态结束（标准输出那一条连 elapsedMs 都量在写出之前，那段等待直接从报告里
    消失），而 README 承诺的是"没有中断点的那一步结束后也要核对预算"。

    判据分两层：

      1) 离线层：build\ecapture-delivery-tests.exe（源码 tests\delivery_state.cpp）。判的是生产
         那两份本体：src\Delivery.cpp 的交付编排 + src\FileSave.cpp 的原子写。出口、时钟与一道
         计数闸门（全局 operator new）注入进来，于是"提交之后预算才跨""半段流留在管道里"
         "改名那一瞬间撞上 --no-overwrite""图已经收不回来而记账刚好缺内存"这些都排得出
         确定的先后。文件走真磁盘，所以每一判都同时核对**磁盘事实**（文件在不在、多少字节、
         旧文件有没有被破坏、目录里有没有多余条目）与 **JSON 那一头**（images 条目、notes、
         errors 的码与 stage、以及退出码），而不是只断言预算读数。
         十五节覆盖：按时完成、开工前已超时、文件提交后跨限、标准输出完整写后跨限、半段 stdout、
         一个字节都没出去、--no-overwrite 撞名、写失败之后才看见期限（真实原因不被超时覆盖）、
         改名失败、第一张已交付第二张不开工、最后一张慢写、没设预算、入账只一次、
         输出之前分配失败（没开始写也就没记任何账）、提交之后才缺内存（已落地的图与提示不许
         从报告里消失，调用方手里那一步的交付事实也说得出阶段）。
      2) 真机层：把标准输出那一头真的堵住。父侧只读走第一块就不再读，于是子进程卡在 WriteFile 里
         （窗口内容那一路，带 --yes，不弹确认框、也不拍任何桌面像素）：
         * 2a) 等过预算之后再把管道排干 —— 图应当**照常交付**：captured=1、images[0].bytes 与
           父侧实际收到的字节数逐字节相同，另外一条 io.timeout（stage=stdout），退出码按部分成功给。
           超时不许把已经到达的字节说成没到达，也不许把文件删掉。
         * 2b) 等过预算之后直接关掉读的那一端 —— 那一次写就地失败，这一张**不算交付**：
           captured=0、一条 io.write_failed（stage=stdout，带断管的 Win32 原值），退出码 8。
           父侧收到的那一段以 BMP 头开头 = 管道里确实留下过半段流，而工具没有把它当成一张完成的图。

    本机判不了的两条如实记未验证，不伪造：
      * 真慢盘上的提交（把用户机器上的存储设置改慢不在允许范围内）—— 文件那一路的"提交后跨限"
        由离线层用真 FileSave + 假时钟判，那条现场在这里造不出来。
      * 2a / 2b 万一没堵住（这张图小到一次就写完了）：那半截前提不成立，记 SKIP 说明原因，
        不算通过也不算失败。
.EXAMPLE
    .\tests\delivery.ps1
    .\tests\delivery.ps1 -SkipState      # 只跑真机层
    .\tests\delivery.ps1 -SkipReal       # 只跑离线判据层
    .\tests\delivery.ps1 -Keep           # 保留临时目录以便人眼看
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
$stateExe = Join-Path $root 'build\ecapture-delivery-tests.exe'

$run = New-EcRunDir -Tag 'delivery'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

$budgetMs = 1500        # 自动处理预算：写得比它慢就是"提交之后预算才跨"
$holdMs = 2600          # 按住管道不读的那一段，明显长于预算

# 把一次交付的整条链跑到"卡在标准输出里"：起进程、读走第一块、按 $holdMs 不读，
# 然后把剩下的字节排干（-CloseReader 时改成关掉读的那一端，让那次写就地失败）。
function Invoke-EcStdoutHold {
    param(
        [Parameter(Mandatory)][string[]]$Arguments,
        [switch]$CloseReader
    )

    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if (Test-EcArgumentListSupported) {
        foreach ($a in $Arguments) { [void]$psi.ArgumentList.Add($a) }
    } else {
        $psi.Arguments = Format-EcWindowsArgv -Arguments $Arguments
    }

    $proc = [Diagnostics.Process]::Start($psi)
    $bytes = @()
    $stderr = ''
    $exit = -1
    try {
        $pipe = $proc.StandardOutput.BaseStream
        $buf = New-Object byte[] 65536
        $ms = New-Object IO.MemoryStream
        $n = $pipe.Read($buf, 0, 4096)          # 只读走第一块，剩下的都堆在那条管道里
        if ($n -gt 0) { [void]$ms.Write($buf, 0, $n) }
        Start-Sleep -Milliseconds $holdMs        # 这一段等待必须长过预算
        if ($CloseReader) {
            $proc.StandardOutput.Close()         # 关掉读的那一端：那次写从此没有对面
        } else {
            while (($n = $pipe.Read($buf, 0, $buf.Length)) -gt 0) { [void]$ms.Write($buf, 0, $n) }
        }
        # JSON 走 stderr（图片占了 stdout），一次一张的量很小，不会因为没人读而堵住子进程。
        $stderr = $proc.StandardError.ReadToEnd()
        $proc.WaitForExit(30000) | Out-Null
        $exit = $proc.ExitCode
        $bytes = $ms.ToArray()
    } finally {
        if (-not $proc.HasExited) { Stop-EcOwnProcess -Process $proc -ExpectedPath $Exe | Out-Null }
        $proc.Dispose()
    }
    return [pscustomobject]@{ Bytes = $bytes; Stderr = $stderr; Exit = $exit }
}

try {
    # =========================================================================
    Write-Host "`n=== 1) 离线层：交付编排与期限合规（测生产函数本体 + 真磁盘写入） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '交付编排的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            Write-Host '  没有 build\ecapture-delivery-tests.exe，先跑一次 .\build.ps1' -ForegroundColor DarkGray
            & (Join-Path $root 'build.ps1')
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "交付编排判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '共 (\d+) 项检查，失败 (\d+)')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "交付编排判据有失败项：$tail"
        Assert-Ec ([int]$m.Groups[1].Value -ge 160) "交付编排判据条数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    if ($SkipReal) {
        Skip-Ec '真机被堵住的管道' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'   # 只跑离线层时直接进 finally 收尾
    }

    # =========================================================================
    Write-Host "`n=== 2) 真机层：把标准输出堵住（窗口内容那一路，不弹确认框） ==="
    # =========================================================================
    $class = "ec-delivery-$tag"
    $title = "交付期限测试窗口 $class"
    # 1200×900 的窗口拍成 BMP 是 4 MB 上下，一定塞不进那条管道 —— 子进程必然卡在 WriteFile 里。
    # 走 --capture wgc 那条窗口内容路径 + --yes：读的是这扇窗口自己的画面，不碰桌面像素。
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '120,120,1320,1020' -Seed 5
    Write-Host ("窗口 PID={0} HWND={1}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd))
    Start-Sleep -Milliseconds 500

    $baseArgs = @('--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes',
                  '--format', 'bmp', '--timeout-ms', [string]$budgetMs, '--out', '-')

    # -------------------------------------------------------------------------
    Write-Host '--- 2a) 排干管道：图照常交付，另记一条 io.timeout ---'
    $late = Invoke-EcStdoutHold -Arguments $baseArgs
    $o = $null
    try { $o = $late.Stderr | ConvertFrom-Json } catch { }
    if ($null -eq $o) {
        Assert-Ec $false "读不出 2a 的结果 JSON（exit=$($late.Exit) stderr=$($late.Stderr.Trim())）"
    } else {
        $codes = @($o.errors | ForEach-Object { $_.code })
        if ($codes -notcontains 'io.timeout') {
            # 前提没成立就不假装判过：这张图小到了子进程一次就写完，管道根本没堵住。
            Skip-Ec '提交之后预算才跨（真机标准输出）' "没堵住管道：exit=$($late.Exit) 收到 $($late.Bytes.Length) 字节"
        } else {
            $img = @($o.images)[0]
            Write-Host ("  errors[0] = {0}" -f (@($o.errors)[0] | ConvertTo-Json -Compress))
            Write-Host ("  images[0].file/bytes/elapsedMs = {0} / {1} / {2}" -f `
                       $img.file, $img.bytes, $img.elapsedMs)
            Assert-Ec ($late.Exit -eq 7) "已经到达的图不许为了一个超时退出码被说成没交付（exit=$($late.Exit)）"
            Assert-Ec ($o.captured -eq 1) "captured 与实际到达 stdout 的字节不打脸"
            Assert-Ec (@($o.errors).Count -eq 1) "只记那一条期限错误"
            Assert-Ec ($codes -contains 'io.timeout') "带一条 io.timeout"
            $err = @($o.errors | Where-Object { $_.code -eq 'io.timeout' })[0]
            Assert-Ec ($err.stage -eq 'stdout') "那条 io.timeout 的 stage 是 stdout"
            Assert-Ec ($err.backend -eq 'wgc') "backend 写的是真正出图那条通道"
            Assert-Ec ($err.target -eq (Get-EcHwndHex $window.Hwnd).ToLower() -or
                       $err.target -eq (Get-EcHwndHex $window.Hwnd)) "错误里认得出是哪个目标"
            # 交付事实两头对照：父侧真收到的字节 == JSON 里报的那一个数，而且真是一段 BMP。
            Assert-Ec ($img.bytes -eq $late.Bytes.Length) "images[0].bytes = 父侧实际收到的字节数（$($late.Bytes.Length)）"
            Assert-Ec ($late.Bytes.Length -gt 100000 -and $late.Bytes[0] -eq 0x42 -and $late.Bytes[1] -eq 0x4D) `
                "到达的是完整一段 BMP（头两字节 BM）"
            Assert-Ec ($img.elapsedMs -ge $budgetMs) "elapsedMs 含写标准输出的那段等待（实测 $($img.elapsedMs) ms）"
        }
    }

    # -------------------------------------------------------------------------
    Write-Host '--- 2b) 关掉读的那一端：半段流不算交付，原因归那一次写 ---'
    $broken = Invoke-EcStdoutHold -Arguments $baseArgs -CloseReader
    $p = $null
    try { $p = $broken.Stderr | ConvertFrom-Json } catch { }
    if ($null -eq $p) {
        Assert-Ec $false "读不出 2b 的结果 JSON（exit=$($broken.Exit) stderr=$($broken.Stderr.Trim())）"
    } else {
        $bcodes = @($p.errors | ForEach-Object { $_.code })
        if ($p.captured -eq 1) {
            # 子进程在关掉那一端之前就已经写完了：前提没成立，不拿它冒充"半段流"判过。
            Skip-Ec '半段标准输出（真机）' "关掉读的一端之前就已经发完：$($p.captured) 张"
        } else {
            Assert-Ec ($broken.Exit -eq 8) "没交付的写失败用 I/O 退出码（exit=$($broken.Exit)）"
            Assert-Ec ($p.captured -eq 0) "半段流不进 images"
            Assert-Ec (@($p.images).Count -eq 0) "images 是空的"
            Assert-Ec ($bcodes -contains 'io.write_failed') "原因归那一次写自己（$($bcodes -join ',')）"
            Assert-Ec ($bcodes -notcontains 'io.timeout') "没被随后看见的超时覆盖掉"
            $berr = @($p.errors)[0]
            Assert-Ec ($berr.stage -eq 'stdout') "stage 是标准输出那一段"
            Assert-Ec ($berr.win32 -eq 109) "断管的 Win32 原值留得住（实测 $($berr.win32)）"
            Assert-Ec ($broken.Bytes.Length -ge 2 -and $broken.Bytes[0] -eq 0x42 -and $broken.Bytes[1] -eq 0x4D) `
                "父侧确实收到过一段 BMP 开头（半段流留在管道里这一事实）"
        }
    }

    Stop-EcWindow -Window $window
} catch {
    if ($_.Exception.Message -ne '__SKIP_REAL__') { throw }
} finally {
    if (-not $Keep) { Remove-EcRunDir $run } else { Write-Host "保留临时目录：$($run.Path)" }
    Stop-EcOwnedWindows
}

exit (Complete-EcSuite -Title '交付与期限合规 ')
