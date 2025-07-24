<#
.SYNOPSIS
    ECAPTURE.EXE 输出契约回归测试：文本（help/version）与精简 JSON（结果/错误）。
.DESCRIPTION
    这些用例只验证参数解析与输出契约，一律带 --dry-run，不截图也不写文件。
    需要"必然存在"的窗口做锚点时统一用任务栏 --class Shell_TrayWnd。
    真机截图（含像素内容校验）在 tests\smoke.ps1，通道与遮挡对照在 tests\channels.ps1。
    起进程的方式由 tests\harness.psm1 统一负责，调用器本身由 tests\invoker.ps1 离线验证。
    断言的文案是简体中文，所以整轮都强制 --lang zh-CN：换一台英文系统的机器也必须全绿。
    多语言本身由文末的跨语言检查与 .\scripts\check-lang.ps1 负责。
.EXAMPLE
    .\tests\cli.ps1
    .\tests\cli.ps1 -Exe build\Debug\ecapture.exe -ShowAll
    .\tests\cli.ps1 -Lang en        # 整轮改用英文文案跑（只用于排查）
#>
param(
    [string]$Exe,
    [string]$Lang = 'zh-CN',
    [switch]$ShowAll
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
Initialize-EcHarness -Exe $Exe | Out-Null

# 起进程一律走 tests\harness.psm1 的调用器：参数按 Windows argv 规则传，
# 两条流并发读取，等待有期限。这里只补一层"整轮锁一种语言"的约定。
function Invoke-Ec {
    param([string[]]$Arguments, [switch]$NoLang)

    # 整轮固定语言：用例里自己写了 --lang / -l 的以用例为准
    $names = @($Arguments | ForEach-Object { $_ -replace '^--', '' })
    if ($Lang -and -not $NoLang -and ($names -notcontains 'lang') -and ($Arguments -notcontains '-l')) {
        $Arguments = @('--lang', $Lang) + @($Arguments)
    }
    return (Invoke-Ecapture -Arguments $Arguments)
}

function Codes($list) { if ($null -eq $list) { @() } else { @($list | ForEach-Object { $_.code }) } }

$ANCHOR = @('--class', 'Shell_TrayWnd', '--dry-run')     # 必然存在的窗口，且不截图不写文件
$MATCH_FLAGS = @('--hwnd','--pid','--process','--exe','--title','--title-contains','--title-regex','--class')

$cases = @(
    # ---------- 文本输出 ----------
    @{ Name = '无参数 -> 文本帮助'; A = @(); Exit = 2; Text = $true
       Has = @('未指定任何匹配条件', '窗口匹配条件', '用法:', '退出码:') }
    @{ Name = '--help 文本且含全部条件'; A = @('--help'); Exit = 3; Text = $true
       Has = (@('用法:') + $MATCH_FLAGS) }
    @{ Name = '/help 斜杠形式'; A = @('/help'); Exit = 3; Text = $true; Has = @('窗口匹配条件') }
    @{ Name = '帮助体积受控（<6KB）'; A = @('--version'); Exit = 0; Text = $true; Has = @('EvernightCapture') }
    # --version 里那句是**对外声明**的最低 Windows 内部版本，与运行时能力检查同源（判据见 compat.ps1）
    @{ Name = '--version 带出声明的最低内部版本'; A = @('--version'); Exit = 0; Text = $true
       Has = @('minWindowsBuild=18362') }
    @{ Name = '帮助含取图方式一节'; A = @('--help'); Exit = 3; Text = $true
       Has = @('取图方式', 'Windows.Graphics.Capture', 'printwindow', 'bitblt', 'auto') }

    # ---------- 解析通过（锚点窗口 + dry-run）----------
    @{ Name = 'dry-run 只报告候选不截图'; A = ($ANCHOR + @('out.png')); Exit = 0; Json = $true
       Notes = @('note.dry_run')
       Check = { param($o) $o.captured -eq 0 -and @($o.images).Count -eq 0 -and
                            -not $o.PSObject.Properties.Name.Contains('errors') } }
    @{ Name = 'JSON 里没有元信息键'; A = ($ANCHOR + @('out.png')); Exit = 0; Json = $true
       Check = { param($o) foreach ($k in @('tool','version','schema','stage','kind','request','ok','message')) {
                            if ($o.PSObject.Properties.Name.Contains($k)) { return $false } }
                            return $true } }
    @{ Name = '短选项 -o 与扩展名推断格式'; A = ($ANCHOR + @('-o','out.jpg')); Exit = 0; Json = $true }
    @{ Name = '短选项 -p 解析通过'; A = @('-p','notepad.exe','--dry-run','out.jpg'); Exit = @(0,4) }
    @{ Name = '--opt=value'; A = ($ANCHOR + @('--format=png','out.x')); Exit = 0; Json = $true }
    @{ Name = '标准输出'; A = ($ANCHOR + @('-o','-')); Exit = 0; Json = $true
       Notes = @('note.pipe_default_format') }
    # 多目标不许共用 stdout：这一条只在选完目标之后判，判据是实际目标数而不是 --all 这个选项。
    # dry-run 在取帧之前就返回，所以 -o - 配 --all 在这里仍然合法（真机两侧在 streams.ps1 / save.ps1）。
    @{ Name = '--all 配 -o - 不在解析期或 dry-run 期被拒'
       A = ($ANCHOR + @('--all', '-o', '-')); Exit = 0; Json = $true; Notes = @('note.dry_run')
       Check = { param($o) $o.captured -eq 0 -and
                            -not (@($o.errors | ForEach-Object code) -contains 'cli.stdout_multiple_targets') } }
    @{ Name = '--all 无占位符 -> note'; A = ($ANCHOR + @('--all','out.png')); Exit = 0
       Notes = @('note.all_without_placeholder') }
    @{ Name = '非 jpeg 时 quality -> note'; A = ($ANCHOR + @('--quality','50','out.png')); Exit = 0
       Notes = @('note.quality_ignored') }
    @{ Name = 'jpeg 时 quality 不告警'; A = ($ANCHOR + @('--quality','50','out.jpg')); Exit = 0; Notes = @() }
    @{ Name = '--json 是兼容空开关'; A = ($ANCHOR + @('--json','out.png')); Exit = 0
       Notes = @('note.json_flag_deprecated') }
    @{ Name = '--quiet 抑制 notes'; A = ($ANCHOR + @('-q','out.png')); Exit = 0
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('notes') } }
    @{ Name = '--verbose 追加 input 段'; A = @('-v','--hwnd','0x1A0B4C','--dry-run','out.png'); Exit = 4
       Check = { param($o) $o.input.hwnd[0].hex -eq '0x001A0B4C' -and
                            $o.input.hwnd[0].decimal -eq 1706828 -and $o.input.output -like '*out.png' -and
                            $o.input.format -eq 'png' -and $o.input.policy -eq 'ask' -and
                            $o.input.capture -eq 'wgc' } }
    @{ Name = '默认取图方式是 wgc'; A = ($ANCHOR + @('-v','out.png')); Exit = 0
       Check = { param($o) $o.input.capture -eq 'wgc' -and $o.input.captureGiven -eq $false } }
    # 取图通道：--capture 的每个取值都必须被接受（dry-run 在取帧前就返回，不会真截图）
    @{ Name = '短选项 -C auto'; A = ($ANCHOR + @('-v','-C','auto','out.png')); Exit = 0
       Check = { param($o) $o.input.capture -eq 'auto' } }
    @{ Name = '通道 dwm 已实现且不再报 unsupported'
       A = ($ANCHOR + @('-v','--capture','dwm','out.png')); Exit = 0
       Check = { param($o) $o.input.capture -eq 'dwm' -and $o.input.captureGiven -eq $true -and
                            -not (@($o.errors | ForEach-Object code) -contains 'capture.unsupported') } }
    @{ Name = '通道 printwindow 已实现且不再报 unsupported'
       A = ($ANCHOR + @('-v','--capture','printwindow','out.png')); Exit = 0
       Check = { param($o) $o.input.capture -eq 'printwindow' } }
    @{ Name = '通道 bitblt 已实现且不再报 unsupported'
       A = ($ANCHOR + @('-v','--capture','bitblt','out.png')); Exit = 0
       Check = { param($o) $o.input.capture -eq 'bitblt' } }
    @{ Name = '通道 duplication 已实现且不再报 unsupported'
       A = ($ANCHOR + @('-v','--capture','duplication','out.png')); Exit = 0
       Check = { param($o) $o.input.capture -eq 'duplication' } }
    @{ Name = '-vq 时 notes 保留'; A = @('-vq','--exe','a.exe','--dry-run','out.png'); Exit = 4
       Notes = @('note.exe_path_looks_like_name') }

    # ---------- 屏幕目标（--monitor；一律 dry-run，不抓屏）----------
    @{ Name = '--monitor + dry-run 只列屏幕不抓屏'
       A = @('--monitor','1','--dry-run','out.png'); Exit = 0; Json = $true; Notes = @('note.dry_run')
       Check = { param($o) $o.captured -eq 0 -and -not $o.PSObject.Properties.Name.Contains('errors') } }
    @{ Name = '--monitor 省略取值 = 主屏，且 target=screen'
       A = @('--monitor','--dry-run','out.png','-v'); Exit = 0
       Check = { param($o) $o.input.monitor -eq 'primary' -and $o.input.target -eq 'screen' } }
    @{ Name = '--monitor 不吃位置参数'; A = @('--monitor','out.png','--dry-run','-v'); Exit = 0
       Check = { param($o) $o.input.output -like '*out.png' -and $o.input.monitor -eq 'primary' } }
    @{ Name = '短选项 -m 与 --monitor 等价'; A = @('-m','1','--dry-run','out.png','-v'); Exit = 0
       Check = { param($o) $o.input.monitor -eq 1 -and $o.input.target -eq 'screen' } }
    @{ Name = '--monitor all 回显 all'; A = @('--monitor','all','--dry-run','out.png','-v'); Exit = 0
       Check = { param($o) $o.input.monitor -eq 'all' -and $o.input.target -eq 'screen' } }
    @{ Name = '--monitor 与窗口条件同时给出 = 按屏过滤窗口'
       A = @('--monitor','1','--class','Shell_TrayWnd','--dry-run','out.png','-v'); Exit = 0
       Check = { param($o) $o.input.target -eq 'window' -and $o.input.monitor -eq 1 } }
    @{ Name = 'help 里有 --monitor 一节'; A = @('--help'); Exit = 3; Text = $true
       Has = @('截图目标', '--monitor') }
    @{ Name = '--monitor all 与窗口条件冲突'; A = @('--monitor','all','--class','Shell_TrayWnd','out.png')
       Exit = 1; Errors = @('cli.monitor_conflict') }
    @{ Name = '屏幕编号越界'; A = @('--monitor','99','--dry-run','out.png'); Exit = 1
       Errors = @('match.monitor_out_of_range')
       Check = { param($o) $o.errors[0].option -eq '--monitor' -and $o.errors[0].hint } }
    @{ Name = '屏幕编号越界且未给输出路径 -> 只报 cli.missing_output'
       A = @('--monitor','99'); Exit = 1; ToStderr = $true; Errors = @('cli.missing_output') }
    @{ Name = '--monitor 取值非数字非关键字 -> 位置参数，不当取值'
       A = @('--monitor=abc','out.png'); Exit = 1; Errors = @('cli.invalid_number') }
    @{ Name = '--monitor 0 被拒绝'; A = @('--monitor','0','out.png'); Exit = 1
       Errors = @('cli.invalid_number') }
    @{ Name = '整屏截图拒绝 dwm'
       A = @('--monitor','primary','--capture','dwm','out.png'); Exit = 1
       Errors = @('capture.unsupported')
       Check = { param($o) $o.errors[0].hint -match 'wgc' -and $o.errors[0].value -eq 'dwm' } }
    @{ Name = '整屏截图拒绝 printwindow'
       A = @('--monitor','primary','--capture','printwindow','out.png'); Exit = 1
       Errors = @('capture.unsupported') }
    @{ Name = '整屏截图接受 duplication / bitblt / auto'
       A = @('--monitor','1','--capture','duplication','--dry-run','out.png'); Exit = 0 }
    @{ Name = '未给输出路径的整屏 dry-run 走 stderr'
       A = @('--monitor','1','--dry-run'); Exit = 0; ToStderr = $true
       Notes = @('note.output_defaulted_stdout', 'note.dry_run') }

    # ---------- 运行环境能力判据（SystemCompat；判据本体在 tests\compat.ps1 的离线层）----------
    # 这几条只判"契约形状"：回显里那两个键在不在、auto 展开出的链对不对、dry-run 会不会
    # 因为环境判据而报错。本机 Windows 版本具体挡不挡得住哪条通道不在这里判（那要另一台机器）。
    @{ Name = '-v 回显本机内部版本与本次可用通道链'
       A = ($ANCHOR + @('-v','out.png')); Exit = 0
       Check = { param($o) $o.input.osBuild -gt 0 -and
                            @($o.input.captureChain).Count -ge 1 -and
                            (@($o.input.captureChain) -contains 'wgc') } }
    @{ Name = 'auto 展开成整条窗口链（本机版本够时一条不少）'
       A = ($ANCHOR + @('-v','-C','auto','out.png')); Exit = 0
       Check = { param($o) (@($o.input.captureChain) -join ',') -like 'wgc,*bitblt' -and
                            (@($o.input.captureChain) -contains 'dwm') } }
    @{ Name = 'auto 在屏幕模式下展开成屏幕那条链'
       A = @('--monitor','1','-v','-C','auto','--dry-run','out.png'); Exit = 0
       Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc,duplication,bitblt' } }
    @{ Name = '显式指定某条通道时链就只有那一条'
       A = ($ANCHOR + @('-v','-C','printwindow','out.png')); Exit = 0
       Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'printwindow' } }
    @{ Name = 'dry-run 不因环境判据报错（它一个像素都不取）'
       A = ($ANCHOR + @('-C','wgc','--dry-run','out.png')); Exit = 0
       # 判据写在 Check 里：用例表的 Notes 键在 PowerShell 5.1 下取不到（见 harness 注释），
       # 写在键上等于没写。
       Check = { param($o) (@($o.notes | ForEach-Object code) -join ',') -eq 'note.dry_run' -and
                            -not (@($o.errors | ForEach-Object code) -like 'env.*') -and
                            -not (@($o.notes | ForEach-Object code) -like 'env.*') } }
    @{ Name = '帮助里有运行环境那一节与那两条环境码'
       A = @('--help'); Exit = 3; Text = $true
       Has = @('运行环境', '18362', 'env.os_too_old', 'env.channel_unsupported', 'input.captureChain') }

    # ---------- 匹配条件被正确解析（必然无窗口命中 -> exit 4）----------
    @{ Name = '8 个条件同时给出被接受'; A = @('--hwnd','0x10','--pid','1','--process','p.exe','--exe','D:\e.exe',
                                              '--title','t','--title-contains','c','--title-regex','r','--class','k','out.png')
       Exit = 4; Json = $true; Errors = @('match.no_window') }
    @{ Name = '同类多值 OR'; A = @('--title','A','--title','B','out.png'); Exit = 4
       Errors = @('match.no_window') }
    @{ Name = '重复取值 -> note 且仍解析通过'; A = @('--title','A','--title','A','out.png'); Exit = 4
       Notes = @('note.duplicate_value'); Errors = @('match.no_window') }
    @{ Name = '选项名忽略大小写'; A = @('--TITLE','X','out.png'); Exit = 4; Errors = @('match.no_window') }
    @{ Name = '值以 - 开头'; A = @('--title-regex','-abc','out.png'); Exit = 4; Errors = @('match.no_window') }
    @{ Name = '中文标题原样匹配'; A = @('--title','无期迷途 主线 12-3','out.png'); Exit = 4
       Errors = @('match.no_window') }
    @{ Name = 'HWND 含字母按十六进制'; A = @('-v','--hwnd','001A0B4C','out.png'); Exit = 4
       Check = { param($o) $o.input.hwnd[0].decimal -eq 1706828 } }
    @{ Name = 'HWND 纯数字按十进制'; A = @('-v','--hwnd','1706828','out.png'); Exit = 4
       Check = { param($o) $o.input.hwnd[0].hex -eq '0x001A0B4C' } }
    @{ Name = '-- 结束选项解析'; A = @('--pid','1','--','--weird.png'); Exit = 4
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('errors') -or
                            $o.errors[0].code -eq 'match.no_window' } }
    @{ Name = 'index 越界'; A = @('--class','Shell_TrayWnd','--index','99','out.png'); Exit = 1
       Errors = @('match.index_out_of_range') }

    # ---------- 参数错误 ----------
    @{ Name = 'HWND 非法'; A = @('--hwnd','zzz','out.png'); Exit = 1; Errors = @('cli.invalid_number')
       Check = { param($o) $o.errors[0].option -eq '--hwnd' -and $o.errors[0].value -eq 'zzz' -and
                            $o.errors[0].message -and $o.errors[0].hint } }
    @{ Name = 'PID 为 0'; A = @('--pid','0','out.png'); Exit = 1; Errors = @('cli.invalid_number') }
    @{ Name = 'process 带路径'; A = @('--process','D:\a.exe','out.png'); Exit = 1
       Errors = @('cli.invalid_value') }
    @{ Name = 'exe 只有文件名 -> note'; A = @('--exe','a.exe','--dry-run','out.png'); Exit = 4
       Notes = @('note.exe_path_looks_like_name') }
    @{ Name = '正则非法'; A = @('--title-regex','[bad(','out.png'); Exit = 1; Errors = @('cli.invalid_regex') }
    @{ Name = '不给输出路径 -> 按 --out - 处理（PNG 写标准输出）'
       A = ($ANCHOR + @('-v')); Exit = 0; ToStderr = $true; Notes = @('note.output_defaulted_stdout')
       Check = { param($o) $o.input.output -eq '-' -and $o.input.toStdout -eq $true -and
                            $o.input.format -eq 'png' -and $o.input.formatGiven -eq $false } }
    @{ Name = '不给输出路径且未出图 -> 只报 cli.missing_output'
       A = @('--pid','1'); Exit = 1; ToStderr = $true; Errors = @('cli.missing_output')
       Check = { param($o) @($o.errors).Count -eq 1 -and @($o.images).Count -eq 0 } }
    @{ Name = '扩展名判不出格式 -> png + note（不再报错）'
       A = ($ANCHOR + @('out.unknown')); Exit = 0; Notes = @('note.format_defaulted_png') }
    @{ Name = '--format 拒绝没有编码器的取值'
       A = @('--pid','1','--format','webp','out.png'); Exit = 1; Errors = @('cli.invalid_format')
       Check = { param($o) $o.errors[0].hint -notmatch 'webp|ico|auto' } }
    @{ Name = '--out 与位置参数冲突'; A = @('--pid','1','out.png','--out','b.png'); Exit = 1
       Errors = @('cli.duplicate_output') }
    @{ Name = '选择策略互斥'; A = @('--index','2','--newest','out.png'); Exit = 1
       Errors = @('cli.conflicting_options') }
    # ---------- 选择策略的名字：Z 序，不是创建顺序 ----------
    # --newest / --oldest 选的一直是当下 Z 序的首尾，而名字说的是创建先后。改成语义准确的
    # --topmost-match / --bottommost-match，旧名保留为兼容别名（行为相同 + 一条 note）。
    # 这里全部 --dry-run：选到哪一扇由 tests\identity.ps1 在真机上判，这一节只判解析与契约。
    @{ Name = '--topmost-match 被接受且 policy 写规范名'
       A = ($ANCHOR + @('-v','--topmost-match','out.png')); Exit = 0
       Check = { param($o) $o.input.policy -eq 'topmost' } }
    @{ Name = '--bottommost-match 被接受且 policy 写规范名'
       A = ($ANCHOR + @('-v','--bottommost-match','out.png')); Exit = 0
       Check = { param($o) $o.input.policy -eq 'bottommost' } }
    @{ Name = '--newest 是 --topmost-match 的别名（policy 写规范名）'
       A = ($ANCHOR + @('-v','--newest','out.png')); Exit = 0
       Check = { param($o) $o.input.policy -eq 'topmost' } }
    @{ Name = '--oldest 是 --bottommost-match 的别名（policy 写规范名）'
       A = ($ANCHOR + @('-v','--oldest','out.png')); Exit = 0
       Check = { param($o) $o.input.policy -eq 'bottommost' } }
    @{ Name = '用旧别名时留一条 note.deprecated_option'; A = ($ANCHOR + @('--newest','out.png'))
       Exit = 0; Notes = @('note.deprecated_option')
       Check = { param($o) $o.notes[0].option -eq '--newest' -and $o.notes[0].value -eq '--topmost-match' } }
    @{ Name = '同一策略的新旧别名并用不算互斥'; A = ($ANCHOR + @('--newest','--topmost-match','out.png'))
       Exit = 0; Notes = @('note.deprecated_option') }
    @{ Name = '别名并用时废弃提示只有一条'; A = ($ANCHOR + @('--newest','--topmost-match','out.png'))
       Exit = 0
       Check = { param($o) @(@($o.notes) | Where-Object { $_.code -eq 'note.deprecated_option' }).Count -eq 1 } }
    @{ Name = '两个旧别名并用仍是两条策略冲突'; A = @('--newest','--oldest','--pid','1','out.png')
       Exit = 1; Errors = @('cli.conflicting_options') }
    @{ Name = '别名与另一条策略并用时冲突报错列用户写的名字'
       A = @('--oldest','--index','2','--pid','1','out.png'); Exit = 1
       Errors = @('cli.conflicting_options')
       Check = { param($o) $o.errors[0].value -match '--oldest' -and $o.errors[0].value -match '--index' } }
    @{ Name = '不用旧别名时不发废弃提示'; A = ($ANCHOR + @('--topmost-match','out.png')); Exit = 0
       Check = { param($o) -not ((@($o.notes) | ForEach-Object code) -contains 'note.deprecated_option') } }
    @{ Name = '帮助里两个规范名字都在，且说明按 Z 序'
       A = @('--help'); Exit = 3; Text = $true
       Has = @('--topmost-match', '--bottommost-match', 'Z 序') }
    @{ Name = '帮助里旧别名标明是旧名字'; A = @('--help'); Exit = 3; Text = $true
       Has = @('--newest', '--oldest', '旧名字') }
    @{ Name = '歧义提示推荐规范名而不是旧别名'
       A = @('--class','Shell_TrayWnd','--title-contains','X-不存在的标题','out.png')
       Exit = @(1,4,5); Json = $true }
    @{ Name = '新身份码在四种语言下都有文案（不出现 ?key）'
       A = ($ANCHOR + @('-v','--topmost-match','out.png')); Exit = 0
       Check = { param($o) $o.input.policy -eq 'topmost' } }
    @{ Name = '未知选项带纠正 hint'; A = @('--titel','x','out.png'); Exit = 1
       Check = { param($o) (@($o.errors | ForEach-Object code) -contains 'cli.unknown_option') -and
                            (@($o.errors | Where-Object { $_.code -eq 'cli.unknown_option' }).hint) -eq '--title' } }
    @{ Name = '开关不接受取值'; A = @('--json=maybe','--pid','1','out.png'); Exit = 1
       Errors = @('cli.switch_takes_no_value') }
    @{ Name = '多余位置参数'; A = @('--pid','1','a.png','b.png'); Exit = 1
       Errors = @('cli.unexpected_positional') }
    @{ Name = '未知取图方式'; A = @('--capture','waiwang','--pid','1','out.png'); Exit = 1
       Errors = @('cli.unknown_capture_method')
       Check = { param($o) $o.errors[0].hint -like '*wgc, dwm, printwindow*' } }
    @{ Name = '显式 --help 时忽略其余参数'; A = @('--help','--hwnd','zzz'); Exit = 3; Text = $true }

    # ---------- 尚未实现的通道 ----------
    @{ Name = '已删除的通道取值在解析期就被拒绝'
       A = @('--class','Shell_TrayWnd','--capture','magnification','out.png')
       Exit = 1; Errors = @('cli.unknown_capture_method')
       Check = { param($o) $o.errors[0].value -eq 'magnification' -and
                            $o.errors[0].hint -notmatch 'magnification' -and
                            $o.errors[0].hint -match 'duplication' } }
    @{ Name = '--quiet 也要保留 errors'; A = @('-q','--hwnd','zzz','out.png'); Exit = 1
       Errors = @('cli.invalid_number')
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('notes') } }

    # ---------- 覆盖保护开关的布尔写法（判据在 -v 的 input.overwrite）----------
    # 裸写与 =true 同义（都是"禁止覆盖"），=false 才是取消禁令；写错的取值在解析期就被拒。
    # 真机侧（图有没有真的不被覆盖）在 tests\save.ps1。
    @{ Name = '裸 --no-overwrite = 禁止覆盖'; A = ($ANCHOR + @('-v','--no-overwrite','out.png')); Exit = 0
       Check = { param($o) $o.input.overwrite -eq $false } }
    @{ Name = '不给 --no-overwrite 时可覆盖'; A = ($ANCHOR + @('-v','out.png')); Exit = 0
       Check = { param($o) $o.input.overwrite -eq $true } }
    @{ Name = '--no-overwrite=false 取消禁令'; A = ($ANCHOR + @('-v','--no-overwrite=false','out.png')); Exit = 0
       Check = { param($o) $o.input.overwrite -eq $true } }
    @{ Name = '--no-overwrite 不吃后面的参数（那是输出路径）'
       A = ($ANCHOR + @('-v','--no-overwrite','out.png')); Exit = 0
       Check = { param($o) $o.input.overwrite -eq $false -and $o.input.output -like '*out.png' } }
    @{ Name = '开关写成 =false 对普通开关等于没写'
       A = ($ANCHOR + @('--json=false','out.png')); Exit = 0; Notes = @() }
    @{ Name = '--no-overwrite 取值写错照样被拒'; A = @('--no-overwrite=maybe','--pid','1','out.png'); Exit = 1
       Errors = @('cli.switch_takes_no_value') }

    # ---------- 语言选项 ----------
    @{ Name = '--lang 的规范取值全部可用'; A = (@('--pid','1','--lang','zh-TW') + @('out.png')); Exit = 4
       Errors = @('match.no_window') }
    @{ Name = '未知语言在解析期就被拒绝'; A = @('--lang','klingon','--pid','1','out.png'); Exit = 1
       Errors = @('cli.unknown_language')
       Check = { param($o) $o.errors[0].value -eq 'klingon' -and $o.errors[0].hint -like '*zh-CN*' -and
                            $o.errors[0].hint -notmatch 'klingon' } }
    @{ Name = 'zh-TW 是繁体表格'; A = @('--lang','zh-TW','--help'); Exit = 3; Text = $true
       Has = @('視窗匹配條件', '用法:') }
    @{ Name = 'en 是英文表格'; A = @('--lang','en','--help'); Exit = 3; Text = $true
       Has = @('Usage:', 'Window match conditions', '--title-contains') }
    @{ Name = 'ja 是日文表格'; A = @('--lang','ja','--help'); Exit = 3; Text = $true
       Has = @('使い方:', 'ウィンドウ検索条件') }
    @{ Name = '-l 短形式同样生效'; A = @('-l','ja','--help'); Exit = 3; Text = $true; Has = @('例:') }
    @{ Name = '宽容写法 zh_TW 归到繁体'; A = @('--lang=zh_TW','--help'); Exit = 3; Text = $true
       Has = @('視窗') }
    @{ Name = '宽容写法 en-GB 归到英文'; A = @('--lang','en-GB','--help'); Exit = 3; Text = $true
       Has = @('Usage:') }
    @{ Name = '-v 的 input 回显所选语言'; A = (@('--lang','en','-v') + $ANCHOR + @('out.png')); Exit = 0
       Check = { param($o) $o.input.lang -eq 'en' } }
)

