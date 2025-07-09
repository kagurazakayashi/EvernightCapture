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
    @{ Name = 'all 与窗口条件冲突'; A = @('--monitor', 'all', '--class', 'Shell_TrayWnd', 'out.png'); Exit = 1 }
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
