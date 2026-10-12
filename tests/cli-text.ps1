<#
.SYNOPSIS
    命令行文本处理与多语言资源的回归判据：编码边界、四语文案、参数与配置、JSON 与标准流通道。
.DESCRIPTION
    这一步把"命令行文本"这一层从 Windows 专属里搬出来（见 MEMORY.md 与本步交接记录），
    所以这套判据判的就是搬出来之后仍然成立的几件事：

      1) 离线层（本脚本唯一的层）：build\ecapture-cli-text-tests.exe（源码 tests\cli_text_state.cpp
         加 tests\platform\cli_text_win_checks.cpp）。它链的是生产本体，不链取帧通道：
           * 文本编码：UTF-8 严格解码（过长编码 / 代理区 / 超范围 / 残缺尾部各算一段并留下
             第一处坏字节的偏移）；出口编码恒为合法 UTF-8，落单的代理换成 U+FFFD 而不写半个序列；
             长度上限按码位数算，两个平台对同一句人话得到同一个数；
           * 入口：wmain 的 UTF-16 参数表逐条原样进 CliTokens（不重新编码一次），
             判据本体就是生产函数 DecodeWindowsArgv；
           * 多语言：四份文案由构建期生成的嵌入物提供（不再走 RCDATA），逐份查整份合法 UTF-8；
             查不到 key 交回 ?key，语言标签认不了返回"不认"而不是悄悄换成语英语；
           * 参数：中文与空格的取值逐字交回，区间 / 互斥 / 缺值 / 未知选项各按既有稳定码报；
             非法编码的参数逐条报 cli.invalid_utf8 并让本次失败，不拿 U+FFFD 当合法的匹配值；
           * 平台边界：--hwnd / --class / --capture 与 --monitor 的两条 Windows 标识写法，
             在 Windows 构建里照旧有效（这一条判的是"没有回归"），非 Windows 构建才报不支持；
           * 配置：真实临时文件的读取、CLI>INI>内置三档来源记账、读不懂整份拒；
           * 通道：图片占用 stdout 之后文字必须改道（字节级现场判据在 POSIX 那一份里，
             Windows 这一侧由 tests\cli.ps1 与 tests\streams.ps1 在真 exe 上判）。

      2) 没有真机层：这一层一个像素都不取、一个框都不弹。Linux 侧对同一批源文件还有
         scripts\core-portability-check.sh 那条"不经 CMake 直接 g++ 编好再跑"的独立证明。

    本机判不了、也不假装判了的：完整 CLI（ECAPTURE.EXE）在 Linux 上跑通 —— 响应组装与取帧、
    窗口/屏幕枚举、辅助进程管道都还在 Windows 那一侧（见 CMakeLists.txt 的 Windows-only 名单）。
.EXAMPLE
    .\tests\cli-text.ps1
    .\tests\cli-text.ps1 -SkipState    # 什么都不跑（与其它套件一致的开关）
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
$stateExe = Join-Path $root 'build\ecapture-cli-text-tests.exe'

try {
    # =========================================================================
    Write-Host "`n=== 1) 离线层：文本、文案、参数与通道（测生产本体，不碰桌面） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '命令行文本处理的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-EcStateBinary -Root $root -Exe $stateExe)) {
            Assert-Ec $false "缺少离线判据程序 $stateExe：源码树里先跑一次 .\build.ps1（嵌入文案由构建期生成，构建目录里应有 generated\ec_lang_data.cpp）"
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 180000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 2 | Select-Object -First 1)
        Assert-Ec ($st.Exit -eq 0) "命令行文本判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "命令行文本判据有失败项：$tail"
        # 数量掉下 100 说明有人删了判据而不是补了判据（这一套覆盖编码 / 文案 / 参数 / 配置 / JSON）。
        Assert-Ec ([int]$m.Groups[1].Value -ge 100) "命令行文本判据通过数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    # =========================================================================
    Write-Host "`n=== 2) 嵌入文案与构建依赖：改完文案要能查出没重编 ==="
    # =========================================================================
    # 四份 .txt 是生成物的依赖，生成物没跟着重生成时，程序里那份与仓库里那份会对不上。
    # 这条对账由 .\scripts\check-lang.ps1 判（它现在读 generated\ec_lang_data.cpp，
    # 不再读 exe 里的 RCDATA —— 那一路已经搬走）。这里只确认那份生成物在本次构建里存在。
    $generated = Join-Path $root 'build\generated\ec_lang_data.cpp'
    if (Test-Path -LiteralPath $generated) {
        Assert-Ec $true '构建目录里有生成出来的嵌入文案源文件'
    } else {
        Assert-Ec $false "找不到 $generated：嵌入文案没被生成（检查 CMakeLists.txt 里 ECAPTURE_LANG_DATA 那条自定义命令）"
    }

    # =========================================================================
    Write-Host "`n=== 3) 本步没有、也不假装有的东西（照实记录） ==="
    # =========================================================================
    Skip-Ec '完整 CLI 在 Linux 上跑通' '响应组装（Report.cpp）、取帧通道、窗口/屏幕枚举与辅助进程管道仍在 Windows 一侧；Linux 可构建的是共用文本层的判据（./test-all.sh 的 cli-text 套件）'
    Skip-Ec 'Linux 上的 main 入口' '入口的换码本体（DecodePosixArgv）已按真字节数组判过；main 那三行要等上面那几段迁完才链得起来，不预先摆一条假入口'
}

exit (Complete-EcSuite -Title '命令行文本处理与多语言资源')