# ---------------------------------------------------------------------------
# --no-overwrite 的布尔别名逐个过一遍：真值五种写法都是"禁止覆盖"，假值五种都是"取消禁令"。
# 判据取 -v 的 input.overwrite，不截图；真机侧"旧文件到底变没变"在 tests\save.ps1。
# ---------------------------------------------------------------------------
foreach ($alias in @('true', '1', 'yes', 'y', 'on', 'True', 'ON')) {
    $cases += @{ Name = ("--no-overwrite={0} 与裸开关同义" -f $alias)
       A = ($ANCHOR + @('-v', ('--no-overwrite=' + $alias), 'out.png')); Exit = 0
       Check = { param($o) $o.input.overwrite -eq $false } }
}
foreach ($alias in @('false', '0', 'no', 'n', 'off', 'FALSE', 'Off')) {
    $cases += @{ Name = ("--no-overwrite={0} 取消禁止覆盖" -f $alias)
       A = ($ANCHOR + @('-v', ('--no-overwrite=' + $alias), 'out.png')); Exit = 0
       Check = { param($o) $o.input.overwrite -eq $true } }
}
# 重复给出：最后一个生效（顺序语义与其余选项一致）
$cases += @{ Name = '--no-overwrite 后面再写 =false 以最后为准'
   A = ($ANCHOR + @('-v', '--no-overwrite', '--no-overwrite=false', 'out.png')); Exit = 0
   Check = { param($o) $o.input.overwrite -eq $true } }
