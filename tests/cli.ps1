<#
.SYNOPSIS
    ECAPTURE.EXE 输出契约回归测试：文本（help/version）与精简 JSON（结果/错误）。
.EXAMPLE
    .\tests\cli.ps1
    .\tests\cli.ps1 -Exe build\Debug\ecapture.exe -ShowAll
#>
param(
    [string]$Exe,
    [switch]$ShowAll
)

$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
if (-not $Exe) { $Exe = Join-Path $root 'build\ecapture.exe' }
if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe（先运行 .\build.ps1）" }
$Exe = (Get-Item -LiteralPath $Exe).FullName

# 用 .NET Process 分别捕获 stdout / stderr：PS 5.1 的 2>&1 会把原生 stderr 包装成
# 错误记录文字，那样就没法校验 body 是不是合法 JSON。
function Quote-NativeArg([string]$a) {
    if ($a -eq '' -or $a -match '[\s"]') {
        $escaped = ($a -replace '(\\+)', '$1$1') -replace '"', '\"'
        return '"' + $escaped + '"'
    }
    return $a
}

function Invoke-Ecapture([string[]]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $psi.Arguments = ($Arguments | ForEach-Object { Quote-NativeArg $_ }) -join ' '
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    # 先读完 stdout 再读 stderr：按契约两者不会同时携带大文档（-o - 时 JSON 才走 stderr）
    $so = $p.StandardOutput.ReadToEnd()
    $se = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return [pscustomobject]@{ Exit = $p.ExitCode; Stdout = $so; Stderr = $se }
}

function Codes($list) { if ($null -eq $list) { @() } else { @($list | ForEach-Object { $_.code }) } }

$MATCH_FLAGS = @('--hwnd','--pid','--process','--exe','--title','--title-contains','--title-regex','--class')

