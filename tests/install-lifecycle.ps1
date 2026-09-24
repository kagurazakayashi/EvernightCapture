<#
.SYNOPSIS
    真装真卸判据：在只属于本次判据的临时目录里安装 / 重装 / 卸载 EvernightCapture 安装包，判目录归属保护、
    用户改动备份、失败现场与卸载边界。
.DESCRIPTION
    需要 Inno Setup 已经编出的安装包（默认取 build\installer\EvernightCapture-<版本>-x64-setup.exe），
    缺安装包或缺编译器时整套如实 SKIP —— 不拿静态检查冒充安装验收。

    全程只用本次自建的临时安装目录，绝不往用户真实的 %UserProfile%\.agents\skills\ 里装东西，
    并且在第一例前后核对那个真实目录没被动过。判的几条：

      1. 空目录安装：退出码 0，ECAPTURE.EXE / SKILL.md / install-manifest.json / 测试程序都到位，
         安装器写下归属标记（product / appid / markerversion + 逐文件大小与写入时间），
         随包 verify-install.ps1 在这个目录里跑只读自检通过。
      2. 原样重装：不该产生任何备份目录（没有东西被改过）。
      3. 用户改过 SKILL.md 再重装：覆盖前把原件复制进 .ecapture-backup-<时间>\ 并留 BACKUP-INFO.txt，
         备份内容等于用户改过的那份，装出来的那份是包里的新版本；再用同一包装一次，
         不该把备份再当"用户改动"重复备份（标记里记的是上次装下来的样子）。
      4. 陌生目录保护：只有 install-manifest.json（假 / 空 / 别的产品的）而无合格归属标记的非空目录，
         静默安装必须**不写任何东西、如实中止**（退出码非 0，原有文件与哨兵内容不变）。
      5. 归属标记被写坏（产品名 / AppId 不对，或格式不合格）同样按陌生目录处理。
      6. 占用现场：把装好的 ECAPTURE.EXE 以"独占"方式打开再重装 → 安装必须失败；
         判"要么整套是新的、要么整套是旧的"，不许出现半新半旧的混装；并且用户后加的文件不受影响。
         Inno 的回滚边界以这一例实测为准（.iss 顶部注释记的就是这里看到的）。
      7. 卸载：只删安装器自己记录的文件与那个归属标记；用户新增文件、备份目录里的原件、
         相邻的别的产品目录都必须在卸载后还在。
      8. verify-install.ps1 的预算参数非法时在起任何进程之前就拒绝（退出码 2）。
      9. 声明的安装后可跑离线计划真的能在这个隔离目录里跑完（不依赖源码树、Visual Studio 或网络）；
         这条最重，用 -SkipOffline 跳过时要如实记 SKIP。

    不弹真实截图确认框，也不替任何人答它：安装与卸载都不调用截图路径。
.EXAMPLE
    .\tests\install-lifecycle.ps1
    .\tests\install-lifecycle.ps1 -SkipOffline          # 不跑安装目录里的离线套件（省时间）
    .\tests\install-lifecycle.ps1 -SetupExe "D:\path\EvernightCapture-0.4.0-x64-setup.exe"
#>
param(
    [string]$SetupExe,
    [switch]$SkipOffline,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Off

Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$repo = Get-EcRepoRoot
$version = ([regex]::Match((Get-Content -LiteralPath (Join-Path $repo 'src\Version.h') -Raw -Encoding UTF8),
                           '#define\s+ECAPTURE_VERSION_STRING\s+"([^"]+)"')).Groups[1].Value
if (-not $SetupExe) { $SetupExe = Join-Path $repo ('build\installer\EvernightCapture-{0}-x64-setup.exe' -f $version) }

Reset-EcSuite
$run = New-EcRunDir -Tag 'install-lifecycle'
Write-Host ("  本次临时目录：{0}" -f $run.Path)

$realSkillDir = Join-Path $env:USERPROFILE '.agents\skills\yashi-evernight-capture'
$realBefore = $null
if (Test-Path -LiteralPath $realSkillDir) {
    $realBefore = @(Get-ChildItem -LiteralPath $realSkillDir -Recurse -File | ForEach-Object { '{0}|{1}|{2}' -f $_.FullName, $_.Length, $_.LastWriteTimeUtc.Ticks }) -join "`n"
}

function Invoke-Installer {
    <# 静默安装 / 卸载：两条流并发消费 + 有期限等待（复用共享调用器），返回退出码与输出。 #>
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string[]]$Arguments, [int]$TimeoutSec = 600)
    return (Invoke-EcProcess -FilePath $Exe -Arguments $Arguments -TimeoutMs ($TimeoutSec * 1000))
}