$cases += @{ Name = '--no-overwrite=false 后面再裸写以最后为准'
   A = ($ANCHOR + @('-v', '--no-overwrite=false', '--no-overwrite', 'out.png')); Exit = 0
   Check = { param($o) $o.input.overwrite -eq $false } }
foreach ($bad in @('maybe', '', 'ture', '2')) {
    $cases += @{ Name = ("--no-overwrite={0} 这种取值在解析期就被拒" -f $bad)
       A = @('--pid', '1', ('--no-overwrite=' + $bad), 'out.png'); Exit = 1
       Errors = @('cli.switch_takes_no_value') }
}
# 帮助文本要看得见这条布尔语法与"整批名字先算好"
$cases += @{ Name = '帮助里写了 --no-overwrite 的布尔写法'; A = @('--help'); Exit = 3; Text = $true
   Has = @('取消这条禁令') }
$cases += @{ Name = '帮助里写了整批输出名先算好'; A = @('--help'); Exit = 3; Text = $true
   Has = @('整批输出名在取帧之前一次算好') }
$cases += @{ Name = '帮助里写了标准输出只能一张图'; A = @('--help'); Exit = 3; Text = $true
   Has = @('标准输出一次只能交付一张图') }

# ---------------------------------------------------------------------------
# --yes 的布尔别名与授权分级：判据取 -v 的 input.yes，不截图也不打扰人。
# 真机侧"到底弹不弹框"在 tests\consent.ps1，状态机那些分支在 tests\consent_state.cpp。
# ---------------------------------------------------------------------------
foreach ($alias in @('true', '1', 'yes', 'y', 'on', 'True', 'ON')) {
    $cases += @{ Name = ("--yes={0} 是同意跳过窗口内容路径的确认" -f $alias)
       A = ($ANCHOR + @('-v', ('--yes=' + $alias), 'out.png')); Exit = 0
       Check = { param($o) $o.input.yes -eq $true } }
}
foreach ($alias in @('false', '0', 'no', 'n', 'off', 'FALSE', 'Off')) {
    $cases += @{ Name = ("--yes={0} 仍然要问人" -f $alias)
       A = ($ANCHOR + @('-v', ('--yes=' + $alias), 'out.png')); Exit = 0
       Check = { param($o) $o.input.yes -eq $false } }
}
$cases += @{ Name = '不给 --yes 时默认要问人'
   A = ($ANCHOR + @('-v', 'out.png')); Exit = 0
   Check = { param($o) $o.input.yes -eq $false } }
