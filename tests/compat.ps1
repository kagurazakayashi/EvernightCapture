<#
.SYNOPSIS
    系统兼容性检查与支持声明的判据：离线注入假版本逐条判那几道门槛，真机核对探测、接线与
    "这个 exe 到底静态依赖了什么"。
.DESCRIPTION
    这次要立的是三件事，彼此不能混：

      1) 每条取图路线各自的 **API 历史下限**（写在 src/SystemCompat.h 的 os_floor 里，逐条注明
         判据出处）。WGC 那条按 18362 判，不是按 Windows.Graphics.Capture 命名空间出现的 17134
         判 —— 本工具不经选择器，用的是 1903 才有的 IGraphicsCaptureItemInterop。
      2) 本工具自己**声明**的下限（kSupportedMinBuild = 18362，x64）。
      3) **实测过**的版本。只有开发机那一台；中间那些"能装载、能启动，但本工具从没在上面跑过"
         的版本一律记未验证，不拿文档推导冒充实测。

    判据分两层：
      1) 离线层：build\ecapture-compat-tests.exe（源码 tests\compat_state.cpp）把假版本注入
         生产判据本体，逐条判边界两侧、显式指定被挡时绝不换通道、auto 少的是哪一条、以及
         "版本问不出来时不许瞎筛"。真机上要判的是同一批分支，可这台机器的 Windows 版本改不了
         （降级装不了，也不该为测试拆机器），所以那几条只能在这里注入判。
      2) 真机层：不截图。判的是探测出来的内部版本确实是系统真正那一份（与 WMI 那处独立问来的
         值对照，防版本伪装与清单影响）、回显的通道链与那个版本自相一致、--dry-run 不会因为
         环境判据报错、帮助与四语文案把这四条下限说到位，以及**这个 exe 的导入表**真的静态依赖
         api-ms-win-core-winrt-* 与 job 那批契约（那条判据支撑"Windows 7 / 8 上根本装载不了、
         而 Windows 8.1 只是装载得起来"这个说法，而不是凭印象写）。

    隐私规矩与本任务无关但照旧生效：这里没有任何一项要拍桌面像素，也不需要人点框。

.EXAMPLE
    .\tests\compat.ps1
    .\tests\compat.ps1 -SkipState       # 只跑真机层
    .\tests\compat.ps1 -Exe build\Debug\ecapture.exe
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
$stateExe = Join-Path $root 'build\ecapture-compat-tests.exe'

# 那几道门槛在测试里独立写一份（与 src/SystemCompat.h 的 os_floor 同源但不同处）：
# 判据要是被实现偷偷改了而这里跟着改，就发现不了了。
$FLOOR_ENCODER = 10240      # WinRT BitmapEncoder：整工具下限
$FLOOR_DUP = 9200           # IDXGIOutput1::DuplicateOutput
$FLOOR_FULLCONTENT = 9600   # PW_RENDERFULLCONTENT（printwindow 与 dwm 的读回都靠它）
$FLOOR_WGC = 18362          # IGraphicsCaptureItemInterop::CreateForWindow / CreateForMonitor
$FLOOR_DECLARED = 18362     # 本工具对外声明的下限

function Json-Of($r) {
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
    $o = $null
    try { $o = $body | ConvertFrom-Json } catch { }
    return $o
}

function Codes($list) {
    if ($null -eq $list) { return @() }
    return @($list | ForEach-Object { $_.code })
}

# 在本机这一份 exe 的原始字节里找一段 ASCII（导入表里的那些 DLL 名就是原样躺在文件里的）
function Test-EcBinaryContains {
    param([string]$Path, [string]$Needle)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    # Latin1 是一字节一字符的双射，拿它解码不会丢掉任何字节，也不会因为无效序列换字符
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    return $text.Contains($Needle)
}

