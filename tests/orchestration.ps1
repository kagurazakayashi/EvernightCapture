<#
.SYNOPSIS
    离线验证一键总跑 test-all.ps1 自己的两件事：执行计划非空校验，以及"没跑成"的分类与退出码。
.DESCRIPTION
    这里不跑任何真实判据：把 test-all.ps1 连同一份测试专用的 `tests\harness.psm1` 和若干**傀儡套件**
    复制进本次临时目录，然后用被测的那份 test-all.ps1 去跑这份假仓库。被测对象是总跑脚本本体
    （筛完的空计划、NOT RUN 的三种去向、退出码），不是它的抄本。

    钉住的几条（都是"机器调用方拿到 0 但其实什么都没验"这一类）：
      1. 计划内点名要跑的套件**脚本文件不见了** → 记 NOT RUN(missing-script)，总退出码必须是 1
      2. 套件源码**解析不干净** → 记 NOT RUN(unparsable-parameters)，总退出码必须是 1
      3. 套件自己一个脚本级参数都没声明（只有 `param()`）→ 必须照常跑；这是"函数 return 空数组被
         PowerShell 摊平成 $null，于是'没有参数'被误读成'读不出来'而整套不跑"的回归位
      4. `-Only x -Except x` 筛出零套件的计划 → 明确报"没有可执行测试"（NO-TESTS-PLANNED）并以 2 结束，
         不进汇总，也不给 0
      5. 只交出 SKIP 的套件（套件自己的退出码是 0）→ 总退出码仍是 0：环境与安全边界不升成失败
      6. 给了 -StopOnFail 时，剩下的记 NOT RUN(stopped-on-fail)，与上面两类**意外**未运行分开计

    全程不截图、不弹框、不碰桌面上任何别人的窗口；-Exe 只被用来过一次 `--version` 只读自检。
    注意：被测脚本仍要抢那个"同一台机器只允许一个总跑"的互斥体，所以本判据不能与真总跑并发。
.EXAMPLE
    .\tests\orchestration.ps1
    .\tests\orchestration.ps1 -Keep      # 保留假仓库现场（日志与傀儡套件）以便人眼看