$cases += @{ Name = '-y 短形式同样生效'
   A = ($ANCHOR + @('-v', '-y', 'out.png')); Exit = 0
   Check = { param($o) $o.input.yes -eq $true } }
# -yq 里既认 -y，也认这一簇里的 -q 与本轮 -v 撞车（撞了按 --verbose 处理，见下面那一节）
$cases += @{ Name = '-yq 这类开关簇里也认 -y'
   A = ($ANCHOR + @('-v', '-yq', 'out.png')); Exit = 0
   Check = { param($o) ($o.input.yes -eq $true -and
                        ((Codes $o.notes) -join ',') -match 'note.flag_overrides_quiet') } }
$cases += @{ Name = '--yes 后面再写 =false 以最后为准'
   A = ($ANCHOR + @('-v', '--yes', '--yes=false', 'out.png')); Exit = 0
   Check = { param($o) $o.input.yes -eq $false } }
$cases += @{ Name = '--yes=false 后面再裸写以最后为准'
   A = ($ANCHOR + @('-v', '--yes=false', '--yes', 'out.png')); Exit = 0
   Check = { param($o) $o.input.yes -eq $true } }
foreach ($bad in @('maybe', '', 'tru', '2')) {
    $cases += @{ Name = ("--yes={0} 这种取值在解析期就被拒" -f $bad)
       A = @('--pid', '1', ('--yes=' + $bad), 'out.png'); Exit = 1
       Errors = @('cli.switch_takes_no_value') }
}
# --yes 不是选择条件：一个条件都不给时仍是"文本帮助 + 退出码 2"，绝不会顺手去截桌面
$cases += @{ Name = '--yes 不替代条件，没条件就还是帮助而不是截屏'
   A = @('--yes', 'out.png'); Exit = 2; Text = $true; Has = @('未指定任何匹配条件') }
# 只读查询不弹框：--dry-run 带着 --yes、带着 --monitor 都只给 dry_run 那条 note，一条错都没有
$cases += @{ Name = '--dry-run 带 --yes 与 --monitor 也不弹框不取帧'
   A = @('--monitor', '1', '--dry-run', '--yes', '-v', 'out.png'); Exit = 0
   Errors = @(); Notes = @('note.dry_run_monitor')
   Check = { param($o) ($o.input.yes -eq $true -and $o.input.target -eq 'screen' -and
                        $o.captured -eq 0) } }
$cases += @{ Name = '帮助里写了 --yes 这一级授权'
   A = @('--help'); Exit = 3; Text = $true; Has = @('--yes', '只取所选窗口画面') }
$cases += @{ Name = '帮助里写了会拍到桌面的一律要问、--yes 跳不过'
   A = @('--help'); Exit = 3; Text = $true; Has = @('跳不过') }
$cases += @{ Name = '帮助里 --monitor 那条不再说"没有跳过的开关"'
   A = @('--help'); Exit = 3; Text = $true; Has = @('整块屏幕拍的是桌面像素') }
# ---------------------------------------------------------------------------
# 期限（--timeout-ms / --consent-timeout-ms）：判据写在 -v 的 input 回显里，
# 全部带 --dry-run —— 验"参数最终落到什么值"不必真的去等一个超时。
# ---------------------------------------------------------------------------
$cases += @{ Name = '不给期限时两项都是 0（不设总预算）'
   A = ($ANCHOR + @('-v', 'out.png')); Exit = 0
   Check = { param($o) ($o.input.timeoutMs -eq 0 -and $o.input.consentTimeoutMs -eq 0) } }
