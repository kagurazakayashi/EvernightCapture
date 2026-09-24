<#
.SYNOPSIS
    离线判据：打包侧的 staging 所有权、载荷依赖闭包、产物身份核对与"缺依赖就不许出包"的端到端负向用例。
.DESCRIPTION
    判的是 installer\packaging.psm1 与 build-installer.ps1 的判定本身，全程不截图、不弹框、
    不碰真实安装目录，也不碰本仓库 build\ 里的既有产物：

      1. staging 归属：合法自有目录可重用与重建；build 本身、仓库根、staging 根本身、盘符根、
         相邻前缀（build-backup）、`..` 逃逸出范围、更深层子目录、别的卷、没有 ownership 标记的
         已有目录、标记指向别的仓库、目录本身是 junction、树里藏 junction —— 全部拒绝。
         每种拒绝都先放哨兵文件，再判"文件一个字节都没被动过"。归一化后仍在范围内的 `..`
         写法不该被误杀，这条也判。
      2. 载荷清单与离线依赖闭包：清单声明的每套离线套件都要有依赖条目，闭包里的脚本 / 二进制 /
         辅助文件在真实源码树里必须存在（少一个就是打包该拦住的东西）；声明了套件却没写依赖条目，
         读表这一步就该失败。
      3. 产物身份：正向拿真实 build\ecapture.exe 核对（版本 / 架构 / buildId / PE 时间戳互相自洽）；
         负向用错误的版本、错误的架构，以及一个根本不是 ECAPTURE 的程序与残缺 PE。
      4. 端到端负向：把打包需要的那几份文件复制进本次自建的临时树，抽掉 ecapture-hdr-tests.exe 与
         ecapture-identity-tests.exe，跑真实的 build-installer.ps1（-SkipBuild），判退出码非 0、
         报的就是这两个依赖、临时树里没多出安装包，并且本仓库现有安装包一个字节都没被改动。
      5. 端到端正向：同一棵（路径含空格与中文的）树补齐依赖再跑一次，判退出码 0、真的出了安装包、
         provenance 落了盘；再重用同一 staging 跑一次 -StageOnly，判"重用"这条路也通。
      6. 安装器静态一致性：ecapture.iss 的 [Setup] AppId / AppIdGuid / 载荷清单 appId 三处必须是
         同一个 GUID，并且带 -Arch 守卫与"没有编译期受管清单就编不过"的守卫。

    第 4 / 5 步要跑真实 ISCC：缺编译器或缺构建产物时如实 SKIP（不假装通过），只做判定级判据。
    临时树建在 %TEMP%\ecapture-tests\ 下本次独占的目录里，删除只认自己那一个。
.EXAMPLE
    .\tests\installer-package.ps1
    .\tests\installer-package.ps1 -SkipE2E        # 只做判定级判据，不跑真实打包
    pwsh -NoProfile -File .\tests\installer-package.ps1
