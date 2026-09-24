<#
.SYNOPSIS
    构建 EvernightCapture 的 Windows 安装包：先构建程序，再按载荷清单收集、校验并编译 Inno Setup 安装器。
.DESCRIPTION
    可以从**任意当前工作目录**按绝对路径调用：仓库根由脚本自身位置定位（$PSScriptRoot），不依赖 CWD。

    步骤：
      1. 从 src/Version.h 读版本号（唯一来源，不再维护第二份手写版本）；
      2. 默认调用仓库根的 build.ps1 得到本次正确配置/架构的程序与全部测试程序，失败即停（退出码 11）；
         给 -SkipBuild 时明确复用现有产物，但仍要核对身份与"构建输入是否比产物新"（见第 5 步）；
      3. 在任何创建 / 复制 / 清理之前先判定 staging 目录的归属：只接受仓库 build\installer-stage 下的
         直接子目录，且已存在的目录必须带着本工具写下的 ownership 标记；判不出归属就保留原文件并报可操作错误；
      4. 按 installer\payload.manifest.json 逐条收集载荷，并做内容守卫：不带源码、不带开发用 AGENTS/MEMORY、
         不带日志与截图；
      5. 校验：必需项齐全、四语 README/SKILL.md/LICENSE/ECAPTURE.EXE 到位、SKILL.md 的 frontmatter 名与
         Skill 目录名一致、**清单声明的安装后离线套件的依赖闭包全都在载荷里**（少一个就失败，不当可选）、
         载荷里的每个 EXE 架构一致、ECAPTURE.EXE 与本次构建产物哈希一致；
      6. 产物身份核对不再看文本子串：--version / --capabilities / --diagnostics 三条查询都走测试共享调用器
         （并发消费两条流、有期限等待、超时只结束本次启动的进程树），要求版本 / 架构 / buildId 三个身份字段
         齐全、互相自洽，并与磁盘上这份 EXE 的 PE 头（Machine、链接时间戳）逐字节对得上；
      7. 生成 provenance（本次重建 / 显式复用、配置来自 CMakeCache 的核对结果、git 提交、EXE SHA-256、
         链接时间戳）并写进入包清单 install-manifest.json 与安装包旁的 <安装包>.provenance.json；
      8. 用 Inno Setup 编译器（ISCC.exe）编译 installer\ecapture.iss —— 这同样走共享调用器，超时不会卡在
         管道上；产物落到 build\installer\，命名带版本与架构，并写出 <安装包>.sha256 与 provenance 旁文件。
         编译前还会把"这次铺下去的受管相对路径清单"生成到 staging 目录并用 /DManagedList 传给 ISCC：
         安装器在复制文件之前就要知道哪些文件归自己管，而那时 {app} 里还没有载荷，Inno 6.7.1 也没有逐文件
         覆盖前的钩子，所以这份清单只能编译期烘进去（缺它时 .iss 自己 #error 拒绝编译）。
         载荷清单的 appId 与 .iss 的 [Setup] AppId / AppIdGuid 三处必须是同一个 GUID，不一致就拒绝打包。

    预算：-IdentityTimeoutSec（每条只读查询，默认 120s）与 -IsccTimeoutSec（编译器，默认 900s）都在
    合法区间之外直接被拒绝；毫秒换算走 [long] 再夹到 [int]，不会溢出成负数而一上来就"超时"。

    架构：本仓库的构建链只调用 vcvars64.bat（build.ps1 里写死 x64 工具链），所以这里只接受 -Arch x64。
    arm64 从来没有真正的构建路径，留着参数只会让包名与清单自称 arm64 而内容是 x64，因此在入口直接拒绝。
    Windows on ARM 的 x64 模拟能不能跑属于未实测的兼容性，不在这里当作 ARM64 原生发行承诺。

    运行库：ECAPTURE.EXE 与全部测试程序都是 CRT 静态链接（CMakeLists.txt 里 MSVC_RUNTIME_LIBRARY=MultiThreaded），
    所以安装包不带 VC++ 运行库，也不带 Visual Studio、源码或网络依赖。

    编译器要求：**Inno Setup 6.3 或更高**（.iss 用到 6.3 引入的 ArchitecturesAllowed=x64compatible）。
    本脚本不自动下载或安装任何工具：找不到 ISCC.exe 时给出诊断并以退出码 32 结束，可用 -IsccPath 显式指定。

    不改 git、不发布、不替换任何已跟踪的发布产物；staging、安装包、日志与哈希都落在 build\（已在 .gitignore 里）。
    含时间戳/构建号的产物不承诺逐字节可重复。payload.sha256.txt 与 <安装包>.sha256 是本工具自算的校验和，
    用于追溯与自检核对，不是发布者签名或认证。

    退出码：0 = 安装包已生成；11 = 构建失败；12 = 载荷/身份/来源收集校验失败；32 = 找不到 Inno Setup 编译器；
            33 = 编译器执行失败或没有产出安装包。