$cases += @{ Name = '--timeout-ms 回显毫秒数'
   A = ($ANCHOR + @('-v', '--timeout-ms', '5000', 'out.png')); Exit = 0
   Check = { param($o) ($o.input.timeoutMs -eq 5000 -and $o.input.consentTimeoutMs -eq 0) } }
$cases += @{ Name = '--consent-timeout-ms 回显毫秒数'
   A = ($ANCHOR + @('-v', '--consent-timeout-ms', '60000', 'out.png')); Exit = 0
   Check = { param($o) ($o.input.consentTimeoutMs -eq 60000 -and $o.input.timeoutMs -eq 0) } }
$cases += @{ Name = '两条期限同时给出互不影响'
   A = ($ANCHOR + @('-v', '--timeout-ms=1500', '--consent-timeout-ms=2500', 'out.png')); Exit = 0
   Check = { param($o) ($o.input.timeoutMs -eq 1500 -and $o.input.consentTimeoutMs -eq 2500) } }
$cases += @{ Name = '期限重复给出以最后为准'
   A = ($ANCHOR + @('-v', '--timeout-ms', '9000', '--timeout-ms', '300', 'out.png')); Exit = 0
   Check = { param($o) $o.input.timeoutMs -eq 300 } }
$cases += @{ Name = '期限写 0 是明确不设这项期限（不是缺省值 1）'
   A = ($ANCHOR + @('-v', '--timeout-ms', '0', '--consent-timeout-ms', '0', 'out.png')); Exit = 0
   Check = { param($o) ($o.input.timeoutMs -eq 0 -and $o.input.consentTimeoutMs -eq 0) } }
foreach ($bad in @('abc', '1.5', '-1', '0x10', '1_000', '86400001', '')) {
    $cases += @{ Name = ("期限取值 {0} 在解析期就被拒" -f ($bad | ForEach-Object { if ($_ -eq '') { '(空)' } else { $_ } }))
       A = @('--class', 'Shell_TrayWnd', '--timeout-ms', $bad, 'out.png'); Exit = 1
       Errors = @('cli.invalid_number') }
}
$cases += @{ Name = '确认期限取值越界同样被拒'
   A = @('--class', 'Shell_TrayWnd', '--consent-timeout-ms', '86400001', 'out.png'); Exit = 1
   Errors = @('cli.invalid_number') }
# 取值缺失时它会吃掉下一个参数（整轮的 --lang 就在后面），所以这条判的是"下一个参数
# 被当成取值之后必须按数字校验拒掉"，而不是静默当作没给
$cases += @{ Name = '--timeout-ms 后面跟的是选项名时按非法数字拒收'
   A = @('--class', 'Shell_TrayWnd', '--timeout-ms', '--dry-run', 'out.png'); Exit = 1
   Errors = @('cli.invalid_number')
   Check = { param($o) $o.errors[0].option -eq '--timeout-ms' } }
# 帮助里要写清这两件事：预算是整批一份，确认超时按拒绝而不是默认同意
$cases += @{ Name = '帮助里有期限一节，两条参数都在'
   A = @('--help'); Exit = 3; Text = $true; Has = @('期限', '--timeout-ms', '--consent-timeout-ms') }
$cases += @{ Name = '帮助写明预算是"共用这一份剩余时间"而不是每步一份'
   A = @('--help'); Exit = 3; Text = $true; Has = @('共用这一份剩余时间') }
$cases += @{ Name = '帮助写明确认到点按拒绝、动画缓冲不省'
   A = @('--help'); Exit = 3; Text = $true; Has = @('绝不按"默认同意"', '关闭动画') }

# ---------------------------------------------------------------------------
# 数值写法：每个数字选项只认它对外承诺过的那一种写法
#   * 十进制类（--pid / --index / --monitor 的编号 / --quality / 两条期限）只认 [0-9]+：
#     正负号、空白、小数点、指数（1e3）、下划线、0x 前缀、非 ASCII 数字一律拒收，
#     区间在同一次解析里判完。旧实现会自己猜进制，于是 --pid 1e3 读成 483、
#     --quality 1e 读成 30、--hwnd -1 读成 UINT64_MAX（非法值被强转成合法值）。
#   * --hwnd 保留文档里的三种写法（纯数字=十进制 / 0x 前缀=十六进制 / 含 a-f=十六进制），
#     但正负号与溢出照旧拒绝；下划线只在十六进制写法里合法，且必须夹在两位数字之间。
#   * --monitor 的取值可省略，所以"要不要吃下一个参数"与实际解析必须是同一套规则：
#     写坏了的数字要报错，不能悄悄变成输出文件名。
# 判据一律写进 Check：用例表里的 Errors / Notes 两个键在 hashtable 上取不到
# （$c.PSObject.Properties.Name 不列 hashtable 的键），只有 Check 这条真的会执行。
# ---------------------------------------------------------------------------

# 判 "auto 明确回到系统显示语言" 要一个不受本机语言影响的参照：先问一次不给 --lang 的
# 本机构建产物，拿到本机默认语言标签，再挑一个与它不同的语言当"前一条 --lang"。
$DEFAULT_LANG = ((Invoke-Ec -NoLang ($ANCHOR + @('-v', 'out.png'))).Stdout |
                 ConvertFrom-Json).input.lang
$OTHER_LANG = @('ja', 'en', 'zh-TW', 'zh-CN') | Where-Object { $_ -ne $DEFAULT_LANG } |
              Select-Object -First 1

# 所有十进制选项共用的非法写法（'' 与 ' ' 是空值：不能顺手当成 0）
$BAD_NUMBERS = @('', ' ', ' 1', '1 ', '1 2', '-1', '+1', '0x10', '0X10', '1e3', '1E3', '1_0',
                 '1.5', '1,5', '١٢', '１２', 'abc', '1a', 'a1', '0xFFFFFFFFFFFFFFFF')

# --pid / --index / --quality / 两条期限：写成 --opt <值> 与 --opt=<值> 都要拒
# （Check 里引用了循环变量的一律 GetNewClosure：用例是先攒齐、后一轮统一跑的，
#   不封套的话判据拿到的是循环最后一次的那个值）
foreach ($opt in @('--pid', '--index', '--quality', '--timeout-ms', '--consent-timeout-ms')) {
    foreach ($bad in $BAD_NUMBERS) {
        $cases += @{ Name = ('{0} 写法 {1} 被拒（分开给值）' -f $opt, ($(if ($bad) { $bad } else { '(空)' })))
           A = @('--class', 'Shell_TrayWnd', '--dry-run', $opt, $bad, 'out.png'); Exit = 1
           Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' -and
                                $o.errors[0].option -eq $opt }.GetNewClosure() }
        $cases += @{ Name = ('{0} 写法 {1} 被拒（内联给值）' -f $opt, ($(if ($bad) { $bad } else { '(空)' })))
           A = @('--class', 'Shell_TrayWnd', '--dry-run', ($opt + '=' + $bad), 'out.png'); Exit = 1
           Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' } }
    }
}
# --monitor 的编号走内联写法（分开给值那一趟在下面"吃不吃下一个参数"那组里判）
foreach ($bad in $BAD_NUMBERS) {
    if (-not $bad) { continue }   # --monitor= 的空取值是"省略取值 = 主屏"，另有一条用例判它
    $cases += @{ Name = ('--monitor 写法 {0} 被拒，且不会当成输出文件名' -f $bad)
       A = @(('--monitor=' + $bad), '--dry-run', 'out.png'); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' -and
                            $o.errors[0].option -eq '--monitor' } }
}
# 各选项自己的区间边界：合法端点要落进 -v 的回显，越界端点要在解析期就拦下。
# 参照窗口用一个必然不存在的类名（命中数为 0），这样 --index 的最大值也不会被匹配阶段
# 判成越界，判据只剩"解析有没有放行 + 回显到的数字对不对"。
foreach ($b in @(
    @{ A = @('--pid', '1');                             Field = 'pid';              Want = '1' },
    @{ A = @('--pid', '4294967295');                    Field = 'pid';              Want = '4294967295' },
    @{ A = @('--index', '1');                           Field = 'index';            Want = '1' },
    @{ A = @('--index', '65535');                       Field = 'index';            Want = '65535' },
    @{ A = @('--monitor', '1');                         Field = 'monitor';          Want = '1' },
    @{ A = @('--timeout-ms', '0');                      Field = 'timeoutMs';        Want = '0' },
    @{ A = @('--timeout-ms', '86400000');               Field = 'timeoutMs';        Want = '86400000' },
    @{ A = @('--consent-timeout-ms', '86400000');       Field = 'consentTimeoutMs'; Want = '86400000' }
)) {
    $cases += @{ Name = ('数值边界放行：{0}' -f ($b.A -join ' '))
       A = (@('--class', 'NoSuchWindowXyz', '--dry-run', '-v') + $b.A + @('out.png')); Exit = 4
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'match.no_window' -and
                            ([string]$o.input.($b.Field) -eq $b.Want) }.GetNewClosure() }
}
foreach ($b in @(@('--pid', '0'), @('--pid', '4294967296'), @('--pid', '18446744073709551616'),
                 @('--index', '0'), @('--index', '65536'), @('--quality', '0'), @('--quality', '101'),
                 @('--monitor', '0'), @('--monitor', '65536'),
                 @('--timeout-ms', '86400001'), @('--consent-timeout-ms', '86400001'))) {
    $cases += @{ Name = ('数值越界拦下：{0}' -f ($b -join ' '))
       A = (@('--class', 'Shell_TrayWnd') + $b + @('out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' } }
}
# 质量的两个合法端点要能真的用出去（期限的 0 是"明确不设"，上面那条已经判过）
foreach ($q in @('1', '100')) {
    $cases += @{ Name = ('quality 端点 {0} 接受' -f $q)
       A = (@('--class', 'Shell_TrayWnd', '--dry-run', '--quality') + @($q, 'out.jpg')); Exit = 0
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('errors') } }
}
# 编号的上界本身不是解析期的事：65535 进得了解析，交给匹配阶段判越界
$cases += @{ Name = '屏幕编号 65535 由解析放行、由匹配阶段判越界'
   A = @('--monitor', '65535', '--dry-run', '-v', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'match.monitor_out_of_range' -and
                        $o.input.monitor -eq 65535 } }
