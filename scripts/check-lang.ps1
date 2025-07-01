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
    另外用 Get-Resource 那段确认 exe 里真的编进了四份资源（改完 .txt 忘记重编时会挡住）。
.EXAMPLE
    .\scripts\check-lang.ps1
    .\scripts\check-lang.ps1 -Exe build\Debug\ecapture.exe
#>
param(
    [string]$Exe
)

$ErrorActionPreference = 'Stop'
$root = (Split-Path -Parent $PSScriptRoot).TrimEnd('\')
if (-not $Exe) { $Exe = Join-Path $root 'build\ecapture.exe' }
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

# exe 里是否真有四份资源：用 FindResource 逐个确认（改完 .txt 没重编时会暴露）。
# RT_RCDATA 是序号 10，所以 type 用 IntPtr 传，传字符串 "10" 找不到。
if (Test-Path -LiteralPath $Exe) {
    if (-not ('ResourceProbe' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ResourceProbe {
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryEx(string file, IntPtr hFile, uint flags);
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr FindResourceW(IntPtr module, string name, IntPtr type);
    [DllImport("kernel32", SetLastError = true)]
    static extern uint SizeofResource(IntPtr module, IntPtr resInfo);
    public static long Size(string file, string name) {
        IntPtr mod = LoadLibraryEx(file, IntPtr.Zero, 0x2 /* LOAD_LIBRARY_AS_DATAFILE */);
        if (mod == IntPtr.Zero) { return -1; }
        IntPtr info = FindResourceW(mod, name, new IntPtr(10) /* RT_RCDATA */);
        if (info == IntPtr.Zero) { return 0; }
        return SizeofResource(mod, info);
    }
}
'@
    }
    $names = @{ 'zh-CN' = 'ECP_TEXT_ZH_CN'; 'zh-TW' = 'ECP_TEXT_ZH_TW'; 'en' = 'ECP_TEXT_EN'; 'ja' = 'ECP_TEXT_JA' }
    foreach ($lang in $langs) {
        $size = [ResourceProbe]::Size($Exe, $names[$lang])
        if ($size -le 0) {
            $problems += "exe 里没有（或为空）$($names[$lang]) 这份资源：$Exe（改完文案要重新构建）"
        } else {
            Write-Host ("  {0,-6} {1,-14} {2} 字节" -f $lang, $names[$lang], $size) -ForegroundColor DarkGray
        }
    }
} else {
    $notes += "找不到 $Exe，跳过资源存在性检查"
}

foreach ($n in $notes) { Write-Host "  提示  $n" -ForegroundColor DarkYellow }
if ($problems.Count) {
    foreach ($p in $problems) { Write-Host "  FAIL  $p" -ForegroundColor Red }
    Write-Host ("文案检查失败：{0} 项" -f $problems.Count) -ForegroundColor Red
    exit 1
}
Write-Host ("文案检查通过：{0} 个 key × {1} 种语言" -f $union.Count, $langs.Count) -ForegroundColor Green
