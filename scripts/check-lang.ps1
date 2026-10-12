<#
.SYNOPSIS
    校验 resources/strings-*.txt 四份语言文案是否对齐。
.DESCRIPTION
    四份文件必须给出一模一样的 key，否则切语言时会静默丢句子。这里查四件事：
      1. 每行必须是 key<TAB>文案 两个字段，且同一文件内 key 不重复；
      2. 四种语言的 key 集合完全一致（缺哪个报哪个）；
      3. 同一个 key 在各语言里用到的占位符（%1 .. %9）必须一致——翻译时把 %2 翻没了，
         就等于把候选窗口列表这类关键信息丢了；
      4. 代码里写到的 key（Msg/Msgf 以及 kOptions 的 "opt.*"）必须在文案里存在。
    另外把构建期生成的 generated\ec_lang_data.cpp 与四份 .txt 逐条对账，
    挡住"改了文案却没重编"（RCDATA 那一路已经换成跨平台嵌入，见 src\LangData.h）。
.EXAMPLE
    .\scripts\check-lang.ps1
    .\scripts\check-lang.ps1 -Generated build\generated\ec_lang_data.cpp
#>
param(
    [string]$Generated
)

$ErrorActionPreference = 'Stop'
$root = (Split-Path -Parent $PSScriptRoot).TrimEnd('\')
if (-not $Generated) { $Generated = Join-Path $root 'build\generated\ec_lang_data.cpp' }
$langs = @('zh-CN', 'zh-TW', 'en', 'ja')
$utf8 = New-Object System.Text.UTF8Encoding($false)

$problems = @()
$notes = @()
$tables = @{}

foreach ($lang in $langs) {
    $path = Join-Path $root "resources\strings-$lang.txt"
    if (-not (Test-Path -LiteralPath $path)) { $problems += "缺少文案文件：resources\strings-$lang.txt"; continue }
    $map = @{}
    $lineNo = 0
    foreach ($line in [System.IO.File]::ReadAllLines($path, $utf8)) {
        $lineNo++
        if ($line -match '^\s*(#|$)') { continue }
        $fields = $line -split "`t"
        if ($fields.Count -ne 2) {
            $problems += "$lang 第 $lineNo 行字段数=$($fields.Count)（应为 key<TAB>文案）：$($fields[0])"
            continue
        }
        if (-not $fields[1].Trim()) {
            # 空文案会被 C++ 侧当成"没有这条"跳过，输出处就变成 ?key
            $problems += "$lang 第 $lineNo 行文案为空：$($fields[0])"
            continue
        }
        if ($map.ContainsKey($fields[0])) {
            $problems += "$lang 第 $lineNo 行 key 重复：$($fields[0])"
            continue
        }
        $map[$fields[0]] = $fields[1]
    }
    $tables[$lang] = $map
}

# key 集合一致性
$union = @{}
foreach ($lang in $langs) {
    if (-not $tables.ContainsKey($lang)) { continue }
    foreach ($key in $tables[$lang].Keys) { $union[$key] = $true }
}
foreach ($key in ($union.Keys | Sort-Object)) {
    $missing = @($langs | Where-Object { $tables.ContainsKey($_) -and -not $tables[$_].ContainsKey($key) })
    if ($missing.Count) { $problems += "key '$key' 缺语言：$($missing -join ', ')" }
}

function Get-Placeholders([string]$text) {
    $found = [regex]::Matches($text, '%[1-9]') | ForEach-Object { $_.Value } | Sort-Object -Unique
    return ($found -join '')
}

foreach ($key in ($union.Keys | Sort-Object)) {
    $sig = @{}
    foreach ($lang in $langs) {
        if ($tables[$lang].ContainsKey($key)) { $sig[$lang] = Get-Placeholders $tables[$lang][$key] }
    }
    if (($sig.Values | Sort-Object -Unique).Count -gt 1) {
        $detail = ($langs | ForEach-Object {
            if ($sig.ContainsKey($_)) { "$_='$($sig[$_])'" } else { "$_=-" }
        }) -join ' '
        $problems += "占位符不一致：'$key' $detail"
    }
}

# 代码里引用的 key 必须存在。只认这两种写法：
#   Msg(L"k") / Msgf(L"k", ...)            —— 诊断与帮助文案
#   L"opt.<选项名>" / L"grp.<组名>"          —— kOptions 与 GroupTitle 里的 key
# 诊断 code（codes::kXxx = L"cli.invalid_number"）不是文案 key，不能一起扫。
$referenced = @{}
$mentioned = @{}
$strictPatterns = @('Msgf?\(\s*L"([a-z][A-Za-z0-9._-]*)"', 'L"(opt\.[A-Za-z0-9._-]+)"', 'L"(grp\.[a-z-]+)"')
$broadPattern = 'L"([a-z][a-z0-9_-]*\.[A-Za-z0-9._-]+)"'
Get-ChildItem -LiteralPath (Join-Path $root 'src') -Include '*.cpp', '*.h' -Recurse | ForEach-Object {
    $text = [System.IO.File]::ReadAllText($_.FullName, $utf8)
    foreach ($pattern in $strictPatterns) {
        foreach ($m in [regex]::Matches($text, $pattern)) { $referenced[$m.Groups[1].Value] = $true }
    }
    # 判"有没有被用到"要放宽：step key 是当参数传给 Fail(...) 的，help.exampleN 在字面量数组里
    foreach ($m in [regex]::Matches($text, $broadPattern)) { $mentioned[$m.Groups[1].Value] = $true }
}
foreach ($key in ($referenced.Keys | Sort-Object)) {
    if (-not $union.ContainsKey($key)) { $problems += "代码引用了不存在的 key：$key" }
}
foreach ($key in ($union.Keys | Sort-Object)) {
    if (-not $mentioned.ContainsKey($key)) { $notes += "文案里没有被代码引用的 key：$key" }
}

# 构建期生成的那份语言数据源文件，与 resources/strings-*.txt 逐条对账：
# 文案四份按原样嵌进 generated\ec_lang_data.cpp（见 scripts\gen-lang-data.cmake），
# 改了 .txt 却没能触发重生成时，这里会当场查出"程序里那份"与"仓库里那份"不一样。
# CMake 的 file(READ) 会把 \r\n 归一成 \n，所以比对之前两边都做同样的归一。
if (Test-Path -LiteralPath $Generated) {
    $text = [System.IO.File]::ReadAllText($Generated, $utf8)
    $open = 'R"ECAP_LANG_DATA('
    $close = ')ECAP_LANG_DATA";'
    $vars = @{ 'zh-CN' = 'kZhCn'; 'zh-TW' = 'kZhTw'; 'en' = 'kEn'; 'ja' = 'kJa' }
    $cursor = 0
    $embedded = @{}
    foreach ($lang in $langs) {
        $decl = $text.IndexOf('static const char ' + $vars[$lang] + '[] = ' + $open, $cursor)
        if ($decl -lt 0) { $problems += "$lang 那份没有嵌进 $Generated（生成脚本被改动过？）"; continue }
        $from = $decl + ('static const char ' + $vars[$lang] + '[] = ' + $open).Length
        $to = $text.IndexOf($close, $from)
        if ($to -lt 0) { $problems += "$lang 那份的嵌入块没有闭合：$Generated"; continue }
        $embedded[$lang] = $text.Substring($from, $to - $from).Replace("`r`n", "`n")
        $cursor = $to + $close.Length
    }
    foreach ($lang in $langs) {
        if (-not $embedded.ContainsKey($lang)) { continue }
        $path = Join-Path $root "resources\strings-$lang.txt"
        if (-not (Test-Path -LiteralPath $path)) { continue }   # 缺文件在前面已经报过
        $source = [System.IO.File]::ReadAllText($path, $utf8).Replace("`r`n", "`n")
        if ($source -ne $embedded[$lang]) {
            $problems += "$lang 的嵌入内容与 resources\strings-$lang.txt 不一致（改完文案要重新构建）"
        } else {
            Write-Host ("  {0,-6} 已嵌入 {1} 字符" -f $lang, $embedded[$lang].Length) -ForegroundColor DarkGray
        }
    }
} else {
    $notes += "找不到 $Generated，跳过嵌入内容对账（先 .\build.ps1 生成一次）"
}

foreach ($n in $notes) { Write-Host "  提示  $n" -ForegroundColor DarkYellow }
if ($problems.Count) {
    foreach ($p in $problems) { Write-Host "  FAIL  $p" -ForegroundColor Red }
    Write-Host ("文案检查失败：{0} 项" -f $problems.Count) -ForegroundColor Red
    exit 1
}
Write-Host ("文案检查通过：{0} 个 key × {1} 种语言" -f $union.Count, $langs.Count) -ForegroundColor Green
