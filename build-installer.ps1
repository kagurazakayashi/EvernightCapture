<#
.SYNOPSIS
    构建 EvernightCapture 的 Windows 安装包：先构建程序，再按载荷清单收集、校验并编译 Inno Setup 安装器。
.DESCRIPTION
    可以从**任意当前工作目录**按绝对路径调用：仓库根由脚本自身位置定位（$PSScriptRoot），不依赖 CWD。

    步骤：
      1. 从 src/Version.h 读版本号（唯一来源，不再维护第二份手写版本）；
      2. 默认调用仓库根的 build.ps1 得到本次正确配置/架构的程序与全部测试程序，失败即停（退出码 11）；
         给 -SkipBuild 时明确跳过重建，但仍会核对来源/配置/架构/身份（buildId），构建失败绝不会退回旧产物；
      3. 在 build\installer-stage\ 下建独立 staging，按 installer\payload.manifest.json 逐条收集载荷
         （glob 条目展开成具体文件），并做内容守卫：不带源码、不带开发用 AGENTS/MEMORY、不带日志与截图；
      4. 校验：必需项齐全、四语 README/SKILL.md/LICENSE/ECAPTURE.EXE 到位、SKILL.md 的 frontmatter 名与
         Skill 目录名一致、声明的安装后离线套件及其依赖都在载荷里；
      5. 生成安装包内的 install-manifest.json（含版本/架构/buildId/逐文件 SHA-256）与 payload.sha256.txt；
      6. 用 Inno Setup 编译器（ISCC.exe）编译 installer\ecapture.iss，产物落到 build\installer\，
         命名带版本与架构，并写出 <安装包>.sha256 以便追溯。

    编译器要求：**Inno Setup 6.3 或更高**（.iss 用到 6.3 引入的 ArchitecturesAllowed=x64compatible）。
    本脚本不自动下载或安装任何工具：找不到 ISCC.exe 时给出诊断并以退出码 32 结束，可用 -IsccPath 显式指定。

    不改 git、不发布、不替换任何已跟踪的发布产物；staging、安装包与哈希都落在 build\（已在 .gitignore 里）。
    含时间戳/构建号的产物不承诺逐字节可重复。

    退出码：0 = 安装包已生成；11 = 构建失败；12 = 载荷收集/校验失败；32 = 找不到 Inno Setup 编译器；
            33 = 编译器执行失败或没有产出安装包。
.EXAMPLE
    .\build-installer.ps1
    .\build-installer.ps1 -Config Release -Clean
    .\build-installer.ps1 -SkipBuild          # 用现有 build\ 产物（会核对身份）
    .\build-installer.ps1 -StageOnly          # 只做构建+载荷 staging+校验，不编译安装包
    .\build-installer.ps1 -IsccPath "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$SkipBuild,
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64',
    [string]$Version,
    [string]$IsccPath,
    [string]$OutputDir,
    [string]$StageDir,
    [switch]$StageOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

$repo = (Get-Item -LiteralPath $PSScriptRoot).FullName
$manifestPath = Join-Path $repo 'installer\payload.manifest.json'
$issPath = Join-Path $repo 'installer\ecapture.iss'
$buildDir = Join-Path $repo 'build'

function Write-Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }

# ---------------------------------------------------------------------------
# 1) 版本号：唯一来源 src/Version.h
# ---------------------------------------------------------------------------
if (-not $Version) {
    $vh = Get-Content -LiteralPath (Join-Path $repo 'src\Version.h') -Raw -Encoding UTF8
    $m = [regex]::Match($vh, '#define\s+ECAPTURE_VERSION_STRING\s+"([^"]+)"')
    if (-not $m.Success) { Write-Host 'src\Version.h 里找不到 ECAPTURE_VERSION_STRING' -ForegroundColor Red; exit 12 }
    $Version = $m.Groups[1].Value
}
Write-Host "EvernightCapture 安装包构建：版本 $Version，架构 $Arch，配置 $Config" -ForegroundColor Cyan
Write-Host "  仓库：$repo"

# ---------------------------------------------------------------------------
# 2) 构建（或显式跳过）
# ---------------------------------------------------------------------------
$exe = Join-Path $buildDir 'ecapture.exe'
if ($SkipBuild) {
    Write-Step "跳过重建（-SkipBuild）：核对现有产物身份"
    if (-not (Test-Path -LiteralPath $exe)) { Write-Host "找不到 $exe；-SkipBuild 也要求先有产物" -ForegroundColor Red; exit 12 }
} else {
    Write-Step "构建（$Config）"
    $buildPs = Join-Path $repo 'build.ps1'
    if (-not (Test-Path -LiteralPath $buildPs)) { Write-Host "找不到 $buildPs" -ForegroundColor Red; exit 11 }
    $bArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildPs, '-Config', $Config)
    if ($Clean) { $bArgs += '-Clean' }
    $shell = (Get-Process -Id $PID).Path
    & $shell @bArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "构建失败（退出码 $LASTEXITCODE），不生成安装包。" -ForegroundColor Red
        exit 11
    }
}
if (-not (Test-Path -LiteralPath $exe)) { Write-Host "构建结束仍找不到 $exe" -ForegroundColor Red; exit 11 }

