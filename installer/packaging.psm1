<#
.SYNOPSIS
    打包侧的共享构件：staging 所有权判定、载荷清单解读、离线依赖闭包、产物身份与构建元数据核对。
.DESCRIPTION
    为什么单独一份模块：build-installer.ps1、tests\installer-package.ps1 与 installer\verify-install.ps1
    需要同一套判定，各自抄一份就会漂出第三套生命周期逻辑。这里只放**判定**，不放流程：
    谁调用、什么时候失败退出，仍由各自的脚本决定。

    1. staging 所有权：只承认「仓库 build\installer-stage 下的直接子目录」这一种形状，并且必须带着
       本模块写下的 ownership 标记。归一化后比较目录边界（父目录补分隔符再比前缀），所以
       build 本身、仓库根、盘符根、build-backup 这类相邻前缀、逃逸用的 .. 一律不合格。
       清理之前还要确认整棵树里没有重解析点（junction / symlink）——PowerShell 5.1 的
       Remove-Item -Recurse 会跟着 junction 走进范围外，这一点必须拦在前面。
       判不出来、证明不了所有权、有歧义：一律保留原文件并给出可操作的错误，不做"先删掉看看"。
    2. 载荷清单：payload.manifest.json 是唯一的依赖来源；installPlan.offlineSuiteDeps 给出每套离线
       套件必需的脚本 / 二进制 / 辅助文件，打包缺件即失败，不降级成可选 glob 或安装后再编译。
    3. 产物身份：核对判据本身住在 tests\harness.psm1（Get-EcPeIdentity / Invoke-EcReadOnlyQuery /
       Test-EcArtifactIdentity），因为安装自检与打包核对要用同一份；本模块只负责把那些判定串起来用，
       不再抄第二份。三条只读查询（--version / --capabilities / --diagnostics）都走共享调用器
       （并发消费两条流、有期限等待、超时只结束本次启动的进程树），并且把清单里声明的 buildId 与
       EXE 自己 PE 头里的链接时间戳、Machine 字段对上 —— 同版本号不等于同一份构建，只看文本子串不够。
    4. 构建元数据：配置（Debug / Release）来自构建目录里的 CMakeCache.txt，不来自命令行请求；
       -SkipBuild 复用现有产物时，先问构建系统自己有没有活要干（Get-EcBuildGraphStatus 跑 `ninja -n`，
       只读依赖图、不编译），问不出答案才退回"最新构建输入 vs 每个产物的链接时间戳"这种保守比对。
       时间戳并集口径会误判：ninja 按需重链，改一个 state.cpp 之后别的产物链接时间戳本来就不动。

    进程调用一律复用 tests\harness.psm1 的 Invoke-EcProcess（以 -Global 方式导入，本模块内可直接用）。
#>
Set-StrictMode -Off

$script:EcPackaging = $PSScriptRoot
$script:EcStagingMarkerName = 'ecapture-staging.marker.json'
$script:EcStagingManagedLeaf = 'installer-stage'
$script:EcStagingMarkerKind = 'ecapture-installer-staging'

# 共享调用器来自测试基础设施：打包侧不再抄第二套进程生命周期逻辑。
# 用 -Global 导入，这样本模块内的函数才拿得到 harness 里那份进程判据（Invoke-EcProcess 等）；
# 路径先算进变量再传参，避免在命令参数位置套嵌套子表达式。
$script:EcHarnessPath = [IO.Path]::GetFullPath((Join-Path $script:EcPackaging '..\tests\harness.psm1'))
Import-Module -Name $script:EcHarnessPath -Global -Force -DisableNameChecking

function Get-EcUnixSeconds {
    <# .NET DateTime -> Unix 秒（1970 起算，UTC）。手工算，避开不同 .NET 版本上的 API 差异。 #>
    param([Parameter(Mandatory)][datetime]$Value)
    $epoch = New-Object datetime(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)
    return [long](([datetime]::SpecifyKind($Value, [DateTimeKind]::Utc) - $epoch).TotalSeconds)
}