try {
    # =========================================================================
    Write-Host "`n=== 0) 离线层：把假版本注进判据本体逐条判 ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '能力判据离线层' '调用方给了 -SkipState'
    } else {
        if (-not (Test-EcStateBinary -Root $root -Exe $stateExe)) {
            Assert-Ec $false "缺少离线判据程序 $stateExe：源码树里先跑一次 .\build.ps1；安装目录里它必须随包发出（说明打包的依赖闭包没兜住）"
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "能力判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '(\d+) 条通过，(\d+) 条失败')
        $failed = if ($m.Success) { [int]$m.Groups[2].Value } else { -1 }
        $passed = if ($m.Success) { [int]$m.Groups[1].Value } else { 0 }
        Assert-Ec ($failed -eq 0) "能力判据有失败项，或摘要读不出来：$tail"
        Assert-Ec ($passed -ge 90) "能力判据通过数不对劲（$passed），用例被删了？"
        Write-Host "  $tail" -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 1) 真机：探测到的内部版本是系统真正那一份 ==="
    # =========================================================================
    # RtlGetVersion 与 WMI 是两条独立的路：后者不受应用清单与版本伪装影响。两边对不上，
    # 说明探测被谎报了 —— 而整套能力判据都是拿这个数判的。
    $wmiBuild = [int](Get-CimInstance -ClassName Win32_OperatingSystem).BuildNumber
    $r = Invoke-EcProcess -FilePath $Exe -Arguments @('--class', 'Shell_TrayWnd', '-v', '--dry-run', 'out.png')
    $o = Json-Of $r
    Assert-Ec ($r.Exit -eq 0 -and $o) "dry-run 没跑通（exit=$($r.Exit)）：$($r.Stderr)"
    $osBuild = [int]$o.input.osBuild
    Assert-Ec ($osBuild -gt 0) "-v 的 input.osBuild 回显出来了：$osBuild"
    Assert-Ec ($osBuild -eq $wmiBuild) "探测到的内部版本 $osBuild 与 WMI 那份 $wmiBuild 不一致（版本被伪装了？）"

    # =========================================================================
    Write-Host "`n=== 2) 真机：回显的通道链与该版本自相一致 ==="
    # =========================================================================
    $chain = @($o.input.captureChain)
    Assert-Ec ($chain.Count -ge 1) "默认那条链是空的：$(ConvertTo-Json -Compress $o.input)"
    Assert-Ec ($chain[0] -eq 'wgc' -or $osBuild -lt $FLOOR_WGC) `
        "本机 $osBuild 不低于 WGC 下限时链首要就是 wgc，实际是 $($chain[0])"
    # 显式指定一条时链就只有那一条，绝不因为版本挡着就换成别的那一条
    $r2 = Invoke-EcProcess -FilePath $Exe -Arguments @('--class', 'Shell_TrayWnd', '-v', '--dry-run',
                                                       '--capture', 'printwindow', 'out.png')
    $chain2 = @((Json-Of $r2).input.captureChain)
    Assert-Ec ((($chain2 -join ',') -eq 'printwindow') -or ($osBuild -lt $FLOOR_FULLCONTENT)) `
        "显式 printwindow 时链就该只有那一条（本机 $osBuild）：$($chain2 -join ',')"
    # 屏幕模式那份链与窗口模式不是同一份顺序
    $r3 = Invoke-EcProcess -FilePath $Exe -Arguments @('--monitor', '1', '-v', '--dry-run', '-C', 'auto', 'out.png')
    $chain3 = @((Json-Of $r3).input.captureChain)
    # 期望那份链由测试自己按同一条判据算一遍（照版本下限筛），与实现各写一处：
    # 只有一处的话，实现把哪一条筛错了就没人拦得住。
    $want3 = @()
    if ($osBuild -ge $FLOOR_WGC) { $want3 += 'wgc' }
    if ($osBuild -ge $FLOOR_DUP) { $want3 += 'duplication' }
    $want3 += 'bitblt'
    Assert-Ec (($chain3 -join ',') -eq ($want3 -join ',')) `
        "屏幕模式 auto 展开不对：本机 $osBuild 得到 $($chain3 -join ',')，期望 $($want3 -join ',')"

    # --dry-run 一个像素都不取，所以环境判据不许在这里把调用挡下来
    Assert-Ec (-not ((Codes $o.errors) -like 'env.*')) "dry-run 报了环境错误：[$((Codes $o.errors) -join ',')]"
    Assert-Ec (-not ((Codes $o.notes) -like 'env.*')) "dry-run 的 notes 里冒出 env.*：[$((Codes $o.notes) -join ',')]"

    # =========================================================================
    Write-Host "`n=== 3) 真机：帮助与文案把这几条下限说到位 ==="
    # =========================================================================
    # --version 那份是纯文本，里面写的声明下限必须与判据用的是同一个数
    $v = (Invoke-EcProcess -FilePath $Exe -Arguments @('--version')).Stdout
    Assert-Ec ($v -match "minWindowsBuild=$FLOOR_DECLARED") "--version 没写出声明下限：$($v.Trim())"
    $h = (Invoke-EcProcess -FilePath $Exe -Arguments @('--help')).Stdout
    Assert-Ec ($h -match '18362') '帮助里没有声明的那个下限 18362'
    Assert-Ec ($h -match 'env\.os_too_old' -and $h -match 'env\.channel_unsupported') `
        '帮助没把那两条环境码说出来（调用方要靠它分清"换通道有用"与"这台机器不行"）'
    Assert-Ec ($h -match 'input\.captureChain') '帮助没说清能力可以在 --verbose 里回显'
    # 四语那份对齐由 .\scripts\check-lang.ps1 判；这里只确认代码里写的那几个 key 真取得到文案
    foreach ($lang in @('zh-CN', 'zh-TW', 'en', 'ja')) {
        $hr = (Invoke-EcProcess -FilePath $Exe -Arguments @('--help', '--lang', $lang)).Stdout
        Assert-Ec ($hr -match '18362' -and $hr -notmatch '\?env\.|\?note\.|\?help\.') `
            "$lang 的帮助里那一段没取到文案（或漏了下限数字）"
    }

    # =========================================================================
    Write-Host "`n=== 4) 真机：这个 exe 的导入表（装载下限那条判据的根据） ==="
    # =========================================================================
    # 说"Windows 7 / 8 上根本装载不了"靠的不是印象：这份二进制静态导入了
    # api-ms-win-core-winrt-error-l1-1-1.dll，而微软给 RoOriginateLanguageException 写明的最低客户端
    # 就是 Windows 8.1；winrt-l1-1-0 与 job-l2-1-0 是 Windows 8 的契约，而 Windows 7 没有 API Set 机制。
    # UCRT 可再分发的那 46 个契约里不含 winrt / job（本机实测），所以这三个都补不到旧系统上。
    # Windows 8.1 能装载，但那时起作用的是 env.os_too_old（10240 之前没有编码器）。
    $imports = @(
        'api-ms-win-core-winrt-l1-1-0.dll',        # RoInitialize / RoGetActivationFactory
        'api-ms-win-core-winrt-error-l1-1-1.dll',  # RoOriginateLanguageException（C++/WinRT 抛异常用）
        'api-ms-win-core-job-l2-1-0.dll'           # AssignProcessToJobObject（辅助进程那道兜底）
    )
    foreach ($dll in $imports) {
        Assert-Ec (Test-EcBinaryContains -Path $Exe -Needle $dll) "导入表里找不到 $dll"
    }

    # =========================================================================
    Write-Host "`n=== 5) 真机：进程完整性级别读的是系统自己那份标签名 ==="
    # =========================================================================
    # 这一条是给"照印象写表"那一类错法准备的：实现里那几个 SECURITY_MANDATORY_*_RID 与级别名
    # 的对应，如果整个错开一档，本机看起来仍会像"问出了一个答案"（medium 报成 low、low 报成
    # untrusted），离线层判不出来。这里走的是另一条路 —— 期望值一律取自系统自己那份名字表：
    # 被测试那个文件**自己的**强制完整性标签（安全描述符里那条 S-1-16-*，由 Get-Acl 交回、
    # 名字由 LSA 换算），没有那条标签时才退回宿主进程的 whoami 那一档。
    # 顺序不能反：镜像带着标签时进程就被带到那一档，与宿主是谁无关。安装版自检把 exe 跑在
    # 带低完整性标签的 Skill 目录里，正是这个差别（拿宿主的 whoami 当期望值会把这份正确的
    # "low"判成失败）。.NET 那条路（WindowsIdentity.Groups / Translate）在本机拿不到 S-1-16-*
    # 这一组，所以不拿它当替代判据。
    $labelName = $null
    $labelSource = $null
    # 文件上那条标签在安全描述符的 SACL 里，Get-Acl 的 Access 只列 DACL，所以读它要走 icacls
    # （它两种都打，而名字同样是 LSA 那份表换算出来的）。
    try {
        $icacls = Join-Path $env:SystemRoot 'System32\icacls.exe'
        if (Test-Path -LiteralPath $icacls) {
            $aclText = (& $icacls $Exe) -join "`n"
            $mAcl = [regex]::Match($aclText, 'Mandatory Label\\(.*?) Mandatory Level')
            if ($mAcl.Success) {
                $labelName = 'Mandatory Label\{0} Mandatory Level' -f $mAcl.Groups[1].Value
                $labelSource = '被测文件自己的标签'
            }
        }
    } catch { $labelName = $null }
    if (-not $labelName) {
        try {
            $whoami = Join-Path $env:SystemRoot 'System32\whoami.exe'
            if (Test-Path -LiteralPath $whoami) {
                $text = (& $whoami /groups) -join "`n"
                $m = [regex]::Match($text, '(?m)^(.+?)\s+Label\s+(S-1-16-\d+)\s*$')
                if ($m.Success) { $labelName = $m.Groups[1].Value.Trim(); $labelSource = '宿主进程' }
            }
        } catch { $labelName = $null }
    }
    if (-not $labelName) {
        Skip-Ec '本进程的完整性级别与系统给出的标签名逐字对上' `
                '被测文件上没有完整性标签，whoami 那一行也没读出来（这一层不拿实现里的数字当期望值，所以宁可记未验证）'
    } else {
        $want = switch -Regex ($labelName) {
            'Untrusted'         { 'untrusted' }
            'Low'               { 'low' }
            'Medium Plus'       { 'medium' }   # 8448 与 8192 都归到"不低于那条线"这一侧
            'Medium'            { 'medium' }
            'High'              { 'high' }
            'System'            { 'system' }
            'Protected Process' { 'protected_process' }
            default             { $null }
        }
        Assert-Ec ($null -ne $want) "系统给的标签名读不出级别：$labelName"
        $rIl = Invoke-EcProcess -FilePath $Exe -Arguments @('--capabilities')
        $oIl = Json-Of $rIl
        Assert-Ec ($rIl.Exit -eq 0 -and $oIl) "--capabilities 没跑通（exit=$($rIl.Exit)）：$($rIl.Stderr)"
        $got = [string]$oIl.session.integrityLevel
        Assert-Ec ($got -eq $want) `
            "本工具报的完整性级别 $got 与系统那份标签名（$labelName → $want，出处：$labelSource）不一致（RID 表错档了？）"
        # 低于 medium 时 caveats 必须带上那条边界说明；不低于时一条都不许多（走的是哪一侧由上面
        # 那个值决定，两边都判到，不靠"这台机器刚好是 medium"混过去）
        $hasCaveat = @($oIl.caveats) -contains 'process_integrity_below_medium'
        $below = ($want -eq 'low' -or $want -eq 'untrusted')
        Assert-Ec ($hasCaveat -eq $below) `
            "caveat 与级别对不上：$want 时 boundary=$below 而 caveats=$hasCaveat"
        Write-Host "  被测这一档：$got（期望：$labelName，出处：$labelSource）" -ForegroundColor DarkGray
    }

    # =========================================================================
    Write-Host "`n=== 6) 本机不具备条件、照实记未验证的部分 ==="
    # =========================================================================
    Skip-Ec '低完整性目录下启动的那一份二进制：取帧被拒与写盘被拒时都补上可操作的提示' `
            '这一档需要一个带 Mandatory Label\Low Mandatory Level 标签的目录才能造出来，而本套件绝不为了测试去改任何目录的完整性标签，也不拿别的用户的数据目录做实验。判据本体（哪几档算低于 medium、哪几步的 access denied 才被补写、确认框上答「否」那一条为什么绝不被当成同一件事）由上面的离线层注入逐条判；这台机器上确实出现过的那一档由第 5 节与系统那份名字表对过。'
    Skip-Ec '低于各条下限的 Windows 上：显式那条通道真的报 env.channel_unsupported 且不落地' `
            '这台机器的 Windows 版本降不下去；那几道边界由离线层注入假版本逐条判'
    Skip-Ec 'Windows 7 / 8 上装载失败时系统给出的具体报错文字' `
            '本机没有那些系统，也不该为测试装一台。这一层只判到"发布版二进制确实静态导入 api-ms-win-core-winrt-error-l1-1-1（文档下限 Windows 8.1）与 winrt-l1-1-0 / job-l2-1-0（Windows 8）"，装载失败的表象本身没实测'
    Skip-Ec ('声明支持之内、而本机之外没实测过的版本（{0} 与 {1} 之间，以及更新的那些版本）' -f $FLOOR_DECLARED, 19045) `
            '本仓库所有真机判据都只在开发机那一台（19045）上跑过；README 与 --help 里写的实测范围就是它，不写成更大'
    Skip-Ec 'Windows N（缺 Media Feature Pack）、ARM64、Windows Server、远程桌面会话下的实际表现' `
            '本机不是那些环境。编码器类型注册不到的话那一步会自己报 capture.encoder_unavailable，本工具没有为此预测'
} finally {
    Stop-EcOwnedWindows
}

exit (Complete-EcSuite -Title '系统兼容性检查与支持声明')