.EXAMPLE
    .\build-installer.ps1
    .\build-installer.ps1 -Config Release -Clean
    .\build-installer.ps1 -SkipBuild          # 复用现有 build\ 产物（仍核对身份与构建输入新鲜度）
    .\build-installer.ps1 -StageOnly          # 只做构建+载荷 staging+校验，不编译安装包
    .\build-installer.ps1 -IsccPath "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$SkipBuild,
    [string]$Arch = 'x64',
    [string]$Version,
    [string]$IsccPath,
    [string]$OutputDir,
    [string]$StageDir,
    [switch]$StageOnly,
    [int]$IdentityTimeoutSec = 120,
    [int]$IsccTimeoutSec = 900
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

$repo = (Get-Item -LiteralPath $PSScriptRoot).FullName
$packagingModule = Join-Path $repo 'installer\packaging.psm1'
$manifestPath = Join-Path $repo 'installer\payload.manifest.json'
$issPath = Join-Path $repo 'installer\ecapture.iss'
$buildDir = Join-Path $repo 'build'

function Write-Step([string]$Text) { Write-Host "`n=== $Text ===" -ForegroundColor Cyan }

# 配置与超时值在动手之前先验一遍：非法值不该等到跑了一半才暴露。
if ($IdentityTimeoutSec -lt 5 -or $IdentityTimeoutSec -gt 3600) { Write-Host '-IdentityTimeoutSec 必须在 5..3600 秒之间' -ForegroundColor Red; exit 12 }
if ($IsccTimeoutSec -lt 30 -or $IsccTimeoutSec -gt 7200) { Write-Host '-IsccTimeoutSec 必须在 30..7200 秒之间' -ForegroundColor Red; exit 12 }

if (-not (Test-Path -LiteralPath $packagingModule)) { Write-Host "找不到打包共享构件：$packagingModule" -ForegroundColor Red; exit 12 }
Import-Module $packagingModule -Force -DisableNameChecking

# ---------------------------------------------------------------------------
# 0) 架构：只保留真实存在的 x64 构建链
# ---------------------------------------------------------------------------
if ($Arch -ne 'x64') {
    Write-Host ("不接受的架构请求：-Arch {0}" -f $Arch) -ForegroundColor Red
    Write-Host '  本仓库的构建脚本只调用 vcvars64.bat（x64 工具链），没有 ARM64 构建路径；' -ForegroundColor Red
    Write-Host '  继续打包只会得到"文件名与清单自称 ARM64、里面却是 x64"的包，所以在这里拒绝。' -ForegroundColor Red
    Write-Host '  需要 ARM64 原生发行，先补上 ARM64 工具链、全部测试产物与安装器架构限制，再放开这个入口。' -ForegroundColor Red
    exit 12
}

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
# 2) staging 目录归属判定：在创建 / 复制 / 清理之前，首次使用与重用同一套规则
# ---------------------------------------------------------------------------
if (-not $StageDir) { $StageDir = Join-Path $buildDir ("installer-stage\{0}-{1}" -f $Version, $Arch) }
$stageVerdict = Test-EcStagingAllowed -StageDir $StageDir -BuildDir $buildDir -RepoRoot $repo
if (-not $stageVerdict.Allowed) {
    Write-Host ("staging 目录不合格：{0}" -f $stageVerdict.Reason) -ForegroundColor Red
    Write-Host '  本工具不会删除自己证明不了归属的目录，也不会"先删掉看看"。' -ForegroundColor Red
    Write-Host "  不给 -StageDir 时用的是受管默认位置：$buildDir\installer-stage\<版本>-<架构>" -ForegroundColor DarkGray
    exit 12
}
$StageDir = $stageVerdict.Full
$payload = Join-Path $StageDir 'payload'

