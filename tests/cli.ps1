<#
.SYNOPSIS
    ECAPTURE.EXE 命令行解析的回归测试（不依赖截图功能）。
.EXAMPLE
    .\tests\cli.ps1
    .\tests\cli.ps1 -Exe build\Debug\ecapture.exe
#>
param(
    [string]$Exe
)

$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
if (-not $Exe) { $Exe = Join-Path $root 'build\ecapture.exe' }
if (-not (Test-Path $Exe)) { throw "找不到可执行文件：$Exe（先运行 .\build.ps1）" }
$Exe = (Get-Item -LiteralPath $Exe).FullName

# 每项：说明 / 参数 / 期望退出码 / 输出中必须包含的片段（可省略）
$cases = @(
    @{ Name = '无参数 -> 帮助';       A = @();                                        Code = 2; Has = @('未指定任何匹配条件', '窗口匹配条件') }
    @{ Name = '--help';               A = @('--help');                                Code = 3; Has = @('ECAPTURE.EXE [条件...]') }
    @{ Name = '/help 斜杠形式';       A = @('/help');                                 Code = 3; Has = @('退出码') }
    @{ Name = '--version';            A = @('--version');                             Code = 0; Has = @('EvernightCapture 0.1.0') }

    @{ Name = '8 个条件全部 AND';     A = @('--hwnd','0x10','--pid','1','--process','p.exe','--exe','e.exe',
                                             '--title','t','--title-contains','c','--title-regex','r','--class','k','out.png')
                                              Code = 0; Has = @('--hwnd','--pid','--process','--exe','--title','--title-contains','--title-regex','--class','8 类 / 8 个值') }
    @{ Name = '同类多值 OR';          A = @('--title','A','--title','B','out.png');   Code = 0; Has = @('"A", "B"', '2 个值') }
    @{ Name = '重复值去重提示';       A = @('--title','A','--title','A','out.png');   Code = 0; Has = @('重复的 --title') }

    @{ Name = '短选项与合并';         A = @('-p','notepad.exe','-o','out.jpg');       Code = 0; Has = @('jpeg') }
    @{ Name = '--opt=value';          A = @('--process=notepad.exe','--format=png','out.x'); Code = 0; Has = @('notepad.exe') }
    @{ Name = '选项名大小写不敏感';   A = @('--TITLE','X','out.png');                 Code = 0; Has = @('--title') }

    @{ Name = 'HWND 十六进制(带字母)'; A = @('--hwnd','001A0B4C','out.png');          Code = 0; Has = @('0x1A0B4C') }
    @{ Name = 'HWND 十进制';          A = @('--hwnd','1706828','out.png');            Code = 0; Has = @('0x1A0B4C') }
    @{ Name = 'HWND 非法';            A = @('--hwnd','zzz','out.png');                Code = 1; Has = @('有效的句柄值') }
    @{ Name = 'PID 为 0';             A = @('--pid','0','out.png');                   Code = 1; Has = @('有效的进程 ID') }
    @{ Name = 'process 带路径';       A = @('--process','D:\a.exe','out.png');        Code = 1; Has = @('只接受映像文件名') }
    @{ Name = 'exe 只有文件名(提示)'; A = @('--exe','a.exe','out.png');               Code = 0; Has = @('不是完整路径') }
    @{ Name = '正则非法';             A = @('--title-regex','[bad(','out.png');       Code = 1; Has = @('正则表达式无效') }
    @{ Name = '正则以 - 开头';        A = @('--title-regex','-abc','out.png');        Code = 0; Has = @('"-abc"') }
    @{ Name = 'title 带空格中文';     A = @('--title','无期迷途 主线','out.png');     Code = 0; Has = @('无期迷途 主线') }

    @{ Name = '缺少输出路径';         A = @('--pid','1');                             Code = 1; Has = @('缺少输出路径') }
    @{ Name = '扩展名无法判定';       A = @('--pid','1','out.unknown');               Code = 1; Has = @('无法从输出文件名确定图片格式') }
    @{ Name = '--out 与位置参数冲突'; A = @('--pid','1','out.png','--out','b.png');   Code = 1; Has = @('输出路径重复') }
    @{ Name = '选择策略互斥';         A = @('--index','2','--newest','out.png');      Code = 1; Has = @('互相冲突') }
    @{ Name = '未知选项 + 纠正';      A = @('--titel','x','out.png');                 Code = 1; Has = @('未知选项', '是否想输入 --title') }
    @{ Name = '开关给了取值';         A = @('--json=maybe','--pid','1','out.png');    Code = 1; Has = @('不接受取值') }
    @{ Name = '标准输出';             A = @('--pid','1','-o','-');                    Code = 0; Has = @('标准输出') }
    @{ Name = '-- 结束选项解析';      A = @('--pid','1','--','--weird.png');          Code = 0; Has = @('"--weird.png"') }
    @{ Name = '--all 无占位符提示';   A = @('--pid','1','--all','out.png');           Code = 0; Has = @('占位符') }
    @{ Name = '--json 结构';          A = @('--pid','7','--json','out.png');          Code = 0; Has = @('"pid": [7]', '"format": "png"') }
    @{ Name = '-vq 冲突提示';         A = @('-vq','--pid','1','out.png');             Code = 0; Has = @('以 --verbose 为准') }
)

$pass = 0; $fail = 0
# 被测程序的 stderr 是正常输出通道：这里必须允许 NativeCommandError，否则 PS 5.1 会中断
$ErrorActionPreference = 'Continue'
foreach ($c in $cases) {
    $argv = $c.A
    $out = & $Exe @argv 2>&1 | Out-String
    $code = $LASTEXITCODE
    $problems = @()
    if ($code -ne $c.Code) { $problems += "退出码 $code != $($c.Code)" }
    foreach ($frag in $c.Has) {
        if (-not $out.Contains($frag)) { $problems += "输出缺少片段: $frag" }   # Contains = 字面匹配，[ ] 不当通配符
    }
    if ($problems.Count -eq 0) {
        $pass++
        Write-Host ("  PASS  {0}" -f $c.Name) -ForegroundColor DarkGreen
    } else {
        $fail++
        Write-Host ("  FAIL  {0}  ->  {1}" -f $c.Name, ($problems -join '; ')) -ForegroundColor Red
        ($out -split "`n" | Where-Object { $_.Trim() } | Select-Object -First 6 | ForEach-Object { "        | $_" })
    }
}

Write-Host ''
Write-Host ("共 {0} 例，通过 {1}，失败 {2}" -f $cases.Count, $pass, $fail) -ForegroundColor $(if ($fail) { 'Red' } else { 'Green' })
if ($fail) { exit 1 }