#>
param(
    [string]$Exe,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
Initialize-EcHarness -Exe $Exe | Out-Null
Reset-EcSuite

$repoRoot = Get-EcRepoRoot
$run = New-EcRunDir -Tag 'orchestration'
Write-Host ("  本次临时目录：{0}" -f $run.Path)

try {
    # -----------------------------------------------------------------------
    # 假仓库搭建：被测的 test-all.ps1 + 它自己要 Import 的 harness + 傀儡套件
    # 傀儡套件要打印各套件那行中文汇总（被测脚本正是按那行文本读计数的），所以源码按 UTF-8 带 BOM
    # 落盘：没 BOM 的 UTF-8 在 Windows PowerShell 5.1 上会按本机码页解，中文先坏一次，汇总行就乱了
    # -----------------------------------------------------------------------
    $fake = Join-Path $run.Path 'fake'
    New-Item -ItemType Directory -Force -Path (Join-Path $fake 'tests') | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot 'tests\harness.psm1') -Destination (Join-Path $fake 'tests\harness.psm1')
    # 被测脚本的副本不能原样用：那份"同一台机器只允许一个总跑"的互斥体是按名字认的，
    # 真总跑跑到本判据时已经持着它，副本一进去就撞在自己的锁上、被当成"已有另一个总跑"退 2。
    # 所以这里只把锁名换成本次专有的，其余一个字节都不改；换名失败要当场叫住，
    # 不能留着原样名字在总跑里自我阻塞（那会让这套判据只在单独跑的时候才是绿的）。
    $srcText = [Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes((Join-Path $repoRoot 'test-all.ps1')))
    $ownLock = 'Local\ecapture-test-all-orch-{0}-{1}' -f $PID, ([Guid]::NewGuid().ToString('N')).Substring(0, 8)
    $lockPattern = 'Local\\ecapture-test-all[^'']*'
    $found = ([regex]::Matches($srcText, $lockPattern)).Count
    $copyText = [regex]::Replace($srcText, $lockPattern, $ownLock)
    Assert-Ec ($found -ge 1 -and $copyText -ne $srcText) `
        "副本里的互斥体名换掉了（原文里找到 $found 处）；换不掉就不许继续跑"
    [IO.File]::WriteAllText((Join-Path $fake 'test-all.ps1'), $copyText, (New-Object Text.UTF8Encoding($true)))

    function Set-FakeSuite {
        <#
            写一份傀儡套件。"这份脚本压根不存在"用 -Remove 明说：以前我用 $null 当记号，结果
            [string] 参数把 $null 收成空字符串，于是"删掉"变成了"写一个 0 字节的空脚本"，
            而空脚本是能正常跑、正常退 0 的 —— 正好把要判的那条 missing-script 掩盖掉。
            源码按 UTF-8 带 BOM 落盘：傀儡套件要打印各套件那行中文汇总，PowerShell 5.1 读无 BOM
            的 UTF-8 会按本机码页解，这里给它一个两种 shell 都读得对的写法。
        #>
        param(
            [Parameter(Mandatory)][string]$Name,
            [AllowEmptyString()][string]$Body = '',
            [switch]$Remove
        )

        $path = Join-Path $fake ('tests\' + $Name + '.ps1')
        if ($Remove) {
            if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
            return
        }
        [IO.File]::WriteAllText($path, $Body, (New-Object Text.UTF8Encoding($true)))
    }

    # 一套正常交出结果的套件：打印自己的汇总行（test-all 的 Get-LogCounts 就认这一行）
    $okBody = @'
param([string]$Exe)
Write-Host "orchestration stub: 1"
Write-Host "  PASS  stub case"
Write-Host "全部通过：7 项"
exit 0
'@
    # 只有 SKIP 的套件：退出码 0，但汇总里写着未验证 —— 不该被升成失败
    $skipBody = @'
param([string]$Exe)
Write-Host "全部通过：5 项，跳过 3 项"
exit 0
'@
    # 真失败的套件：给 -StopOnFail 那条判据当触发点
    $failBody = @'
param([string]$Exe)
Write-Host "  FAIL  stub deliberate failure"
Write-Host "失败：2 项，通过 1 项"
exit 1
'@
    # 一个脚本级参数都没有的套件：必须照常跑（判 $null 摊平那条回归）
    $noParamBody = @'
param()
Write-Host "orchestration stub: no-param"
Write-Host "全部通过：4 项"
exit 0
'@
    # 语法不干净的套件：ParseFile 报错 → 读不出参数
    $brokenBody = @'
param([string]$Exe
Write-Host "unterminated above"
'@

    function Invoke-FakeTotal {
        <# 用被测的那份 test-all.ps1 跑假仓库：-NoBuild -Force 免构建免按键，超时收紧。 #>
        param(
            [Parameter(Mandatory)][string[]]$Only,
            [string[]]$Except = @(),
            [switch]$StopOnFail,
            [string]$Tag = 'run'
        )

        $logDir = Join-Path $run.Path ('logs-' + $Tag)
        $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $fake 'test-all.ps1'),
               '-NoBuild', '-Force', '-Shell', 'auto', '-SuiteTimeoutSec', '60',
               '-LogDir', $logDir, '-Exe', (Get-EcExePath), '-Only', ($Only -join ','))
        if ($Except) { $a += @('-Except', ($Except -join ',')) }
        if ($StopOnFail) { $a += '-StopOnFail' }
        $r = Invoke-EcProcess -FilePath (Get-Process -Id $PID).Path -Arguments $a -TimeoutMs 180000
        return [pscustomobject]@{ Exit = $r.Exit; Text = ('{0}{1}' -f $r.Stdout, $r.Stderr) }
    }

    function Assert-Row {
        <# 汇总那张表里某个套件的状态列（result 那一格按前缀匹配，NOT RUN 要连着括号里的原因一起看）。 #>
        param([Parameter(Mandatory)][string]$Text, [Parameter(Mandatory)][string]$Name,
              [Parameter(Mandatory)][string]$Status)

        $escaped = [regex]::Escape($Status)
        $line = @($Text -split "[`r`n]+" | Where-Object { $_ -match "^\s*$([regex]::Escape($Name))\s+$escaped" })
        return ($line.Count -eq 1)
    }

    # =======================================================================
    Write-Host "`n=== 1) 正常计划：跑到并通过 → 总退出码 0 ==="
    # =======================================================================
    Set-FakeSuite -Name 'cli' -Body $okBody
    Set-FakeSuite -Name 'windows' -Body $okBody
    $r = Invoke-FakeTotal -Only @('cli', 'windows') -Tag 'pass'
    Assert-Ec ($r.Exit -eq 0) "两套都通过时总退出码为 0（实际 $($r.Exit)）"
    Assert-Ec (-not ($r.Text -match 'NO-TESTS-PLANNED')) "正常计划不会报'没有可执行测试'"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'cli' -Status 'PASS') "汇总里 cli 那一行是 PASS"
    Assert-Ec ($r.Text -match '意外没跑成 0 套') "汇总把意外未运行如实记为 0"

    # =======================================================================
    Write-Host "`n=== 2) 计划内的套件脚本不见了：整套没跑成必须判失败 ==="
    # =======================================================================
    Set-FakeSuite -Name 'windows' -Remove
    $r = Invoke-FakeTotal -Only @('cli', 'windows') -Tag 'missing'
    Assert-Ec ($r.Exit -eq 1) "有套件整套没跑成时总退出码为 1（实际 $($r.Exit)）"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'windows' -Status 'NOT RUN(missing-script)') `
        "缺失的套件记成 NOT RUN(missing-script)，原因写进表里"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'cli' -Status 'PASS') "同一轮里跑过的另一套不受影响"
    Assert-Ec ($r.Text -match '意外没跑成 1 套') "意外未运行的计数是 1，不是 0"

    # =======================================================================
    Write-Host "`n=== 3) 套件解析不干净：同样整套没跑成，也必须判失败 ==="
    # =======================================================================
    Set-FakeSuite -Name 'screens' -Body $brokenBody
    $r = Invoke-FakeTotal -Only @('cli', 'screens') -Tag 'broken'
    Assert-Ec ($r.Exit -eq 1) "读不出参数的套件让总退出码为 1（实际 $($r.Exit)）"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'screens' -Status 'NOT RUN(unparsable-parameters)') `
        "解析不干净的套件记成 NOT RUN(unparsable-parameters)"

    # =======================================================================
    Write-Host "`n=== 4) 只写 param() 的套件：不许被误读成'读不出来'而整套不跑 ==="
    # =======================================================================
    Set-FakeSuite -Name 'save' -Body $noParamBody
    $r = Invoke-FakeTotal -Only @('cli', 'save') -Tag 'noparam'
    Assert-Ec ($r.Exit -eq 0) "零参数套件照常跑，总退出码 0（实际 $($r.Exit)）"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'save' -Status 'PASS') `
        "零参数套件是 PASS，不是 NOT RUN（空数组被摊平成 `$null 的那条回归位）"

    # =======================================================================
    Write-Host "`n=== 5) 筛完是空计划：明确报'没有可执行测试'，不给 0 ==="
    # =======================================================================
    $r = Invoke-FakeTotal -Only @('cli') -Except @('cli') -Tag 'empty'
    Assert-Ec ($r.Exit -eq 2) "零套件的计划按前置不成立交 2（实际 $($r.Exit)）"
    Assert-Ec ($r.Text -match 'NO-TESTS-PLANNED') "空计划报出了'没有可执行测试'那句话"
    Assert-Ec ($r.Text -match '-Only cli') "那句话里带上生效的筛选，便于机器与人都看得懂"
    Assert-Ec ($r.Text -notmatch '汇总 ====') "空计划不进汇总，也不会打出一句'全部通过'"

    # =======================================================================
    Write-Host "`n=== 6) 只交出 SKIP 的套件：环境边界，不升成失败 ==="
    # =======================================================================
    Set-FakeSuite -Name 'hdr' -Body $skipBody
    $r = Invoke-FakeTotal -Only @('cli', 'hdr') -Tag 'skip'
    Assert-Ec ($r.Exit -eq 0) "只有 SKIP 时总退出码仍为 0（实际 $($r.Exit)）"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'hdr' -Status 'PASS') "SKIP 那套仍按跑过记录"
    Assert-Ec ($r.Text -match '未验证 3 项') "套件自己报的跳过数被如实汇总（3 项）"

    # =======================================================================
    Write-Host "`n=== 7) -StopOnFail 的剩余：与意外未运行分开 ==="
    # =======================================================================
    Set-FakeSuite -Name 'crop' -Body $failBody
    Set-FakeSuite -Name 'scale' -Body $okBody
    $r = Invoke-FakeTotal -Only @('crop', 'scale') -StopOnFail -Tag 'stopfail'
    Assert-Ec ($r.Exit -eq 1) "有 FAIL 时总退出码 1（实际 $($r.Exit)）"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'crop' -Status 'FAIL') "触发停止的那套记 FAIL"
    Assert-Ec (Assert-Row -Text $r.Text -Name 'scale' -Status 'NOT RUN(stopped-on-fail)') `
        "剩余那套记成主动没跑，不混进意外未运行"
    Assert-Ec ($r.Text -match '意外没跑成 0 套') "主动停下的剩余不会虚增意外未运行"
    Assert-Ec ($r.Text -match '因 -StopOnFail 主动没跑 1 套') "主动没跑的条数如实报 1"
} finally {
    if ($Keep) { Write-Host ("  假仓库现场保留在 {0}" -f $run.Path) -ForegroundColor DarkGray }
    else { Remove-EcRunDir $run -Quiet }
}

exit (Complete-EcSuite -Title '总跑计划与退出码')