# 载荷清单在读到之后就只有一份来源：离线依赖闭包与"哪些产物属于本次构建图"都由它决定。
$manifest = Get-EcPayloadManifest -Path $manifestPath
# 声明为 current-build 的条目 = 本次构建图应当交出来的产物；复用模式下逐个核新鲜度。
$buildGraphNames = @($manifest.entries | Where-Object { $_.freshness -eq 'current-build' } |
                    ForEach-Object { Split-Path -Leaf (([string]$_.from) -replace '/', '\') })

# ---------------------------------------------------------------------------
# 3) 日志目录：出问题的现场要留得下来
# ---------------------------------------------------------------------------
$logDir = Join-Path $buildDir ('installer-logs\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
Write-Host ("  日志：{0}" -f $logDir) -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# 4) 构建（或显式复用现有产物）
# ---------------------------------------------------------------------------
$exe = Join-Path $buildDir 'ecapture.exe'
$sourceKind = 'rebuilt'
if ($SkipBuild) {
    $sourceKind = 'reused'
    Write-Step "跳过重建（-SkipBuild）：复用现有产物，先核对身份与新鲜度"
    if (-not (Test-Path -LiteralPath $exe)) { Write-Host "找不到 $exe；-SkipBuild 也要求先有产物" -ForegroundColor Red; exit 12 }
} else {
    Write-Step "构建（$Config）"
    $buildPs = Join-Path $repo 'build.ps1'
    if (-not (Test-Path -LiteralPath $buildPs)) { Write-Host "找不到 $buildPs" -ForegroundColor Red; exit 11 }
    $bArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildPs, '-Config', $Config)
    if ($Clean) { $bArgs += '-Clean' }
    $shell = (Get-Process -Id $PID).Path
    # 构建日志直接进当前控制台（不重定向），所以这里不存在"两条流互相等死"的问题；
    # 构建失败绝不退回旧产物，直接停。
    & $shell @bArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "构建失败（退出码 $LASTEXITCODE），不生成安装包。" -ForegroundColor Red
        exit 11
    }
}
if (-not (Test-Path -LiteralPath $exe)) { Write-Host "构建结束仍找不到 $exe" -ForegroundColor Red; exit 11 }

# ---------------------------------------------------------------------------
# 5) 构建元数据与配置核对：配置来自构建目录里的 CMakeCache，不来自命令行请求
# ---------------------------------------------------------------------------
$meta = Get-EcBuildMetadata -BuildDir $buildDir
$configVerified = $false
$configFromCache = [string]$meta.Config
if (-not $meta.CacheExists) {
    Write-Host ("构建目录里没有 CMakeCache.txt（{0}），无法证明这批产物是 {1} 配置。" -f $buildDir, $Config) -ForegroundColor Red
    Write-Host '  这里拒绝把命令行请求当成已核实的事实：请先用 .\build.ps1 -Config ' -ForegroundColor Red
    Write-Host ("  {0} 构建（或删掉 build\ 后重建），再打包。" -f $Config) -ForegroundColor Red
    exit 12
}
if ($configFromCache -ne $Config) {
    Write-Host ("构建目录当前的配置是 {0}，与请求的 -Config {1} 不一致：这批产物不能标成 {1}。" -f $configFromCache, $Config) -ForegroundColor Red
    Write-Host '  要么按构建目录的实际配置打包，要么重新构建；这里不猜。' -ForegroundColor Red
    exit 12
}
$configVerified = $true

# -SkipBuild 的额外新鲜度核对：先问构建系统自己（ninja -n 读依赖图、不编译）。
# 时间戳并集口径会误判：ninja 是按需重链的，改一个 tests\x_state.cpp 之后其余产物的链接时间戳
# 本来就不会变，所以只有在问不出答案时（不是 Ninja、ninja 跑不起来）才退回保守的时间戳比对。
$freshness = 'not-applicable-rebuilt'
if ($SkipBuild) {
    $graph = Get-EcBuildGraphStatus -BuildDir $buildDir -NinjaPath ([string]$meta.MakeProgram) `
                                    -Generator ([string]$meta.Generator) -TimeoutMs 60000
    if ($graph.Known -and -not $graph.UpToDate) {
        Write-Host '构建系统认为这批产物落后于当前源码（ninja -n 有活要干），不能当成当前源码的结果打包：' -ForegroundColor Red
        foreach ($line in @(($graph.Reason -split "`r?`n" | Where-Object { $_ } | Select-Object -First 8))) {
            Write-Host "  | $line" -ForegroundColor Red
        }
        Write-Host '  请去掉 -SkipBuild 重新构建（ninja 只重编改过的那部分）。' -ForegroundColor Red
        exit 12
    } elseif ($graph.Known) {
        $freshness = 'build-graph-clean'
    } else {
        Write-Host ("  问不出构建图状态（{0}），退回保守的时间戳比对。" -f $graph.Reason) -ForegroundColor DarkYellow
        $newest = Get-EcNewestSourceWrite -RepoRoot $repo -IncludeTestSources
        if ($newest) {
            $stale = @(Get-EcStaleBinaries -BuildDir $buildDir -NewestInputUtc $newest -Names $buildGraphNames)
            if ($stale.Count) {
                Write-Host '以下要随包的产物比构建输入旧，且构建图问不出答案，不能当成当前源码的结果打包：' -ForegroundColor Red
                Write-Host ("  最新构建输入：{0}（UTC）" -f $newest.ToString('yyyy-MM-dd HH:mm:ss')) -ForegroundColor Red
                foreach ($s in $stale) { Write-Host ("  - {0}：{1}" -f $s.Name, $s.Reason) -ForegroundColor Red }
                Write-Host '  请去掉 -SkipBuild 重新构建。' -ForegroundColor Red
                exit 12
            }
        }
        $freshness = 'timestamps-union-fallback-clean'
    }
}

$gitInfo = $null
# 在工作树根里问 git：否则继承的是调用方的当前目录，换个目录调用就会拿到别的仓库的答案。
$gitOut = Invoke-EcProcess -FilePath 'git.exe' -Arguments @('rev-parse', '--short', 'HEAD') -TimeoutMs 15000 -WorkingDirectory $repo
if (-not $gitOut.StartError -and $gitOut.Exit -eq 0) {
    $dirtyOut = Invoke-EcProcess -FilePath 'git.exe' -Arguments @('status', '--porcelain') -TimeoutMs 30000 -WorkingDirectory $repo
    $dirty = [bool]($dirtyOut.Exit -eq 0 -and $dirtyOut.Stdout.Trim())
    $gitInfo = [pscustomobject]@{ Commit = $gitOut.Stdout.Trim(); Dirty = $dirty }
}

# ---------------------------------------------------------------------------
# 6) 产物身份：三条只读查询 + PE 头互校（起不来 / 超时 / 非零退出 / JSON 解不动都算失败）
# ---------------------------------------------------------------------------
Write-Step '核对产物身份'
$identityMs = $IdentityTimeoutSec * 1000
$identity = Test-EcArtifactIdentity -Exe $exe -ExpectedVersion $Version -ExpectedArch $Arch -TimeoutMs $identityMs
if ($identity.VersionText) { Write-Host ("  被测产物：{0}" -f $identity.VersionText.Trim()) -ForegroundColor DarkGray }
if ($identity.Pe -and $identity.Pe.Known) {
    Write-Host ("  PE：Machine=0x{0:X4} 链接时间戳=0x{1:X8}" -f $identity.Pe.Machine, $identity.Pe.TimeDateStamp) -ForegroundColor DarkGray
}
Write-Host ("  buildId：{0}" -f $(if ($identity.BuildId) { $identity.BuildId } else { '（没拿到）' })) -ForegroundColor DarkGray
if (-not $identity.Ok) {
    Write-Host '产物身份核对不通过，不生成安装包：' -ForegroundColor Red
    foreach ($p in @($identity.Problems)) { Write-Host "  - $p" -ForegroundColor Red }
    exit 12
}
$buildId = [string]$identity.BuildId

# ---------------------------------------------------------------------------
# 7) 载荷 staging（到这里才允许动目录）
# ---------------------------------------------------------------------------
Write-Step "收集载荷（按 installer\payload.manifest.json）"
if ($stageVerdict.Exists) {
    $cleanup = Remove-EcManagedStaging -StageDir $StageDir -BuildDir $buildDir -RepoRoot $repo
    if (-not $cleanup.Removed) {
        Write-Host ("清理旧 staging 被拒绝：{0}" -f $cleanup.Reason) -ForegroundColor Red
        Write-Host '  原有文件一律保留，不"先删掉看看"。' -ForegroundColor Red
        exit 12
    }
}
New-Item -ItemType Directory -Force -Path $payload | Out-Null
[void](Write-EcStagingMarker -StageDir $StageDir -RepoRoot $repo -PayloadDir $payload -Version $Version -Arch $Arch)

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
# 8) 校验
# ---------------------------------------------------------------------------
Write-Step '校验载荷'
foreach ($f in @('ECAPTURE.EXE', 'SKILL.md', 'README.md', 'README.zh-CN.md', 'README.zh-TW.md', 'README.ja-JP.md', 'LICENSE', 'verify-install.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $f))) { $problems += "载荷缺少必需文件：$f" }
}
foreach ($f in @('resources\icon.ico', 'tests\harness.psm1', 'tests\helper\ec_window.cs', 'test-all.ps1')) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $f))) { $problems += "载荷缺少必需文件：$f" }
}

# 离线依赖闭包：清单声明"安装后可跑"的每一套，脚本与测试程序都必须真的在包里。
# 少一个就是打包失败 —— 不当可选 glob，不降级成安装后的 SKIP，也不留"装完再编一次"这条路。
$closure = Get-EcOfflineClosure -Manifest $manifest
foreach ($suite in @($manifest.installPlan.offlineTests)) {
    $deps = $closure.PerSuite[[string]$suite]
    foreach ($rel in @($deps.Scripts)) {
        $p = Join-Path $payload (($rel -replace '/', '\'))
        if (-not (Test-Path -LiteralPath $p)) { $problems += "离线套件 [$suite] 缺少脚本：$rel" }
    }
    foreach ($rel in @($deps.Binaries)) {
        $p = Join-Path $payload (($rel -replace '/', '\'))
        if (-not (Test-Path -LiteralPath $p)) { $problems += "离线套件 [$suite] 缺少依赖二进制：$rel（打包阶段失败，不留到安装后再编译）" }
    }
    foreach ($rel in @($deps.Aux)) {
        $p = Join-Path $payload (($rel -replace '/', '\'))
        if (-not (Test-Path -LiteralPath $p)) { $problems += "离线套件 [$suite] 缺少辅助文件：$rel" }
    }
}

# 每个 EXE 的架构都要与包声明一致（x64）；测试程序也是安装版要跑的产物。
foreach ($f in @(Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object { $_.Extension -eq '.exe' })) {
    $pe = Get-EcPeIdentity -Path $f.FullName
    if (-not $pe.Known) { $problems += "读不出 PE 头：$($f.Name)（$($pe.Error)）"; continue }
    if ($pe.Machine -ne 0x8664) { $problems += ("{0} 的 PE Machine=0x{1:X4}，不是 x64" -f $f.Name, $pe.Machine) }
}

# 内容守卫：不带源码 / 开发用记忆 / 日志与截图 / 打包侧构件
$forbidden = @(Get-ChildItem -LiteralPath $payload -Recurse -File | Where-Object {
    $_.Extension -in @('.cpp', '.h', '.hpp', '.obj', '.pdb', '.log', '.png', '.jpg', '.bmp') -or
    $_.Name -in @('AGENTS.md', 'MEMORY.md', 'USER.md', 'SOUL.md', 'CMakeLists.txt', 'build.ps1', 'clean.ps1', 'run.ps1', 'packaging.psm1', 'payload.manifest.json', 'ecapture.iss')
})
if ($forbidden.Count) { $problems += "载荷里混进了不该随包的东西：" + (($forbidden | ForEach-Object { $_.FullName.Substring($payload.Length) }) -join ', ') }

# SKILL.md frontmatter 名与目录名一致
$skillName = [string]$manifest.skill.dirName
$skillMd = Get-Content -LiteralPath (Join-Path $payload 'SKILL.md') -Raw -Encoding UTF8
$nameMatch = [regex]::Match($skillMd, '(?m)^name:\s*(\S+)\s*$')
if (-not $nameMatch.Success) { $problems += 'SKILL.md 的 frontmatter 缺少 name:' }
elseif ($nameMatch.Groups[1].Value -ne $skillName) { $problems += "SKILL.md 的 name=$($nameMatch.Groups[1].Value) 与清单里的 Skill 名 $skillName 不一致" }

# 本安装包的程序必须来自本次这份产物
$stagedExe = Join-Path $payload 'ECAPTURE.EXE'
$stagedHash = (Get-FileHash -LiteralPath $stagedExe -Algorithm SHA256).Hash.ToLowerInvariant()
$buildHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($stagedHash -ne $buildHash) { $problems += '载荷里的 ECAPTURE.EXE 与本次构建产物哈希不一致' }

if ($problems.Count) {
    Write-Host '载荷校验失败：' -ForegroundColor Red
    foreach ($p in $problems) { Write-Host "  - $p" -ForegroundColor Red }
    Write-Host "  日志目录：$logDir" -ForegroundColor DarkGray
    exit 12
}
Write-Host '  载荷校验通过。' -ForegroundColor Green

# ---------------------------------------------------------------------------
# 9) 生成安装包内的清单、provenance 与哈希
# ---------------------------------------------------------------------------
$provenance = [ordered]@{
    source            = $sourceKind          # rebuilt = 本次重建；reused = 显式复用现有产物
    sourceNote        = $(if ($sourceKind -eq 'reused') {
                            '-SkipBuild：本次没有重建，包里的产物来自 build\ 里已有的构建结果；新鲜度按"构建输入不晚于链接时间戳"核对过，这不等于逐字节证明它来自当前源码。'
                         } else {
                            '本次由 build.ps1 重新构建后打包。'
                         })
    freshnessCheck    = $freshness
    config            = $Config
    configVerified    = $configVerified
    configSource      = "CMakeCache.txt CMAKE_BUILD_TYPE=$configFromCache"
    generator         = [string]$meta.Generator
    arch              = $Arch
    archEvidence      = $(if ($identity.Pe) { ('PE Machine=0x{0:X4}' -f $identity.Pe.Machine) } else { 'unknown' })
    buildId           = $buildId
    linkTimestampHex  = $(if ($identity.Pe) { ('{0:X8}' -f $identity.Pe.TimeDateStamp) } else { 'unknown' })
    exeSha256         = $buildHash
    gitCommit         = $(if ($gitInfo) { $gitInfo.Commit } else { 'unknown' })
    gitDirty          = $(if ($gitInfo) { $gitInfo.Dirty; } else { $null })
    builtAtUtc        = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    builtOn           = $env:COMPUTERNAME
    runtime           = 'CRT 静态链接（CMakeLists.txt MSVC_RUNTIME_LIBRARY=MultiThreaded），不需要 VC++ 运行库'
    hashesAre         = '本工具自算的 SHA-256，用于追溯与安装自检，不是发布者签名或认证'
}

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
    ownershipMarker = [string]$manifest.ownershipMarkerFile
    version         = $Version
    arch            = $Arch
    config          = $Config
    buildId         = $buildId
    provenance      = $provenance
    skill           = $manifest.skill
    installPlan     = $manifest.installPlan
    files           = $fileEntries
}
$installManifestPath = Join-Path $payload 'install-manifest.json'
$jsonText = ($installManifest | ConvertTo-Json -Depth 10)
[IO.File]::WriteAllText($installManifestPath, $jsonText, (New-Object Text.UTF8Encoding($false)))
$provenancePath = Join-Path $logDir 'provenance.json'
[IO.File]::WriteAllText($provenancePath, ($provenance | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))

# 逐文件哈希清单（人眼可核，另有一份 JSON 供程序用；不是发布者认证）
$hashLines = @($fileEntries | ForEach-Object { '{0}  {1}' -f $_.sha256, $_.path })
$hashFile = Join-Path $payload 'payload.sha256.txt'
[IO.File]::WriteAllLines($hashFile, $hashLines, (New-Object Text.ASCIIEncoding))
Write-Host ("  清单：{0}" -f $installManifestPath)
Write-Host ("  哈希：{0}（{1} 个文件）" -f $hashFile, $fileEntries.Count)
Write-Host ("  provenance：{0}" -f $provenancePath)

# ---------------------------------------------------------------------------
# 9b) 受管路径清单：安装器要靠它在覆盖前认出同名文件。
#     PrepareToInstall 在所有文件复制**之前**执行，那时 {app} 里还没有载荷，随包文件读不到，
#     而本机实测 Inno 6.7.1 已经没有 OnFileCopy 这类逐文件钩子 —— 所以这份清单必须编译期烘进安装器。
# ---------------------------------------------------------------------------
$managedRelPaths = @($fileEntries | ForEach-Object { [string]$_.path })
$managedRelPaths += @('install-manifest.json', 'payload.sha256.txt')
$managedRelPaths = @($managedRelPaths | Sort-Object -Unique | Where-Object { $_ })
# 只允许能安全写进 Pascal 字符串字面量与归属标记记录行的路径：
# 引号、反斜杠、竖线（标记的分隔符）与非可打印 ASCII 一律拒绝，不猜转义写法。
$badPaths = @($managedRelPaths | Where-Object { $_ -match "[`"'\\|]" -or (($_ -replace '[\x20-\x7E]', '') -ne '') })
if ($badPaths.Count) {
    Write-Host '受管路径里有安装器无法安全表达的名字，拒绝打包（不猜转义写法）：' -ForegroundColor Red
    foreach ($b in $badPaths) { Write-Host "  - $b" -ForegroundColor Red }
    exit 12
}
$appIdRaw = [string]$manifest.appId
if ($appIdRaw -notmatch '^\{\{([0-9A-Fa-f\-]{20,40})\}\}?$') {
    Write-Host "载荷清单里的 appId 不是 {{{{GUID}}}} 这种 Inno 转义形状：$appIdRaw" -ForegroundColor Red
    exit 12
}
$appIdGuid = $Matches[1]
$appIdPlain = '{' + $appIdGuid + '}'
$issText = Get-Content -LiteralPath $issPath -Raw -Encoding UTF8
# manifest / .iss 的 [Setup] AppId / .iss 里给归属标记用的 AppIdGuid 三处必须是同一个 GUID，
# 不一致会让"升级时认出这是自己的目录"这件事悄悄失效。
if ($issText -notmatch ('AppId=\{\{' + [regex]::Escape($appIdGuid) + '\}')) {
    Write-Host "installer\ecapture.iss 的 [Setup] AppId 与载荷清单的 appId 不是同一个 GUID（$appIdGuid），拒绝打包。" -ForegroundColor Red
    exit 12
}
if ($issText -notmatch ('#define AppIdGuid "\{' + [regex]::Escape($appIdGuid) + '\}"')) {
    Write-Host "installer\ecapture.iss 里的 AppIdGuid 默认值与载荷清单的 appId 不一致（$appIdGuid），拒绝打包。" -ForegroundColor Red
    exit 12
}
$managedLines = @('  SetArrayLength(ManagedPaths, {0});' -f $managedRelPaths.Count)
$i = 0
foreach ($rel in $managedRelPaths) {
    $managedLines += ('  ManagedPaths[{0}] := ''{1}'';' -f $i, $rel)
    $i++
}
$managedListPath = Join-Path $StageDir 'managed-list.pinc'
[IO.File]::WriteAllLines($managedListPath, $managedLines, (New-Object Text.ASCIIEncoding))
Write-Host ("  受管路径清单：{0}（{1} 条）" -f $managedListPath, $managedRelPaths.Count) -ForegroundColor DarkGray

# ---------------------------------------------------------------------------
# 10) 编译安装包
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
    ("/DAppIdGuid={0}" -f $appIdPlain),
    "/DManagedList=$managedListPath",
    "/DIconFile=$(Join-Path $repo 'resources\icon.ico')",
    $issPath
)
# 走共享调用器：从进程启动那一刻起就在期限内同时消费两条流，超时只结束本次启动的这一棵，
# 并且保留已经拿到的部分输出（老写法先 ReadToEnd 再判超时，挂住时既拿不到日志也等不到头）。
$isccMs = $IsccTimeoutSec * 1000
if ($isccMs -lt 1 -or $isccMs -gt [int]::MaxValue) { Write-Host '-IsccTimeoutSec 换算成毫秒后超出可用范围' -ForegroundColor Red; exit 12 }
$isccResult = Invoke-EcProcess -FilePath $iscc -Arguments $isccArgs -TimeoutMs $isccMs
[IO.File]::WriteAllBytes((Join-Path $logDir 'iscc.out.txt'), $isccResult.StdoutBytes)
[IO.File]::WriteAllBytes((Join-Path $logDir 'iscc.err.txt'), $isccResult.StderrBytes)
Write-Host $isccResult.Stdout
if ($isccResult.Stderr.Trim()) { Write-Host $isccResult.Stderr -ForegroundColor DarkYellow }

