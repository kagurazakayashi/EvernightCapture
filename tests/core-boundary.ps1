<#
.SYNOPSIS
    跨平台核心边界的回归判据：共用模型不许渗出平台类型，功能矩阵与隐私判据必须说得出口。
.DESCRIPTION
    这一步（划分跨平台核心与平台实现边界）交付的不是一个新功能，而是一条**界限**，
    所以这套判据判的也是界限：

      1) 离线层（本脚本唯一的层）：build\ecapture-core-tests.exe（源码 tests\core_boundary_state.cpp）。
         这个可执行文件的源文件**一条都不 include windows.h**，只用共用模型：
           * 坐标：PhysRect 的四条边是 int32，虚拟屏幕坐标允许为负，空矩形不被特殊对待
             （RectContains / RectOverlaps 与搬动之前逐字相同 —— 搬家不许把授权判据改松或改紧）；
           * 帧：CapturedFrame 的像素缓冲不挂任何句柄，上限与 ImageOps / GDI / 辅助进程同源，
             没问出来的读数保持"没答案"那一档（displayHdrState 不折成 kSdr、光标两格不冒充读过）；
           * 目标：平台 + 会话 + 那一家自己的标识；跨平台、跨会话、没有标识一律比不出结论（unknown），
             而"比不出"与"确认是两个对象"是两种下场；
           * 矩阵：三个平台族 × 八项功能，一格都不许缺、不许是 kUnknown；
             非 Windows 那一列不许声称已实现（本构建一行 Linux 取帧代码都没有）；
           * 隐私：--yes 只豁免真正绑定窗口画面的那一路；桌面像素（含通道内部的屏幕退路、
             没登记过的路径名）永远要人确认；Wayland 少系统授权那一层就不放行，
             只有系统授权而没有程序确认同样不放行；关框/弹不出框默认不是同意。

      2) 没有真机层：这一层一个像素都不取、一个框都不弹，也不需要桌面。
         反过来，Linux 侧对同一份源文件还有一条**编译**判据（scripts\core-portability-check.sh），
         它才是真正的"边界不渗平台类型"的证明；本脚本判的是这些判据在 Windows 构建里同样成立。

    本机判不了、也不假装判了的：X11 与 Wayland 的实际截图能力。本构建没有那两个后端，
    矩阵里它们那一列全部是"计划支持 / 明确不做"，任何一格被写成已实现都是造假，测试会红。
.EXAMPLE
    .\tests\core_boundary.ps1
    .\tests\core_boundary.ps1 -SkipState    # 什么都不跑（保留与其它套件一致的开关）
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
$stateExe = Join-Path $root 'build\ecapture-core-tests.exe'

try {
    # =========================================================================
    Write-Host "`n=== 1) 离线层：共用模型与隐私判据（测生产本体，不碰桌面） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '跨平台核心边界的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-EcStateBinary -Root $root -Exe $stateExe)) {
            Assert-Ec $false "缺少离线判据程序 $stateExe：源码树里先跑一次 .\build.ps1；安装目录里它必须随包发出（说明打包的依赖闭包没兜住）"
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 2 | Select-Object -First 1)
        Assert-Ec ($st.Exit -eq 0) "核心边界判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "核心边界判据有失败项：$tail"
        # 这一层最要紧的是"格子填满"：三个平台族 × 八项功能 = 24 格，加模型与隐私那些条。
        # 数量掉下 100 说明有人删了判据而不是补了判据。
        Assert-Ec ([int]$m.Groups[1].Value -ge 100) "核心边界判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    # =========================================================================
    Write-Host "`n=== 2) 本步没有、也不假装有的东西（照实记录） ==="
    # =========================================================================
    Skip-Ec 'Linux X11 与 Wayland 的实际截图' '本构建没有那两个后端：矩阵里那一列全部登记成"计划支持/明确不做"，把没实现的格子写成可用就是造假（见 core/Capability.h）'
    Skip-Ec 'wxWidgets 自有界面与 Portal 授权联调' '本步只把接缝（IConsentPrompt / ToastSender / RequiredGrantLayers）划出来，实现仍在 Windows 那两侧；接线是后续步骤'
}

exit (Complete-EcSuite -Title '跨平台核心边界')
