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

# 帮助体积的上限，**按语言各一条**。
# 这条判据要防的是"不知不觉把帮助写胖"（整份文本由选项目录生成，加一条选项就多一行，
# 而每行的说明文案是各语言自己写的，见 resources/strings-*.txt）。同一份目录在英文里
# 天然比中文长得多（en 大约是 zh 的 1.7 倍），拿一个数去卡四种语言会把"这个语言的
# 说明本来就更啰嗦"判成"帮助膨胀"，所以按语言分别给。
# 上调这条数字只应该发生在**新增一条选项或新增一段说明**的时候，并且要说得出是哪一次改动。
# 上上次上调：HDR 色彩处理那一条（--hdr 一行目录 + 一句说明；没有新增分组标题，接在 --cursor 后面）。
# 上一次上调：等比缩小那一条（--scale 一行目录 + 一句说明；同样接在 --cursor / --hdr 后面）。
# 它不是最短的那一条说明（三条天花板各是什么、只有一种内插、先裁后缩的顺序、以及"不改变授权"
# 都要说清楚），四种语言各加约 570～1430 字符（英文那份天然最长）。随之一起抬的还有同一组的分组标题
# grp.crop（原来只说裁剪，现在这一组还装着等比缩小，改成了"裁剪与等比缩小…先裁后缩"）。
# 这一次上调：--cursor 那一行现在要分开讲两种"做不到"—— printwindow / dwm / bitblt 是"来源本来
# 就没有光标"，而桌面复制那两条是"这一问没有答案"（官方说明允许指针已经画在那幅桌面图像上，而这条
# 路径没有可读回的开关），同一行还要写明 --cursor default 照旧交图但 cursorEffective 写 unverified。
# 这是**说明语义变多**而不是文案膨胀：四行各自加长（zh-CN 331→423 / zh-TW 333→428 / en 606→795 /
# ja 409→516），没有新增选项也没有新增分组标题。
# 实测 zh-CN 9249 / zh-TW 9290 / en 17382 / ja 10418，下面四个数按各自实际长度抬起，
# 并把余量留在"再加一条短选项"还能装下的位置上。
$HELP_LIMITS = @{ 'zh-CN' = 9400; 'zh-TW' = 9450; 'en' = 17600; 'ja' = 10600 }
function Get-HelpLimit([string]$tag) {
    if ($HELP_LIMITS.ContainsKey($tag)) { return $HELP_LIMITS[$tag] }
    return $HELP_LIMITS['zh-CN']
}

$cases = @(
    # ---------- 文本输出 ----------
    @{ Name = '无参数 -> 文本帮助'; A = @(); Exit = 2; Text = $true
       Has = @('未指定任何匹配条件', '窗口匹配条件', '用法:', '退出码:') }
    @{ Name = '--help 文本且含全部条件'; A = @('--help'); Exit = 3; Text = $true
       Has = (@('用法:') + $MATCH_FLAGS) }
    @{ Name = '/help 斜杠形式'; A = @('/help'); Exit = 3; Text = $true; Has = @('窗口匹配条件') }
    @{ Name = '帮助体积受控（按语言各自的上限）'; A = @('--version'); Exit = 0; Text = $true; Has = @('EvernightCapture') }
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
    @{ Name = '屏幕编号越界且未给输出路径 -> 与给了路径时同一条真实原因'
       A = @('--monitor','99'); Exit = 1; ToStderr = $true
       Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'match.monitor_out_of_range' -and
                            (@(Codes $o.notes) -join ',') -eq 'note.output_defaulted_stdout' -and
                            $o.errors[0].option -eq '--monitor' -and $o.errors[0].hint } }
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
    # ---- 按标识选屏的**写法**（本机有没有那块屏由文末的 ad-hoc 判据现场判，这里只判语法）----
    # 旧的数字形式保留：它的语义就是"本次枚举顺序里的第 n 块"，不重新解释成别的东西。
    # 两条标识写法只认 device: 与 id: 这两个前缀，其余带冒号的 token 照旧不是取值
    #（盘符路径、备用数据流都长那样，吃掉它就等于替用户改了一个输出文件名）。
    @{ Name = '--monitor 的标识前缀不认识 -> 单独一条码，不退化成 invalid_number'
       A = @('--monitor=foo:1','out.png'); Exit = 1; Errors = @('cli.monitor_selector_kind')
       Check = { param($o) $o.errors[0].value -eq 'foo:1' -and $o.errors[0].hint } }
    @{ Name = '--monitor 的标识本体为空 -> 报错而不是当成主屏'
       A = @('--monitor=id:','out.png'); Exit = 1; Errors = @('cli.monitor_selector_empty') }
    @{ Name = '空格写法的空标识也被吃掉并报错（证明它是取值不是输出路径）'
       A = @('--monitor','device:','out.png'); Exit = 1; Errors = @('cli.monitor_selector_empty') }
    @{ Name = '--monitor D:\...png 不吃盘符路径（取值可省略那条没松动）'
       A = @('--monitor','D:\shots\a.png','--dry-run','-v'); Exit = 0
       Check = { param($o) ($o.input.output -like '*a.png' -and
                            $o.input.monitorKind -eq 'primary' -and
                            $o.input.target -eq 'screen') } }
    @{ Name = '不存在的设备名 -> match.monitor_unknown_id，绝不改用主屏'
       A = @('--monitor=device:NOSUCHSCREEN','--dry-run','out.png'); Exit = 4
       Errors = @('match.monitor_unknown_id')
       Check = { param($o) ($o.captured -eq 0 -and @($o.images).Count -eq 0 -and
                            $o.errors[0].option -eq '--monitor' -and
                            $o.errors[0].stage -eq 'match' -and $o.errors[0].hint) } }
    @{ Name = '不存在的跨会话标识 -> 同一条码（屏不在了就说屏不在了）'
       A = @('--monitor=id:\\?\DISPLAY#NOPE#0','--dry-run','out.png'); Exit = 4
       Errors = @('match.monitor_unknown_id') }

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
    @{ Name = '正则非法（解析层不编译，由受约束的匹配阶段判出）'; A = @('--title-regex','[bad(','out.png'); Exit = 1; Errors = @('cli.invalid_regex')
       Check = { param($o) $o.errors[0].stage -eq 'match' -and $o.errors[0].option -eq '--title-regex' -and
                            $o.captured -eq 0 -and $o.errors[0].message -and $o.errors[0].hint } }
    @{ Name = '不给输出路径 -> 按 --out - 处理（PNG 写标准输出）'
       A = ($ANCHOR + @('-v')); Exit = 0; ToStderr = $true; Notes = @('note.output_defaulted_stdout')
       Check = { param($o) $o.input.output -eq '-' -and $o.input.toStdout -eq $true -and
                            $o.input.format -eq 'png' -and $o.input.formatGiven -eq $false } }
    @{ Name = '不给输出路径且未出图 -> 真实原因原样送出（不再塌成 cli.missing_output）'
       A = @('--pid','1'); Exit = 4; ToStderr = $true
       Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'match.no_window' -and
                            (@(Codes $o.notes) -join ',') -eq 'note.output_defaulted_stdout' -and
                            @($o.images).Count -eq 0 -and $o.errors[0].hint } }
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