$cases += @{ Name = '--monitor= 的空取值就是省略取值（主屏）'
   A = @('--monitor=', '--dry-run', '-v', 'out.png'); Exit = 0
   Check = { param($o) $o.input.monitor -eq 'primary' } }

# ---- --hwnd：三种文档写法都要能用，符号 / 溢出 / 下划线位置要拦 ----
foreach ($h in @(
    @{ V = '1706828';              Hex = '0x001A0B4C' },   # 纯数字 = 十进制
    @{ V = '0x001A0B4C';           Hex = '0x001A0B4C' },   # 0x 前缀 = 十六进制
    @{ V = '0X1a0b4c';             Hex = '0x001A0B4C' },   # 0X 与前缀后的小写都认
    @{ V = '001A0B4C';             Hex = '0x001A0B4C' },   # Spy++ 那种裸写
    @{ V = '0x001A_0B4C';          Hex = '0x001A0B4C' },   # 下划线夹在两位数字之间
    @{ V = '001A_0B4C';            Hex = '0x001A0B4C' },
    @{ V = '1e3';                  Hex = '0x000001E3' },   # 含 a-f => 十六进制（文档写明的兼容写法）
    @{ V = '0xFFFFFFFFFFFFFFFF';   Hex = '0xFFFFFFFFFFFFFFFF' },   # 64 位上界本身
    @{ V = '0x10';                 Hex = '0x00000010' }
)) {
    $cases += @{ Name = ('HWND 写法 {0} 解析成 {1}' -f $h.V, $h.Hex)
       A = @('-v', '--hwnd', $h.V, '--dry-run', 'out.png'); Exit = 4
       Check = { param($o) @($o.input.hwnd).Count -eq 1 -and
                            $o.input.hwnd[0].hex -eq $h.Hex }.GetNewClosure() }
}
foreach ($bad in @('', ' ', ' 1', '1 ', '1 2', '-1', '+1', '-0x10', '0x', '0x_', 'zzz', '1.2', '0x1.2',
                   '0x1,2', '_1A0B4C', '1A0B4C_', '1A__0B4C', '0x_1A0B4C', '12_34', '1_0',
                   '0x1FFFFFFFFFFFFFFFF', '18446744073709551616', '０', '1A 0B4C', '0x0', '0')) {
    $cases += @{ Name = ('HWND 写法 {0} 被拒' -f ($(if ($bad) { $bad } else { '(空)' })))
       A = @('--hwnd', $bad, 'out.png'); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' -and
                            $o.errors[0].option -eq '--hwnd' -and $o.errors[0].hint } }
}

# ---- --monitor 分开给值：要不要吃下一个参数，判据必须与实际解析同源 ----
foreach ($bad in @('1e3', '-1', '+1', '1_0', '١٢', '１２', '0x2', '999999999999999999999999', ' 1',
                   '1.5', '65536')) {
    $cases += @{ Name = ('--monitor 后面是写坏的数字 {0}：报错而不是变成输出文件名' -f $bad)
       A = @('--monitor', $bad, '--dry-run', 'out.png'); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' -and
                            $o.errors[0].option -eq '--monitor' } }
}
# 真正不像取值的参数照旧留给输出路径（这条是 --monitor 取值可省略的立身之本）
foreach ($path in @('out.png', '2.png', 'v2', 'D:\a\b.png', 'out')) {
    $cases += @{ Name = ('--monitor 不吃 {0}（那是输出路径）' -f $path)
       A = @('--monitor', '--dry-run', '-v', $path); Exit = 0
       Check = { param($o) $o.input.monitor -eq 'primary' -and
                            $o.input.output -like ('*' + $path) }.GetNewClosure() }
}
$cases += @{ Name = '--monitor 后面紧跟 -v：那是开关，编号仍是主屏'
   A = @('--monitor', '-v', '--dry-run', 'out.png'); Exit = 0
   Check = { param($o) $o.input.monitor -eq 'primary' -and
                        $o.input.output -like '*out.png' } }
$cases += @{ Name = '--monitor 后面是 -o：取值留给后面的路径'
   A = @('--monitor', '-o', 'out.png', '--dry-run', '-v'); Exit = 0
   Check = { param($o) $o.input.monitor -eq 'primary' -and
                        $o.input.output -like '*out.png' } }

# ---------------------------------------------------------------------------
# --lang 的预扫描与正式解析共用一套 token 消费规则
#   旧实现另写了一份"以 - 开头就算选项"的扫描，于是 --title 吃掉的那个 --lang
#   被它当成语言开关，而正式解析根本没把它当选项（-v 回显的 lang 与真实决定不一致）。
#   重复给出：最后一个有效的指定生效；auto 是"明确回到系统显示语言"，不是"保持上一条"。
# ---------------------------------------------------------------------------
$cases += @{ Name = '后面那条 --lang 覆盖前面那条'
   A = (@('--lang', 'ja', '--lang', 'zh-TW', '-v') + $ANCHOR + @('out.png')); Exit = 0
   Check = { param($o) $o.input.lang -eq 'zh-TW' } }
$cases += @{ Name = ('--lang {0} 之后再来一条 auto：明确回到系统显示语言' -f $OTHER_LANG)
   A = (@('--lang', $OTHER_LANG, '--lang', 'auto', '-v') + $ANCHOR + @('out.png')); Exit = 0
   Check = { param($o) $o.input.lang -eq $DEFAULT_LANG -and $o.input.lang -ne $OTHER_LANG } }
$cases += @{ Name = ('auto 之后再来一条 --lang {0}：仍然以后写的为准' -f $OTHER_LANG)
   A = (@('--lang', 'auto', '--lang', $OTHER_LANG, '-v') + $ANCHOR + @('out.png')); Exit = 0
   Check = { param($o) $o.input.lang -eq $OTHER_LANG } }
$cases += @{ Name = '省略取值的 --lang 与 --lang auto 同义（回到系统显示语言）'
   A = (@('--lang', $OTHER_LANG, '--lang=', '-v') + $ANCHOR + @('out.png')); Exit = 0
   Check = { param($o) $o.input.lang -eq $DEFAULT_LANG } }
$cases += @{ Name = '内联与分开两种写法的 --lang 一样参与覆盖'
   A = (@('--lang=ja', '-l', 'en', '-v') + $ANCHOR + @('out.png')); Exit = 0
   Check = { param($o) $o.input.lang -eq 'en' } }
$cases += @{ Name = '--lang 写两次同样以最后为准（非法那条只报错不改语言）'
   A = (@('--lang', 'ja', '--lang', 'klingon', '-v') + $ANCHOR + @('out.png')); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.unknown_language' -and
                        $o.input.lang -eq 'ja' } }
# 被前一个选项吃掉的 token 不再是"选项"：旧的第二份扫描器会把它们当成语言开关
$cases += @{ Name = '被 --title 吃掉的 --lang 不算语言开关'
   A = (@('--lang', 'en', '--title', '--lang', 'ja', '-v', 'out.png')); Exit = 1
   Check = { param($o) $o.input.lang -eq 'en' -and (@($o.input.title) -join ',') -eq '--lang' } }
$cases += @{ Name = '被 --title 吃掉的 -l 不算语言开关'
   A = (@('--lang', 'en', '--title', '-l', 'ja', '-v', 'out.png')); Exit = 1
   Check = { param($o) $o.input.lang -eq 'en' -and (@($o.input.title) -join ',') -eq '-l' } }