#>
param(
    [string]$Exe,
    [switch]$SkipE2E
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$repo = Get-EcRepoRoot
Import-Module (Join-Path $repo 'installer\packaging.psm1') -Force -DisableNameChecking

$buildDir = Join-Path $repo 'build'
$realExe = Join-Path $buildDir 'ecapture.exe'
$manifestPath = Join-Path $repo 'installer\payload.manifest.json'
$issPath = Join-Path $repo 'installer\ecapture.iss'
$versionMatch = [regex]::Match((Get-Content -LiteralPath (Join-Path $repo 'src\Version.h') -Raw -Encoding UTF8),
                               '#define\s+ECAPTURE_VERSION_STRING\s+"([^"]+)"')
$version = $versionMatch.Groups[1].Value

Reset-EcSuite
Initialize-EcHarness -Exe $Exe -AllowMissing | Out-Null
$run = New-EcRunDir -Tag 'installer-pkg'
Write-Host ("  本次临时目录：{0}" -f $run.Path)

function New-TreeStaging {
    <# 在本次独占的临时树里搭一个"仓库 / build / installer-stage"形状，供归属判定使用。 #>
    param([string]$Name)
    $r = Join-Path $run.Path ("repo-{0}" -f $Name)
    $b = Join-Path $r 'build'
    $m = Join-Path $b 'installer-stage'
    New-Item -ItemType Directory -Force -Path $m | Out-Null
    return [pscustomobject]@{ Repo = $r; Build = $b; Managed = $m }
}

function New-Junction {
    <# 用 cmd 的 mklink /J 在指定路径建一个 junction；建不出来（权限 / 非本机策略）返回 $false。 #>
    param([Parameter(Mandatory)][string]$Link, [Parameter(Mandatory)][string]$Target)
    $null = & cmd.exe /c "mklink /J `"$Link`" `"$Target`"" 2>&1
    return [bool](Test-Path -LiteralPath $Link)
}

function Assert-Refused {
    <#
        判定必须拒绝，并且被拒绝的目录里哨兵文件原样留着 —— "不先删掉看看"要能被验证，
        不是口头承诺。清理请求也必须被驳回（清理走的是另一条更严的路径，两个都得拦）。
        已经存在又无法在里面放哨兵的路径（盘符根）只判"拒绝 + 没被动过"。
    #>
    param([Parameter(Mandatory)]$Tree, [Parameter(Mandatory)][string]$StageDir, [Parameter(Mandatory)][string]$Label)

    $sentinel = $null
    $content = $null
    $existed = Test-Path -LiteralPath $StageDir
    if (-not $existed) {
        New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
        $sentinel = Join-Path $StageDir 'sentinel.txt'
        $content = 'SENTINEL-{0}' -f ([Guid]::NewGuid().ToString('N'))
        Set-Content -LiteralPath $sentinel -Value $content -Encoding ASCII
    }

    $v = Test-EcStagingAllowed -StageDir $StageDir -BuildDir $Tree.Build -RepoRoot $Tree.Repo
    Assert-Ec (-not $v.Allowed) "$Label —— 本该拒绝，实际 Allowed=True"
    Assert-Ec ([bool]$v.Reason) "$Label —— 拒绝了但没给出原因" -Quiet
    $del = Remove-EcManagedStaging -StageDir $StageDir -BuildDir $Tree.Build -RepoRoot $Tree.Repo
    Assert-Ec (-not $del.Removed) "$Label —— 清理请求本该被驳回，实际动了目录"
    if ($sentinel) {
        Assert-Ec (Test-Path -LiteralPath $sentinel) "$Label —— 哨兵文件被删掉了"
        $now = (Get-Content -LiteralPath $sentinel -Raw -Encoding ASCII).Trim()
        Assert-Ec ($now -eq $content) "$Label —— 哨兵文件内容被改过"
    }
}

try {
    # =========================================================================
    Write-Host "`n=== 1) staging 归属判定：合法形状放行，其余拒绝且不动文件 ==="
    # =========================================================================
    $t1 = New-TreeStaging -Name 'guard'
    $ok = Test-EcStagingAllowed -StageDir (Join-Path $t1.Managed '0.4.0-x64') -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec $ok.Allowed "合法的新建 staging 应该放行（实际：$($ok.Reason)）" -Quiet

    # 归一化以后仍然落在受管范围内的 .. 写法：不能误杀，否则正常用法也过不了
    $insideDotDot = Test-EcStagingAllowed -StageDir (Join-Path $t1.Managed 'tmp\..\0.4.0-x64') -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec $insideDotDot.Allowed "归一化后仍在范围内的 .. 写法被误杀了（实际：$($insideDotDot.Reason)）" -Quiet

    foreach ($c in @(
        @{ Label = 'build 目录本身';            Path = $t1.Build },
        @{ Label = '仓库根';                    Path = $t1.Repo },
        @{ Label = '受管 staging 根本身';        Path = $t1.Managed },
        @{ Label = '盘符根';                    Path = 'C:\' },
        @{ Label = '相邻前缀目录 build-backup';  Path = (Join-Path $t1.Repo 'build-backup\stage') },
        @{ Label = '.. 逃逸出范围';             Path = (Join-Path $t1.Managed '..\..\escaped') },
        @{ Label = '更深层子目录';              Path = (Join-Path $t1.Managed 'a\b') },
        @{ Label = '别的卷';                    Path = 'D:\not-managed\stage' }
    )) {
        Assert-Refused -Tree $t1 -StageDir $c.Path -Label $c.Label
    }

    # 含空格与中文的 staging 目录名：形状合法就该接受（路径里有空格/中文不是拒绝理由）
    $spaced = Test-EcStagingAllowed -StageDir (Join-Path $t1.Managed '0.4.0-x64 我的 暂存') -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec $spaced.Allowed "带空格与中文的合法 staging 目录名该放行（实际：$($spaced.Reason)）"

    # 空路径 / 不存在的东西占了目录位置
    $empty = Test-EcStagingAllowed -StageDir '' -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec (-not $empty.Allowed) '不给路径必须判不合格'
    $asFile = Join-Path $t1.Managed 'is-a-file'
    Set-Content -LiteralPath $asFile -Value 'x' -Encoding ASCII
    $fv = Test-EcStagingAllowed -StageDir $asFile -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec (-not $fv.Allowed) '同路径上是个文件时不该被判成可用 staging'

    # 已有目录但没有 ownership 标记：陌生 payload 不能被当成自己的东西清理
    $foreign = Join-Path $t1.Managed 'foreign-payload'
    New-Item -ItemType Directory -Force -Path (Join-Path $foreign 'build') | Out-Null
    Set-Content -LiteralPath (Join-Path $foreign 'build\ecapture-hdr-tests.exe') -Value 'NOT OURS' -Encoding ASCII
    Assert-Refused -Tree $t1 -StageDir $foreign -Label '已有目录但没有所有权标记'
    Assert-Ec (Test-Path -LiteralPath (Join-Path $foreign 'build\ecapture-hdr-tests.exe')) '陌生 payload 被顺手删了'

    # 标记存在但内容不是合法 JSON / kind 不对：都属于"证明不了归属"
    $brokenMarker = Join-Path $t1.Managed 'broken-marker'
    New-Item -ItemType Directory -Force -Path $brokenMarker | Out-Null
    Set-Content -LiteralPath (Join-Path $brokenMarker 'ecapture-staging.marker.json') -Value '{ not json' -Encoding ASCII
    $vBroken = Test-EcStagingAllowed -StageDir $brokenMarker -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec (-not $vBroken.Allowed) '标记不是合法 JSON 时却放行了' -Quiet
    $wrongKind = Join-Path $t1.Managed 'wrong-kind'
    New-Item -ItemType Directory -Force -Path $wrongKind | Out-Null
    Set-Content -LiteralPath (Join-Path $wrongKind 'ecapture-staging.marker.json') -Value '{"kind":"someone-else"}' -Encoding ASCII
    Assert-Ec (-not (Test-EcStagingAllowed -StageDir $wrongKind -BuildDir $t1.Build -RepoRoot $t1.Repo).Allowed) '标记的 kind 不对时却放行了'

    # 合法自有目录：建标记 -> 重用 -> 清理 -> 重建，全程只动自己那一个
    $mine = Join-Path $t1.Managed 'mine'
    New-Item -ItemType Directory -Force -Path $mine | Out-Null
    [void](Write-EcStagingMarker -StageDir $mine -RepoRoot $t1.Repo -PayloadDir (Join-Path $mine 'payload') -Version '0.4.0' -Arch 'x64')
    $vMine = Test-EcStagingAllowed -StageDir $mine -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec $vMine.Allowed "带合格标记的自有目录应可重用（实际：$($vMine.Reason)）" -Quiet
    Set-Content -LiteralPath (Join-Path $mine 'payload.txt') -Value 'ours' -Encoding ASCII
    $delMine = Remove-EcManagedStaging -StageDir $mine -BuildDir $t1.Build -RepoRoot $t1.Repo
    Assert-Ec $delMine.Removed '自己的 staging 没能清理' -Quiet
    Assert-Ec (-not (Test-Path -LiteralPath $mine)) '清理后目录还在'

    # 标记指向别的仓库：不能证明是"这个仓库"的打包工具建的
    $otherRepo = Join-Path $run.Path 'repo-other'
    New-Item -ItemType Directory -Force -Path $otherRepo | Out-Null
    $t2 = New-TreeStaging -Name 'marker-mismatch'
    $cross = Join-Path $t2.Managed 'cross'
    New-Item -ItemType Directory -Force -Path $cross | Out-Null
    [void](Write-EcStagingMarker -StageDir $cross -RepoRoot $otherRepo -PayloadDir (Join-Path $cross 'payload') -Version '0.4.0' -Arch 'x64')
    Assert-Refused -Tree $t2 -StageDir $cross -Label '标记记录的仓库不是当前仓库'

    # junction：目录本身是重解析点，或树里藏着重解析点，清理都必须拒绝
    $t3 = New-TreeStaging -Name 'reparse'
    $junSelf = Join-Path $t3.Managed 'jun-self'
    if (New-Junction -Link $junSelf -Target $t3.Repo) {
        Assert-Refused -Tree $t3 -StageDir $junSelf -Label 'staging 目录本身是 junction'
    } else {
        Skip-Ec 'staging 目录本身是 junction' '本机建不出 junction（mklink /J 失败），这条判据没跑到'
    }
    $t4 = New-TreeStaging -Name 'reparse-inside'
    $inside = Join-Path $t4.Managed 'inside'
    New-Item -ItemType Directory -Force -Path $inside | Out-Null
    [void](Write-EcStagingMarker -StageDir $inside -RepoRoot $t4.Repo -PayloadDir (Join-Path $inside 'payload') -Version '0.4.0' -Arch 'x64')
    $junInside = Join-Path $inside 'jun-out'
    if (New-Junction -Link $junInside -Target (Join-Path $run.Path 'repo-guard')) {
        $keep = Join-Path $inside 'keep.txt'
        Set-Content -LiteralPath $keep -Value 'keep me' -Encoding ASCII
        $d = Remove-EcManagedStaging -StageDir $inside -BuildDir $t4.Build -RepoRoot $t4.Repo
        Assert-Ec (-not $d.Removed) '目录里有 junction 时清理没有被拒绝'
        Assert-Ec (Test-Path -LiteralPath $keep) 'junction 场景下自己的文件被删了'
        $rp = @(Get-EcReparseInside -Path $inside)
        Assert-Ec ($rp.Count -ge 1) '重解析点扫描没发现树里的 junction' -Quiet
        # 目标目录里的东西不能被这次清理带走（junction 指向的正是另一棵本次自建的树）
        Assert-Ec (Test-Path -LiteralPath (Join-Path $run.Path 'repo-guard\build\installer-stage')) '清理沿 junction 走进了范围外'
    } else {
        Skip-Ec '目录里有 junction 时拒绝清理' '本机建不出 junction，这条判据没跑到'
    }

    # =========================================================================
    Write-Host "`n=== 2) 载荷清单与离线依赖闭包 ==="
    # =========================================================================
    $manifest = Get-EcPayloadManifest -Path $manifestPath
    Assert-Ec ([string]$manifest.product -eq 'EvernightCapture') "清单产品名应为 EvernightCapture（实际 $([string]$manifest.product)）" -Quiet
    $offline = @($manifest.installPlan.offlineTests)
    Assert-Ec ($offline.Count -ge 10) "清单声明的离线套件数量：$($offline.Count)" -Quiet
    $closure = Get-EcOfflineClosure -Manifest $manifest
    Assert-Ec (@($closure.Binaries).Count -ge 11) "离线闭包至少要含 11 个测试程序（实际 $(@($closure.Binaries).Count) 个）"
    foreach ($need in @('build/ecapture-hdr-tests.exe', 'build/ecapture-identity-tests.exe', 'build/ecapture-windows-tests.exe')) {
        Assert-Ec (@($closure.Binaries) -contains $need) "离线闭包里应该有 $need" -Quiet
    }
    Assert-Ec (@($closure.Scripts) -contains 'tests/harness.psm1') '每条离线套件都依赖 tests/harness.psm1，闭包里必须有它'
    Assert-Ec (@($closure.Scripts) -contains 'test-all.ps1') '安装后的总跑入口 test-all.ps1 必须在闭包里'
    Assert-Ec (@($closure.Aux) -contains 'tests/helper/ec_window.cs') 'invoker 要现编测试窗口程序，ec_window.cs 必须在闭包里'
    Assert-Ec (@($closure.Scripts) -notcontains 'build/ecapture-consent-tests.exe') '没声明可安装运行的套件不该把它的二进制拖进闭包'
    foreach ($suite in $offline) {
        $deps = $closure.PerSuite[[string]$suite]
        Assert-Ec ($null -ne $deps) "套件 $suite 没有依赖条目" -Quiet
        Assert-Ec (@($deps.Scripts).Count -ge 1) "套件 $suite 的依赖条目里没有脚本"
    }
    $notInTree = @()
    foreach ($rel in (@($closure.Scripts) + @($closure.Binaries) + @($closure.Aux))) {
        $p = Join-Path $repo (($rel -replace '/', '\'))
        if (-not (Test-Path -LiteralPath $p)) { $notInTree += $rel }
    }
    if (Test-Path -LiteralPath $realExe) {
        Assert-Ec ($notInTree.Count -eq 0) "真实源码树里缺这些离线依赖：$($notInTree -join ', ')"
    } else {
        Skip-Ec '真实源码树里的离线依赖齐全' "还没有构建产物（$realExe 不在），先跑 .\build.ps1 再判这一条"
    }
    # 声明了套件却没写依赖条目 = 清单自己不一致，读表这一步就该失败
    $badManifest = Join-Path $run.Path 'manifest-no-deps.json'
    $mfText = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
    Set-Content -LiteralPath $badManifest -Value ($mfText -replace '(?s)"hdr"\s*:\s*\{.*?\},\s*', '') -Encoding UTF8
    $threw = $false
    try { [void](Get-EcPayloadManifest -Path $badManifest) } catch { $threw = $true }
    Assert-Ec $threw '清单声明了离线套件却没有它的依赖条目时，读表必须失败（不能默默少跑一套）'

    # =========================================================================
    Write-Host "`n=== 3) 产物身份核对：正向自洽，负向逐条说清 ==="
    # =========================================================================
    $notPe = Join-Path $run.Path 'not-a-pe.exe'
    Set-Content -LiteralPath $notPe -Value 'this is not a portable executable at all, needs a full page' -Encoding ASCII
    $pe1 = Get-EcPeIdentity -Path $notPe
    Assert-Ec (-not $pe1.Known) "文本文件被当成有效 PE 读出了东西：Machine=0x$($pe1.Machine.ToString('X4'))"
    if (-not (Test-Path -LiteralPath $realExe)) {
        Skip-Ec '产物身份正向核对' "没有 $realExe，先跑 .\build.ps1"
        Skip-Ec '产物身份负向核对' "没有 $realExe，先跑 .\build.ps1"
    } else {
        $id = Test-EcArtifactIdentity -Exe $realExe -ExpectedVersion $version -ExpectedArch 'x64' -TimeoutMs 60000
        Assert-Ec $id.Ok ("真实产物身份核对没通过：" + (@($id.Problems) -join ' | '))
        Assert-Ec ($id.BuildId -like "$version-x64-*") "buildId 形状应为 $version-x64-<8 位十六进制>（实际 $($id.BuildId)）"
        Assert-Ec ($id.Pe.Known -and $id.Pe.Machine -eq 0x8664) 'PE Machine 应为 x64（0x8664）'
        $stampHex = ([regex]::Match($id.BuildId, '-([0-9A-Fa-f]{8})$')).Groups[1].Value
        Assert-Ec ([Convert]::ToUInt32($stampHex, 16) -eq $id.Pe.TimeDateStamp) 'buildId 里的时间戳必须就是 PE 头里那个（同版本号不等于同一份构建）'
        $idWrongVer = Test-EcArtifactIdentity -Exe $realExe -ExpectedVersion '9.9.9-not-this' -ExpectedArch 'x64' -TimeoutMs 60000
        Assert-Ec (-not $idWrongVer.Ok) '版本号对不上时身份核对必须判不通过' -Quiet
        Assert-Ec (@($idWrongVer.Problems).Count -ge 2) "版本不符要逐条说清（实际 $(@($idWrongVer.Problems).Count) 条）"
        $idWrongArch = Test-EcArtifactIdentity -Exe $realExe -ExpectedVersion $version -ExpectedArch 'arm64' -TimeoutMs 60000
        Assert-Ec (-not $idWrongArch.Ok) '声称 arm64 时身份核对必须判不通过（这份构建链只有 x64）' -Quiet
        Assert-Ec ((@($idWrongArch.Problems) -join ' ') -match '架构') '架构不符的原因要在报告里说得出来'

        $trunc = Join-Path $run.Path 'truncated.exe'
        $bytes = [IO.File]::ReadAllBytes($realExe)
        [IO.File]::WriteAllBytes($trunc, $bytes[0..2047])
        $pe2 = Get-EcPeIdentity -Path $trunc
        Assert-Ec (-not $pe2.Known) '比一页还短的残缺 PE 被判成了可读' -Quiet
        Assert-Ec ([bool]$pe2.Error) '读不出来时没给原因'
        $idMissing = Test-EcArtifactIdentity -Exe (Join-Path $run.Path 'no-such.exe') -ExpectedVersion $version -ExpectedArch 'x64' -TimeoutMs 10000
        Assert-Ec (-not $idMissing.Ok) '产物不存在时身份核对不该判通过' -Quiet
    }

    # =========================================================================
    Write-Host "`n=== 4) 安装器静态一致性：AppId 三处一致 + 架构/清单守卫 ==="
    # =========================================================================
    $issText = Get-Content -LiteralPath $issPath -Raw -Encoding UTF8
    $appIdJson = [string]$manifest.appId
    $guid = ([regex]::Match($appIdJson, '\{\{([0-9A-Fa-f\-]{20,40})\}')).Groups[1].Value
    Assert-Ec ([bool]$guid) "载荷清单里的 appId 不是 {{{{GUID}} 这种 Inno 转义形状：$appIdJson"
    Assert-Ec ($issText -match ('AppId=\{\{' + [regex]::Escape($guid) + '\}')) 'ecapture.iss 的 [Setup] AppId 与载荷清单的 appId 不是同一个 GUID'
    Assert-Ec ($issText -match ('#define AppIdGuid "\{' + [regex]::Escape($guid) + '\}"')) 'ecapture.iss 里的 AppIdGuid 默认值与载荷清单不一致（归属标记会认不出自己的目录）'
    Assert-Ec ($issText -match '(?m)^\s*#error .*managed-list') 'ecapture.iss 缺少"没有编译期受管清单就编不过"的守卫'
    Assert-Ec ($issText -match '(?m)^\s*#error .*arm64') 'ecapture.iss 缺少拒绝 -Arch arm64 的守卫'
    Assert-Ec ($issText -match 'ArchitecturesAllowed=x64compatible') 'ecapture.iss 应限制在 x64compatible 上安装'
    Assert-Ec ($issText -match '(?m)^#if Arch != "x64"') 'ecapture.iss 缺少按 -Arch 的编译期守卫'
    Assert-Ec ($issText -match 'SuppressibleMsgBox') '未知目录确认必须用受 /SUPPRESSMSGBOXES 管辖的弹框'
    Assert-Ec ($issText -match 'IDNO') '静默模式下未知目录确认的默认值要是不装'

    # =========================================================================
    Write-Host "`n=== 5) 端到端：独立临时树里少依赖必须出不了包 ==="
    # =========================================================================
    if ($SkipE2E) {
        Skip-Ec '端到端打包（缺依赖 / 齐全）' '调用方给了 -SkipE2E'
    } elseif (-not (Test-Path -LiteralPath $realExe) -or -not (Test-Path -LiteralPath (Join-Path $buildDir 'CMakeCache.txt'))) {
        Skip-Ec '端到端打包（缺依赖 / 齐全）' "缺少可用的构建产物或 CMakeCache（$buildDir），先跑 .\build.ps1"
    } else {
        $fake = Join-Path $run.Path 'repo-fake 我的 目录'
        $fakeBuild = Join-Path $fake 'build'
        New-Item -ItemType Directory -Force -Path $fakeBuild | Out-Null
        foreach ($d in @('src', 'resources', 'installer', 'tests\helper', '.agents\skills\yashi-evernight-capture\references')) {
            New-Item -ItemType Directory -Force -Path (Join-Path $fake $d) | Out-Null
        }
        Copy-Item -LiteralPath (Join-Path $repo 'src\Version.h') -Destination (Join-Path $fake 'src\Version.h') -Force
        Copy-Item -LiteralPath (Join-Path $buildDir 'CMakeCache.txt') -Destination (Join-Path $fakeBuild 'CMakeCache.txt') -Force
        Copy-Item -LiteralPath $realExe -Destination (Join-Path $fakeBuild 'ecapture.exe') -Force
        foreach ($f in @('installer\payload.manifest.json', 'installer\ecapture.iss', 'installer\packaging.psm1',
                         'installer\verify-install.ps1', 'build-installer.ps1', 'test-all.ps1',
                         'README.md', 'README.zh-CN.md', 'README.zh-TW.md', 'README.ja-JP.md', 'LICENSE',
                         'resources\icon.ico', '.agents\skills\yashi-evernight-capture\SKILL.md')) {
            Copy-Item -LiteralPath (Join-Path $repo $f) -Destination (Join-Path $fake $f) -Force
        }
        Copy-Item -LiteralPath (Join-Path $repo 'tests\harness.psm1') -Destination (Join-Path $fake 'tests\harness.psm1') -Force
        Copy-Item -LiteralPath (Join-Path $repo 'tests\helper\ec_window.cs') -Destination (Join-Path $fake 'tests\helper\ec_window.cs') -Force
        foreach ($p in @(Get-ChildItem -LiteralPath (Join-Path $repo 'tests') -File -Filter '*.ps1')) {
            Copy-Item -LiteralPath $p.FullName -Destination (Join-Path $fake ('tests\' + $p.Name)) -Force
        }
        foreach ($r in @(Get-ChildItem -LiteralPath (Join-Path $repo '.agents\skills\yashi-evernight-capture\references') -File -Filter '*.md')) {
            Copy-Item -LiteralPath $r.FullName -Destination (Join-Path $fake ('.agents\skills\yashi-evernight-capture\references\' + $r.Name)) -Force
        }
        $closureExes = @($closure.Binaries | Where-Object { $_ -like 'build/*' } | ForEach-Object { $_ -replace '^build/', '' })
        Assert-Ec ($closureExes.Count -ge 11) "闭包里要复制的测试程序数不对劲：$($closureExes.Count)"
        foreach ($name in $closureExes) {
            Copy-Item -LiteralPath (Join-Path $buildDir $name) -Destination (Join-Path $fakeBuild $name) -Force
        }

        $eng = (Get-Process -Id $PID).Path
        $installer = Join-Path $fake 'build-installer.ps1'
        $setupDir = Join-Path $fakeBuild 'installer'
        $realSetup = Join-Path $buildDir ('installer\EvernightCapture-{0}-x64-setup.exe' -f $version)
        $realSetupHashBefore = $null
        if (Test-Path -LiteralPath $realSetup) { $realSetupHashBefore = (Get-FileHash -LiteralPath $realSetup -Algorithm SHA256).Hash }

        # ---- 负向：抽掉两个依赖，打包必须失败且临时树里不出安装包 ----
        foreach ($drop in @('ecapture-hdr-tests.exe', 'ecapture-identity-tests.exe')) {
            Remove-Item -LiteralPath (Join-Path $fakeBuild $drop) -Force
        }
        $neg = Invoke-EcProcess -FilePath $eng -TimeoutMs 900000 -WorkingDirectory $fake `
                -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer, '-SkipBuild')
        Assert-Ec ($neg.Exit -ne 0) "抽掉离线依赖后打包居然成功了（退出码 $($neg.Exit)）：缺依赖必须在打包阶段拦住"
        Assert-Ec ($neg.Stdout -match 'ecapture-hdr-tests') '打包失败的原因里没提到缺 hdr 依赖'
        Assert-Ec ($neg.Stdout -match 'ecapture-identity-tests') '打包失败的原因里没提到缺 identity 依赖'
        $negSetups = @(Get-ChildItem -LiteralPath $setupDir -File -Filter '*-setup.exe' -ErrorAction SilentlyContinue)
        Assert-Ec ($negSetups.Count -eq 0) "缺依赖的这一轮居然发布了安装包：$(($negSetups | ForEach-Object { $_.Name }) -join ', ')"
        if ($realSetupHashBefore) {
            $after = (Get-FileHash -LiteralPath $realSetup -Algorithm SHA256).Hash
            Assert-Ec ($after -eq $realSetupHashBefore) '本仓库现有的安装包被这次判据改动了'
        }

        # ---- 正向：补齐依赖，同一棵（带空格与中文的）树应该出包 ----
        foreach ($name in $closureExes) {
            $victim = Join-Path $fakeBuild $name
            if (-not (Test-Path -LiteralPath $victim)) {
                Copy-Item -LiteralPath (Join-Path $buildDir $name) -Destination $victim -Force
            }
        }
        $pos = Invoke-EcProcess -FilePath $eng -TimeoutMs 900000 -WorkingDirectory $fake `
                -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer, '-SkipBuild')
        $posSetups = @(Get-ChildItem -LiteralPath $setupDir -File -Filter '*-setup.exe' -ErrorAction SilentlyContinue)
        Assert-Ec ($pos.Exit -eq 0) ("依赖齐全时在临时树里打包应该成功（退出码 $($pos.Exit)）：" +
                    (($pos.Stdout -split "`r?`n" | Select-Object -Last 12) -join ' / '))
        Assert-Ec ($posSetups.Count -ge 1) '依赖齐全的这一轮没出安装包'
        Assert-Ec ($pos.Stdout -match '来源：reused') '打包输出应说明这批产物是复用还是本次重建'
        $provFiles = @(Get-ChildItem -LiteralPath $setupDir -File -Filter '*.provenance.json' -ErrorAction SilentlyContinue)
        Assert-Ec ($provFiles.Count -ge 1) '安装包旁边没有 provenance 记录'
        $prov = Get-Content -LiteralPath $provFiles[0].FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        Assert-Ec ([string]$prov.source -eq 'reused') "provenance 的 source 应为 reused（实际 $([string]$prov.source)）"
        Assert-Ec ([bool]$prov.configVerified) 'provenance 应记录配置核对结果'
        Assert-Ec (@($manifest.entries | Where-Object { $_.freshness -eq 'current-build' }).Count -ge 12) '载荷清单里 current-build 的条目数量不对劲'

        # 主口径：问构建系统自己（ninja -n 只读依赖图、不编译）。真实 build\ 现在应该是干净的。
        $metaReal = Get-EcBuildMetadata -BuildDir $buildDir
        $graph = Get-EcBuildGraphStatus -BuildDir $buildDir -NinjaPath ([string]$metaReal.MakeProgram) `
                                        -Generator ([string]$metaReal.Generator) -TimeoutMs 60000
        if ($metaReal.CacheExists -and ($metaReal.Generator -match 'Ninja')) {
            Assert-Ec $graph.Known "本机的构建图这一问应该有答案（实际：$($graph.Reason)）"
            Assert-Ec ($graph.UpToDate -eq $true) "真实 build\ 的构建图不干净，这一轮的对照判据前提不成立：$($graph.Reason)"
        } else {
            Skip-Ec '构建图新鲜度（ninja -n）主口径' "构建目录的生成器不是 Ninja 或缺 CMakeCache（实际：$([string]$metaReal.Generator)）"
        }

        # ---- 复用旧产物：构建输入比随包产物新时必须拒绝，也不许发布新包 ----
        $staleProbe = @(Get-EcStaleBinaries -BuildDir $fakeBuild -NewestInputUtc (Get-Date).ToUniversalTime() -Names @('ecapture.exe'))
        Assert-Ec ($staleProbe.Count -ge 1) '拿"现在"当最新构建输入时没把旧产物挑出来（新鲜度判据失效）' -Quiet
        $epochProbe = @(Get-EcStaleBinaries -BuildDir $fakeBuild -NewestInputUtc ([datetime]::new(1970, 1, 1, 0, 0, 0, [DateTimeKind]::Utc)) -Names @('ecapture.exe'))
        Assert-Ec ($epochProbe.Count -eq 0) ("输入比产物旧时不该报陈旧（实际：{0}）" -f (@($epochProbe | ForEach-Object { $_.Name }) -join ','))

        $setupWriteTime = (Get-Item -LiteralPath $posSetups[0].FullName).LastWriteTime
        $fakeStateCpp = Join-Path $fake 'tests\hdr_state.cpp'
        Set-Content -LiteralPath $fakeStateCpp -Value '// 本次判据造的更新现场：源码比随包产物新' -Encoding UTF8
        try {
            $stale = Invoke-EcProcess -FilePath $eng -TimeoutMs 900000 -WorkingDirectory $fake `
                      -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer, '-SkipBuild')
            Assert-Ec ($stale.Exit -ne 0) "构建输入比随包产物新时打包居然成功了（退出码 $($stale.Exit)）：旧产物不能当成当前源码的结果出包"
            Assert-Ec ($stale.Stdout -match '比构建输入旧|不能当成当前源码') "拒绝理由没讲清：$(($stale.Stdout -split "`r?`n" | Select-Object -Last 8) -join ' / ')"
            $newerSetups = @(Get-ChildItem -LiteralPath $setupDir -File -Filter '*-setup.exe' -ErrorAction SilentlyContinue |
                             Where-Object { $_.LastWriteTime -gt $setupWriteTime })
            Assert-Ec ($newerSetups.Count -eq 0) "陈旧产物的这一轮还是发布了新安装包：$(($newerSetups | ForEach-Object { $_.Name }) -join ', ')"
        } finally {
            Remove-Item -LiteralPath $fakeStateCpp -Force -ErrorAction SilentlyContinue
        }

        # ---- 重用同一条 staging 路径：带标记的目录必须能继续被本工具使用 ----
        $again = Invoke-EcProcess -FilePath $eng -TimeoutMs 900000 -WorkingDirectory $fake `
                  -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer, '-SkipBuild', '-StageOnly')
        Assert-Ec ($again.Exit -eq 0) ("重用已有 staging 时打包失败（退出码 $($again.Exit)）：" +
                    (($again.Stdout -split "`r?`n" | Select-Object -Last 8) -join ' / '))
        Assert-Ec ($again.Stdout -match '载荷校验通过') '重用 staging 的这一次没走到校验通过的现场'

        Write-Host ("  负向退出码 {0}；正向出包 {1} 个；provenance：{2}" -f `
                    $neg.Exit, $posSetups.Count, $(if ($provFiles.Count) { '已落盘' } else { '缺' })) -ForegroundColor DarkGray
    }

    Write-Host ("  受管 staging 根：{0}" -f (Join-Path $buildDir 'installer-stage')) -ForegroundColor DarkGray
} finally {
    Remove-EcRunDir $run -Quiet
}

exit (Complete-EcSuite -Title '打包与安装判据')
