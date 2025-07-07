<#
.SYNOPSIS
    用各语言 --help 的原样输出重写四份 README 里的帮助段。
.DESCRIPTION
    四份 README 各有一段被 `<!-- BEGIN ECAPTURE-HELP -->` / `<!-- END ECAPTURE-HELP -->`
    夹住的 ```text 围栏，内容必须是对应语言 `ECAPTURE.EXE --help --lang <语言>` 的原始输出——
    帮助正文由选项目录生成，所以它是唯一会随代码变化而 silently 过期的文档段。

    本脚本只改这两行标记之间的部分，标记行本身与其余正文不动（正文归人维护）。
    统一用 LF 换行、UTF-8 无 BOM 写回，和仓库其余文本文件一致。
    缺标记、标记成对不上、取不到 --help 都直接报错退出，不会"跳过"某个文件。

    -Check 只比对不写：四份都新鲜就返回 0，有任何一份过期就返回 1 并列出，适合挂在测试里。
.EXAMPLE
    .\scripts\mkreadme.ps1              # 重写四份 README 的帮助段
    .\scripts\mkreadme.ps1 -Check       # 只检查是否过期
    .\scripts\mkreadme.ps1 -Exe build\Debug\ecapture.exe
#>
param(
    [string]$Exe,
    [switch]$Check
)

$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
if (-not $Exe) { $Exe = Join-Path $root 'build\ecapture.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { throw "找不到 $Exe，先跑 .\build.ps1" }

# 文件名 => --lang 取值。README.md 是默认英文版，其余按语言后缀命名（与 --lang 的标签同源）。
$targets = [ordered]@{
    'README.md'       = 'en'
    'README.zh-CN.md' = 'zh-CN'
    'README.zh-TW.md' = 'zh-TW'
    'README.ja-JP.md' = 'ja'
}
$beginMark = '<!-- BEGIN ECAPTURE-HELP -->'
$endMark   = '<!-- END ECAPTURE-HELP -->'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Get-HelpText([string]$lang) {
    # --help 写 stdout 且退出码是 3，不是失败；stderr 必须空着
    $lines = & $Exe --help --lang $lang 2>$null
    if ($LASTEXITCODE -ne 3) { throw "--help --lang $lang 退出码应为 3，实测 $LASTEXITCODE（先确认 $Exe 能跑）" }
    if (-not $lines) { throw "--help --lang $lang 没有输出" }
    $text = ($lines -join "`n")
    # 某个 key 没编进资源时 Lang 会回显 "?key"，那样写进 README 就是坏文档
    if ($text -match '\?[a-z][a-z0-9_]*\.') { throw "--help --lang $lang 里出现 ?key 形状，说明该语言文案缺条目" }
    return $text
}

$stale = @()
foreach ($name in $targets.Keys) {
    $lang = $targets[$name]
    $path = Join-Path $root $name
    if (-not (Test-Path -LiteralPath $path)) { throw "缺少 $name" }
    $help = Get-HelpText $lang
    $block = "``````text`n$help`n``````"

    $text = ([System.IO.File]::ReadAllText($path, $utf8NoBom)).Replace("`r`n", "`n")
    $i = $text.IndexOf($beginMark)
    $j = $text.IndexOf($endMark)
    if ($i -lt 0 -or $j -lt 0 -or $j -le $i) { throw "$name 里没找到成对的 $beginMark / $endMark" }
    if ($text.IndexOf($beginMark, $i + 1) -ge 0 -or $text.IndexOf($endMark, $j + 1) -ge 0) { throw "$name 里标记出现了多次" }

    # 标记行之后到 endMark 之前整体重写，标记行自身保持原样
    $new = $beginMark + "`n" + $block + "`n" + $text.Substring($j)
    $merged = $text.Substring(0, $i) + $new
    if ($Check) {
        if ($merged -ne $text) { $stale += $name; Write-Host "  过期  $name（$lang 的帮助段与 --help 输出不一致）" -ForegroundColor Yellow }
        else { Write-Host "  新鲜  $name（$lang，$($help.Split("`n").Count) 行）" -ForegroundColor DarkGray }
    } else {
        [System.IO.File]::WriteAllText($path, $merged, $utf8NoBom)
        Write-Host "  已写  $name（$lang，$((Get-Item $path).Length) 字节）" -ForegroundColor Green
    }
}

if ($Check) {
    if ($stale.Count) { Write-Host "README 帮助段过期：$($stale -join ', ')（跑 .\scripts\mkreadme.ps1 重生成）" -ForegroundColor Red; exit 1 }
    Write-Host "四份 README 的帮助段都与 --help 输出一致" -ForegroundColor Green
    exit 0
}
Write-Host "四份 README 的帮助段已按各自语言的 --help 输出重生成" -ForegroundColor Green
