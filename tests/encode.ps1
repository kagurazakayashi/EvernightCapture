<#
.SYNOPSIS
    跨平台图像处理与编码的回归判据：合成帧 → 裁剪 → 缩放 → 编码 → 容器核对 → 交付与历史。
.DESCRIPTION
    这一步（实现跨平台图像处理与编码）把编码那一段拆成"共用判据 + 各家平台自己写字节"：
    共用的是 src\Encoder.cpp（帧形状、紧凑行、来源色彩空间、格式支持、开工预算五道前置判据）
    与 src\core\CodecCapability.h（哪条编码路径带不带得回哪一种格式那张表）；
    平台层是 src\platform\windows\WinEncoder.cpp（Windows.Graphics.Imaging.BitmapEncoder）与
    Linux 那一份 src\platform\linux\PosixEncoder.cpp（libpng + libjpeg + 自写 BMP）。

    本脚本跑 build\ecapture-encode-tests.exe（源码 tests\encode_state.cpp）—— 它与 Linux 上
    ./test-all.sh 的 codec-encode 套件是**同一份判据源文件**，各自链自己那平台的编码实现，
    所以这里判的是 Windows 真编码器（WinRT BitmapEncoder）的实际行为：

      1) 登记表完整性：两条编码路径 × 五种格式每一格都登记过，没有 kUnregistered 兜底；
      2) 帧规范：行末填充不参与、编码前后事实字段不改变产物字节、确定性；
      3) 先裁剪后缩放、只缩不放、越界的 --roi 整条拒绝（不自动裁短）；
      4) 通道顺序 / alpha / 预乘：边界色块与透明像素逐条核对；
      5) HDR 边界：来源说不出是 8 位 sRGB 又没过 tone mapping 的帧，编码当场拒绝
         （capture.hdr_unverifiable），不把广色域像素硬按 SDR 写出去；
      6) 能力与实际请求一致：AvailableFormats()、登记表与"真编一次"三方同进同出；
      7) 预算：开工前拒一次、不交出半张图；
      8) 同一份编码字节供主交付与历史副本（注入出口与归档桩，判两边拿到的字节逐字相同）。

    Windows 侧本轮**没有解码回读**那一段：判据里的 PNG / BMP 容器结构核对（块边界、CRC32、
    IHDR / DIB 头字段）两个平台都跑，而像素级解码只在 Linux 用 libpng / libjpeg 实测。
    脚本把这条差别照实记成 Skip，不拿结构核对冒充像素核对。
.EXAMPLE
    .\tests\encode.ps1
    .\tests\encode.ps1 -SkipState    # 什么都不跑（与其它套件一致的开关）
#>
param(
    [string]$Exe,
    [switch]$SkipState
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-encode-tests.exe'

try {
    # =========================================================================
    Write-Host "`n=== 1) 离线层：编码链的形状、颜色、格式与预算（测真编码器，不碰桌面） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '图像处理与编码的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-EcStateBinary -Root $root -Exe $stateExe)) {
            Assert-Ec $false "缺少离线判据程序 $stateExe：源码树里先跑一次 .\build.ps1"
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 180000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 2 | Select-Object -First 1)
        Assert-Ec ($st.Exit -eq 0) "图像处理与编码判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "图像处理与编码判据有失败项：$tail"
        # 这一套覆盖登记表 / 形状 / 裁剪缩放 / 颜色与 alpha / HDR 边界 / 能力一致 / 预算 / 交付共享字节，
        # 数量掉下 100 说明有人删了判据而不是补了判据。
        Assert-Ec ([int]$m.Groups[1].Value -ge 100) "图像处理与编码判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    # =========================================================================
    Write-Host "`n=== 2) 编码器接线：Windows 这一路仍然只有 WinRT 那一份 ==="
    # =========================================================================
    # 编码从共用层分派到平台实现，所以"Windows 上到底链了哪一份"要能在构建产物里查得到：
    # WinEncoder.cpp 与 WinrtApartment.cpp 都在 ecapture 与这份判据的源文件清单里。
    $cmake = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    Assert-Ec ($cmake -match 'src/platform/windows/WinEncoder\.cpp') `
        'CMakeLists.txt 里没有把 Windows 编码实现（src/platform/windows/WinEncoder.cpp）链进目标'
    Assert-Ec ($cmake -match 'src/platform/linux/PosixEncoder\.cpp') `
        'CMakeLists.txt 里没有 Linux 编码实现那一份（src/platform/linux/PosixEncoder.cpp）'

    # =========================================================================
    Write-Host "`n=== 3) 本步没有、也不假装有的东西（照实记录） ==="
    # =========================================================================
    Skip-Ec 'Windows 侧的像素级解码回读' '判据里的容器结构核对（PNG 块与 CRC、BMP 头、JPEG SOF）两个平台都跑，像素回读在 Linux 用 libpng / libjpeg 实测；Windows 那一路要读回来得另接 BitmapDecoder，本步不引入这条依赖'
    Skip-Ec '真实 HDR 显示环境下的编码' '本机没有 HDR 显示器：--hdr 的 tone mapping 数学与"广色域帧不进编码器"那道边界都由合成帧判过，真机 HDR 现场仍是 manual 层待人工'
    Skip-Ec 'Linux 编码器在 Windows 上跑' '两份平台实现各判各的现场，不把一平台的通过记成另一平台的通过'
}

exit (Complete-EcSuite -Title '跨平台图像处理与编码')