if ($isccResult.StartError) {
    Write-Host ("ISCC 没能启动：{0}（日志：{1}）" -f $isccResult.StartError, $logDir) -ForegroundColor Red
    exit 33
}
if ($isccResult.TimedOut) {
    Write-Host ("ISCC 在 {0}s 内没有退出：安装包没有编译成功（已结束本次启动的编译进程树；日志：{1}）" -f $IsccTimeoutSec, $logDir) -ForegroundColor Red
    exit 33
}
if ($isccResult.Exit -ne 0) {
    Write-Host ("ISCC 退出码 {0}：安装包没有编译成功（日志：{1}）" -f $isccResult.Exit, $logDir) -ForegroundColor Red
    exit 33
}

$setupExe = Join-Path $OutputDir ("$outBase.exe")
if (-not (Test-Path -LiteralPath $setupExe)) { Write-Host "ISCC 结束但找不到安装包：$setupExe" -ForegroundColor Red; exit 33 }
$setupHash = (Get-FileHash -LiteralPath $setupExe -Algorithm SHA256).Hash.ToLowerInvariant()
$shaFile = "$setupExe.sha256"
Set-Content -LiteralPath $shaFile -Value ("{0}  {1}" -f $setupHash, (Split-Path -Leaf $setupExe)) -Encoding ASCII
$setupProvenance = Join-Path $OutputDir ("{0}.provenance.json" -f $outBase)
Copy-Item -LiteralPath $provenancePath -Destination $setupProvenance -Force

Write-Host ''
Write-Host '安装包已生成：' -ForegroundColor Green
Write-Host ("  路径：{0}" -f $setupExe)
Write-Host ("  版本：{0}   架构：{1}   buildId：{2}" -f $Version, $Arch, $buildId)
Write-Host ("  来源：{0}（配置 {1}，{2}）" -f $sourceKind, $Config, $freshness)
Write-Host ("  SHA-256：{0}" -f $setupHash)
Write-Host ("  哈希文件：{0}" -f $shaFile)
Write-Host ("  provenance：{0}" -f $setupProvenance)
Write-Host ("  日志目录：{0}" -f $logDir)
Write-Host '  注意：这只表示安装包已构建，不代表已发布，也不代表已在真实用户目录安装过。' -ForegroundColor DarkGray
Write-Host "OUTPUT=$setupExe"
Write-Host "OUTPUT=$shaFile"
exit 0