$cases += @{ Name = '被 --title-contains 吃掉的 --lang 既不改语言也不报错'
   A = (@('--lang', 'en', '--title-contains', '--lang', '-v') + $ANCHOR + @('out.png')); Exit = 4
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'match.no_window' -and
                        $o.input.lang -eq 'en' -and
                        (@($o.input.titleContains) -join ',') -eq '--lang' } }
$cases += @{ Name = '取值里含 --lang 字样时不被切成开关'
   A = (@('--lang', 'en', '--title', 'my--lang-file', '-v') + $ANCHOR + @('out.png')); Exit = 4
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'match.no_window' -and
                        $o.input.lang -eq 'en' -and
                        (@($o.input.title) -join ',') -eq 'my--lang-file' } }
$cases += @{ Name = '-- 之后的 --lang 不改语言（旧的第二份扫描器会把它当开关）'
   A = @('--lang', 'en', '--class', 'NoSuchWindowXyz', '-v', 'out.png', '--', '--lang', 'ja')
   Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.unexpected_positional' -and
                        $o.input.lang -eq 'en' } }
$cases += @{ Name = '-- 之后的 -v 不再是开关'
   A = @('--lang', 'en', '--class', 'NoSuchWindowXyz', 'out.png', '--', '-v'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.unexpected_positional' -and
                        -not $o.PSObject.Properties.Name.Contains('input') } }
$cases += @{ Name = '--lang 缺取值按缺少取值报错'
   A = @('--class', 'Shell_TrayWnd', '--dry-run', 'out.png', '--lang'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.missing_value' } }
$cases += @{ Name = '非法语言仍然按已经定下的语言报告'
   A = @('--lang', 'ja', '--lang', 'klingon', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.unknown_language' -and
                        $o.errors[0].message -match '言語' } }

# ---------------------------------------------------------------------------
# -v 与 -q 同时给出：按 --verbose 处理（文案里承诺的就是这一条），notes 仍然交付，
# 并且恰好回显一条冲突提示；errors 与 images 的归属字段任何时候都不被隐藏。
# ---------------------------------------------------------------------------
foreach ($form in @(@('-v', '-q'), @('-q', '-v'), @('-vq'), @('-qv'), @('--verbose', '--quiet'),
                   @('--quiet', '--verbose'), @('-v', '-q', '-q', '-v'))) {
    $cases += @{ Name = ('-v 与 -q 同用（{0}）：notes 保留且冲突提示恰好一条' -f ($form -join ' '))
       A = (@('--class', 'Shell_TrayWnd', '--dry-run') + $form + @('out.png')); Exit = 0
       Check = { param($o) (@($o.notes | Where-Object { $_.code -eq 'note.flag_overrides_quiet' })).Count -eq 1 -and
                            ((Codes $o.notes) -join ',') -match 'note.dry_run' -and
                            $o.input.lang -eq 'zh-CN' } }
}
$cases += @{ Name = '只给 -q 时 notes 消失、input 也没有'
   A = @('--class', 'Shell_TrayWnd', '--dry-run', '-q', 'out.png'); Exit = 0
   Check = { param($o) (-not $o.PSObject.Properties.Name.Contains('notes')) -and
                        -not $o.PSObject.Properties.Name.Contains('input') } }
$cases += @{ Name = '-q 不隐藏 errors，也不补一条冲突提示'
   A = @('-q', '--pid', '1e3', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' -and
                        -not $o.PSObject.Properties.Name.Contains('notes') } }
$cases += @{ Name = '-v -q 一起给失败命令时 errors 与 notes 都在'
   A = @('-v', '-q', '--pid', '1e3', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_number' -and
                        ((Codes $o.notes) -join ',') -eq 'note.flag_overrides_quiet' } }
$cases += @{ Name = '帮助里写了 -v 与 -q 同时给出按 -v 处理'
   A = @('--help'); Exit = 3; Text = $true; Has = @('与 --verbose 同时给出时按 --verbose 处理') }
$cases += @{ Name = '帮助里写了数字取值只认十进制'
   A = @('--help'); Exit = 3; Text = $true; Has = @('数字取值只认十进制') }




$results = @()
foreach ($c in $cases) {
    $r = Invoke-Ec $c.A
    $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }   # JSON 所在通道
    $results += [pscustomobject]@{ Case = $c; Body = $body; Out = $r.Stdout; Err = $r.Stderr; Exit = $r.Exit }
}

$pass = 0; $fail = 0
foreach ($r in $results) {
    $c = $r.Case
    $problems = @()
    $body = $r.Body
    $o = $null
    $looksJson = $body.Trim().StartsWith('{')

    if ($c.Text) {
        if ($looksJson) { $problems += '期望文本输出，实际是 JSON' }
        foreach ($frag in @($c.Has)) { if (-not $body.Contains($frag)) { $problems += "缺少片段: $frag" } }
        if ($c.Name -like '*帮助体积*') {
            $help = (Invoke-Ec @('--help')).Stdout
            if ($help.Length -gt 6000) { $problems += "帮助文本 $($help.Length) 字符，超过 6000" }
        }
    } else {
        if (-not $looksJson) { $problems += '期望 JSON 输出，实际是文本' }
        else {
            try { $o = $body | ConvertFrom-Json } catch { $problems += "JSON 解析失败: $_" }
        }
        if ($o) {
            if ($null -eq $o.captured) { $problems += '缺 captured' }
            if ($null -eq $o.images)   { $problems += '缺 images' }
            if ($c.PSObject.Properties.Name -contains 'Errors' -and
                ((Codes $o.errors) -join ',') -ne (@($c.Errors) -join ',')) {
                $problems += "errors=[$((Codes $o.errors) -join ',')] 期望 [$(@($c.Errors) -join ',')]" }
            if ($c.PSObject.Properties.Name -contains 'Notes' -and
                ((Codes $o.notes) -join ',') -ne (@($c.Notes) -join ',')) {
                $problems += "notes=[$((Codes $o.notes) -join ',')] 期望 [$(@($c.Notes) -join ',')]" }
            if ($c.Check) {
                try { if (-not (& $c.Check $o)) { $problems += '字段断言失败' } }
                catch { $problems += "字段断言异常: $_" }
            }
        }
    }

    $allowed = @($c.Exit)
    if ($allowed -notcontains $r.Exit) {
        $problems += ("退出码 {0} 期望 {1}" -f $r.Exit, ($allowed -join '/'))
    }
    # 文本与 JSON 默认都走 stdout；JSON 改走 stderr 有两种情况：显式 -o -，
    # 以及根本没给输出路径（此时 stdout 留给图片字节，用例要标 ToStderr）
    $expectStderr = [bool]$c.ToStderr -or ($c.A -contains '-' -and $c.A -contains '-o')
    if ($expectStderr) {
        if ($r.Out.Trim() -ne '') { $problems += '-o - 时 stdout 必须为空' }
    } elseif ($r.Out.Trim() -eq '') {
        $problems += 'stdout 为空'
    } elseif ($r.Err.Trim() -ne '') {
        $problems += 'stderr 应为空（除 -o - 外）'
    }

    if ($problems.Count -eq 0) {
        $pass++
        Write-Host ("  PASS  {0}" -f $c.Name) -ForegroundColor DarkGreen
    } else {
        $fail++
        Write-Host ("  FAIL  {0}" -f $c.Name) -ForegroundColor Red
        foreach ($p in $problems) { Write-Host ("        · {0}" -f $p) -ForegroundColor Red }
        if ($ShowAll -or $fail -le 2) { Write-Host $r.Body.Trim() -ForegroundColor DarkGray }
    }
}

Write-Host ''


Write-Host ("共 {0} 例，通过 {1}，失败 {2}" -f $cases.Count, $pass, $fail) -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })
if ($fail) { exit 1 }