$cases = @(
    # ---------- 文本输出 ----------
    @{ Name = '无参数 -> 文本帮助'; A = @(); Exit = 2; Text = $true
       Has = @('未指定任何匹配条件', '窗口匹配条件', '用法:', '退出码:') }
    @{ Name = '--help 文本且含全部条件'; A = @('--help'); Exit = 3; Text = $true
       Has = (@('用法:') + $MATCH_FLAGS) }
    @{ Name = '/help 斜杠形式'; A = @('/help'); Exit = 3; Text = $true; Has = @('窗口匹配条件') }
    @{ Name = '帮助体积受控（<6KB）'; A = @('--version'); Exit = 0; Text = $true
       Has = @('EvernightCapture') }

    # ---------- 成功：精简 JSON ----------
    @{ Name = '解析通过 -> 只有 captured/images'; A = @('--process','notepad.exe','out.png')
       Exit = 0; Json = $true
       Check = { param($o) $o.captured -eq 0 -and @($o.images).Count -eq 0 -and
                            -not $o.PSObject.Properties.Name.Contains('errors') -and
                            -not $o.PSObject.Properties.Name.Contains('input') -and
                            -not $o.PSObject.Properties.Name.Contains('tool') -and
                            -not $o.PSObject.Properties.Name.Contains('version') -and
                            -not $o.PSObject.Properties.Name.Contains('schema') } }
    @{ Name = '8 个条件同时给出仍是通过'; A = @('--hwnd','0x10','--pid','1','--process','p.exe','--exe','D:\e.exe',
                                                '--title','t','--title-contains','c','--title-regex','r','--class','k','out.png')
       Exit = 0; Json = $true }
    @{ Name = '同类多值 OR'; A = @('--title','A','--title','B','out.png'); Exit = 0; Json = $true }
    @{ Name = '重复取值 -> note'; A = @('--title','A','--title','A','out.png'); Exit = 0; Json = $true
       Notes = @('note.duplicate_value') }
    @{ Name = '短选项 -p/-o 与格式推断'; A = @('-p','notepad.exe','-o','out.jpg'); Exit = 0; Json = $true }
    @{ Name = '--opt=value'; A = @('--process=notepad.exe','--format=png','out.x'); Exit = 0; Json = $true }
    @{ Name = '选项名忽略大小写'; A = @('--TITLE','X','out.png'); Exit = 0; Json = $true }
    @{ Name = '值以 - 开头'; A = @('--title-regex','-abc','out.png'); Exit = 0; Json = $true }
    @{ Name = '中文标题'; A = @('--title','无期迷途 主线 12-3','out.png'); Exit = 0; Json = $true }
    @{ Name = '标准输出'; A = @('--pid','1','-o','-'); Exit = 0; Json = $true
       Notes = @('note.pipe_default_format') }
    @{ Name = '-- 结束选项解析'; A = @('--pid','1','--','--weird.png'); Exit = 0; Json = $true }
    @{ Name = '--all 无占位符 -> note'; A = @('--pid','1','--all','out.png'); Exit = 0; Json = $true
       Notes = @('note.all_without_placeholder') }
    @{ Name = '非 jpeg 时 quality -> note'; A = @('--pid','1','--quality','50','out.png'); Exit = 0
       Notes = @('note.quality_ignored') }
    @{ Name = '--json 是兼容空开关'; A = @('--json','--pid','1','out.png'); Exit = 0
       Notes = @('note.json_flag_deprecated') }

    # ---------- 错误：JSON + code ----------
    @{ Name = 'HWND 非法'; A = @('--hwnd','zzz','out.png'); Exit = 1; Json = $true
       Errors = @('cli.invalid_number')
       Check = { param($o) $o.errors[0].option -eq '--hwnd' -and $o.errors[0].value -eq 'zzz' -and
                            $o.errors[0].message -and $o.errors[0].hint } }
    @{ Name = 'PID 为 0'; A = @('--pid','0','out.png'); Exit = 1; Errors = @('cli.invalid_number'); Json = $true }
    @{ Name = 'process 带路径'; A = @('--process','D:\a.exe','out.png'); Exit = 1
       Errors = @('cli.invalid_value'); Json = $true }
    @{ Name = 'exe 只有文件名 -> note'; A = @('--exe','a.exe','out.png'); Exit = 0
       Notes = @('note.exe_path_looks_like_name'); Json = $true }
    @{ Name = '正则非法'; A = @('--title-regex','[bad(','out.png'); Exit = 1
       Errors = @('cli.invalid_regex'); Json = $true }
    @{ Name = '缺少输出路径'; A = @('--pid','1'); Exit = 1; Errors = @('cli.missing_output'); Json = $true }
    @{ Name = '扩展名无法判定'; A = @('--pid','1','out.unknown'); Exit = 1
       Errors = @('cli.unrecognized_extension'); Json = $true }
    @{ Name = '--out 与位置参数冲突'; A = @('--pid','1','out.png','--out','b.png'); Exit = 1
       Errors = @('cli.duplicate_output'); Json = $true }
    @{ Name = '选择策略互斥'; A = @('--index','2','--newest','out.png'); Exit = 1
       Errors = @('cli.conflicting_options'); Json = $true }
    @{ Name = '未知选项带纠正 hint'; A = @('--titel','x','out.png'); Exit = 1; Json = $true
       Check = { param($o) (@($o.errors | ForEach-Object code) -contains 'cli.unknown_option') -and
                            (@($o.errors | Where-Object { $_.code -eq 'cli.unknown_option' }).hint) -eq '--title' } }
    @{ Name = '开关不接受取值'; A = @('--json=maybe','--pid','1','out.png'); Exit = 1
       Errors = @('cli.switch_takes_no_value'); Json = $true }
    @{ Name = '多余位置参数'; A = @('--pid','1','a.png','b.png'); Exit = 1
       Errors = @('cli.unexpected_positional'); Json = $true }

    # ---------- --verbose / --quiet 的形状差异 ----------
    @{ Name = '--verbose 追加 input 段'; A = @('-v','--pid','12','--hwnd','0x1A0B4C','out.png'); Exit = 0
       Check = { param($o) $o.input.pid[0] -eq 12 -and $o.input.hwnd[0].hex -eq '0x001A0B4C' -and
                            $o.input.hwnd[0].decimal -eq 1706828 -and $o.input.output -like '*out.png' -and
                            $o.input.format -eq 'png' -and $o.input.policy -eq 'ask' } }
    @{ Name = '--quiet 抑制 notes 但保留 errors'; A = @('-q','--hwnd','zzz','out.png'); Exit = 1
       Errors = @('cli.invalid_number')
       Check = { param($o) -not $o.PSObject.Properties.Name.Contains('notes') } }
    @{ Name = '-vq 时 notes 保留'; A = @('-vq','--exe','a.exe','out.png'); Exit = 0
       Notes = @('note.exe_path_looks_like_name') }
)

$results = @()
foreach ($c in $cases) {
    $r = Invoke-Ecapture $c.A
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
            $help = (Invoke-Ecapture @('--help')).Stdout
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

    if ($r.Exit -ne $c.Exit) { $problems += "退出码 $($r.Exit) 期望 $($c.Exit)" }
    # 文本与 JSON 默认都走 stdout；只有 -o - 时 JSON 才改走 stderr（此时 stdout 必须空）
    $expectStderr = ($c.A -contains '-' -and $c.A -contains '-o')
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
# ---------------------------------------------------------------------------
$r = Invoke-Ecapture @('--pid', '1', '--format', 'png', '-o', '-')
$o = $null
try { $o = $r.Stderr | ConvertFrom-Json } catch { }
if ($r.Stdout.Trim() -eq '' -and $o -and $o.captured -eq 0 -and $r.Exit -eq 0) {
    Write-Host '  PASS  -o - 时 stdout 保持纯净，JSON 走 stderr' -ForegroundColor DarkGreen
} else {
    Write-Host ("  FAIL  通道分离：stdout={0} 字节，stderr 可解析={1}" -f $r.Stdout.Length, ($null -ne $o)) -ForegroundColor Red
    exit 1
}
