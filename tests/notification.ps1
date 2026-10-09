<#
.SYNOPSIS
    截图后桌面通知的回归判据：离线内容判据层 + 只读 CLI 契约层 + 真机一条最小闭环。
.DESCRIPTION
    默认开启的功能要判的是"每张交付完成的图，往系统的通知平台交一条 toast"，以及这件事成不成
    都**不许**回头改交付事实。判据分三层：

      1) 离线层：build\ecapture-notification-tests.exe（源码 tests\notification_state.cpp）。判生产的
         src\Notification.cpp：三档 detail 各交出哪些字段、落点与附图只认"真已提交的那一份历史副本"、
         --purpose 占哪一格、合并模式列不下时数量怎么说、窗口标题里的标签与控制字符在 XML 里被转义
         （那是注入面），以及"全成功 / 部分失败 / 全失败有码 / 全失败拿不出码 / 零交付"五种归类，
         并核对通知失败不改动 images。
      2) 只读契约层：拿真实 exe 跑零交付与查询那几路（一个像素都不取、不弹框、不落地），判
         --notify / --notify-merge / --notify-detail / --purpose 四条的取值判定、CLI>INI>内置的合并与
         来源记账、只读查询不被凭空添一格、以及非法写法当场失败。
      3) 真机层：自建、无隐私的 fixture 窗口，用 --capture wgc + --yes（读的是那扇窗口自己的画面，
         不弹确认框、也不拍任何桌面像素）真截一张，判 notification 与 images[].history 两份事实
         互相对得上：历史开着才有 withImage、--notify off 时一条都不交、合并成一条时 submitted=1。

    两条如实记未验证，不冒充成自动化验收：
      * **屏幕上到底弹没弹**：那要人自己看。脚本判的是"已交给系统的通知平台"（status=submitted），
        这个词从来不代表通知中心里真的出现过一条东西 —— 专注助手、通知设置与未注册的
        AppUserModelID 都能让它不见，而程序这边问不出来。
      * **AUMID 有没有跟着安装注册上**：那属于安装期，判在 tests\install-lifecycle.ps1 与真人眼里。
        这一版故意不留「不创建快捷方式」的退路（`AllowNoIcons` 关着，`/NOICONS` 会被忽略），所以缺它的
        现场只剩两种：从 `build\` 直接跑的那一份，以及快捷方式后来被删掉的那些。
.EXAMPLE
    .\tests\notification.ps1
    .\tests\notification.ps1 -SkipState        # 只跑 CLI 与真机两层
    .\tests\notification.ps1 -SkipReal         # 只跑离线判据与只读契约
    .\tests\notification.ps1 -Keep             # 保留临时目录以便人眼看
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
$stateExe = Join-Path $root 'build\ecapture-notification-tests.exe'
$run = New-EcRunDir -Tag 'notify'

# 一次跑完，把退出码与 JSON 分开交回来。这里**不**假设零交付那几路的退出码是 0：
# "没命中窗口"本来就该非零，而 notification 那一格照样要在，两者各说各的事。
function Invoke-EcNotifyRun {
    param([Parameter(Mandatory)][string[]]$Arguments, [string]$ExePath)
    $file = if ($ExePath) { $ExePath } else { $Exe }
    $r = Invoke-EcProcess -FilePath $file -Arguments $Arguments -TimeoutMs 60000
    $json = $null
    $text = ''
    if (-not $r.StartError -and -not $r.TimedOut) {
        # 配置没读成那一路 JSON 恒走 stderr（图片可能正把标准输出当字节通道，两个通道永不混流），
        # 所以这里与渲染层问同一条判据：标准输出有东西就用它，否则看 stderr。
        # 只读 stdout 会把"配置失败"看成"没交出 JSON"，那条判据就废了。
        $text = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
        try { $json = $text | ConvertFrom-Json } catch { }
    }
    return [pscustomobject]@{ Exit = $r.Exit; Json = $json; Text = $text; Started = [bool](-not $r.StartError) }
}

function Get-EcCodeOf {
    param($Q)
    if (-not $Q.Json) { return '' }
    $e = @($Q.Json.errors)
    if ($e.Count -eq 0) { return '' }
    return [string]$e[0].code
}

try {
    # -----------------------------------------------------------------------
    # 1) 离线判据层
    # -----------------------------------------------------------------------
    if ($SkipState) {
        Write-Host '[1] 离线判据层：按 -SkipState 跳过'
    } else {
        if (-not (Test-Path -LiteralPath $stateExe)) {
            throw "找不到离线判据可执行文件：$stateExe（先跑 .\build.ps1）"
        }
        $s = Invoke-EcProcess -FilePath $stateExe -Arguments @() -TimeoutMs 120000
        Assert-Ec ($s.Exit -eq 0) "通知离线判据没过：退出码 $($s.Exit)`n$($s.Stdout)"
        Assert-Ec ($s.Stdout -match '0 failures') "通知离线判据的输出里没有 '0 failures'：$($s.Stdout)"
        Write-Host "[1] 离线判据层通过（$($s.Stdout -split "`n" | Select-Object -Last 2 | Out-String).Trim())"
    }

    # -----------------------------------------------------------------------
    # 2) 只读契约层：一个像素都不取
    # -----------------------------------------------------------------------
    $missing = @('--title', '__EC_NOTIF_NOSUCHWINDOW__')
    $out1 = Join-Path $run.Path 'never-written.png'

    # 零交付那一格必须在，且说的是"没有可报的东西"而不是"送失败了"。
    $q = Invoke-EcNotifyRun -Arguments ($missing + @('--out', $out1, '--no-config', '--lang', 'en'))
    Assert-Ec ($null -ne $q.Json) "零交付那次没交出可解析的 JSON：$($q.Text)"
    Assert-Ec ($q.Json.captured -eq 0) '这条判据要的是零交付现场'
    Assert-Ec ($null -ne $q.Json.notification) '截图那一路缺了 notification 那一格'
    Assert-Ec ([string]$q.Json.notification.status -eq 'no_delivery') "零交付应报 no_delivery，实报 $($q.Json.notification.status)"
    Assert-Ec ([string]$q.Json.notification.mode -eq 'per_target') '默认档必须是每目标一条'
    Assert-Ec ([string]$q.Json.notification.detail -eq 'title') '默认档必须写满到标题那一档'
    Assert-Ec ($null -eq $q.Json.notification.submitted) '根本没试时不许报计数'

    # 关掉：disabled 与"试了而没出去"是两件事，而计数一格都不该出现。
    $q = Invoke-EcNotifyRun -Arguments ($missing + @('--out', $out1, '--notify', 'off', '--no-config', '--lang', 'en'))
    Assert-Ec ([string]$q.Json.notification.status -eq 'disabled') "--notify off 应报 disabled，实报 $($q.Json.notification.status)"
    Assert-Ec ($null -eq $q.Json.notification.submitted -and $null -eq $q.Json.notification.failed) '关闭那一路不该有任何发送计数'

    # 两条覆盖开关真的落到机器名上。
    $q = Invoke-EcNotifyRun -Arguments ($missing + @('--out', $out1, '--notify-merge', 'on',
                                                     '--notify-detail', 'minimal', '--no-config', '--lang', 'en'))
    Assert-Ec ([string]$q.Json.notification.mode -eq 'merged') '--notify-merge on 没生效'
    Assert-Ec ([string]$q.Json.notification.detail -eq 'minimal') '--notify-detail minimal 没生效'

    # 非法写法当场失败，且不退化成默认档（写错"我要少写点"不能变成"照旧把标题写上锁屏"）。
    $q = Invoke-EcNotifyRun -Arguments ($missing + @('--out', $out1, '--notify-detail', 'bogus', '--no-config', '--lang', 'en'))
    Assert-Ec ($q.Exit -eq 1) "--notify-detail 非法取值退出码应为 1，实报 $($q.Exit)"
    Assert-Ec ((Get-EcCodeOf $q) -eq 'cli.invalid_value') "非法档位应报 cli.invalid_value，实报 $(Get-EcCodeOf $q)"
    $q = Invoke-EcNotifyRun -Arguments ($missing + @('--out', $out1, '--notify', 'maybe', '--no-config', '--lang', 'en'))
    Assert-Ec ($q.Exit -eq 1) '--notify 非法取值退出码应为 1'
    Assert-Ec ((Get-EcCodeOf $q) -eq 'cli.invalid_value') '非法 on/off 写法应报 cli.invalid_value'
    $long = 'A' * 201
    $q = Invoke-EcNotifyRun -Arguments ($missing + @('--out', $out1, '--purpose', $long, '--no-config', '--lang', 'en'))
    Assert-Ec ($q.Exit -eq 1) '--purpose 超长应在解析期失败'
    Assert-Ec ((Get-EcCodeOf $q) -eq 'cli.purpose_too_long') "超长 purpose 应报 cli.purpose_too_long，实报 $(Get-EcCodeOf $q)"
    # 上限之内照收，且 -v 把原文交回（那是调用方写的话，本工具不解读也不改写）。
    $q = Invoke-EcNotifyRun -Arguments ($missing + @('--out', $out1, '--purpose', 'release check',
                                                     '--no-config', '--lang', 'en', '-v'))
    Assert-Ec ([string]$q.Json.input.purpose -eq 'release check') '--purpose 的原文没能回显在 input.purpose'

    # 只读查询不被添格子：那几路本来就不发通知，也不该被读成"这次通知没送出去"。
    foreach ($probe in @('--capabilities', '--diagnostics', '--list')) {
        $q = Invoke-EcNotifyRun -Arguments @($probe, '--no-config', '--lang', 'en')
        Assert-Ec ($null -ne $q.Json) "$probe 没交出可解析的 JSON"
        Assert-Ec ($null -eq $q.Json.notification) "只读查询 $probe 被凭空添了 notification 那一格"
    }
    # 而查询与截图偏好混着给是冲突，不是"挑一个执行"。
    $q = Invoke-EcNotifyRun -Arguments @('--capabilities', '--notify', 'off', '--no-config', '--lang', 'en')
    Assert-Ec ($q.Exit -eq 1) "查询带 --notify 应算冲突（退出 1），实报 $($q.Exit)"
    Assert-Ec ((Get-EcCodeOf $q) -eq 'cli.query_conflict') "查询带 --notify 应报 cli.query_conflict，实报 $(Get-EcCodeOf $q)"

    # --show-config 能观察这三条，且来源如实。
    $q = Invoke-EcNotifyRun -Arguments @('--show-config', '--no-config', '--lang', 'en')
    $srcs = @($q.Json.settings)
    $one = $srcs | Where-Object { $_.key -eq 'notification.enabled' }
    Assert-Ec ($null -ne $one) '--show-config 里没有 notification.enabled（schema 与来源表脱节了）'
    Assert-Ec ([string]$one.value -eq 'true' -and [string]$one.source -eq 'builtin') '默认应是弹、来源 builtin'
    $one = $srcs | Where-Object { $_.key -eq 'notification.merge' }
    Assert-Ec ([string]$one.value -eq 'false') '默认不该合并'
    $one = $srcs | Where-Object { $_.key -eq 'notification.detail' }
    Assert-Ec ([string]$one.value -eq 'title') '默认档应是 title'
    # 配置视图带着覆盖跑是合法的（它的用途正是观察偏好）。
    $q = Invoke-EcNotifyRun -Arguments @('--show-config', '--notify-detail', 'basic', '--no-config', '--lang', 'en')
    $one = @($q.Json.settings) | Where-Object { $_.key -eq 'notification.detail' }
    Assert-Ec ([string]$one.value -eq 'basic' -and [string]$one.source -eq 'cli') '配置视图里 CLI 覆盖没生效'

    # 一份 INI 写三条：来源要变成 config，值要跟着变。
    $ini = Join-Path $run.Path 'ECAPTURE.notify.ini'
    @(
        '[config]', 'version = 1', '', '[notification]',
        'enabled = false', 'merge = true', 'detail = minimal'
    ) | Set-Content -LiteralPath $ini -Encoding UTF8
    $q = Invoke-EcNotifyRun -Arguments @('--show-config', '--config', $ini, '--lang', 'en')
    $srcs = @($q.Json.settings)
    Assert-Ec ([string](@($srcs | Where-Object { $_.key -eq 'notification.enabled' }).source) -eq 'config') 'enabled 的来源应是 config'
    Assert-Ec ([string](@($srcs | Where-Object { $_.key -eq 'notification.enabled' }).value) -eq 'false') 'INI 的 false 没生效'
    Assert-Ec ([string](@($srcs | Where-Object { $_.key -eq 'notification.detail' }).value) -eq 'minimal') 'INI 的档位没生效'
    # CLI 明确覆盖 INI：写了 false 也算显式给过，不能被配置里的 true 顶回去。
    $q = Invoke-EcNotifyRun -Arguments @('--show-config', '--config', $ini, '--notify-detail', 'basic', '--lang', 'en')
    $one = @($q.Json.settings) | Where-Object { $_.key -eq 'notification.detail' }
    Assert-Ec ($one.value -eq 'basic' -and $one.source -eq 'cli') 'CLI 没覆盖住 INI 那一档'
    # 配置里一个非法档位必须在读配置那一步失败，而不是降级成缺省继续截。
    $bad = Join-Path $run.Path 'ECAPTURE.bad.ini'
    @('[config]', 'version = 1', '', '[notification]', 'detail = weird') | Set-Content -LiteralPath $bad -Encoding UTF8
    $q = Invoke-EcNotifyRun -Arguments @('--out', $out1, '--title', 'x', '--config', $bad, '--lang', 'en')
    Assert-Ec ($q.Exit -eq 1) '坏配置应在取帧之前就让本次失败'
    Assert-Ec ((Get-EcCodeOf $q) -like 'config.*') "坏配置要报 config.* 那一族，实报 $(Get-EcCodeOf $q)"
    Assert-Ec ($q.Json.captured -eq 0 -and -not (Test-Path -LiteralPath $out1)) '配置没读成却仍然落了文件：判据不成立'

    # --help 由选项目录生成，四条都要看得见（漏一条就是文档与契约脱节）。
    $h = Invoke-EcProcess -FilePath $Exe -Arguments @('--help', '--lang', 'en', '--no-config') -TimeoutMs 30000
    foreach ($flag in @('--notify ', '--notify-merge', '--notify-detail', '--purpose')) {
        Assert-Ec ($h.Stdout -match [regex]::Escape($flag)) "--help 里少了 $flag"
    }
    Write-Host '[2] 只读契约层通过'

    # -----------------------------------------------------------------------
    # 3) 真机层：自建 fixture 窗口，读那扇窗口自己的画面
    # -----------------------------------------------------------------------
    if ($SkipReal) { throw '__SKIP_REAL__' }

    $w = Start-EcWindow -RunDir $run -Class 'EcapNotifFixture' -Title 'ECAPTURE notification fixture' `
                        -Rect '140,140,560,420' -Mode window
    $hwndHex = '0x{0:X}' -f [int64]$w.Hwnd

    # 默认那一档：历史开着、副本已提交，所以 withImage 必须与它同源。
    $shot = Join-Path $run.Path 'n1.png'
    $q = Invoke-EcNotifyRun -Arguments @('--hwnd', $hwndHex, '--capture', 'wgc', '--yes',
                                         '--out', $shot, '--no-config', '--lang', 'en')
    Assert-Ec ($q.Json.captured -eq 1) "fixture 没截成功：$(Get-EcCodeOf $q)"
    Assert-Ec ([string]$q.Json.notification.status -eq 'submitted') "真截一张时应报 submitted，实报 $($q.Json.notification.status)"
    Assert-Ec ($q.Json.notification.submitted -eq 1) '一张图一条'
    Assert-Ec ([string]$q.Json.images[0].history.status -eq 'saved') '默认应有历史副本'
    Assert-Ec ($q.Json.notification.withImage -eq 1) '有已提交副本时那条通知该带上图'

    # 历史关掉：通知照交，只是没图 —— 这正是"附图跟随历史"那一条承诺。
    $shot = Join-Path $run.Path 'n2.png'
    $q = Invoke-EcNotifyRun -Arguments @('--hwnd', $hwndHex, '--capture', 'wgc', '--yes',
                                         '--history', 'off', '--out', $shot, '--no-config', '--lang', 'en')
    Assert-Ec ([string]$q.Json.notification.status -eq 'submitted') '历史关了也应把通知交出去'
    Assert-Ec ($null -eq $q.Json.notification.withImage) '没有已提交副本时不许报 withImage'
    Assert-Ec ([string]$q.Json.images[0].history.status -eq 'disabled') '历史那一格该说 disabled'

    # 关掉通知：一条都不交，而图照旧交付完整。
    $shot = Join-Path $run.Path 'n3.png'
    $q = Invoke-EcNotifyRun -Arguments @('--hwnd', $hwndHex, '--capture', 'wgc', '--yes',
                                         '--notify', 'off', '--out', $shot, '--no-config', '--lang', 'en')
    Assert-Ec ([string]$q.Json.notification.status -eq 'disabled') '--notify off 那一路要报 disabled'
    Assert-Ec ($q.Json.captured -eq 1 -and (Test-Path -LiteralPath $shot)) '关掉通知不影响主交付'

    # 两个目标合并成一条。
    $w2 = Start-EcWindow -RunDir $run -Class 'EcapNotifFixture2' -Title 'ECAPTURE notification fixture 2' `
                         -Rect '600,140,980,420' -Mode window
    $hwndHex2 = '0x{0:X}' -f [int64]$w2.Hwnd
    # 两个 hwnd 是同一类条件的 OR（同类 OR、跨类 AND），命中两扇就是歧义 —— 要 --all 才成批。
    $shot = Join-Path $run.Path 'n%h.png'
    $q = Invoke-EcNotifyRun -Arguments @('--hwnd', $hwndHex, '--hwnd', $hwndHex2, '--capture', 'wgc',
                                         '--yes', '--all', '--notify-merge', 'on', '--out', $shot,
                                         '--no-config', '--lang', 'en')
    Assert-Ec ($q.Json.captured -eq 2) "两目标现场没成立：$(Get-EcCodeOf $q)"
    Assert-Ec ([string]$q.Json.notification.mode -eq 'merged') '合并开关没生效'
    Assert-Ec ($q.Json.notification.submitted -eq 1) '合并成一条时只该交一条'

    # 每目标一条时是两条（同一批，只换开关）。
    $shot = Join-Path $run.Path 'm%h.png'
    $q = Invoke-EcNotifyRun -Arguments @('--hwnd', $hwndHex, '--hwnd', $hwndHex2, '--capture', 'wgc',
                                         '--yes', '--all', '--notify-merge', 'off', '--out', $shot,
                                         '--no-config', '--lang', 'en')
    Assert-Ec ($q.Json.notification.submitted -eq 2) '每目标一条时该交两条'

    Write-Host '[3] 真机层通过（屏幕可见性不在自动判据里，见脚本头）'
} catch {
    if ($_.Exception.Message -notin @('__SKIP_REAL__')) { throw }
} finally {
    Stop-EcOwnedWindows
    if (-not $Keep) { Remove-EcRunDir $run } else { Write-Host "保留临时目录：$($run.Path)" }
}

exit (Complete-EcSuite -Title '截图后桌面通知')