function Test-EcPathEqual {
    <# 路径等价判断：大小写不敏感、结尾分隔符无关，Ordinal 之外不猜文化设置。 #>
    param([string]$A, [string]$B)
    if (-not $A -or -not $B) { return $false }
    $x = $A.TrimEnd('\', '/')
    $y = $B.TrimEnd('\', '/')
    return [bool]($x.Equals($y, [StringComparison]::OrdinalIgnoreCase))
}

function Test-EcPathInside {
    <#
        $Child 是否**严格位于** $Parent 之下：给父目录补上目录分隔符再比前缀。
        直接比 GetFullPath(build) 的前缀会把 build-backup 也算成 build 里面（旧实现就栽在这里），
        所以边界必须带分隔符。
    #>
    param([string]$Parent, [string]$Child)
    if (-not $Parent -or -not $Child) { return $false }
    $p = [IO.Path]::GetFullPath($Parent.TrimEnd('\', '/')) + [IO.Path]::DirectorySeparatorChar
    $c = [IO.Path]::GetFullPath($Child)
    return [bool]$c.StartsWith($p, [StringComparison]::OrdinalIgnoreCase)
}

function Test-EcReparsePoint {
    <#
        这个**具体**路径自己是不是重解析点（junction / symlink / mount point）。
        用 Win32 FindFirstFile 直接读重解析标记：Get-Item 在某些情况下会把属性归到目标上，判不出真实形状。
        读不到属性时按"形状不明"处理（返回 $true），宁可拒绝清理也不越界。
    #>
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { return $false }
    try {
        $attr = [int][IO.File]::GetAttributes($Path)
        return [bool](($attr -band [IO.FileAttributes]::ReparsePoint) -ne 0)
    } catch {
        return $true
    }
}

function Get-EcReparseInside {
    <# 递归列出目录树里的重解析点；目录读不动（权限等）同样算"形状不明"，一并回报。 #>
    param([Parameter(Mandatory)][string]$Path)

    $found = New-Object System.Collections.Generic.List[string]
    if (Test-EcReparsePoint -Path $Path) { $found.Add($Path) }
    try {
        foreach ($item in @(Get-ChildItem -LiteralPath $Path -Force -Recurse -ErrorAction Stop)) {
            try {
                if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { $found.Add($item.FullName) }
            } catch { $found.Add($item.FullName) }
        }
    } catch {
        $found.Add("$Path（目录读不完整：$($_.Exception.Message)）")
    }
    return $found
}

function Get-EcStagingLayout {
    <# 由仓库根算出受管 staging 根与标记文件路径；这里只算路径，不创建、不判定。 #>
    param([Parameter(Mandatory)][string]$BuildDir, [Parameter(Mandatory)][string]$RepoRoot)

    $buildFull = [IO.Path]::GetFullPath($BuildDir.TrimEnd('\', '/'))
    return [pscustomobject]@{
        Repo        = [IO.Path]::GetFullPath($RepoRoot.TrimEnd('\', '/'))
        Build       = $buildFull
        ManagedRoot = Join-Path $buildFull $script:EcStagingManagedLeaf
    }
}

function Test-EcStagingAllowed {
    <#
    .SYNOPSIS
        判定一个 staging 目录能不能被本打包工具创建 / 重用 / 清理，返回结构化结论，不产生任何副作用。
    .DESCRIPTION
        首次使用与重用走同一套规则：先归一化路径、再判形状、最后才涉及存在性。
        合格的条件（全部满足）：
          * 能解析成绝对路径，且不是盘符根；
          * 是受管 staging 根（仓库 build\installer-stage）的**直接子目录**，不是根本身；
          * 不等于仓库根、不等于 build 目录；叶子名里没有 .. 与路径分隔符；
          * staging 根与目标目录本身都不是重解析点；
          * 目标已存在时：必须是真实目录，且带着与本仓库一致的 ownership 标记。
        任一条件不满足 → Allowed=$false 并给出 Reason，调用方保留原文件不动。
    #>
    param(
        # 故意不写 Mandatory：空路径本身就是"该拒绝的一种形状"，要由判定给出原因，而不是让参数绑定先炸。
        [string]$StageDir,
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][string]$RepoRoot
    )

    $layout = Get-EcStagingLayout -BuildDir $BuildDir -RepoRoot $RepoRoot
    $deny = {
        param([string]$why, [string]$shown)
        [pscustomobject]@{ Allowed = $false; Reason = $why; Full = $shown; ManagedRoot = $layout.ManagedRoot; MarkerPath = $null; Exists = $false }
    }

    if (-not $StageDir) { return (& $deny '没有给出目录路径' '') }

    $full = $null
    try { $full = [IO.Path]::GetFullPath($StageDir) } catch { return (& $deny "路径无法解析：$StageDir" $StageDir) }
    $full = $full.TrimEnd('\', '/')

    if ($full -eq [IO.Path]::GetPathRoot($full)) { return (& $deny "拒绝盘符根：$full" $full) }
    if (Test-EcPathEqual $full $layout.Repo)   { return (& $deny "拒绝仓库根：$full" $full) }
    if (Test-EcPathEqual $full $layout.Build)  { return (& $deny "拒绝 build 目录本身：$full" $full) }
    if (Test-EcPathEqual $full $layout.ManagedRoot) { return (& $deny "拒绝受管 staging 根本身：$full" $full) }
    if (-not (Test-EcPathInside $layout.ManagedRoot $full)) {
        return (& $deny "不在受管 staging 根之下（含相邻前缀目录，如 build-backup）：$full；允许的形状是 $($layout.ManagedRoot)\<目录名>" $full)
    }

    $leaf = Split-Path -Path $full -Leaf
    if (-not $leaf -or $leaf -eq '.' -or $leaf -eq '..' -or $leaf -match '[\\/:*?"<>|]') {
        return (& $deny "staging 目录名不合法：[$leaf]" $full)
    }
    # 直接子目录：叶子之外不能再有层级，否则清理范围会跟着调用方给的深度走。
    $parentOfLeaf = [IO.Path]::GetFullPath((Split-Path -Path $full -Parent)).TrimEnd('\', '/')
    if (-not (Test-EcPathEqual $parentOfLeaf $layout.ManagedRoot)) {
        return (& $deny "只接受受管 staging 根的直接子目录：$full" $full)
    }

    if (Test-EcReparsePoint -Path $layout.ManagedRoot) {
        return (& $deny "受管 staging 根自身是重解析点或属性读不出来：$($layout.ManagedRoot)" $full)
    }
    if (Test-EcReparsePoint -Path $full) {
        return (& $deny "staging 目录本身是重解析点或属性读不出来：$full" $full)
    }

    $exists = Test-Path -LiteralPath $full
    $marker = Join-Path $full $script:EcStagingMarkerName
    if ($exists) {
        $isDir = $false
        try { $isDir = [bool]((Get-Item -LiteralPath $full -Force).PSIsContainer) } catch { return (& $deny "读不出目录属性：$full" $full) }
        if (-not $isDir) { return (& $deny "同路径上已经有一个不是目录的东西：$full" $full) }
        if (-not (Test-Path -LiteralPath $marker)) {
            return (& $deny "已有目录缺少本工具的 staging 所有权标记（$script:EcStagingMarkerName），不能证明归打包工具管，拒绝清理：$full" $full)
        }
        $info = $null
        try { $info = Get-Content -LiteralPath $marker -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return (& $deny "staging 所有权标记读不出或不是合法 JSON：$marker" $full) }
        if ([string]$info.kind -ne $script:EcStagingMarkerKind) {
            return (& $deny "staging 所有权标记的 kind 不是 $script:EcStagingMarkerKind：$marker" $full)
        }
        if (-not (Test-EcPathEqual ([string]$info.repo) $layout.Repo)) {
            return (& $deny "staging 所有权标记记录的仓库是 $([string]$info.repo)，与当前仓库 $($layout.Repo) 不一致" $full)
        }
    }
    if (-not $exists -and (Test-Path -LiteralPath $marker)) {
        # 目录不存在却又找得到标记文件：多半是父级被换成 junction / 大小写改写之类的歧义，不猜。
        return (& $deny "目录与标记的存在状态互相矛盾，拒绝动手：$full" $full)
    }

    return [pscustomobject]@{ Allowed = $true; Reason = ''; Full = $full; ManagedRoot = $layout.ManagedRoot; MarkerPath = $marker; Exists = [bool]$exists }
}

function Write-EcStagingMarker {
    <# 在已经判定合格的 staging 目录里写下所有权标记（创建与每次重用都刷新）。 #>
    param(
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$PayloadDir,
        [string]$Version,
        [string]$Arch
    )

    $marker = [ordered]@{
        kind       = $script:EcStagingMarkerKind
        product    = 'EvernightCapture'
        repo       = [IO.Path]::GetFullPath($RepoRoot.TrimEnd('\', '/'))
        staging    = [IO.Path]::GetFullPath($StageDir)
        payload    = [IO.Path]::GetFullPath($PayloadDir)
        version    = $Version
        arch       = $Arch
        writtenUtc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    $path = Join-Path $StageDir $script:EcStagingMarkerName
    [IO.File]::WriteAllText($path, ($marker | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
    return $path
}

function Remove-EcManagedStaging {
    <#
        清理 staging：重新走一遍形状判定，再确认整棵树没有重解析点，才允许递归删除。
        判定不过 / 有重解析点 / 有歧义 → 一个文件都不动，返回原因交给调用方报错。
    #>
    param(
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][string]$RepoRoot
    )

    $verdict = Test-EcStagingAllowed -StageDir $StageDir -BuildDir $BuildDir -RepoRoot $RepoRoot
    if (-not $verdict.Allowed) {
        return [pscustomobject]@{ Removed = $false; Reason = $verdict.Reason; Path = $verdict.Full }
    }
    if (-not $verdict.Exists) {
        return [pscustomobject]@{ Removed = $false; Reason = '目录本来就不存在，无需清理' ; Path = $verdict.Full }
    }
    $reparse = @(Get-EcReparseInside -Path $verdict.Full)
    if ($reparse.Count) {
        return [pscustomobject]@{ Removed = $false; Reason = ("目录树里有重解析点，递归删除会走到范围外：" + ($reparse -join ' | ')); Path = $verdict.Full }
    }
    try {
        Remove-Item -LiteralPath $verdict.Full -Recurse -Force
    } catch {
        return [pscustomobject]@{ Removed = $false; Reason = "删除失败：$($_.Exception.Message)"; Path = $verdict.Full }
    }
    return [pscustomobject]@{ Removed = $true; Reason = ''; Path = $verdict.Full }
}

function Get-EcPayloadManifest {
    <# 读载荷清单并做结构守卫：字段不齐就当清单不可用，绝不在半份表上继续打包。 #>
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) { throw "找不到载荷清单：$Path" }
    $text = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    if (-not $text) { throw "载荷清单是空的：$Path" }
    $mf = $text | ConvertFrom-Json
    $missing = @()
    foreach ($key in @('manifestVersion', 'product', 'appId', 'ownershipMarkerFile', 'entries', 'skill', 'installPlan')) {
        if ($null -eq $mf.$key) { $missing += $key }
    }
    $plan = $mf.installPlan
    foreach ($key in @('offlineTests', 'offlineSuiteDeps', 'excludedFromInstall')) {
        if ($null -eq $plan.$key) { $missing += "installPlan.$key" }
    }
    if ($missing.Count) { throw "载荷清单缺少必需字段：$($missing -join ', ')（$Path）" }
    foreach ($suite in @($plan.offlineTests)) {
        if ($null -eq $plan.offlineSuiteDeps.$suite) {
            throw "载荷清单声明了离线套件 [$suite]，但 offlineSuiteDeps 里没有它的依赖条目（$Path）"
        }
    }
    return $mf
}

function Get-EcOfflineClosure {
    <#
        把 installPlan.offlineSuiteDeps 摊平成「这套离线计划必需的载荷内相对路径」清单。
        二进制在安装目录里的位置固定是 build\<name>（与套件脚本里 Join-Path $root 'build\<name>' 一致）。
    #>
    param([Parameter(Mandatory)]$Manifest)

    $scripts = New-Object System.Collections.Generic.List[string]
    $binaries = New-Object System.Collections.Generic.List[string]
    $aux = New-Object System.Collections.Generic.List[string]
    $perSuite = @{}
    foreach ($suite in @($Manifest.installPlan.offlineTests)) {
        $deps = $Manifest.installPlan.offlineSuiteDeps.$suite
        $sScripts = @(); $sBins = @(); $sAux = @()
        # 逐条走显式循环并跳过空值：@($null | ForEach-Object {...}) 会对"空"执行一次脚本块，
        # 那样会凭空造出 build\ 这种半截路径。
        foreach ($x in @($deps.scripts))  { if ($x) { $sScripts += [string]$x } }
        foreach ($x in @($deps.binaries)) { if ($x) { $sBins += "build/$x" } }
        foreach ($x in @($deps.aux))      { if ($x) { $sAux += [string]$x } }
        $perSuite[[string]$suite] = [pscustomobject]@{ Scripts = $sScripts; Binaries = $sBins; Aux = $sAux }
        foreach ($x in $sScripts) { if (-not $scripts.Contains($x))  { [void]$scripts.Add($x) } }
        foreach ($x in $sBins)    { if (-not $binaries.Contains($x)) { [void]$binaries.Add($x) } }
        foreach ($x in $sAux)     { if (-not $aux.Contains($x))      { [void]$aux.Add($x) } }
    }
    # 每条离线套件都从 tests\harness.psm1 取调用器与断言，test-all.ps1 是入口：这些也是必需项，不靠 glob 碰运气
    foreach ($shared in @('tests/harness.psm1', 'test-all.ps1')) {
        if (-not $scripts.Contains($shared)) { [void]$scripts.Add($shared) }
    }
    return [pscustomobject]@{ Scripts = @($scripts); Binaries = @($binaries); Aux = @($aux); PerSuite = $perSuite }
}

function Get-EcBuildMetadata {
    <#
        配置与构建来源的事实，只来自构建目录里真实存在的元数据，不来自命令行请求：
          * CMakeCache.txt 的 CMAKE_BUILD_TYPE —— 这份 build 目录当前被配置成什么；
          * CMAKE_GENERATOR 与 CMAKE_MAKE_PROGRAM —— 后面新鲜度那一问要找谁来问。
        读不出 CMAKE_BUILD_TYPE 时 Config 为 $null，由调用方决定"报未知"还是"拒绝打包"，这里不猜。
    #>
    param([Parameter(Mandatory)][string]$BuildDir)

    $out = [pscustomobject]@{ Config = $null; Generator = $null; MakeProgram = $null; CachePath = $null; CacheExists = $false }
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    $out.CachePath = $cache
    if (-not (Test-Path -LiteralPath $cache)) { return $out }
    $out.CacheExists = $true
    foreach ($line in @(Get-Content -LiteralPath $cache -Encoding UTF8)) {
        if ($line -match '^CMAKE_BUILD_TYPE:STRING=(.+)$') { $out.Config = $Matches[1].Trim() }
        elseif ($line -match '^CMAKE_GENERATOR:INTERNAL=(.+)$') { $out.Generator = $Matches[1].Trim() }
        elseif ($line -match '^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$') { $out.MakeProgram = $Matches[1].Trim() }
    }
    return $out
}

function Get-EcBuildGraphStatus {
    <#
    .SYNOPSIS
        问构建系统自己："这批产物相对当前源码还新鲜吗？"
    .DESCRIPTION
        -SkipBuild 复用的是已有产物，命令行请求本身不是证据。最可靠的判据不是比时间戳 ——
        ninja 是按需重编的：改一个 tests\*_state.cpp 只会重链那一个测试程序，其余产物会一直保留
        各自的旧链接时间戳，用"最新输入 vs 每个产物"的并集口径会把它们全部误判成陈旧。
        所以这里直接问 ninja（构建图与依赖它最清楚）：`ninja -n` 只读依赖图、不执行编译，
        "no work to do" 就是说这张图里没有任何产物落后于源码。
        返回 UpToDate=$null 表示这一问没答案（生成器不是 Ninja、ninja 跑不起来等），
        由调用方决定退回时间戳口径还是拒绝打包，不假装通过。
    #>
    param(
        [Parameter(Mandatory)][string]$BuildDir,
        [string]$NinjaPath,
        [string]$Generator,
        [int]$TimeoutMs = 60000
    )

    if ($Generator -and ($Generator -notmatch 'Ninja')) {
        return [pscustomobject]@{ Known = $false; UpToDate = $null; Reason = "生成器不是 Ninja：$Generator" }
    }
    if (-not $NinjaPath -or -not (Test-Path -LiteralPath $NinjaPath)) {
        return [pscustomobject]@{ Known = $false; UpToDate = $null; Reason = "找不到 CMakeCache 里记的 ninja：$NinjaPath" }
    }
    $r = Invoke-EcProcess -FilePath $NinjaPath -Arguments @('-n') -TimeoutMs $TimeoutMs -WorkingDirectory $BuildDir
    if ($r.StartError) {
        return [pscustomobject]@{ Known = $false; UpToDate = $null; Reason = "ninja 没能启动：$($r.StartError)" }
    }
    if ($r.TimedOut) {
        return [pscustomobject]@{ Known = $false; UpToDate = $null; Reason = 'ninja -n 没有按时退出' }
    }
    if ($r.Exit -ne 0) {
        return [pscustomobject]@{ Known = $false; UpToDate = $null; Reason = "ninja -n 退出码 $($r.Exit)：$($r.Stdout.Trim()) $($r.Stderr.Trim())" }
    }
    $text = ('{0}{1}' -f $r.Stdout, $r.Stderr)
    $clean = ($text -match 'no work to do')
    return [pscustomobject]@{ Known = $true; UpToDate = [bool]$clean; Reason = $text.Trim() }
}

function Get-EcNewestSourceWrite {
    <#
        构建输入里最新的写入时间；一个都读不到时返回 $null。
        默认算发布产物的输入（src\**、resources\**、CMakeLists.txt）；
        -IncludeTestSources 再加上 tests\**_state.cpp —— 那批离线判据程序的真实输入。
        tests\helper\ec_window.cs 不算：它由 csc 在测试时现编，不进 CMake 构建图。
    #>
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [switch]$IncludeTestSources
    )

    $newest = $null
    $scopes = @('src', 'resources')
    foreach ($rel in $scopes) {
        $dir = Join-Path $RepoRoot $rel
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        foreach ($f in @(Get-ChildItem -LiteralPath $dir -File -Recurse -ErrorAction SilentlyContinue)) {
            if ($null -eq $newest -or $f.LastWriteTimeUtc -gt $newest) { $newest = $f.LastWriteTimeUtc }
        }
    }
    if ($IncludeTestSources) {
        $t = Join-Path $RepoRoot 'tests'
        if (Test-Path -LiteralPath $t) {
            foreach ($f in @(Get-ChildItem -LiteralPath $t -File -Filter '*_state.cpp' -ErrorAction SilentlyContinue)) {
                if ($null -eq $newest -or $f.LastWriteTimeUtc -gt $newest) { $newest = $f.LastWriteTimeUtc }
            }
        }
    }
    $cmake = Join-Path $RepoRoot 'CMakeLists.txt'
    if (Test-Path -LiteralPath $cmake) {
        $t2 = (Get-Item -LiteralPath $cmake).LastWriteTimeUtc
        if ($null -eq $newest -or $t2 -gt $newest) { $newest = $t2 }
    }
    return $newest
}

function Get-EcStaleBinaries {
    <#
        -SkipBuild 复用现有产物时，逐个比"构建输入的最新写入时间"与每个 EXE 的链接时间戳。
        这里取的是并集口径：只要有任何一个输入比某个 EXE 新，就把那个 EXE 报出来。
        这样会偏保守（改了一个 state.cpp 会让别的测试程序也被质疑），但 -SkipBuild 本来就不重建，
        保守比把旧构建的 EXE 当成当前源码的产物包出去要诚实。
    #>
    param(
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][datetime]$NewestInputUtc,
        [string[]]$Names = @()
    )

    $newestUnix = Get-EcUnixSeconds -Value $NewestInputUtc
    $stale = @()
    $names = @($Names)
    if (-not $names.Count) { $names = @(Get-ChildItem -LiteralPath $BuildDir -File -Filter '*.exe' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) }
    foreach ($n in $names) {
        $p = Join-Path $BuildDir $n
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $pe = Get-EcPeIdentity -Path $p
        if (-not $pe.Known) { $stale += [pscustomobject]@{ Name = $n; Reason = "读不出 PE 头：$($pe.Error)" }; continue }
        if ($newestUnix -gt [long]$pe.TimeDateStamp) {
            $stale += [pscustomobject]@{ Name = $n; Reason = ('链接时间戳 {0} 早于最新构建输入 {1}' -f $pe.TimeDateStamp, $newestUnix) }
        }
    }
    return $stale
}
Export-ModuleMember -Function *-* -Variable @('EcStagingMarkerName', 'EcStagingManagedLeaf', 'EcStagingMarkerKind')