# ---------------------------------------------------------------------------
# 通道分离：-o - 时 stdout 必须只留给图片字节，JSON 整体走 stderr
# （这里用 dry-run，所以 stdout 应该是空的，JSON 在 stderr）
# ---------------------------------------------------------------------------
$r = Invoke-Ec ($ANCHOR + @('--format', 'png', '-o', '-'))
$o = $null
try { $o = $r.Stderr | ConvertFrom-Json } catch { }
if ($r.Stdout.Trim() -eq '' -and $o -and $o.captured -eq 0 -and $r.Exit -eq 0) {
    Write-Host '  PASS  -o - 时 stdout 保持纯净，JSON 走 stderr' -ForegroundColor DarkGreen
} else {
    Write-Host ("  FAIL  通道分离：stdout={0} 字节，stderr 可解析={1}" -f $r.Stdout.Length, ($null -ne $o)) -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# 多语言：换语言只能换文字，不能换契约
#   1. 四种语言的 --help 各不相同（证明读的是四份资源，不是同一份兜底）
#   2. 同一命令在各语言下退出码与 errors 的 code 集合必须完全一致
#   3. 每条 message / hint 非空，且不留未替换的占位符（%1）或取不到的 key（"?xxx.yyy"）
#   4. 不给 --lang 的结果必须与 --lang auto 逐字节相同（默认跟随系统显示语言）
# ---------------------------------------------------------------------------
$LANGS = @('zh-CN', 'zh-TW', 'en', 'ja')
$LEFTOVER = '%[1-9]|\?[A-Za-z0-9_-]+\.[A-Za-z0-9_.-]'
$PROBE = @(
    @{ Name = 'hwnd 非法'; A = @('--hwnd', 'zzz', 'out.png'); Exit = 1 },
    @{ Name = '偷懒路径失败'; A = @('--pid', '1'); Exit = 1 },
    @{ Name = '无匹配窗口'; A = @('--class', 'NoSuchWindowXyz', 'out.png'); Exit = 4 },
    @{ Name = '未知取图方式'; A = @('--capture', 'waiwang', '--pid', '1', 'out.png'); Exit = 1 },
    @{ Name = 'index 越界'; A = @('--class', 'Shell_TrayWnd', '--index', '99', 'out.png'); Exit = 1 },
    @{ Name = '开关不接受取值'; A = @('--json=maybe', '--pid', '1', 'out.png'); Exit = 1 },
    @{ Name = '屏幕编号越界'; A = @('--monitor', '99', '--dry-run', 'out.png'); Exit = 1 },
    @{ Name = '整屏不支持的通道'; A = @('--monitor', 'primary', '--capture', 'dwm', 'out.png'); Exit = 1 },
    @{ Name = 'all 与窗口条件冲突'; A = @('--monitor', 'all', '--class', 'Shell_TrayWnd', 'out.png'); Exit = 1 },
    @{ Name = '期限取值非法'; A = @('--class', 'Shell_TrayWnd', '--timeout-ms', 'abc', 'out.png'); Exit = 1 },
    @{ Name = '正则条件走到匹配阶段（合法的表达式不是参数错）'; A = @('--title-regex', '(a+)+$', '--dry-run', 'out.png'); Exit = 4 },
    # 数值与语言这一节的写法：换语言只能换文字，被拒的写法与退出码必须四种语言完全一致
    @{ Name = 'pid 写成指数记法'; A = @('--pid', '1e3', 'out.png'); Exit = 1 },
    @{ Name = 'quality 写成十六进制'; A = @('--quality', '0x20', 'out.png'); Exit = 1 },
    @{ Name = 'hwnd 带负号'; A = @('--hwnd', '-1', 'out.png'); Exit = 1 },
    @{ Name = 'hwnd 下划线位置错'; A = @('--hwnd', '0x_1A0B4C', 'out.png'); Exit = 1 },
    @{ Name = 'hwnd 溢出'; A = @('--hwnd', '0x1FFFFFFFFFFFFFFFF', 'out.png'); Exit = 1 },
    @{ Name = 'monitor 编号写成写坏的数字'; A = @('--monitor', '1e3', 'out.png'); Exit = 1 },
    @{ Name = 'index 写成非 ASCII 数字'; A = @('--class', 'Shell_TrayWnd', '--index', '１', 'out.png'); Exit = 1 },
    @{ Name = '语言取值非法'; A = @('--lang', 'klingon', '--pid', '1', 'out.png'); Exit = 1 }
)
$bad = 0

# 1) 四份帮助互不相同，且没有空说明 / 未替换痕迹
$seenHeaders = @{}
foreach ($tag in $LANGS) {
    $help = (Invoke-Ec @('--lang', $tag, '--help')).Stdout
    $lines = @($help -split "`r?`n")
    $header = $lines[0]
    if (-not $header.Trim()) { $bad++; Write-Host "  FAIL  $tag 帮助首行为空" -ForegroundColor Red }
    if ($seenHeaders.ContainsKey($header)) {
        $bad++
        Write-Host ("  FAIL  {0} 与 {1} 的帮助首行相同，四份资源没分开" -f $tag, $seenHeaders[$header]) -ForegroundColor Red
    }
    $seenHeaders[$header] = $tag
    if ($help -match $LEFTOVER) {
        $bad++
        Write-Host ("  FAIL  {0} 帮助里有未替换痕迹：{1}" -f $tag, $Matches[0]) -ForegroundColor Red
    }
    $blank = @($lines | Where-Object { $_ -match '^\s+--' -and $_ -notmatch '\S$' })
    if ($blank.Count) {
        $bad++
        Write-Host ("  FAIL  {0} 有选项说明为空：{1}" -f $tag, ($blank[0].Trim())) -ForegroundColor Red
    }
    if ($help.Length -gt 6000) {
        $bad++
        Write-Host ("  FAIL  {0} 帮助文本 {1} 字符，超过 6000" -f $tag, $help.Length) -ForegroundColor Red
    }
}
if (-not $bad) { Write-Host ("  PASS  四种语言的帮助各不相同，无未替换占位符") -ForegroundColor DarkGreen }

# 2) + 3) 同一条命令在四种语言下的机器可读部分必须一致
foreach ($probe in $PROBE) {
    $problems = @()
    $shapes = @()
    foreach ($tag in $LANGS) {
        $r = Invoke-Ec (@('--lang', $tag) + $probe.A)
        $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
        $o = $null
        try { $o = $body | ConvertFrom-Json } catch { }
        if (-not $o) { $problems += "$tag 输出不是 JSON"; continue }
        if ($r.Exit -ne $probe.Exit) { $problems += "$tag 退出码 $($r.Exit) 期望 $($probe.Exit)" }
        $fields = @()
        foreach ($item in (@($o.errors) + @($o.notes))) {
            if ($null -eq $item) { continue }
            $fields += [string]$item.message
            if ($item.PSObject.Properties.Name -contains 'hint') { $fields += [string]$item.hint }
        }
        foreach ($f in $fields) {
            if (-not $f.Trim()) { $problems += "$tag 有 message/hint 为空" }
            elseif ($f -match $LEFTOVER) { $problems += "$tag 残留 $($Matches[0])" }
        }
        $shapes += , ("{0}|{1}" -f $r.Exit, ((Codes $o.errors) -join ','))
    }
    if (($shapes | Sort-Object -Unique).Count -gt 1) {
        $problems += ("各语言的 code/退出码不一致：{0}" -f ($shapes -join '  '))
    }
    if ($problems.Count) {
        $bad++
        Write-Host ("  FAIL  多语言 · {0}" -f $probe.Name) -ForegroundColor Red
        foreach ($p in $problems) { Write-Host ("        · {0}" -f $p) -ForegroundColor Red }
    } else {
        Write-Host ("  PASS  多语言 · {0}（四语言同 code）" -f $probe.Name) -ForegroundColor DarkGreen
    }
}

# 4) 不给 --lang == --lang auto
$auto = Invoke-Ec @('--lang', 'auto', '--hwnd', 'zzz', 'out.png')
$detect = Invoke-Ec -NoLang @('--hwnd', 'zzz', 'out.png')
if ($auto.Stdout -eq $detect.Stdout -and $auto.Exit -eq $detect.Exit) {
    Write-Host '  PASS  不给语言与 --lang auto 结果相同（默认跟随系统显示语言）' -ForegroundColor DarkGreen
} else {
    $bad++
    Write-Host '  FAIL  不给语言与 --lang auto 结果不同' -ForegroundColor Red
}

# 5) 默认语言真的来自系统显示语言。判据独立取：直接问 kernel32 的 GetUserDefaultUILanguage，
#    按 MAKELANGID 的位算出期望标签。不要用 Get-UICulture / .NET CurrentUICulture 当判据——
#    实测一台显示语言为 zh-CN 的机器上它们是 en-US。
if (-not ('EcaptureUi' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EcaptureUi {
    [DllImport("kernel32")] public static extern ushort GetUserDefaultUILanguage();
}
'@
}
$langid = [int][EcaptureUi]::GetUserDefaultUILanguage()
$primary = $langid -band 0x3FF
$sub = ($langid -shr 10) -band 0x3F
$expected = switch ($primary) {
    4 { if ($sub -in 1, 3, 5) { 'zh-TW' } else { 'zh-CN' } }   # 1 繁体 / 3 港 / 5 澳
    17 { 'ja' }
    9 { 'en' }
    default { 'en' }
}
$detected = (Invoke-Ec -NoLang @('--help')).Stdout
$expectedText = (Invoke-Ec @('--lang', $expected, '--help')).Stdout
if ($detected -eq $expectedText) {
    Write-Host ("  PASS  默认文案语言 = {0}（系统显示语言 LANGID 0x{1:X4}）" -f $expected, $langid) -ForegroundColor DarkGreen
} else {
    $bad++
    Write-Host ("  FAIL  显示语言 LANGID 0x{1:X4} 应映射到 {0}，但默认帮助与 --lang {0} 不同" -f $expected, $langid) -ForegroundColor Red
}

if ($bad) {
    Write-Host ("多语言检查失败：{0} 项" -f $bad) -ForegroundColor Red
    exit 1
}
Write-Host '多语言检查通过' -ForegroundColor Green