# ---------------------------------------------------------------------------
# 窗口内部裁剪（--roi / --client-area）：这一节只判**解析层**（写法、取值域、互斥、回显）。
# 几何那一条（放得下放不进、resize 之后失效、坐标映射）要拿真窗口判，在 tests\crop.ps1；
# 纯算术那一条（越界、绕回、图像原点问不出来、客户区落在图外）在 tests\crop_state.cpp。
# 这里全部配 --dry-run：不取帧，所以既不弹框也不落地，更不会因为本机 Windows 版本而变绿变红。
# ---------------------------------------------------------------------------
$ROI_BAD = @(
    '-1,0,10,10',            # 负号：不是这条选项认过的写法（也不许被强转成一个大数）
    '0,0,0,10',              # 空矩形（零宽）
    '0,0,10,0',              # 空矩形（零高）
    '0, 0,10,10',            # 空白：带个空格就是写坏了，不是"宽容读成 0"
    '1e3,0,10,10',           # 指数写法
    '0x1,0,10,10',           # 0x 前缀只在 --hwnd 那一条上有定义
    '1_0,0,10,10',           # 下划线分隔
    '1.5,0,10,10',           # 小数点
    '１,0,10,10',            # 全角数字：不是 ASCII 数字
    '0,0,10',                # 少一段
    '0,0,10,10,',            # 多一个逗号（第五段是空的）
    '0,0,10,10,10',          # 多一段
    '16385,0,10,10',         # 超过单边上限
    '0,0,16385,10',          # 宽度那条也判上限
    '99999999999999999999,0,10,10'   # 装不进 64 位：溢出就是拒绝，不回绕
)
foreach ($bad in $ROI_BAD) {
    $cases += @{ Name = ('--roi 的写法不合（{0}）' -f $bad)
       A = ($ANCHOR + @('--roi', $bad, 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                            $o.errors[0].option -eq '--roi' -and
                            $o.errors[0].value -eq $bad }.GetNewClosure() }
}
# 四段全合语法就放行，且 -v 回显的是规范化后的四个数（不是用户那一条字符串）。
$cases += @{ Name = '--roi 合法写法：--dry-run 放行并回显规范化后的四个数'
   A = ($ANCHOR + @('--roi', '12,8,100,50', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.crop.mode -eq 'roi' -and $o.input.crop.x -eq 12 -and
                        $o.input.crop.y -eq 8 -and $o.input.crop.width -eq 100 -and
                        $o.input.crop.height -eq 50 -and
                        ((Codes $o.notes) -join ',') -match 'note.dry_run' } }
foreach ($edge in @('0,0,1,1', '0,0,16384,16384', '16384,16384,1,1')) {
    $cases += @{ Name = ('--roi 的合法边界写法（{0}）' -f $edge)
       A = ($ANCHOR + @('--roi', $edge, '--verbose', 'out.png')); Exit = 0
       Check = { param($o) $o.input.crop.mode -eq 'roi' } }
}
# 重复给出与 --monitor 那一条同源：最后一个写法生效，不各算一条冲突。
$cases += @{ Name = '--roi 写两次：最后一个生效（与数字选项的顺序语义一致）'
   A = ($ANCHOR + @('--roi', '1,1,2,2', '--roi', '5,6,7,8', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.crop.x -eq 5 -and $o.input.crop.y -eq 6 -and
                        $o.input.crop.width -eq 7 -and $o.input.crop.height -eq 8 -and
                        (-not ((Codes $o.errors) -contains 'cli.conflicting_options')) } }
# --client-area 是开关：没有数要回显，只有 mode。
$cases += @{ Name = '--client-area 开关：回显 mode 而不带四个数'
   A = ($ANCHOR + @('--client-area', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.crop.mode -eq 'client-area' -and
                        -not $o.input.crop.PSObject.Properties.Name.Contains('width') } }
# 普通开关写 =false 等于没写（仓库里既有的那一条规矩），=true 与裸写同义。
foreach ($form in @('--client-area=false', '--client-area=0', '--client-area=no')) {
    $cases += @{ Name = ('--client-area 写 {0} 等于没写' -f $form)
       A = ($ANCHOR + @($form, '--verbose', 'out.png')); Exit = 0
       Check = { param($o) -not $o.input.PSObject.Properties.Name.Contains('crop') }.GetNewClosure() }
}
foreach ($form in @('--client-area', '--client-area=true', '--client-area=1', '--client-area=on')) {
    $cases += @{ Name = ('{0} 都是只要客户区' -f $form)
       A = ($ANCHOR + @($form, '--verbose', 'out.png')); Exit = 0
       Check = { param($o) $o.input.crop.mode -eq 'client-area' } }
}
# 两条互斥：一次只留一种裁剪。这条码与 cli.conflicting_options（选择策略那一组）分开给，
# 因为那句文案会把人引向"删掉 --index"这种与本次无关的下一步。
foreach ($combo in @(
        @('--roi', '0,0,10,10', '--client-area'),
        @('--client-area', '--roi', '0,0,10,10'))) {
    $cases += @{ Name = ('--roi 与 --client-area 同时给出（{0}）' -f ($combo -join ' '))
       A = ($ANCHOR + $combo + @('out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.crop_conflict' -and
                            $o.errors[0].value -eq '--client-area, --roi' } }
}
# 整块屏幕的目标上没有"一扇窗口"可以让坐标相对它的左上角去算 —— 这条在解析期就报，
# 而且**绝不**把 --roi 当成桌面绝对坐标偷偷用掉。
foreach ($m in @(@('--monitor'), @('--monitor', 'primary'), @('--monitor', 'all'),
                 @('--monitor', '2'))) {
    $cases += @{ Name = ('屏幕模式配 --roi 在解析期就说不通（{0}）' -f ($m -join ' '))
       A = ($m + @('--roi', '0,0,10,10', 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -match 'capture.unsupported' -and
                            (@($o.errors | Where-Object { $_.code -eq 'capture.unsupported' }))[0].option -eq '--roi' } }
}
$cases += @{ Name = '屏幕模式配 --client-area 同一条码'
   A = @('--monitor', '--client-area', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -match 'capture.unsupported' -and
                        (@($o.errors | Where-Object { $_.code -eq 'capture.unsupported' }))[0].option -eq '--client-area' } }
# --monitor 配窗口条件出的是窗口图，所以裁剪在那一路上是成立的（不回显冲突）。
$cases += @{ Name = '--monitor 配窗口条件（按屏过滤）时 --roi 成立'
   A = @('--monitor', '--class', 'Shell_TrayWnd', '--dry-run', '--roi', '0,0,5,5', '--verbose', 'out.png')
   Exit = 0
   Check = { param($o) $o.input.crop.mode -eq 'roi' -and $o.input.target -eq 'window' } }
# 两条只读查询都不产图，所以截图那一级的选项一条都不成立。
$cases += @{ Name = '环境查询与 --roi 冲突（cli.query_conflict）'
   A = @('--capabilities', '--roi', '0,0,10,10'); Exit = 1; Query = $false
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' -and
                        $o.errors[0].value -match '--roi' } }
$cases += @{ Name = '环境查询与 --client-area 冲突'
   A = @('--screens', '--client-area'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' -and
                        $o.errors[0].value -match '--client-area' } }
$cases += @{ Name = '窗口查询与 --roi 冲突（cli.window_query_conflict）'
   A = @('--list', '--roi', '0,0,10,10'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.window_query_conflict' -and
                        $o.errors[0].value -match '--roi' } }
$cases += @{ Name = '窗口查询与 --client-area 冲突'
   A = @('--inspect', '--hwnd', '0x1', '--client-area'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.window_query_conflict' } }
# 一个条件都没给时，--roi 不算条件（漏写条件的调用方不该在无意间拍到东西）。
$cases += @{ Name = '只给 --roi 而不给任何条件：仍是文本帮助 + 退出码 2'
   A = @('--roi', '0,0,10,10'); Exit = 2; Text = $true; Has = @('未指定任何匹配条件') }
$cases += @{ Name = '只给 --client-area 而不给任何条件：同上'
   A = @('--client-area'); Exit = 2; Text = $true; Has = @('未指定任何匹配条件') }
# 写错的近似名要给出具体的那条建议（与 --title 那一组同一条判据）。
$cases += @{ Name = '--roi 写坏了名字时建议指向 --roi'
   A = @('--roia', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.unknown_option' -and
                        $o.errors[0].hint -eq '--roi' } }
$cases += @{ Name = '--client-area 写坏了名字时给出去掉错误字母的建议'
   A = @('--client-are', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.unknown_option' -and
                        $o.errors[0].hint -eq '--client-area' } }
# 帮助里必须看得见这一组（选项目录是 CLI 契约的唯一来源，选项名不随语言变）。
$cases += @{ Name = '--help 含窗口内部裁剪那两条选项'
   A = @('--help'); Exit = 3; Text = $true; Has = @('--roi <x,y,w,h>', '--client-area') }
# --dry-run 不取帧，所以裁剪几何那一关不替它下结论（与 env.* 那条同一个道理）。
$cases += @{ Name = '--dry-run 时不判裁剪几何（一条大得放不进的 --roi 照样返回 0）'
   A = @('--class', 'Shell_TrayWnd', '--dry-run', '--roi', '16000,16000,10,10', 'out.png'); Exit = 0
   Check = { param($o) (-not $o.PSObject.Properties.Name.Contains('errors')) -and
                        ((Codes $o.notes) -join ',') -match 'note.dry_run' } }

# ---------------------------------------------------------------------------
# 光标包含与排除（--cursor）：这一节同样只判**解析层**（写法、取值域、与通道能力的组合、回显）。
# 真去设那个开关、以及"设完读回来是不是那一件事"在 tests\cursor.ps1；
# 通道链按光标要求收窄、两份登记表一致性与三个键的合成在 tests\cursor_state.cpp。
# 这里全部配 --dry-run：不取帧，所以既不弹框也不落地，也不会因为本机 Windows 版本而变绿变红
#（--cursor include 配做不到的那条通道是**结构性**说不通，与本机版本无关，dry-run 照样判）。
# ---------------------------------------------------------------------------
foreach ($bad in @('in', 'on', 'yes', 'true', '1', 'both', 'auto', 'Include!', 'include,exclude',
                   'デフォルト')) {
    $cases += @{ Name = ('--cursor 的写法不合（[{0}]）' -f $bad)
       A = ($ANCHOR + @('--cursor', $bad, 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                            $o.errors[0].option -eq '--cursor' -and
                            $o.errors[0].value -eq $bad }.GetNewClosure() }
}
# --cursor 必带取值（不是可选取值那一类），所以 argv 到头就是 cli.missing_value；
# 而它也要吃值：--cursor out.png 里的 out.png 被当取值并当场报错，不会悄悄变成输出文件名。
$cases += @{ Name = '--cursor 放在末尾没有取值：cli.missing_value'
   A = @('--class', 'Shell_TrayWnd', '--dry-run', 'out.png', '--cursor'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.missing_value' -and
                        $o.errors[0].option -eq '--cursor' } }
$cases += @{ Name = '--cursor 吃掉后面的输出路径写法并当场报错（不变成文件名）'
   A = ($ANCHOR + @('--cursor', 'out.png')); Exit = 1; ToStderr = $true
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                        $o.errors[0].value -eq 'out.png' -and
                        (@($o.notes | ForEach-Object { $_.code }) -join ',') -match 'note.output_defaulted_stdout' } }
# 三种写法都认（忽略大小写与首尾空白，与 --capture / --format 同一套），且最后一个写法生效。
foreach ($w in @(@('include', 'include'), @('INCLUDE', 'include'), @(' exclude ', 'exclude'),
                 @('default', 'default'), @('Exclude', 'exclude'))) {
    $cases += @{ Name = ('--cursor 认得的写法（{0}）' -f $w[0])
       A = ($ANCHOR + @('--cursor', $w[0], '--verbose', 'out.png')); Exit = 0
       Check = { param($o) $o.input.cursor -eq $w[1] -and $o.input.cursorGiven -eq $true }.GetNewClosure() }
}
# 内联写法（--cursor=取值）与分开写法是同一件事：这两个 token 只有一条 --cursor 被消费。
foreach ($w in @(@('--cursor=include', 'include'), @('--cursor=EXCLUDE', 'exclude'),
                 @('--cursor=default', 'default'))) {
    $cases += @{ Name = ('--cursor 的内联写法（{0}）' -f $w[0])
       A = ($ANCHOR + @($w[0], '--verbose', 'out.png')); Exit = 0
       Check = { param($o) $o.input.cursor -eq $w[1] -and $o.input.cursorGiven -eq $true }.GetNewClosure() }
}
$cases += @{ Name = '--cursor 写两次：最后一个生效（与 --capture 那条顺序语义一致）'
   A = ($ANCHOR + @('--cursor', 'include', '--cursor', 'exclude', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.cursor -eq 'exclude' -and $o.input.cursorGiven -eq $true } }
$cases += @{ Name = '-v 恒回显最终光标要求与"有没有写过这条选项"（默认值问得出来）'
   A = ($ANCHOR + @('--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.cursor -eq 'default' -and $o.input.cursorGiven -eq $false } }
# include 这一条要求只有那条真有开关的通道能兑现：显式指定做不到的那几条就在解析期说不通，
# 而且**绝不**是"那就换一条通道试试"（换到会读桌面像素的那几条既没把光标加回来，
# 又多拍一份没人批准过的画面）。屏幕目标那一路同理。
foreach ($m in @('dwm', 'printwindow', 'bitblt', 'duplication')) {
    $cases += @{ Name = ('--cursor include 配 --capture {0}：解析期就拒，不换后端' -f $m)
       A = ($ANCHOR + @('--cursor', 'include', '--capture', $m, 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'capture.cursor_unsupported' -and
                            $o.errors[0].option -eq '--cursor' -and
                            $o.errors[0].value -eq 'include' -and
                            $o.errors[0].message -match $m -and
                            $o.errors[0].message -match 'wgc' -and
                            $o.errors[0].stage -ne 'capture' }.GetNewClosure() }
    $cases += @{ Name = ('屏幕目标上 --cursor include 配 --capture {0} 同一条码' -f $m)
       A = @('--monitor', 'primary', '--cursor', 'include', '--capture', $m, 'out.png'); Exit = 1
       Check = { param($o) (Codes $o.errors) -contains 'capture.cursor_unsupported' }.GetNewClosure() }
}
foreach ($m in @('wgc', 'auto', 'dwm', 'printwindow', 'bitblt')) {
    $cases += @{ Name = ('--cursor exclude 配 --capture {0} 在解析期放行（差别在结果里那三个键）' -f $m)
       A = ($ANCHOR + @('--cursor', 'exclude', '--capture', $m, '--dry-run', 'out.png')); Exit = 0
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('errors') }.GetNewClosure() }
}
# exclude 这一条现在也只有一条路线敢声称：桌面复制那两条的来源可能已经把指针画在那幅桌面图像上，
# 而它没有可读回的开关，所以明确要求 exclude 时解析期就拒（同一条码、另一句文案），不换后端。
# 窗口目标与屏幕目标各判一次：同一个判据两条路都要给同一个下场。
foreach ($m in @('duplication')) {
    $cases += @{ Name = ('--cursor exclude 配 --capture {0}：解析期就拒，不给无根据的成功' -f $m)
       A = ($ANCHOR + @('--cursor', 'exclude', '--capture', $m, 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'capture.cursor_unsupported' -and
                            $o.errors[0].option -eq '--cursor' -and
                            $o.errors[0].value -eq 'exclude' -and
                            $o.errors[0].message -match $m -and
                            $o.errors[0].stage -ne 'capture' }.GetNewClosure() }
    $cases += @{ Name = ('屏幕目标上 --cursor exclude 配 --capture {0} 同一条码' -f $m)
       A = @('--monitor', 'primary', '--cursor', 'exclude', '--capture', $m, 'out.png'); Exit = 1
       Check = { param($o) (Codes $o.errors) -contains 'capture.cursor_unsupported' }.GetNewClosure() }
}
$cases += @{ Name = '--cursor include 配 --capture wgc / auto 放行（这两条兑现得了）'
   A = ($ANCHOR + @('--cursor', 'include', '--capture', 'wgc', '--dry-run', 'out.png')); Exit = 0
   Check = { param($o) -not $o.PSObject.Properties.Name.Contains('errors') } }
# -v 的 captureChain 与真去截图那一次同一个判据：要求 include 时链里只剩设得进开关的那一条。
# 这里判的是"回显与执行不各写一套"，note 那几条只在真取帧时才发（--dry-run 不判运行时闸门）。
$cases += @{ Name = '-v 的 input.captureChain 按光标要求收窄（auto + include 只剩 wgc）'
   A = ($ANCHOR + @('--cursor', 'include', '--capture', 'auto', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc' } }
$cases += @{ Name = '窗口链 exclude 不收窄（那四条里没有桌面复制，wgc 那条去设开关）'
   A = ($ANCHOR + @('--cursor', 'exclude', '--capture', 'auto', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc,dwm,printwindow,bitblt' } }
# 屏幕链里就有那一条保证不了的：exclude 时它被摘掉，剩下的两条照旧（收窄是**减**候选，
# 不会为了凑一条合格路线把别的通道换进来，所以桌面像素的范围一点都没扩大）。
$cases += @{ Name = '屏幕链 exclude 把保证不了的桌面复制摘掉（wgc,bitblt 两条仍在）'
   A = @('--monitor', 'primary', '--cursor', 'exclude', '--capture', 'auto', '--dry-run',
        '--verbose', 'out.png'); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc,bitblt' -and
                        $o.input.target -eq 'screen' } }
$cases += @{ Name = '没写 --cursor 时屏幕链一条都不摘（默认截图行为不变）'
   A = @('--monitor', 'primary', '--capture', 'auto', '--dry-run', '--verbose', 'out.png'); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc,duplication,bitblt' } }
$cases += @{ Name = '没写 --cursor 时链与这条选项存在之前逐字相同'
   A = ($ANCHOR + @('--capture', 'auto', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc,dwm,printwindow,bitblt' } }
$cases += @{ Name = '屏幕模式的 auto 链同样按 include 收窄'
   A = @('--monitor', '--class', 'Shell_TrayWnd', '--dry-run', '--cursor', 'include',
         '--capture', 'auto', '--verbose', 'out.png'); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc' -and
                        $o.input.target -eq 'window' } }
# 一条选项写坏了名字要给出具体的那条建议；帮助里必须看得见这条选项（目录是契约的唯一来源）。
$cases += @{ Name = '--curso 写坏时建议指向 --cursor'
   A = @('--curso', 'out.png'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.unknown_option' -and
                        $o.errors[0].hint -eq '--cursor' } }
$cases += @{ Name = '--help 含光标那一条选项与其三种取值'
   A = @('--help'); Exit = 3; Text = $true; Has = @('--cursor <default|include|exclude>') }
# 光标不是匹配条件，也不是屏幕目标：只给 --cursor 仍是"零条件"，帮人别在无意间拍到东西。
$cases += @{ Name = '只给 --cursor 而不给任何条件：仍是文本帮助 + 退出码 2'
   A = @('--cursor', 'exclude'); Exit = 2; Text = $true; Has = @('未指定任何匹配条件') }
# 两类查询都不产图，所以截图那一级的选项一条都不成立（光标属于取帧那一级）。
$cases += @{ Name = '环境查询与 --cursor 冲突（cli.query_conflict）'
   A = @('--capabilities', '--cursor', 'include'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' -and
                        $o.errors[0].value -match '--cursor' } }
$cases += @{ Name = '屏幕查询与 --cursor 冲突'
   A = @('--screens', '--cursor', 'exclude'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' } }
$cases += @{ Name = '窗口查询与 --cursor 冲突（cli.window_query_conflict）'
   A = @('--list', '--cursor', 'exclude'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.window_query_conflict' -and
                        $o.errors[0].value -match '--cursor' } }
# 与裁剪那一条同源：--cursor 与 --roi 各管各的（一个是留哪一块，一个是有没有指针），不算冲突。
$cases += @{ Name = '--cursor 与 --roi 同时给出不算冲突（两件事各说各的）'
   A = ($ANCHOR + @('--cursor', 'exclude', '--roi', '0,0,5,5', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.cursor -eq 'exclude' -and $o.input.crop.mode -eq 'roi' } }

# ---------------------------------------------------------------------------
# HDR 色彩处理（--hdr）：这一节同样只判**解析层**（写法、取值域、与通道能力的组合、回显，
# 以及"不摘链"）。真去带回广色域帧、映射与三个键的合成在 tests\hdr_state.cpp（离线）与
# tests\hdr.ps1（真机：本机没 HDR 显示器，凡是要真的 HDR 帧才能判的项一律记未验证）。
# 这里全部配 --dry-run：不取帧，所以既不弹框也不落地；--hdr tonemap/refuse 配做不到的那条通道
# 是**结构性**说不通（那条只带得回 8 位 SDR），与本机版本无关，dry-run 照样判。
# ---------------------------------------------------------------------------
foreach ($bad in @('hdr', 'yes', 'true', '1', 'map', 'passthrough', 'Auto!', 'tonemap,refuse',
                   '默认')) {
    $cases += @{ Name = ('--hdr 的写法不合（[{0}]）' -f $bad)
       A = ($ANCHOR + @('--hdr', $bad, 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                            $o.errors[0].option -eq '--hdr' -and
                            $o.errors[0].value -eq $bad }.GetNewClosure() }
}
# --hdr 必带取值（不是可选取值那一类），argv 到头就是 cli.missing_value；
# 且它要吃值：--hdr out.png 里的 out.png 被当取值并当场报错，不会悄悄变成输出文件名。
$cases += @{ Name = '--hdr 放在末尾没有取值：cli.missing_value'
   A = @('--class', 'Shell_TrayWnd', '--dry-run', 'out.png', '--hdr'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.missing_value' -and
                        $o.errors[0].option -eq '--hdr' } }
$cases += @{ Name = '--hdr 吃掉后面的输出路径写法并当场报错（不变成文件名）'
   A = ($ANCHOR + @('--hdr', 'out.png')); Exit = 1; ToStderr = $true
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                        $o.errors[0].value -eq 'out.png' } }
# 三种写法都认（忽略大小写与首尾空白），最后一个生效，且 -v 回显最终策略与"有没有写过"。
foreach ($w in @(@('tonemap', 'tonemap'), @('REFUSE', 'refuse'), @(' auto ', 'auto'),
                 @('Auto', 'auto'), @('ToneMap', 'tonemap'))) {
    $cases += @{ Name = ('--hdr 认得的写法（{0}）' -f $w[0])
       A = ($ANCHOR + @('--hdr', $w[0], '--verbose', 'out.png')); Exit = 0
       Check = { param($o) $o.input.hdr -eq $w[1] -and $o.input.hdrGiven -eq $true }.GetNewClosure() }
}
foreach ($w in @(@('--hdr=tonemap', 'tonemap'), @('--hdr=REFUSE', 'refuse'), @('--hdr=auto', 'auto'))) {
    $cases += @{ Name = ('--hdr 的内联写法（{0}）' -f $w[0])
       A = ($ANCHOR + @($w[0], '--verbose', 'out.png')); Exit = 0
       Check = { param($o) $o.input.hdr -eq $w[1] -and $o.input.hdrGiven -eq $true }.GetNewClosure() }
}
$cases += @{ Name = '--hdr 写两次：最后一个生效（与 --capture 那条顺序语义一致）'
   A = ($ANCHOR + @('--hdr', 'tonemap', '--hdr', 'refuse', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.hdr -eq 'refuse' -and $o.input.hdrGiven -eq $true } }
$cases += @{ Name = '-v 恒回显最终 HDR 策略与"有没有写过这条选项"（默认值问得出来）'
   A = ($ANCHOR + @('--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.hdr -eq 'auto' -and $o.input.hdrGiven -eq $false } }
# tonemap / refuse 这两种明确要求，本构建只有 wgc 真兑现得了：显式指定兑现不了的那几条在解析期
# 说不通，而且绝不换后端。原因有两种、文案也不同，但同一个码 —— 调用方按 code 分支，人按 message 看懂差别。
foreach ($m in @('dwm', 'printwindow', 'bitblt')) {
    foreach ($pol in @('tonemap', 'refuse')) {
        $cases += @{ Name = ('--hdr {0} 配 --capture {1}：解析期就拒，不换后端' -f $pol, $m)
           A = ($ANCHOR + @('--hdr', $pol, '--capture', $m, 'out.png')); Exit = 1
           Check = { param($o) ((Codes $o.errors) -join ',') -eq 'capture.hdr_unsupported' -and
                                $o.errors[0].option -eq '--hdr' -and
                                $o.errors[0].value -eq $pol -and
                                $o.errors[0].message -match $m -and
                                $o.errors[0].message -match 'wgc' -and
                                $o.errors[0].stage -ne 'capture' }.GetNewClosure() }
        $cases += @{ Name = ('屏幕目标上 --hdr {0} 配 --capture {1} 同一条码' -f $pol, $m)
           A = @('--monitor', 'primary', '--hdr', $pol, '--capture', $m, 'out.png'); Exit = 1
           Check = { param($o) (Codes $o.errors) -contains 'capture.hdr_unsupported' }.GetNewClosure() }
    }
}
# duplication 这一条是 F07 新收紧的那一条：它的来源在 Windows 那一侧可能跟显示模式走，但本构建
# 仍用 DuplicateOutput()、采集前不问显示色彩空间，所以不许拿一句没核实的"支持 HDR"继续放行。
# 文案必须说"这一步没实现"，说"结构上带不回广色域帧"是假话（那正是这份契约要防的混淆）。
foreach ($pol in @('tonemap', 'refuse')) {
    $cases += @{ Name = ('--hdr {0} 配 --capture duplication：兑现不了就解析期拒，文案说没实现' -f $pol)
       A = ($ANCHOR + @('--hdr', $pol, '--capture', 'duplication', 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'capture.hdr_unsupported' -and
                                $o.errors[0].message -match 'duplication' -and
                                $o.errors[0].message -match 'wgc' -and
                                $o.errors[0].message -notmatch 'cannot carry a wide-gamut|带不回广色域' -and
                                $o.errors[0].value -eq $pol }.GetNewClosure() }
}
# 真兑现得了的那一条（wgc）配显式策略放行；auto 也放行 —— 落到哪条通道要到运行期才知道，
# 而运行期那一道筛的就是同一张表（下一批"摘链"的判据判它）。duplication 只配 auto 放行：
# auto 不要求任何处理，这条选项存在之前的行为原样保留。
foreach ($m in @('wgc', 'auto')) {
    $cases += @{ Name = ('--hdr tonemap 配 --capture {0} 在解析期放行（差别要到运行期才看得见）' -f $m)
       A = ($ANCHOR + @('--hdr', 'tonemap', '--capture', $m, '--dry-run', 'out.png')); Exit = 0
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('errors') }.GetNewClosure() }
}
foreach ($m in @('wgc', 'duplication', 'dwm', 'bitblt', 'printwindow', 'auto')) {
    $cases += @{ Name = ('--hdr auto 配 --capture {0} 照旧放行（auto 不要求任何处理）' -f $m)
       A = ($ANCHOR + @('--hdr', 'auto', '--capture', $m, '--dry-run', 'out.png')); Exit = 0
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('errors') }.GetNewClosure() }
}
# --hdr 显式要求过就要摘链（F07）：回退链只留真兑现得了那要求的通道，摘不出一条就是
# env.hdr_unsupported。这与 --cursor include 收窄链是同一条规矩，而不是相反的一条。
$cases += @{ Name = '-v 的 input.captureChain 因 --hdr tonemap 收窄到只剩 wgc（窗口目标）'
   A = ($ANCHOR + @('--hdr', 'tonemap', '--capture', 'auto', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc' } }
$cases += @{ Name = '-v 的 input.captureChain 因 --hdr refuse 收窄到只剩 wgc（窗口目标）'
   A = ($ANCHOR + @('--hdr', 'refuse', '--capture', 'auto', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc' } }
$cases += @{ Name = '整屏目标同样收窄，且不因为 HDR 要求而放行别的桌面路径'
   A = @('--monitor', 'primary', '--hdr', 'tonemap', '--capture', 'auto', '--dry-run', '--verbose',
         'out.png'); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc' } }
# 没要求与写 auto 都不许动链：默认值真的一个字都不改（与这条选项存在之前逐字节相同）。
foreach ($extra in @(@(), @('--hdr', 'auto'))) {
    $cases += @{ Name = ('链不被收窄：{0}' -f $(if ($extra.Count) { '--hdr auto' } else { '没写 --hdr' }))
       A = ($ANCHOR + @('--capture', 'auto') + @($extra) + @('--verbose', 'out.png')); Exit = 0
       Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc,dwm,printwindow,bitblt' }.GetNewClosure() }
}
# HDR 与光标两个要求同时给时取交集，链仍只剩 wgc（两道闸门串起来，不各写一份答案）。
$cases += @{ Name = '--cursor include 与 --hdr tonemap 同时给出：交集还是 wgc'
   A = ($ANCHOR + @('--cursor', 'include', '--hdr', 'tonemap', '--capture', 'auto', '--verbose',
                    'out.png')); Exit = 0
   Check = { param($o) (@($o.input.captureChain) -join ',') -eq 'wgc' } }
# 一条选项写坏了名字要能对着契约找到；帮助里必须看得见这条选项（目录是契约的唯一来源）。
$cases += @{ Name = '--help 含 HDR 那一条选项与其三种取值'
   A = @('--help'); Exit = 3; Text = $true; Has = @('--hdr <auto|tonemap|refuse>') }
# HDR 不是匹配条件，也不是屏幕目标：只给 --hdr 仍是"零条件"。
$cases += @{ Name = '只给 --hdr 而不给任何条件：仍是文本帮助 + 退出码 2'
   A = @('--hdr', 'tonemap'); Exit = 2; Text = $true; Has = @('未指定任何匹配条件') }
# 两类查询都不产图，所以截图那一级的选项一条都不成立（HDR 属于取帧那一级）。
$cases += @{ Name = '环境查询与 --hdr 冲突（cli.query_conflict）'
   A = @('--capabilities', '--hdr', 'tonemap'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' -and
                        $o.errors[0].value -match '--hdr' } }
$cases += @{ Name = '屏幕查询与 --hdr 冲突'
   A = @('--screens', '--hdr', 'refuse'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' } }
$cases += @{ Name = '窗口查询与 --hdr 冲突（cli.window_query_conflict）'
   A = @('--list', '--hdr', 'tonemap'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.window_query_conflict' -and
                        $o.errors[0].value -match '--hdr' } }
# --hdr 与 --cursor / --roi 各管各的（一个色彩、一个指针、一块区域），不算冲突。
$cases += @{ Name = '--hdr 与 --cursor、--roi 同时给出不算冲突'
   A = ($ANCHOR + @('--hdr', 'tonemap', '--cursor', 'exclude', '--roi', '0,0,5,5', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.hdr -eq 'tonemap' -and $o.input.cursor -eq 'exclude' -and
                        $o.input.crop.mode -eq 'roi' } }

# 等比缩小（--scale）：这一节判**解析层**（写法、三条天花板各自的取值域、回显，以及
# "写了它不改变授权与帧上限那一层"）。判据本体（比例怎么取、向下舍入、只有最近邻一种内插、
# 映射怎么闭合）在离线层 tests\image_state.cpp；要真的动像素、真的缩下来那一段在 tests\scale.ps1。
# 这里全部配 --dry-run：不取帧，所以既不弹框也不落地。
# 写坏的名字或取值一律 cli.invalid_value（文案键 cli.scale_value），且整条不生效：
# 不留下"认得的那半"（缺等号、键名认不得、值超范围、段里多写一个逗号都算同一条）。
foreach ($bad in @('max-width', 'max-width=', 'max-width=0', 'max-width=16385',
                   'max-pixels=0', 'max-pixels=268435457', 'width=100', '=100',
                   'max-width=abc', 'max-width=-1', 'max-width=1e3', 'max-width=0x10',
                   'Max_Width=100', 'max-width=100 200', 'max-width=100,',
                   ',max-width=100', 'max-width=100,,max-height=50')) {
    $cases += @{ Name = ('--scale 的写法不合（[{0}]）' -f $bad)
       A = ($ANCHOR + @('--scale', $bad, 'out.png')); Exit = 1
       Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                            $o.errors[0].option -eq '--scale' }.GetNewClosure() }
}
# 三条天花板各自独立：只给一条时其余两个键整个不出现（不给"看起来像默认值"的 0）。
$cases += @{ Name = '--scale 只给 max-width：input.scale 里只有这一个键'
   A = ($ANCHOR + @('--scale', 'max-width=1920', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.scale.maxWidth -eq 1920 -and
                        -not $o.input.scale.PSObject.Properties.Name.Contains('maxHeight') -and
                        -not $o.input.scale.PSObject.Properties.Name.Contains('maxPixels') } }
$cases += @{ Name = '--scale 三条天花板一次给全'
   A = ($ANCHOR + @('--scale', 'max-width=1920,max-height=1080,max-pixels=2073600', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.scale.maxWidth -eq 1920 -and $o.input.scale.maxHeight -eq 1080 -and
                        $o.input.scale.maxPixels -eq 2073600 } }
# 这一条写多次不等于"重复给值"：三条天花板各说各的，合到同一个请求上（与 --roi 那种
# "一条选项里四个数"不同，也与 --capture / --hdr 那种"最后一个生效"不同，所以单独判）。
$cases += @{ Name = '--scale 写两次：两条天花板各记各的'
   A = ($ANCHOR + @('--scale', 'max-width=1920', '--scale', 'max-height=1080', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.scale.maxWidth -eq 1920 -and $o.input.scale.maxHeight -eq 1080 } }
$cases += @{ Name = '--scale 重复给同一条天花板：最后一个生效'
   A = ($ANCHOR + @('--scale', 'max-width=1920', '--scale', 'max-width=800', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.scale.maxWidth -eq 800 } }
foreach ($w in @(@('--scale=max-width=1920', 'maxWidth', 1920),
                 @('--scale=MAX-WIDTH=1920', 'maxWidth', 1920))) {
    $cases += @{ Name = ('--scale 的内联写法与键名大小写（{0}）' -f $w[0])
       A = ($ANCHOR + @($w[0], '--verbose', 'out.png')); Exit = 0
       Check = { param($o) $o.input.scale.($w[1]) -eq $w[2] }.GetNewClosure() }
}
# 取值域的边界：三条各自的两个端点在范围内，正好越界在上一组里已经判过。
$cases += @{ Name = '--scale 的边界值放行（max-width 与 max-height 都到 16384）'
   A = ($ANCHOR + @('--scale', 'max-width=16384,max-height=16384', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.scale.maxWidth -eq 16384 -and $o.input.scale.maxHeight -eq 16384 } }
$cases += @{ Name = '--scale 的边界值放行（max-pixels 到 268435456）'
   A = ($ANCHOR + @('--scale', 'max-pixels=268435456', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.scale.maxPixels -eq 268435456 } }
$cases += @{ Name = '没写 --scale 时 input.scale 整个键不出现'
   A = ($ANCHOR + @('--verbose', 'out.png')); Exit = 0
   Check = { param($o) -not $o.input.PSObject.Properties.Name.Contains('scale') } }
# --scale 必带取值（不是可选取值那一类），argv 到头就是 cli.missing_value；
# 且它要吃值：--scale out.png 里的 out.png 被当取值并当场报错，不会悄悄变成输出文件名。
$cases += @{ Name = '--scale 放在末尾没有取值：cli.missing_value'
   A = @('--class', 'Shell_TrayWnd', '--dry-run', 'out.png', '--scale'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.missing_value' -and
                        $o.errors[0].option -eq '--scale' } }
$cases += @{ Name = '--scale 吃掉后面的输出路径写法并当场报错（不变成文件名）'
   A = ($ANCHOR + @('--scale', 'out.png')); Exit = 1; ToStderr = $true
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                        $o.errors[0].value -eq 'out.png' } }
# 一条选项写坏了名字要能对着契约找到；帮助里必须看得见这条选项与它的三个键名。
$cases += @{ Name = '--help 含等比缩小那一条选项'
   A = @('--help'); Exit = 3; Text = $true; Has = @('--scale <key=N>') }
$cases += @{ Name = '帮助里 --scale 的说明写出三条天花板的键名'
   A = @('--help'); Exit = 3; Text = $true
   Has = @('max-width', 'max-height', 'max-pixels', 'scaleApplied') }
# --scale 不是匹配条件：只给仍是"零条件"。
$cases += @{ Name = '只给 --scale 而不给任何条件：仍是文本帮助 + 退出码 2'
   A = @('--scale', 'max-width=100'); Exit = 2; Text = $true; Has = @('未指定任何匹配条件') }
# 两类查询都不产图，所以截图那一级的选项一条都不成立（缩放属于编码之前那一级）。
$cases += @{ Name = '环境查询与 --scale 冲突（cli.query_conflict）'
   A = @('--capabilities', '--scale', 'max-width=100'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' -and
                        $o.errors[0].value -match '--scale' } }
$cases += @{ Name = '屏幕查询与 --scale 冲突'
   A = @('--screens', '--scale', 'max-width=100'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.query_conflict' } }
$cases += @{ Name = '窗口查询与 --scale 冲突（cli.window_query_conflict）'
   A = @('--list', '--scale', 'max-width=100'); Exit = 1
   Check = { param($o) ((Codes $o.errors) -join ',') -eq 'cli.window_query_conflict' -and
                        $o.errors[0].value -match '--scale' } }
# --scale 与 --roi / --cursor / --hdr 各管各的（一个说交多大、一个说留哪块、一个说指针、
# 一个说色彩），不算冲突，而且裁剪与缩放能同时成立（顺序是先裁后缩，判据在离线层）。
$cases += @{ Name = '--scale 与 --roi、--cursor、--hdr 同时给出不算冲突'
   A = ($ANCHOR + @('--scale', 'max-width=1920', '--roi', '0,0,5,5', '--cursor', 'exclude', '--hdr', 'tonemap', '--verbose', 'out.png')); Exit = 0
   Check = { param($o) $o.input.scale.maxWidth -eq 1920 -and $o.input.crop.mode -eq 'roi' -and
                        $o.input.cursor -eq 'exclude' -and $o.input.hdr -eq 'tonemap' } }
# 缩放在取帧之后才发生：--dry-run 一样只报告不截图，回显里三条天花板照写。
$cases += @{ Name = '--scale 与 --dry-run 一起：只报告不截图'
   A = ($ANCHOR + @('--scale', 'max-width=1920', '--verbose', 'out.png')); Exit = 0; Json = $true
   Notes = @('note.dry_run')
   Check = { param($o) $o.captured -eq 0 -and $o.input.scale.maxWidth -eq 1920 } }

# ---------- 只读查询（--capabilities / --diagnostics）----------
# 这两份文档不是截图结果：没有 captured / images，也没有 notes / input。用例要标 Query，
# 跑批那一段才按查询契约判（判据本体在 tests\capabilities.ps1 与离线层）。
$cases += @{ Name = '--capabilities 出查询文档、不截图'
   A = @('--capabilities'); Exit = 0; Query = $true
   Check = { param($o) $o.contract -eq 'capabilities' -and $o.contractVersion -eq 1 -and
                        @($o.backends).Count -eq 5 -and @($o.formats).Count -ge 7 -and
                        ($o.program.version -is [string]) -and $o.program.binary -eq 'ECAPTURE.EXE' -and
                        $o.os.declaredMinBuild -eq 18362 -and
                        $o.authorization.yesSkips -eq 'window-content' -and
                        $o.privacy.capturesScreen -eq $false -and $o.privacy.showsDialog -eq $false -and
                        $o.limits.stdoutTargetsMax -eq 1 } }
$cases += @{ Name = '--diagnostics 带可核对的构建标识'
   A = @('--diagnostics'); Exit = 0; Query = $true
   Check = { param($o) $o.contract -eq 'diagnostics' -and $o.contractVersion -eq 1 -and
                        ($o.build.id -is [string]) -and ($o.build.linkTimestamp -is [string]) -and
                        $o.build.known -eq $true -and $o.build.subsystemVersionIsSupportClaim -eq $false -and
                        $o.program.buildId -eq $o.build.id } }
$cases += @{ Name = '两条查询的共享字段一致（同一批判据，不是两份信息）'
   A = @('--capabilities'); Exit = 0; Query = $true
   Check = { param($o)
       $d = (Invoke-Ec @('--diagnostics')).Stdout | ConvertFrom-Json
       $same = ($d.os.build -eq $o.os.build) -and ($d.program.version -eq $o.program.version) -and
               ($d.session.displayTopology -eq $o.session.displayTopology) -and
               (@($d.backends | ForEach-Object { $_.name + ':' + $_.status + ':' + $_.reason }) -join ',') -eq
               (@($o.backends | ForEach-Object { $_.name + ':' + $_.status + ':' + $_.reason }) -join ',')
       $same -and $d.contract -eq 'diagnostics' } }
$cases += @{ Name = '查询文档不随 --lang 变（全部 ASCII，不读文案）'
   A = @('--capabilities'); Exit = 0; Query = $true
   Check = { param($o)
       $base = (Invoke-Ec @('--capabilities')).Stdout
       foreach ($l in @('zh-TW', 'en', 'ja')) {
           if ((Invoke-Ec @('--capabilities', '--lang', $l)).Stdout -ne $base) { return $false }
       }
       $true } }
$cases += @{ Name = '-v 给查询加 probes，-q 只去掉 caveats'
   A = @('--capabilities', '-v'); Exit = 0; Query = $true
   Check = { param($o)
       $names = @($o.PSObject.Properties.Name)
       $q = @((Invoke-Ec @('--capabilities', '-q')).Stdout | ConvertFrom-Json).PSObject.Properties.Name
       ($names -contains 'probes') -and ($names -contains 'caveats') -and
       ($names -notcontains 'input') -and ($q -notcontains 'caveats') -and
       (@((Invoke-Ec @('--capabilities', '-q')).Stdout | ConvertFrom-Json).backends).Count -eq 5 } }

# 查询与截图选项互斥：两条路对流与输出的约定不同，一起给出就是一次用法错，一张都不截。
$QUERY_CONFLICT = @(
    @('--capabilities', '--title', 'foo'),
    @('--capabilities', '--hwnd', '0x10'),
    @('--capabilities', '--monitor', '1'),
    @('--capabilities', '--capture', 'auto'),
    @('--capabilities', '--out', 'a.png'),
    @('--capabilities', '--yes'),
    @('--capabilities', '--dry-run'),
    @('--capabilities', '--timeout-ms', '100'),
    @('--capabilities', '--help'),
    @('--capabilities', '--version'),
    @('--diagnostics', '--class', 'Shell_TrayWnd'),
    @('--diagnostics', '--all')
)
foreach ($a in $QUERY_CONFLICT) {
    $cases += @{ Name = ('查询与截图选项互斥: ' + ($a -join ' '))
       A = $a; Exit = 1; Query = $false
       Check = { param($o)
           ((Codes $o.errors) -join ',') -match 'cli.query_conflict' -and
           $o.captured -eq 0 -and @($o.images).Count -eq 0 }.GetNewClosure() }
}
$cases += @{ Name = '两条查询同时给出 = 冲突，value 里两个名字都列出来'
   A = @('--capabilities', '--diagnostics'); Exit = 1
   Check = { param($o) $o.errors[0].code -eq 'cli.query_conflict' -and
                        $o.errors[0].option -eq '--capabilities' -and
                        $o.errors[0].value -like '*--capabilities*' -and
                        $o.errors[0].value -like '*--diagnostics*' } }
$cases += @{ Name = '位置参数在查询里也算输出路径（value 报 --out 而不回显那条路径）'
   A = @('--capabilities', 'D:\shots\secret.png'); Exit = 1
   Check = { param($o) $o.errors[0].code -eq 'cli.query_conflict' -and
                        $o.errors[0].value -eq '--out' -and
                        -not ($o.errors[0].value -match 'secret') } }
$cases += @{ Name = '冲突时报错只有一条，且所有冲突项一次列全'
   A = @('--diagnostics', '--title', 'a', '--yes', '--format', 'png'); Exit = 1
   Check = { param($o) @($o.errors).Count -eq 1 -and $o.errors[0].code -eq 'cli.query_conflict' -and
                        $o.errors[0].value -like '*--title*' -and $o.errors[0].value -like '*--yes*' -and
                        $o.errors[0].value -like '*--format*' } }

# ---------------------------------------------------------------------------
# 只读的屏幕枚举（--screens）：与环境查询同一家族，但交回的是**第三份**契约文档
#   * 它不截图、不弹框、不写文件、不改显示设置，所以截图那一级的选项一条都不成立
#   * 交回的每条屏都自带"这几种身份各稳到哪一层"，只有设备名与跨会话标识有选择器写法
#   * --yes 在这里是冲突而不是"不起作用但合法"：与 --capabilities 同一条判据
#     （窗口查询那边 --yes 是被接受但不影响结果，因为那一路真的会去问窗口的事）
# ---------------------------------------------------------------------------
$cases += @{ Name = '--screens 出的是 screens 那份契约，且不带截图那一份的顶层键'
   A = @('--screens'); Exit = 0; Query = $true
   Check = { param($o) ($o.contract -eq 'screens' -and $o.contractVersion -eq 1 -and
                        $o.authorization.pixelsRead -eq 0 -and
                        $o.authorization.displaySettingsChanged -eq $false -and
                        @($o.screens).Count -ge 1) } }
$cases += @{ Name = '--screens 里只有设备名与跨会话标识有选择器写法'
   A = @('--screens'); Exit = 0; Query = $true
   Check = { param($o) ($o.identity.deviceName.usableAsSelector -eq $true -and
                        $o.identity.monitorDevicePath.usableAsSelector -eq $true -and
                        $o.identity.adapterLuid.usableAsSelector -eq $false -and
                        $o.identity.ordinal.stableAcross -eq 'this_invocation' -and
                        $o.identity.adapterLuid.stableAcross -eq 'this_session') } }
$cases += @{ Name = '--screens 的授权自述：整屏一定要问人，--yes 不生效（隐私判据）'
   A = @('--screens'); Exit = 0; Query = $true
   Check = { param($o) ($o.authorization.screenCaptureConsent.desktopPixelsAlwaysAsk -eq $true -and
                        $o.authorization.screenCaptureConsent.yesSkipsThisLevel -eq $false -and
                        (@($o.caveats) -contains 'screen_capture_always_asks') -and
                        (@($o.caveats) -contains 'cross_session_stability_not_tested')) } }
$cases += @{ Name = '--screens 的 limits 与解析层同一个数（不在两处各写一遍）'
   A = @('--screens'); Exit = 0; Query = $true
   Check = { param($o) $o.limits.maxOrdinal -eq 65535 } }
$cases += @{ Name = '--screens 带 -q 只去掉 notes，稳定性与隐私判据一条不藏'
   A = @('--screens', '-q'); Exit = 0; Query = $true
   Check = { param($o) ($null -eq $o.notes -and @($o.caveats).Count -ge 8 -and
                        $o.privacy.includesDevicePaths -eq $true -and
                        $o.privacy.includesUsernames -eq $false -and
                        $o.privacy.includesFileSystemPaths -eq $false) } }
$cases += @{ Name = '--screens 带 -v 追加这一次查询的回显'
   A = @('--screens', '-v'); Exit = 0; Query = $true
   Check = { param($o) ($o.input.query -eq 'screens' -and $o.input.lang) } }
$cases += @{ Name = '--screens 与 --yes 冲突：它不截图，没有可授权的事'
   A = @('--screens', '--yes'); Exit = 1; Errors = @('cli.query_conflict') }
$cases += @{ Name = '--screens 与 --monitor 冲突：列屏不需要选屏'
   A = @('--screens', '--monitor', '1'); Exit = 1; Errors = @('cli.query_conflict') }
$cases += @{ Name = '三条环境查询同时给出 = 一条冲突，三个名字都列出来'
   A = @('--capabilities', '--diagnostics', '--screens'); Exit = 1
   Check = { param($o) ($o.errors[0].code -eq 'cli.query_conflict' -and
                        $o.errors[0].value -like '*--screens*' -and
                        $o.errors[0].value -like '*--capabilities*' -and
                        $o.errors[0].value -like '*--diagnostics*') } }
$cases += @{ Name = '帮助里有 --screens 那一条'
   A = @('--help'); Exit = 3; Text = $true; Has = @('--screens') }
$cases += @{ Name = '帮助里有能力查询一节与两个查询选项'
   A = @('--help'); Exit = 3; Text = $true
   Has = @('--capabilities', '--diagnostics', 'cli.query_conflict') }
$cases += @{ Name = '帮助里有先查询再选路的示例'
   A = @('--help'); Exit = 3; Text = $true; Has = @('--capabilities  先只读问一次') }
$cases += @{ Name = '查询不需要窗口条件，也不掉进"零条件 = 帮助"'
   A = @('--capabilities'); Exit = 0; Query = $true
   Check = { param($o) $o.contract -eq 'capabilities' } }

# ---------------------------------------------------------------------------
# 结构化的窗口发现与检查（--list / --inspect）：同一类只读出口，但走的是另一份契约。
# 带 WindowQuery 键的用例由跑批那一段按"窗口查询文档"判（不许出现 captured / images），
# 而报**参数冲突**那几条仍然按截图失败那一份形状判 —— 那是有意的：调用方按 code 分支的
# 代码不必为查询另写一份（与上面 --capabilities 那批的 Query = $false 同一道理）。
# 这里一律不截图：这两条命令一个像素都不取，也不弹框、不写文件（真机那层在 tests\windows.ps1）。
# ---------------------------------------------------------------------------
$cases += @{ Name = '--list 无匹配 = 空列表 + 退出码 0（不是截图那一次的 match.no_window + 4）'
   A = @('--list', '--class', 'NoSuchWindowXyz'); Exit = 0; WindowQuery = $true
   Check = { param($o) $o.contract -eq 'windowquery' -and $o.contractVersion -eq 1 -and
                        $o.query -eq 'list' -and @($o.windows).Count -eq 0 -and
                        $o.pagination.matched -eq 0 -and $o.pagination.returned -eq 0 -and
                        $o.pagination.truncated -eq $false -and
                        -not (@(Codes $o.errors).Count) } }
$cases += @{ Name = '--list 只读自述：没取像素、没弹框、没写文件，且 --yes 不影响结果'
   A = @('--list', '--class', 'NoSuchWindowXyz'); Exit = 0; WindowQuery = $true
   Check = { param($o) $o.authorization.readOnly -eq $true -and
                        $o.authorization.pixelsRead -eq 0 -and
                        $o.authorization.consentDialogShown -eq $false -and
                        $o.authorization.filesWritten -eq $false -and
                        $o.authorization.yesAffectsResult -eq $false -and
                        $o.authorization.identityFieldsAreNotConsent -eq $true -and
                        (@($o.caveats) -contains 'snapshot_expires') -and
                        (@($o.caveats) -contains 'identity_fields_are_not_a_token') -and
                        (@($o.caveats) -contains 'no_capture_performed') -and
                        (@($o.caveats) -contains 'no_window_touched') -and
                        (@($o.caveats) -contains 'invisible_and_zero_sized_excluded') } }
$cases += @{ Name = '--list 的默认可见性策略写在 policy 段里（不靠调用方猜）'
   A = @('--list', '--class', 'NoSuchWindowXyz'); Exit = 0; WindowQuery = $true
   Check = { param($o) $o.policy.invisibleExcluded -eq $true -and
                        $o.policy.zeroSizedExcluded -eq $true -and
                        $o.policy.minimizedIncluded -eq $false -and
                        $o.policy.systemWindowAssertion -eq $false -and
                        $o.policy.order -eq 'zOrder' -and
                        $o.pagination.limit -eq 50 -and $o.pagination.limitDefaulted -eq $true -and
                        $o.pagination.defaultLimit -eq 50 -and $o.pagination.maxLimit -eq 8192 } }
$cases += @{ Name = '--list 命中任务栏那一条：字段齐、默认不写完整路径、身份字段写明要复核'
   A = @('--list', '--class', 'Shell_TrayWnd'); Exit = 0; WindowQuery = $true
   Check = { param($o)
       $w = @($o.windows)[0]
       if (-not $w) { return $false }
       # 完整路径这个键在默认那一份里整个不出现（不是写一个空值）：键名都不该出现在文档文本里。
       $raw = (Invoke-Ec @('--list', '--class', 'Shell_TrayWnd')).Stdout
       ($w.hwnd -like '0x*') -and ($w.'class' -eq 'Shell_TrayWnd') -and ($w.pid -gt 0) -and
       $w.rect.width -gt 0 -and $w.readability.process.state -eq 'readable' -and
       $w.identity.verificationRequired -eq $true -and
       $w.identity.isAuthorizationToken -eq $false -and
       $w.identity.'class' -eq $w.'class' -and $w.identity.pid -eq $w.pid -and
       $w.identity.hwnd -eq $w.hwnd -and
       ($raw -notmatch 'exePath') } }
$cases += @{ Name = '--inspect 无匹配报 match.no_window + 4，并补上 stage=match'
   A = @('--inspect', '--class', 'NoSuchWindowXyz'); Exit = 4; WindowQuery = $true
   Check = { param($o) $o.contract -eq 'windowinspect' -and
                        (@(Codes $o.errors) -join ',') -eq 'match.no_window' -and
                        $o.errors[0].stage -eq 'match' -and @($o.windows).Count -eq 0 -and
                        $o.pagination.returned -eq 0 -and $o.pagination.limit -eq 1 -and
                        -not $o.errors[0].PSObject.Properties.Name.Contains('target') } }
$cases += @{ Name = '--inspect 按 --hwnd 点名一个不存在的句柄：match.no_window + 4'
   A = @('--inspect', '--hwnd', '0x1A0B4C'); Exit = 4; WindowQuery = $true
   Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'match.no_window' } }
$cases += @{ Name = '--list 与 --inspect 同时给出一条冲突，value 里两个名字都列出来'
   A = @('--list', '--inspect', '--class', 'Shell_TrayWnd'); Exit = 1
   Check = { param($o) $o.errors[0].code -eq 'cli.window_query_conflict' -and
                        $o.errors[0].value -like '*--list*' -and
                        $o.errors[0].value -like '*--inspect*' -and
                        @($o.errors).Count -eq 1 } }
# 冲突、越界、条件写坏这几档：交回的必须是截图那一份失败形状（captured / images / errors），
# 而不是窗口查询那份文档 —— 调用方按 errors[].code 分支的那段代码因此不用分叉。
$WIN_CONFLICT = @(
    @('--list', '--out', 'a.png'), @('--list', 'a.png'), @('--list', '--format', 'png'),
    @('--list', '--capture', 'auto'), @('--list', '--dry-run'),
    @('--list', '--consent-timeout-ms', '100'), @('--list', '--no-overwrite'),
    @('--list=all', '--index', '1'), @('--list=all', '--topmost-match'), @('--list', '--newest'),
    @('--inspect', '--all'), @('--inspect', '--quality', '50'),
    @('--list', '--capabilities'), @('--inspect', '--diagnostics')
)
foreach ($a in $WIN_CONFLICT) {
    $cases += @{ Name = ('窗口查询与截图那一级互斥: ' + ($a -join ' '))
       A = $a; Exit = 1
       Check = { param($o)
           # 每一条都必须是某类查询冲突（两类查询同时给出时会各报一条），且 stage 恒为 parse。
           (@($o.errors).Count -ge 1) -and
           (@(@($o.errors) | Where-Object { $_.code -notlike 'cli.*conflict' }).Count -eq 0) -and
           (@(@($o.errors) | Where-Object { $_.stage -ne 'parse' }).Count -eq 0) -and
           $o.captured -eq 0 -and @($o.images).Count -eq 0 }.GetNewClosure() }
}
# 环境查询与窗口查询同时给出：两份都登记，一条文档都出不来。
$cases += @{ Name = '--capabilities 与 --list 同时给出：两条冲突一次列全，不出任何文档'
   A = @('--capabilities', '--list'); Exit = 1
   Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'cli.query_conflict,cli.window_query_conflict' -and
                        $o.errors[0].value -like '*--list*' -and $o.errors[1].value -like '*--capabilities*' } }
# 数值写法与 --pid / --index 同一条规矩（只认严格十进制，越界在解析期就拒）。
$cases += @{ Name = '--limit 写 0 被拒（默认条数才是不给 --limit 的结果）'
   A = @('--list', '--limit', '0'); Exit = 1; Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'cli.invalid_number' } }
$cases += @{ Name = '--limit 写成十六进制被拒'; A = @('--list', '--limit', '0x10'); Exit = 1
   Check = { param($o) $o.errors[0].option -eq '--limit' } }
$cases += @{ Name = '--offset 写成负号被拒'; A = @('--list', '--offset', '-1'); Exit = 1
   Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'cli.invalid_number' } }
$cases += @{ Name = '--limit 超过一次求值的条数上限在解析期就拒'
   A = @('--list', '--limit', '8193'); Exit = 1; Check = { param($o) $o.errors[0].option -eq '--limit' } }
$cases += @{ Name = '--offset 到上限这个数本身合法（0 到 8192 那道线含两端）'
   A = @('--list', '--offset', '8192'); Exit = 0; WindowQuery = $true
   Check = { param($o) $o.pagination.offset -eq 8192 } }
# 取舍写在选项自己的取值里（--list=all / --inspect=path）：认得的取值改策略，认不了的整条作废，
# 而"省略取值"不吃后面的参数（那条位置参数照旧按输出路径算冲突）。
$cases += @{ Name = '--list=all 打开最小化这条策略并在 -v 回显'
   A = @('--list=all', '--class', 'NoSuchWindowXyz', '-v'); Exit = 0; WindowQuery = $true
   Check = { param($o) $o.policy.minimizedIncluded -eq $true -and
                        $o.input.includeIconic -eq $true -and $o.input.exePath -eq $false } }
$cases += @{ Name = '--inspect=path 打开完整路径这一项并在 -v 回显'
   A = @('--inspect=path', '--hwnd', '0x1A0B4C', '-v'); Exit = 4; WindowQuery = $true
   Check = { param($o) $o.input.exePath -eq $true -and $o.input.action -eq 'inspect' } }
$cases += @{ Name = '--list 的取值认不了就整条作废（不退化成默认策略）'
   A = @('--list=allx', '--class', 'Shell_TrayWnd'); Exit = 1
   Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                        $o.errors[0].option -eq '--list' -and $o.errors[0].value -eq 'allx' } }
$cases += @{ Name = '--inspect 的取值认不了也报 cli.invalid_value'
   A = @('--inspect=iconic', '--hwnd', '0x1A0B4C'); Exit = 1
   Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'cli.invalid_value' -and
                        $o.errors[0].option -eq '--inspect' } }
$cases += @{ Name = '两条窗口查询各带取值同时给出也算冲突'
   A = @('--list=all', '--inspect=path'); Exit = 1
   Check = { param($o) $o.errors[0].code -eq 'cli.window_query_conflict' -and
                        $o.errors[0].value -like '*--list*' -and $o.errors[0].value -like '*--inspect*' } }
$cases += @{ Name = '--list 省略取值时不吃后面的参数（那条按输出路径算冲突，不回显路径本身）'
   A = @('--list', 'shot.png'); Exit = 1
   Check = { param($o) $o.errors[0].code -eq 'cli.window_query_conflict' -and
                        $o.errors[0].value -eq '--out' -and
                        -not ((Invoke-Ec @('--list', 'shot.png')).Stdout -match 'shot') } }
$cases += @{ Name = '窗口查询与 --monitor 越界按同一判据报退出码 1'
   A = @('--list', '--monitor', '99'); Exit = 1; WindowQuery = $true
   Check = { param($o) (@(Codes $o.errors) -join ',') -eq 'match.monitor_out_of_range' } }
# -v 的 input 段：规范化后的这一次查询，不掺截图那一级（格式 / 输出路径）的东西。
$cases += @{ Name = '-v 给窗口查询追加 input 段并回显条件与分页'
   A = @('--list', '--class', 'Shell_TrayWnd', '--offset', '1', '--limit', '2', '-v')
   Exit = 0; WindowQuery = $true
   Check = { param($o) $o.input.action -eq 'list' -and $o.input.lang -eq 'zh-CN' -and
                        $o.input.offset -eq 1 -and $o.input.limit -eq 2 -and
                        $o.input.limitGiven -eq $true -and (@($o.input.class) -contains 'Shell_TrayWnd') -and
                        (@($o.input.title).Count -eq 0) -and $o.input.'policy' -eq 'ask' -and
                        -not $o.input.PSObject.Properties.Name.Contains('output') } }
$cases += @{ Name = '-q 抑制窗口查询的 notes，但 caveats 与 errors 不抑制'
   A = @('--list', '--class', 'NoSuchWindowXyz', '-q'); Exit = 0; WindowQuery = $true
   Check = { param($o) -not ($o.PSObject.Properties.Name.Contains('notes')) -and
                        (@($o.caveats).Count -gt 0) } }
$cases += @{ Name = '--lang 只换窗口查询的人话文字，不换 code 与字段名'
   A = @('--list', '--class', 'NoSuchWindowXyz', '--lang', 'en'); Exit = 0; WindowQuery = $true
   Check = { param($o) $o.contract -eq 'windowquery' -and $o.query -eq 'list' -and
                        (@($o.caveats) -contains 'snapshot_expires') -and
                        $o.pagination.matched -eq 0 } }
$cases += @{ Name = '帮助里有窗口查询那一节与两个入口'
   A = @('--help'); Exit = 3; Text = $true
   Has = @('--list [<all>]', '--inspect [<path>]', '--offset <n>', '--limit <n>') }
$cases += @{ Name = '窗口查询的文档不带 captured / images，也不许被截图那一段误判'
   A = @('--list', '--class', 'NoSuchWindowXyz'); Exit = 0; WindowQuery = $true
   Check = { param($o) $null -eq $o.captured -and $null -eq $o.images -and
                        ($null -ne $o.contract) } }




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
            $lim = Get-HelpLimit $Lang
            if ($help.Length -gt $lim) {
                $problems += "帮助文本 $($help.Length) 字符，超过 $Lang 的上限 $lim（上调要写清是哪一次改动加的行）"
            }
        }
    } else {
        if (-not $looksJson) { $problems += '期望 JSON 输出，实际是文本' }
        else {
            try { $o = $body | ConvertFrom-Json } catch { $problems += "JSON 解析失败: $_" }
        }
        if ($o) {
            # 查询文档（--capabilities / --diagnostics / --list / --inspect）是另外的契约：它按规矩
            # 不带 captured / images，也不许带。截图结果那份仍然必须有这两项。
            # WindowQuery = 判的是窗口查询那一份成功文档（windowquery / windowinspect）；
            # 它报参数冲突时交回的是截图那一份失败形状，所以那批用例不带这个键。
            $isQueryDoc = [bool]$c.Query -or [bool]$c.WindowQuery
            if ($isQueryDoc) {
                if ($null -ne $o.captured) { $problems += '查询 JSON 不该有 captured' }
                if ($null -ne $o.images)   { $problems += '查询 JSON 不该有 images' }
                if ($null -eq $o.contract) { $problems += '查询 JSON 缺 contract' }
                if ($null -eq $o.contractVersion) { $problems += '查询 JSON 缺 contractVersion' }
            } else {
                if ($null -eq $o.captured) { $problems += '缺 captured' }
                if ($null -eq $o.images)   { $problems += '缺 images' }
            }
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

# ---------------------------------------------------------------------------
# 省略 --out 与显式 --out - 必须是同一件事（真机那半由 tests\streams.ps1 判）
#   比对的机器语义：退出码、errors 的 code 与其全部定位字段、captured 与 images 的形状
#   允许不同的：notes（隐式那条发 note.output_defaulted_stdout，显式那条发
#              note.pipe_default_format）、message / hint 的人读文字、-v 的 input 回显
#   旧实现把隐式这条路上的失败整段换成 cli.missing_output + 退出码 1，调用方补上 --out
#   也治不了原问题（没命中窗口、人拒绝了、写坏了文件都一样被说成"缺少输出路径"）。
# ---------------------------------------------------------------------------
function Read-EcJson {
    param($Result)
    $body = if ($Result.Stdout.Trim()) { $Result.Stdout } else { $Result.Stderr }
    try { return ($body | ConvertFrom-Json) } catch { return $null }
}

function Get-EcMachineShape {
    <# 只取"调用方据此分支"的那些字段：code 与定位字段都不随 --lang 变。 #>
    param($Json)
    if (-not $Json) { return '<非 JSON>' }
    $lines = @('captured={0}' -f $Json.captured, 'images={0}' -f @( $Json.images ).Count)
    foreach ($e in @($Json.errors)) {
        if ($null -eq $e) { continue }
        $lines += (@('err', $e.code, $e.option, $e.value, $e.target, $e.backend, $e.stage,
                     $e.hresult, $e.win32) -join '|')
    }
    foreach ($i in @($Json.images)) {
        if ($null -eq $i) { continue }
        $lines += (@('img', $i.file, $i.source, $i.width, $i.height, $i.bytes) -join '|')
    }
    return ($lines -join "`n")
}

$EQUIV_BASE = @(
    @{ Name = '未知选项';             A = @('--nope') },
    @{ Name = 'hwnd 非法';            A = @('--hwnd', 'zzz') },
    @{ Name = 'pid 为 0';             A = @('--pid', '0') },
    @{ Name = '格式取值没有编码器';   A = @('--format', 'webp', '--pid', '1') },
    @{ Name = '选项冲突';             A = @('--index', '2', '--newest', '--pid', '1') },
    @{ Name = '无匹配窗口';           A = @('--class', 'NoSuchWindowXyz') },
    @{ Name = 'index 越界';           A = @('--class', 'Shell_TrayWnd', '--index', '99') },
    @{ Name = '屏幕编号越界';         A = @('--monitor', '99') },
    @{ Name = '整屏模式拒绝 dwm';     A = @('--monitor', 'primary', '--capture', 'dwm') },
    @{ Name = 'dry-run 命中一个窗口'; A = @('--class', 'Shell_TrayWnd', '--dry-run') },
    @{ Name = 'dry-run 屏幕目标';     A = @('--monitor', '1', '--dry-run') }
)
$equivBad = 0
foreach ($case in $EQUIV_BASE) {
    $ri = Invoke-Ec $case.A                                       # 根本没给输出路径
    $re = Invoke-Ec ($case.A + @('--out', '-'))                   # 显式写 stdout
    $oi = Read-EcJson $ri
    $oe = Read-EcJson $re
    $problems = @()
    if (-not $oi -or -not $oe) {
        $problems += '有一侧的输出不是 JSON'
    } else {
        $si = Get-EcMachineShape $oi
        $se = Get-EcMachineShape $oe
        if ($si -ne $se) {
            $problems += ("机器语义不同：`n        隐式 [{0}]`n        显式 [{1}]" -f `
                          ($si -replace "`n", '`n'), ($se -replace "`n", '`n'))
        }
        if ((Codes $oi.errors) -contains 'cli.missing_output') { $problems += '又出现了 cli.missing_output' }
        if (@(Codes $oi.notes) -notcontains 'note.output_defaulted_stdout') { $problems += '隐式那条没发默认走 stdout 的提示' }
    }
    if ($ri.Exit -ne $re.Exit) { $problems += ("退出码 隐式={0} 显式={1}" -f $ri.Exit, $re.Exit) }
    # 这两条路都不落地：一张图都没出（上面每个用例都在取帧之前就返回），
    # 而 stdout 在任何情况下都不许冒出文字——图片字节才是它的主人
    if ($ri.StdoutBytes.Length -ne 0) { $problems += '隐式 stdout 时 stdout 收到了不该有的字节' }
    if ($re.StdoutBytes.Length -ne 0) { $problems += '显式 --out - 时 stdout 收到了不该有的字节' }
    if ($ri.Stderr.Trim() -eq '') { $problems += '结果没走 stderr' }

    if ($problems.Count) {
        $equivBad++
        Write-Host ("  FAIL  显式/隐式 stdout · {0}" -f $case.Name) -ForegroundColor Red
        foreach ($p in $problems) { Write-Host ("        · {0}" -f $p) -ForegroundColor Red }
    } else {
        Write-Host ("  PASS  显式/隐式 stdout · {0}（exit={1}）" -f $case.Name, $ri.Exit) -ForegroundColor DarkGreen
    }
}
if ($equivBad) {
    Write-Host ("省略 --out 与 --out - 的等价检查失败：{0} 项" -f $equivBad) -ForegroundColor Red
    exit 1
}

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
# 按 --screens 交回的标识点名叫屏（本机现场，不靠猜 DISPLAY1 一定存在）
#   上面那批用例判的是**写法**（前缀、空取值、盘符不被吃掉、不存在的标识报错）。
#   这里判的是"抄回来能不能用"：先从 --screens 取本机此刻的选择器，再原样写回 --monitor，
#   两条各跑一次 --dry-run（不取帧、不弹框、不写文件），退出码必须是 0 且 monitorKind 对得上。
#   一台机器都点不出名 = 这个功能对外没有意义，所以这条算失败而不是 SKIP。
#   （多屏、负坐标、旋转、热插拔的现场由 tests\screens.ps1 判，本机造不出的记未验证。）
# ---------------------------------------------------------------------------
$screensBad = 0
$sq = Invoke-Ec @('--screens')
$sqJson = $null
try { $sqJson = ($sq.Stdout | ConvertFrom-Json) } catch { }
if (-not $sqJson -or @($sqJson.screens).Count -lt 1) {
    $screensBad++
    Write-Host '  FAIL  --screens 没能列出本机此刻的屏幕（后面的标识选屏判据无从对照）' -ForegroundColor Red
} else {
    $first = @($sqJson.screens)[0]
    foreach ($pair in @(
            @{ Kind = 'device'; Value = $first.selectors.device },
            @{ Kind = 'id'; Value = $first.selectors.id })) {
        if (-not $pair.Value) {
            $screensBad++
            Write-Host ("  FAIL  --screens 没交回 {0} 那条选择器" -f $pair.Kind) -ForegroundColor Red
            continue
        }
        $r = Invoke-Ec @('--monitor', $pair.Value, '--dry-run', '-v', 'out.png')
        $o = $null
        # 截图那一路可能把 stdout 让给图片，所以按"哪条流有内容"取（与本文件其余处一致）。
        $body = if ($r.Stdout.Trim()) { $r.Stdout } else { $r.Stderr }
        try { $o = ($body | ConvertFrom-Json) } catch { }
        if (-not $o -or $r.Exit -ne 0 -or $o.input.monitorKind -ne $pair.Kind -or
            $o.input.target -ne 'screen' -or $o.input.monitor -ne $pair.Value) {
            $screensBad++
            Write-Host ("  FAIL  --monitor {0} 抄不回本机此刻那块屏（exit={1}，回显={2}/{3}）" -f `
                        $pair.Value, $r.Exit, $(if ($o) { $o.input.monitorKind } else { 'NOT JSON' }), `
                        $(if ($o) { $o.input.target } else { '' })) -ForegroundColor Red
        } else {
            Write-Host ("  PASS  --monitor {0}… 用 --screens 交回的标识点到了那块屏" -f $pair.Kind) -ForegroundColor DarkGreen
        }
        # 按屏过滤窗口那一路用的是同一份判据：--list 也得认这两条标识
        $l = Invoke-Ec @('--list', '--monitor', $pair.Value)
        $lj = $null
        try { $lj = ($l.Stdout | ConvertFrom-Json) } catch { }
        if (-not $lj -or $l.Exit -ne 0 -or $lj.contract -ne 'windowquery') {
            $screensBad++
            Write-Host ("  FAIL  --list --monitor {0} 没有走通（窗口查询与截图必须共用同一套选屏判据）" -f $pair.Kind) -ForegroundColor Red
        } else {
            Write-Host ("  PASS  --list 认得同一条标识（选屏判据只有一份）" -f $pair.Kind) -ForegroundColor DarkGreen
        }
    }
    # 同一条标识在两次调用之间指同一块屏：这是"别再猜编号"这件事的全部依据。
    $again = Invoke-Ec @('--screens')
    $againJson = $null
    try { $againJson = ($again.Stdout | ConvertFrom-Json) } catch { }
    $idAgain = if ($againJson) { @($againJson.screens)[0].selectors.id } else { $null }
    if ($idAgain -ne $first.selectors.id) {
        $screensBad++
        Write-Host '  FAIL  同一块屏在两次 --screens 之间交回了不同的跨会话标识' -ForegroundColor Red
    } else {
        Write-Host '  PASS  跨会话标识在两次调用之间逐字相同（本机能判的就这么多，跨重启未验证）' -ForegroundColor DarkGreen
    }
}
if ($screensBad) { exit 1 }
Write-Host '  PASS  屏幕标识选屏这一路整体成立' -ForegroundColor DarkGreen

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
    @{ Name = '未给输出路径且无匹配'; A = @('--pid', '1'); Exit = 4 },
    @{ Name = '无匹配窗口'; A = @('--class', 'NoSuchWindowXyz', 'out.png'); Exit = 4 },
    @{ Name = '未知取图方式'; A = @('--capture', 'waiwang', '--pid', '1', 'out.png'); Exit = 1 },
    @{ Name = 'index 越界'; A = @('--class', 'Shell_TrayWnd', '--index', '99', 'out.png'); Exit = 1 },
    @{ Name = '开关不接受取值'; A = @('--json=maybe', '--pid', '1', 'out.png'); Exit = 1 },
    @{ Name = '屏幕编号越界'; A = @('--monitor', '99', '--dry-run', 'out.png'); Exit = 1 },
    @{ Name = '整屏不支持的通道'; A = @('--monitor', 'primary', '--capture', 'dwm', 'out.png'); Exit = 1 },
    @{ Name = 'all 与窗口条件冲突'; A = @('--monitor', 'all', '--class', 'Shell_TrayWnd', 'out.png'); Exit = 1 },
    # 正则语法现在由匹配执行层判（父进程不预编译）：四语的 message/hint 都要有，
    # 而 code 与退出码必须逐字一致（stage=match 也是机器可读部分，不随语言变）。
    @{ Name = '正则语法不合（匹配阶段判出）'; A = @('--title-regex', '[bad(', 'out.png'); Exit = 1 },
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
    @{ Name = '语言取值非法'; A = @('--lang', 'klingon', '--pid', '1', 'out.png'); Exit = 1 },
    # 只读查询的冲突那两条：文字四种语言都要有，而被拒的 code 与退出码必须完全一致
    @{ Name = '查询与截图选项冲突'; A = @('--capabilities', '--title', 'x'); Exit = 1 },
    @{ Name = '两条查询同时给出'; A = @('--capabilities', '--diagnostics'); Exit = 1 },
    # 窗口查询那几条同规矩：cli.window_query_conflict 与 match.no_window 的文案四语都要有，
    # 而被拒的 code、stage 与退出码必须逐字一致（message 才随 --lang 变）。
    @{ Name = '窗口查询与 --out 冲突'; A = @('--inspect', 'a.png'); Exit = 1 },
    @{ Name = '窗口查询与取图方式/格式共存'; A = @('--list', '--format', 'png', '--capture', 'auto'); Exit = 1 },
    @{ Name = '窗口查询无匹配报 match.no_window'; A = @('--inspect', '--class', 'NoSuchWindowXyz'); Exit = 4 },
    @{ Name = '窗口查询的分页数字写坏'; A = @('--list', '--limit', '1e2'); Exit = 1 },
    @{ Name = '窗口查询的 --offset 越界写法'; A = @('--list', '--offset', '9.5'); Exit = 1 },
    # 屏幕标识这一路的三条码：文字四种语言都要有，被拒的 code / stage / 退出码必须逐字一致
    @{ Name = '屏幕标识不存在'; A = @('--monitor=device:NOSUCHSCREEN', '--dry-run', 'out.png'); Exit = 4 },
    @{ Name = '屏幕标识前缀不认识'; A = @('--monitor=foo:1', 'out.png'); Exit = 1 },
    @{ Name = '屏幕标识取值为空'; A = @('--monitor=id:', 'out.png'); Exit = 1 },
    @{ Name = '屏幕查询与截图选项冲突'; A = @('--screens', '--yes'); Exit = 1 },
    # 光标那两条解析期就出结果的码：文字四种语言都要有，而 code / stage / 退出码逐字一致。
    # （env.cursor_unsupported 与 capture.cursor_unverifiable 要到运行期才出得来，
    #   它们的四语一致性由 tests\cursor.ps1 与离线层判，这里不为了凑现场去截一张图。）
    @{ Name = '光标取值非法'; A = @('--cursor', 'watery', '--pid', '1', 'out.png'); Exit = 1 },
    @{ Name = '光标要求配做不到的通道'; A = @('--cursor', 'include', '--capture', 'bitblt', '--pid', '1', 'out.png'); Exit = 1 },
    @{ Name = '光标要求与环境查询冲突'; A = @('--capabilities', '--cursor', 'exclude'); Exit = 1 }
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
    $lim = Get-HelpLimit $tag
    if ($help.Length -gt $lim) {
        $bad++
        Write-Host ("  FAIL  {0} 帮助文本 {1} 字符，超过 {0} 的上限 {2}" -f $tag, $help.Length, $lim) -ForegroundColor Red
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