# --- 产物身份核对：版本 + 架构（PE 头） ---
$verOut = (& $exe --version) 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host "产物跑不了 --version" -ForegroundColor Red; exit 12 }
if ($verOut -notmatch [regex]::Escape($Version)) {
    Write-Host "产物版本与 src\Version.h 不一致：$verOut" -ForegroundColor Red; exit 12
}
function Get-PeMachine([string]$Path) {
    $fs = [IO.File]::OpenRead($Path)
    try {
        $br = New-Object IO.BinaryReader($fs)
        $fs.Position = 0x3C
        $peOffset = $br.ReadInt32()
        $fs.Position = $peOffset
        if ($br.ReadUInt32() -ne 0x00004550) { return 0 }   # 'PE\0\0'
        return $br.ReadUInt16()
    } finally { $fs.Dispose() }
}
$machine = Get-PeMachine $exe
$wantMachine = if ($Arch -eq 'x64') { 0x8664 } else { 0xAA64 }
if ($machine -ne $wantMachine) {
    Write-Host ("产物架构与 -Arch {0} 不一致（PE Machine=0x{1:X4}，期望 0x{2:X4}）" -f $Arch, $machine, $wantMachine) -ForegroundColor Red
    exit 12
}
$capsText = (& $exe --capabilities --lang en) 2>$null
$buildId = $null
try { $buildId = (($capsText | ConvertFrom-Json).program.buildId) } catch { }
Write-Host ("  被测产物：{0}" -f $verOut.Trim()) -ForegroundColor DarkGray
Write-Host ("  buildId：{0}（PE Machine=0x{1:X4}）" -f $buildId, $machine) -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# 3) 载荷 staging
# ---------------------------------------------------------------------------
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if (-not $StageDir) { $StageDir = Join-Path $buildDir ("installer-stage\{0}-{1}" -f $Version, $Arch) }
$payload = Join-Path $StageDir 'payload'
if (Test-Path -LiteralPath $payload) {
    $full = [IO.Path]::GetFullPath($payload)
    if (-not $full.StartsWith([IO.Path]::GetFullPath($buildDir), [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "拒绝清理不在 build\ 下的 staging：$full" -ForegroundColor Red; exit 12
    }
    Remove-Item -LiteralPath $payload -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $payload | Out-Null

Write-Step "收集载荷（按 installer\payload.manifest.json）"
$problems = @()
$staged = New-Object System.Collections.Generic.List[object]
foreach ($e in $manifest.entries) {
    $fromRel = ([string]$e.from) -replace '/', '\'
    $toRel = ([string]$e.to) -replace '/', '\'
    $toIsDir = $toRel.EndsWith('\')
    $collected = @()
    if ($e.glob) {
        $parent = Split-Path -Parent (Join-Path $repo $fromRel)
        $leaf = Split-Path -Leaf $fromRel
        if (Test-Path -LiteralPath $parent) {
            $collected = @(Get-ChildItem -LiteralPath $parent -File -Filter $leaf | Sort-Object Name)
        }
    } else {
        $src = Join-Path $repo $fromRel
        if (Test-Path -LiteralPath $src) { $collected = @(Get-Item -LiteralPath $src) }
    }
    if (-not $collected.Count) {
        if ($e.required) { $problems += "必需载荷没找到：$fromRel（role=$($e.role)）" }
        else { Write-Host ("  - 可选载荷跳过：{0}" -f $fromRel) -ForegroundColor DarkYellow }
        continue
    }
    foreach ($item in $collected) {
        if ($item.Length -eq 0) { $problems += "载荷是 0 字节：$fromRel"; continue }
        $dest = if ($toIsDir) { Join-Path $payload (Join-Path $toRel $item.Name) } else { Join-Path $payload $toRel }
        $destDir = Split-Path -Parent $dest
        if (-not (Test-Path -LiteralPath $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
        Copy-Item -LiteralPath $item.FullName -Destination $dest -Force
        $rel = $dest.Substring($payload.Length).TrimStart('\') -replace '\\', '/'
        $staged.Add([pscustomobject]@{ path = $rel; role = [string]$e.role; freshness = [string]$e.freshness })
    }
    Write-Host ("  + {0} -> {1}（{2} 个文件）" -f $fromRel, $toRel, $collected.Count) -ForegroundColor DarkGray
}

# ---------------------------------------------------------------------------
# 4) 校验
# ---------------------------------------------------------------------------
Write-Step '校验载荷'
foreach ($f in @('ECAPTURE.EXE', 'SKILL.md', 'README.md', 'README.zh-CN.md', 'README.zh-TW.md', 'README.ja-JP.md', 'LICENSE', 'verify-install.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $f))) { $problems += "载荷缺少必需文件：$f" }
}
foreach ($f in @('resources\icon.ico', 'tests\harness.psm1', 'tests\helper\ec_window.cs', 'test-all.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $f))) { $problems += "载荷缺少必需文件：$f" }
}
# 内容守卫：不带源码 / 开发用记忆 / 日志与截图
$forbidden = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object {
    $_.Extension -in @('.cpp', '.h', '.hpp', '.obj', '.pdb', '.log', '.png', '.jpg', '.bmp') -or
    $_.Name -in @('AGENTS.md', 'MEMORY.md', 'USER.md', 'SOUL.md', 'CMakeLists.txt')
})
if ($forbidden.Count) { $problems += "载荷里混进了不该随包的东西：" + (($forbidden | ForEach-Object { $_.FullName.Substring($payload.Length) }) -join ', ') }

# SKILL.md frontmatter 名与目录名一致
$skillName = [string]$manifest.skill.dirName
$skillMd = Get-Content -LiteralPath (Join-Path $payload 'SKILL.md') -Raw -Encoding UTF8
$nameMatch = [regex]::Match($skillMd, '(?m)^name:\s*(\S+)\s*$')
if (-not $nameMatch.Success) { $problems += 'SKILL.md 的 frontmatter 缺少 name:' }
elseif ($nameMatch.Groups[1].Value -ne $skillName) { $problems += "SKILL.md 的 name=$($nameMatch.Groups[1].Value) 与清单里的 Skill 名 $skillName 不一致" }

# 声明的安装后离线套件及其依赖都在载荷里
foreach ($suite in @($manifest.installPlan.offlineTests)) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload ("tests\$suite.ps1")))) { $problems += "声明可跑的离线套件缺少脚本：tests\$suite.ps1" }
}
foreach ($bin in @('ecapture-capabilities-tests.exe', 'ecapture-compat-tests.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload "build\$bin"))) { $problems += "声明的离线套件缺少依赖二进制：build\$bin" }
}
# 本安装包的程序必须来自本次构建
$stagedExe = Join-Path $payload 'ECAPTURE.EXE'
$stagedHash = (Get-FileHash -LiteralPath $stagedExe -Algorithm SHA256).Hash
$buildHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if ($stagedHash -ne $buildHash) { $problems += '载荷里的 ECAPTURE.EXE 与本次构建产物哈希不一致' }

if ($problems.Count) {
    Write-Host '载荷校验失败：' -ForegroundColor Red
    foreach ($p in $problems) { Write-Host "  - $p" -ForegroundColor Red }
    exit 12
}
Write-Host '  载荷校验通过。' -ForegroundColor Green

# ---------------------------------------------------------------------------
# 5) 生成安装包内的清单与哈希
# ---------------------------------------------------------------------------
$fileEntries = @()
foreach ($f in @(Get-ChildItem -LiteralPath $payload -Recurse -File | Sort-Object FullName)) {
    $rel = $f.FullName.Substring($payload.Length).TrimStart('\') -replace '\\', '/'
    $fileEntries += [pscustomobject]@{
        path   = $rel
        size   = $f.Length
        sha256 = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$installManifest = [ordered]@{
    manifestVersion = 1
    product         = [string]$manifest.product
    appId           = [string]$manifest.appId
    version         = $Version
    arch            = $Arch
    config          = $Config
    buildId         = $buildId
    builtAtUtc      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    builtOn         = $env:COMPUTERNAME
    skill           = $manifest.skill
    installPlan     = $manifest.installPlan
    files           = $fileEntries
}
$installManifestPath = Join-Path $payload 'install-manifest.json'
$jsonText = ($installManifest | ConvertTo-Json -Depth 8)
[IO.File]::WriteAllText($installManifestPath, $jsonText, (New-Object Text.UTF8Encoding($false)))

# 逐文件哈希清单（人眼可核，另有一份 JSON 供程序用）
$hashLines = @($fileEntries | ForEach-Object { '{0}  {1}' -f $_.sha256, $_.path })
$hashFile = Join-Path $payload 'payload.sha256.txt'
[IO.File]::WriteAllLines($hashFile, $hashLines, (New-Object Text.ASCIIEncoding))
Write-Host ("  清单：{0}" -f $installManifestPath)
Write-Host ("  哈希：{0}（{1} 个文件）" -f $hashFile, $fileEntries.Count)

# ---------------------------------------------------------------------------
# 6) 编译安装包
# ---------------------------------------------------------------------------
if ($StageOnly) {
    Write-Host ''
    Write-Host '只做 staging（-StageOnly）：未编译安装包（不代表已发布）。' -ForegroundColor Yellow
    Write-Host "  载荷目录：$payload"
    Write-Host "OUTPUT=$(Get-Item -LiteralPath $installManifestPath | Select-Object -ExpandProperty FullName)"
    Write-Host "OUTPUT=$(Get-Item -LiteralPath $hashFile | Select-Object -ExpandProperty FullName)"
    exit 0
}

Write-Step '编译安装包（Inno Setup）'
function Resolve-Iscc([string]$Explicit) {
    if ($Explicit) {
        if (Test-Path -LiteralPath $Explicit) { return (Get-Item -LiteralPath $Explicit).FullName }
        return $null
    }
    $c = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($base in @(${env:ProgramFiles(x86)}, $env:ProgramFiles, $env:LOCALAPPDATA)) {
        if (-not $base) { continue }
        foreach ($sub in @('Inno Setup 6', 'Inno Setup 5')) {
            $cand = Join-Path $base ("$sub\ISCC.exe")
            if (Test-Path -LiteralPath $cand) { return (Get-Item -LiteralPath $cand).FullName }
        }
    }
    return $null
}
$iscc = Resolve-Iscc $IsccPath
if (-not $iscc) {
    Write-Host '找不到 Inno Setup 编译器 ISCC.exe。' -ForegroundColor Red
    Write-Host '  要求：Inno Setup 6.3 或更高（.iss 使用 ArchitecturesAllowed=x64compatible）。' -ForegroundColor Red
    Write-Host '  安装后可用 -IsccPath "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" 指定。' -ForegroundColor Red
    Write-Host '  本脚本不会自动下载或安装任何工具。' -ForegroundColor Red
    exit 32
}
Write-Host ("  编译器：{0}" -f $iscc) -ForegroundColor DarkGray

if (-not $OutputDir) { $OutputDir = Join-Path $buildDir 'installer' }
if (-not (Test-Path -LiteralPath $OutputDir)) { New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null }
$outBase = 'EvernightCapture-{0}-{1}-setup' -f $Version, $Arch

$isccArgs = @(
    "/DSourceDir=$payload",
    "/DAppVersion=$Version",
    "/DArch=$Arch",
    "/DOutputDir=$OutputDir",
    "/DOutputBase=$outBase",
    "/DIconFile=$(Join-Path $repo 'resources\icon.ico')",
    $issPath
)
$psi = New-Object Diagnostics.ProcessStartInfo
$psi.FileName = $iscc
$psi.Arguments = ($isccArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_ + '"' } else { $_ } }) -join ' '
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [Diagnostics.Process]::Start($psi)
$so = $p.StandardOutput.ReadToEnd(); $se = $p.StandardError.ReadToEnd(); $p.WaitForExit(); $code = $p.ExitCode; $p.Dispose()
Write-Host $so
if ($se.Trim()) { Write-Host $se -ForegroundColor DarkYellow }
if ($code -ne 0) { Write-Host "ISCC 退出码 $code：安装包没有编译成功。" -ForegroundColor Red; exit 33 }

$setupExe = Join-Path $OutputDir ("$outBase.exe")
if (-not (Test-Path -LiteralPath $setupExe)) { Write-Host "ISCC 结束但找不到安装包：$setupExe" -ForegroundColor Red; exit 33 }
$setupHash = (Get-FileHash -LiteralPath $setupExe -Algorithm SHA256).Hash.ToLowerInvariant()
$shaFile = "$setupExe.sha256"
Set-Content -LiteralPath $shaFile -Value ("{0}  {1}" -f $setupHash, (Split-Path -Leaf $setupExe)) -Encoding ASCII

Write-Host ''
Write-Host '安装包已生成：' -ForegroundColor Green
Write-Host ("  路径：{0}" -f $setupExe)
Write-Host ("  版本：{0}   架构：{1}   buildId：{2}" -f $Version, $Arch, $buildId)
Write-Host ("  SHA-256：{0}" -f $setupHash)
Write-Host ("  哈希文件：{0}" -f $shaFile)
Write-Host '  注意：这只表示安装包已构建，不代表已发布，也不代表已在真实用户目录安装过。' -ForegroundColor DarkGray
Write-Host "OUTPUT=$setupExe"
Write-Host "OUTPUT=$shaFile"
exit 0