function Install-To {
    <#
        每次安装都必须同时满足两条硬闸，否则直接 throw：
          * /DIR 必须落在本次自建的临时根里面 —— 历史上一旦参数被吃掉，"静默安装"会以完整 GUI 向导
            启动，只要有人点一次 Next 就装进真实的 %UserProfile%\.agents\skills\ 里。
          * 命令行必须显式带 /VERYSILENT 与 /SUPPRESSMSGBOXES（从 Git Bash 直接起时 msys 会把
            /VERYSILENT 改写成路径，参数整段消失；这里经 PowerShell 传参并断言，不让那种事悄悄发生）。
    #>
    param([Parameter(Mandatory)][string]$Dir)
    $dirFull = [IO.Path]::GetFullPath($Dir)
    $rootFull = [IO.Path]::GetFullPath($run.Path)
    if (-not $dirFull.StartsWith($rootFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "安装目标不在本次临时根内，拒绝执行：$dirFull（临时根：$rootFull）"
    }
    $installArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/NOICONS', ('/DIR={0}' -f $dirFull))
    foreach ($must in @('/VERYSILENT', '/SUPPRESSMSGBOXES')) {
        if ($installArgs -notcontains $must) { throw "安装命令缺少 $must，拒绝执行（无人值守不该弹界面）" }
    }
    return (Invoke-Installer -Exe $SetupExe -Arguments $installArgs)
}

function Uninstall-From {
    param([Parameter(Mandatory)][string]$Dir)
    $un = Join-Path $Dir 'unins000.exe'
    if (-not (Test-Path -LiteralPath $un)) { return $null }
    return (Invoke-Installer -Exe $un -Arguments @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -TimeoutSec 300)
}

function Count-Backups {
    param([Parameter(Mandatory)][string]$Dir)
    return @(Get-ChildItem -LiteralPath $Dir -Directory -Force -Filter '.ecapture-backup-*' -ErrorAction SilentlyContinue).Count
}

function Read-OwnerMarker {
    param([Parameter(Mandatory)][string]$Dir)
    $p = Join-Path $Dir 'evernightcapture.owner.ini'
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    return (Get-Content -LiteralPath $p -Raw -Encoding UTF8)
}

function Get-FileFacts {
    param([Parameter(Mandatory)][string]$Dir, [Parameter(Mandatory)][string[]]$Names)
    $out = @()
    foreach ($n in $Names) {
        $p = Join-Path $Dir $n
        if (Test-Path -LiteralPath $p) { $i = Get-Item -LiteralPath $p; $out += ('{0}|{1}|{2}' -f $n, $i.Length, $i.LastWriteTimeUtc.Ticks) }
        else { $out += ('{0}|missing' -f $n) }
    }
    return ($out -join "`n")
}

if (-not (Test-Path -LiteralPath $SetupExe)) {
    Write-Host "找不到安装包：$SetupExe —— 整套安装/卸载判据没法跑（先跑 .\build-installer.ps1）。" -ForegroundColor Yellow
    Skip-Ec '安装 / 重装 / 卸载全周期' "没有安装包：$SetupExe"
    Skip-Ec '陌生目录与坏标记保护' '没有安装包，装不了也就判不了'
    Skip-Ec '占用导致的失败现场' '没有安装包'
    Skip-Ec '卸载边界' '没有安装包'
    Skip-Ec '安装目录里的离线计划' '没有安装包'
    exit (Complete-EcSuite -Title '安装全周期判据')
}
Write-Host ("  安装包: {0}" -f $SetupExe)

try {
    # =========================================================================
    Write-Host "`n=== 1) 空目录安装 ==="
    # =========================================================================
    $d1 = Join-Path $run.Path 'inst-basic'
    New-Item -ItemType Directory -Force -Path $d1 | Out-Null
    $r = Install-To -Dir $d1
    Assert-Ec (-not $r.TimedOut) '安装跑超时了（说明等待没有收住）'
    Assert-Ec ($r.Exit -eq 0) "静默安装退出码 $($r.Exit)，应为 0：$(($r.Stdout + $r.Stderr) -replace '[\r\n]+', ' | ')"
    foreach ($f in @('ECAPTURE.EXE', 'SKILL.md', 'install-manifest.json', 'verify-install.ps1', 'test-all.ps1',
                     'tests\harness.psm1', 'tests\installer-package.ps1', 'tests\hdr.ps1', 'build\ecapture-hdr-tests.exe',
                     'build\ecapture-identity-tests.exe', 'resources\icon.ico', 'README.zh-CN.md', 'LICENSE')) {
        Assert-Ec (Test-Path -LiteralPath (Join-Path $d1 $f)) "装完缺少 $f"
    }
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $d1 'build.ps1'))) '开发用的 build.ps1 不该随包发出'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $d1 'AGENTS.md'))) '开发用的 AGENTS.md 不该随包发出'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $d1 'src'))) '安装目录里不该有源码目录'
    $marker = Read-OwnerMarker -Dir $d1
    # 按行拆开来判：归属标记是 CRLF 的文本，^...$ 会被行尾的 \r 绊住，逐行比更可靠。
    $mLines = @($marker -split "`r?`n")
    Assert-Ec ([bool]$marker) '安装器没写下归属标记'
    Assert-Ec (@($mLines -contains 'product=EvernightCapture').Count -gt 0) '归属标记里的 product 不对'
    Assert-Ec (@($mLines | Where-Object { $_ -like 'appid={B7A2E1C4-*' }).Count -eq 1) '归属标记里的 AppId 不对'
    Assert-Ec (@($mLines -contains 'markerversion=1').Count -gt 0) '归属标记里的 markerversion 不对'
    Assert-Ec (@($mLines | Where-Object { $_ -match '^-\-files-\-$' }).Count -eq 1) '归属标记里没有文件记录段'
    Assert-Ec (@($mLines | Where-Object { $_ -match '^(.+)\|\d+\|\d+$' }).Count -ge 40) '归属标记记下的文件数量不对劲'
    Assert-Ec ((Count-Backups -Dir $d1) -eq 0) '空目录安装不该产生备份目录'

    Write-Host "`n=== 2) 隔离安装目录里的只读自检 ==="
    $self = Invoke-Installer -Exe (Get-Process -Id $PID).Path -Arguments @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $d1 'verify-install.ps1'),
        '-InstallDir', $d1) -TimeoutSec 600
    Assert-Ec ($self.Exit -eq 0) "安装目录里的只读自检没通过（退出码 $($self.Exit)）：$((($self.Stdout + $self.Stderr) -split "`r?`n" | Where-Object { $_ -match 'FAIL' } | Select-Object -First 6) -join ' / ')"
    Assert-Ec (($self.Stdout + $self.Stderr) -notmatch '  FAIL  ') '只读自检里有 FAIL'
    # 非法预算要在起任何进程之前就被拒绝（这一条不跑真实套件）
    $badBudget = Invoke-Installer -Exe (Get-Process -Id $PID).Path -Arguments @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $d1 'verify-install.ps1'),
        '-InstallDir', $d1, '-RunOfflineTests', '-SuiteTimeoutSec', '0') -TimeoutSec 120
    Assert-Ec ($badBudget.Exit -eq 2) "非法 -SuiteTimeoutSec 应在起进程前被拒绝（退出码 $($badBudget.Exit)）" -Quiet
    $badOverall = Invoke-Installer -Exe (Get-Process -Id $PID).Path -Arguments @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $d1 'verify-install.ps1'),
        '-InstallDir', $d1, '-OverallTimeoutSec', '999999') -TimeoutSec 120
    Assert-Ec ($badOverall.Exit -eq 2) '非法 -OverallTimeoutSec 应在起进程前被拒绝' -Quiet

    # =========================================================================
    Write-Host "`n=== 3) 原样重装不该备份，用户改动该被备份 ==="
    # =========================================================================
    $r2 = Install-To -Dir $d1
    Assert-Ec ($r2.Exit -eq 0) "原样重装退出码 $($r2.Exit)"
    Assert-Ec ((Count-Backups -Dir $d1) -eq 0) '原样重装不该产生备份目录（没有任何东西被改过）'

    $skill = Join-Path $d1 'SKILL.md'
    $userEdit = '  ' + ('我的本地补充说明 ' + [Guid]::NewGuid().ToString('N'))
    Add-Content -LiteralPath $skill -Value $userEdit -Encoding UTF8
    $edited = Get-Content -LiteralPath $skill -Raw -Encoding UTF8

    $r3 = Install-To -Dir $d1
    Assert-Ec ($r3.Exit -eq 0) "带用户改动的重装退出码 $($r3.Exit)"
    $bCount = Count-Backups -Dir $d1
    Assert-Ec ($bCount -eq 1) "用户改过 SKILL.md 后重装应有 1 个备份目录（实际 $bCount）"
    $bDir = @(Get-ChildItem -LiteralPath $d1 -Directory -Force -Filter '.ecapture-backup-*')[0].FullName
    $bSkill = Join-Path $bDir 'SKILL.md'
    Assert-Ec (Test-Path -LiteralPath $bSkill) '备份目录里没有 SKILL.md 的原件'
    Assert-Ec ((Get-Content -LiteralPath $bSkill -Raw -Encoding UTF8) -eq $edited) '备份出来的原件与用户改过的那份不一致'
    Assert-Ec (Test-Path -LiteralPath (Join-Path $bDir 'BACKUP-INFO.txt')) '备份目录里缺少说明文件'
    $nowSkill = Get-Content -LiteralPath $skill -Raw -Encoding UTF8
    Assert-Ec ($nowSkill -ne $edited) '重装后 SKILL.md 没有被更新成包里的版本'
    Assert-Ec ($nowSkill -notmatch [regex]::Escape($userEdit)) '新装出来的 SKILL.md 里还留着用户的补充（说明覆盖没生效）'

    # 再装一次：此时磁盘上的东西与标记记的一致，不该再算"用户改动"
    $r4 = Install-To -Dir $d1
    Assert-Ec ($r4.Exit -eq 0) "第三次重装退出码 $($r4.Exit)"
    Assert-Ec ((Count-Backups -Dir $d1) -eq 1) '第三次重装把上次刚装下来的文件又当成用户改动备份了'

    # 备份目录与用户后加的文件都不属于本产品：卸载前它们就在，卸载后也必须在（见第 6 步）
    $userFile = Join-Path $d1 '我的笔记.txt'
    Set-Content -LiteralPath $userFile -Value '用户自己的文件' -Encoding UTF8

    # =========================================================================
    Write-Host "`n=== 4) 陌生目录与坏标记：静默安装必须不写任何东西 ==="
    # =========================================================================
    $cases = @(
        @{ Name = '假清单（别的产品的 JSON）'; Manifest = '{"product":"SomeoneElsesTool","files":[]}'; Extra = 'keep.txt' },
        @{ Name = '空清单文件';                Manifest = '';                                          Extra = 'keep2.txt' },
        @{ Name = '坏 JSON 清单';              Manifest = '{ not json at all';                         Extra = 'keep3.txt' },
        @{ Name = '没有清单的非空目录';        Manifest = $null;                                       Extra = 'keep4.txt' }
    )
    $ci = 0
    foreach ($c in $cases) {
        $ci++
        # 目录名带序号：几种"陌生目录"的中文标签去掉非 ASCII 后可能撞成同一个名字。
        $dir = Join-Path $run.Path ('inst-unknown-{0}' -f $ci)
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $keepFile = Join-Path $dir $c.Extra
        $keepContent = 'SENTINEL-' + [Guid]::NewGuid().ToString('N')
        Set-Content -LiteralPath $keepFile -Value $keepContent -Encoding ASCII
        if ($null -ne $c.Manifest) { Set-Content -LiteralPath (Join-Path $dir 'install-manifest.json') -Value $c.Manifest -Encoding UTF8 }
        $rr = Install-To -Dir $dir
        Assert-Ec ($rr.Exit -ne 0) "$($c.Name)：静默安装居然往这个目录里写了东西（退出码 $($rr.Exit)）"
        Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $dir 'ECAPTURE.EXE'))) "$($c.Name)：被拒绝的安装还是铺下了 ECAPTURE.EXE"
        Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $dir 'evernightcapture.owner.ini'))) "$($c.Name)：被拒绝的安装写下了归属标记"
        Assert-Ec ((Get-Content -LiteralPath $keepFile -Raw -Encoding ASCII).Trim() -eq $keepContent) "$($c.Name)：目录里原有的哨兵文件被动过"
        Write-Host ("    {0} -> 退出码 {1}（没有写入）" -f $c.Name, $rr.Exit) -ForegroundColor DarkGray
    }

    # 归属标记存在但被写坏（产品名 / AppId 不对）：同样按陌生目录处理
    foreach ($breakIt in @('product=SomeoneElse', 'appid={00000000-0000-0000-0000-000000000000}', 'markerversion=99')) {
        $dir = Join-Path $run.Path ('inst-broken-' + ($breakIt -replace '[^0-9A-Za-z]', ''))
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $mLines = @('# marker', ('{0}' -f $breakIt), 'appid={B7A2E1C4-5D3F-4E6A-9C21-8F0B3D5E7A10}', 'markerversion=1', '--files--')
        if ($breakIt -like 'appid=*') { $mLines = @('# marker', 'product=EvernightCapture', ('{0}' -f $breakIt), 'markerversion=1', '--files--') }
        if ($breakIt -like 'markerversion=*') { $mLines = @('# marker', 'product=EvernightCapture', 'appid={B7A2E1C4-5D3F-4E6A-9C21-8F0B3D5E7A10}', ('{0}' -f $breakIt), '--files--') }
        Set-Content -LiteralPath (Join-Path $dir 'evernightcapture.owner.ini') -Value $mLines -Encoding UTF8
        $keepFile = Join-Path $dir 'keep.txt'
        Set-Content -LiteralPath $keepFile -Value 'SENTINEL' -Encoding ASCII
        $rr = Install-To -Dir $dir
        Assert-Ec ($rr.Exit -ne 0) "归属标记被写成 $($breakIt) 时静默安装居然继续了（退出码 $($rr.Exit)）"
        Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $dir 'ECAPTURE.EXE'))) "坏标记（$breakIt）场景下仍然铺了文件"
    }

    # =========================================================================
    Write-Host "`n=== 5) 文件被占用时的失败现场：不许半新半旧 ==="
    # =========================================================================
    $exePath = Join-Path $d1 'ECAPTURE.EXE'
    $tracked = @('ECAPTURE.EXE', 'SKILL.md', 'README.md', 'install-manifest.json', 'tests\hdr.ps1', 'build\ecapture-hdr-tests.exe')
    $factsBefore = Get-FileFacts -Dir $d1 -Names $tracked
    $lock = $null
    try {
        $lock = [IO.File]::Open($exePath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $r5 = Install-To -Dir $d1
        Assert-Ec ($r5.Exit -ne 0) "ECAPTURE.EXE 被独占打开时安装居然报成功（退出码 $($r5.Exit)）"
        $factsAfter = Get-FileFacts -Dir $d1 -Names $tracked
        # 要么整套还是失败前那份，要么整套已是新包那份：两者都不成立就是混装。
        $mixed = ($factsAfter -ne $factsBefore)
        if ($mixed) {
            # 与"全新安装"的样子比一次：拿本次临时目录里另一份干净安装做对照
            $dClean = Join-Path $run.Path 'inst-clean-for-compare'
            New-Item -ItemType Directory -Force -Path $dClean | Out-Null
            $rc = Install-To -Dir $dClean
            Assert-Ec ($rc.Exit -eq 0) "对照组干净安装失败（退出码 $($rc.Exit)），混装判据没法判" -Quiet
            $factsClean = Get-FileFacts -Dir $dClean -Names $tracked
            Assert-Ec ($factsAfter -eq $factsClean) '失败的安装留下了半新半旧的混装现场（既不是原样，也不是完整新包）'
        }
        Assert-Ec (Test-Path -LiteralPath $userFile) '失败的安装把用户后加的文件带走了'
        Assert-Ec ((Get-Content -LiteralPath $userFile -Raw -Encoding UTF8).Trim() -eq '用户自己的文件') '用户后加的文件内容被改过'
        Assert-Ec (Test-Path -LiteralPath $skill) 'SKILL.md 在这个失败现场里不见了'
        Write-Host ("  占用安装退出码 {0}；现场判定：{1}" -f $r5.Exit, $(if ($mixed) { '整套是新包那份（Inno 已把能复制的复制完）' } else { '整套保持失败前原样（Inno 回滚了）' })) -ForegroundColor DarkGray
    } finally {
        if ($lock) { try { $lock.Dispose() } catch { } }
    }

    # =========================================================================
    Write-Host "`n=== 6) 卸载边界：只删自己记录的东西 ==="
    # =========================================================================
    $sibling = Join-Path $run.Path 'inst-basic-sibling-other-skill'
    New-Item -ItemType Directory -Force -Path $sibling | Out-Null
    Set-Content -LiteralPath (Join-Path $sibling 'SKILL.md') -Value '别的 skill' -Encoding UTF8
    $u = Uninstall-From -Dir $d1
    Assert-Ec ($u -and $u.Exit -eq 0) "静默卸载退出码不对（$($u.Exit)）"
    Assert-Ec (-not (Test-Path -LiteralPath $exePath)) '卸载后 ECAPTURE.EXE 还在'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $d1 'tests\hdr.ps1'))) '卸载后随包测试脚本还在'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $d1 'evernightcapture.owner.ini'))) '卸载没清掉自己写的归属标记'
    Assert-Ec (Test-Path -LiteralPath $userFile) '卸载删掉了用户新增的文件'
    Assert-Ec ((Count-Backups -Dir $d1) -ge 1) '卸载删掉了备份目录'
    Assert-Ec (Test-Path -LiteralPath $bSkill) '备份目录里的用户原件被卸载带走了'
    Assert-Ec (Test-Path -LiteralPath $sibling) '卸载把相邻目录带走了'
    Assert-Ec (Test-Path -LiteralPath $run.Path) '卸载把本次临时根目录带走了'

    # =========================================================================
    Write-Host "`n=== 7) 真实用户目录没有被这次判据碰过 ==="
    # =========================================================================
    if ($null -ne $realBefore) {
        $realAfter = @(Get-ChildItem -LiteralPath $realSkillDir -Recurse -File | ForEach-Object { '{0}|{1}|{2}' -f $_.FullName, $_.Length, $_.LastWriteTimeUtc.Ticks }) -join "`n"
        Assert-Ec ($realAfter -eq $realBefore) "本次判据动了用户真实的 Skill 目录（$realSkillDir）"
    } else {
        Assert-Ec (-not (Test-Path -LiteralPath $realSkillDir)) "本次判据在用户的真实 Skill 目录里建了东西：$realSkillDir"
    }
    Assert-Ec (Test-Path -LiteralPath $SetupExe) '判据把仓库里的安装包弄没了（不该动它）'

    # =========================================================================
    Write-Host "`n=== 8) 隔离安装目录里跑声明的离线计划（不依赖源码树） ==="
    # =========================================================================
    if ($SkipOffline) {
        Skip-Ec '安装目录里的离线计划' '调用方给了 -SkipOffline'
    } else {
        $dOff = Join-Path $run.Path 'inst-offline-plan'
        New-Item -ItemType Directory -Force -Path $dOff | Out-Null
        $ri = Install-To -Dir $dOff
        Assert-Ec ($ri.Exit -eq 0) "为离线计划准备的安装失败（退出码 $($ri.Exit)）" -Quiet
        # 关键：在工作目录完全无关的地方跑，证明它不靠源码树、也不靠 PATH 上的开发产物
        $offline = Invoke-Installer -Exe (Get-Process -Id $PID).Path -Arguments @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $dOff 'verify-install.ps1'),
            '-InstallDir', $dOff, '-RunOfflineTests', '-KeepLogs') -TimeoutSec 5400
        $offText = $offline.Stdout + $offline.Stderr
        foreach ($line in @(($offText -split "`r?`n" | Where-Object { $_ -match 'FAIL|没通过|超时|没能启动' } | Select-Object -First 10))) {
            Write-Host "    $line" -ForegroundColor DarkGray
        }
        Assert-Ec (-not $offline.TimedOut) '安装目录里的离线计划在总预算内没跑完（看上面的尾部）'
        Assert-Ec ($offline.Exit -eq 0) "安装版离线测试退出码 $($offline.Exit)，应为 0（日志见 -KeepLogs 保留的临时目录）"
        Assert-Ec ($offText -match '安装版离线测试全部通过') '离线计划里至少有一套没给出通过结论'
        # 随包必须带着离线套件的测试程序：抽掉一个就该由打包阶段拦下，这里只确认装下来的确实在
        foreach ($need in @('build\ecapture-hdr-tests.exe', 'build\ecapture-identity-tests.exe', 'build\ecapture-windows-tests.exe')) {
            Assert-Ec (Test-Path -LiteralPath (Join-Path $dOff $need)) "安装目录里缺 $need" -Quiet
        }
    }
} finally {
    if ($Keep) {
        Write-Host ("  保留现场：{0}" -f $run.Path) -ForegroundColor Yellow
    } else {
        # 卸载残留（unins000.* 与 Inno 的临时记录）由 Inno 自己处理；这里只清本次自建的目录。
        Remove-EcRunDir $run -Quiet
    }
}

exit (Complete-EcSuite -Title '安装全周期判据')
