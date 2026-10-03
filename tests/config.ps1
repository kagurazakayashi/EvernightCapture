<#
.SYNOPSIS
    用户偏好配置（ECAPTURE.ini）的回归判据：离线解析/合并层 + 有限真机层。
.DESCRIPTION
    这一条功能要判的是"一份磁盘上的偏好文件，最后到底以哪一档来源进了生产链路"，判据分两层：

      1) 离线层：build\ecapture-config-tests.exe（源码 tests\config_state.cpp）。判生产的
         src\Config.cpp（UTF-8/BOM/CRLF、整行注释与不许行内注释、成对引号不转义、重复节与键、
         未知节与键、缺等号、不闭合引号、NUL、单行与整文件上限）与逐键判定（版本、只认 true/false
         的布尔、枚举、严格十进制与区间），合并那一步一律通过真实入口 ParseCommandLine 跑：
         CLI > INI > 内置、显式 false/0/unset 与"没写"分得开、输出格式那条优先级链、裁剪/缩放整组
         替换与取消、详略成一个组、只读查询不被截图那一级污染、坏配置时 help/version/--no-config
         仍然可用。
      2) 真机层：把 ECAPTURE.EXE 复制进本次自建的临时目录（配置跟着"实际运行的那一个 exe"走，
         所以位置必须可控），用**自建、无隐私**的 fixture 窗口（--capture wgc + --yes：读的是那扇
         窗口自己的画面，不弹确认框、也不拍任何桌面像素）判：--show-config 真的什么都不建、
         默认位置的 absent/loaded/failed 三种下场、history off/on 与结果里那一格一致、
         配置里的 overwrite=false 真的挡住覆盖、配置里的 scale 上限真的进了出图链路，
         以及构建清理与模板复制都不碰那份用户配置。

    如实记未验证的：
      * 用户真实安装目录里的配置：本套件一律在本次自建的临时目录里跑，不改用户那份文件。
      * 安装生命周期（首次创建 / 升级保留 / 卸载保留）由 tests\installer-package.ps1 与
        tests\install-lifecycle.ps1 各自判，不在这里重复。
      * HDR / 多屏 / 真人答"否"那一路：与本条功能无关，也仍是如实记未验证（见 AGENTS.md）。
.EXAMPLE
    .\tests\config.ps1
    .\tests\config.ps1 -SkipState     # 只跑真机层
    .\tests\config.ps1 -SkipReal      # 只跑离线判据层
    .\tests\config.ps1 -Keep          # 保留临时目录以便人眼看
#>
param(
    [string]$Exe,
    [switch]$SkipState,
    [switch]$SkipReal,
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
$Exe = Initialize-EcHarness -Exe $Exe
Set-EcDpiAware
Reset-EcSuite

$root = Get-EcRepoRoot
$stateExe = Join-Path $root 'build\ecapture-config-tests.exe'
$template = Join-Path $root 'resources\ECAPTURE.example.ini'

$run = New-EcRunDir -Tag 'config'
Write-Host "本次临时目录：$($run.Path)"

function New-EcExeCopy {
    param([Parameter(Mandatory)][string]$Leaf)
    $dir = Join-Path $run.Path $Leaf
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dest = Join-Path $dir 'ECAPTURE.EXE'
    Copy-Item -LiteralPath $Exe -Destination $dest -Force
    $a = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
    $b = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    Assert-Ec ($a -eq $b) "复制出来的 exe 与本次被测产物哈希不一致（$Leaf）"
    return [pscustomobject]@{ Dir = $dir; Exe = $dest; Ini = (Join-Path $dir 'ECAPTURE.ini');
                              History = (Join-Path $dir 'history') }
}

# 写一份本次自建的配置：一律 UTF-8 无 BOM + CRLF（生产两种换行都收，这里固定一种就好）。
function Write-EcIni {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Body)
    [IO.File]::WriteAllText($Path, $Body, (New-Object Text.UTF8Encoding($false)))
    return $Path
}

function Get-EcDirLeafs {
    param([Parameter(Mandatory)][string]$Dir)
    if (-not (Test-Path -LiteralPath $Dir)) { return @() }
    return @(Get-ChildItem -LiteralPath $Dir -Force | ForEach-Object { $_.Name } | Sort-Object)
}

try {
    # =========================================================================
    Write-Host "`n=== 1) 离线层：语法、逐键判定与合并（判的是生产本体） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '配置的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-EcStateBinary -Root $root -Exe $stateExe)) {
            Assert-Ec $false "缺少离线判据程序 $stateExe：源码树里先跑一次 .\build.ps1"
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 180000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "配置判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '共 (\d+) 项检查，失败 (\d+)')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "配置判据有失败项：$tail"
        Assert-Ec ([int]$m.Groups[1].Value -ge 140) "配置判据条数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  PASS  配置离线判据全绿：$tail" -ForegroundColor DarkGreen
    }

    # =========================================================================
    Write-Host "`n=== 2) 入库模板本身是一份读得通的配置 ==="
    # =========================================================================
    if (-not (Test-Path -LiteralPath $template)) {
        Assert-Ec $false "缺少入库模板 resources\ECAPTURE.example.ini"
    }
    $t1 = New-EcExeCopy -Leaf 'tpl'
    $r = Invoke-EcProcess -FilePath $t1.Exe -TimeoutMs 30000 `
         -Arguments @('--show-config', '--config', $template)
    $j = $null
    try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $j) "模板当配置读要能通（exit=$($r.Exit)）：$($r.Stderr)"
    Assert-Ec ($j.contract -eq 'configview') "配置视图的契约名不对：$($j.contract)"
    Assert-Ec ($j.config.state -eq 'loaded') "模板没被读到：$($j.config.state)"
    Assert-Ec (@($j.settings).Count -ge 20) "模板里报出来的项数不对劲：$(@($j.settings).Count)"
    # 模板里每条赋值都写成注释：复制一份出来当配置，语义上必须**等价于没有配置文件**。
    # 这一条判的是安装现场：首次部署会从模板独占创建 ECAPTURE.ini，那份文件不许把任何一项
    # 点亮成"用户提出过的要求"（否则安装版与裸装版就不是同一个程序，given/explicit 那组契约
    # 会在用户什么都没做过的情况下翻转）。
    $notBuiltin = @($j.settings | Where-Object { $_.source -ne 'builtin' })
    Assert-Ec ($notBuiltin.Count -eq 0) `
        "模板不该让任何一项变成显式要求：$(($notBuiltin | ForEach-Object { $_.key + '=' + $_.source }) -join ', ')"
    # 模板给的是出厂默认：任何一项的实际值都应与它自己的出厂语义一致（改默认值时这里会挡住）。
    $byKey = @{}
    foreach ($s in @($j.settings)) { $byKey[$s.key] = $s }
    Assert-Ec ($byKey['capture.method'].value -eq 'wgc') "模板的默认取法不是 wgc"
    Assert-Ec ($byKey['capture.cursor'].value -eq 'unset') '模板把默认光标写成了提过要求'
    Assert-Ec ($byKey['output.format'].value -eq 'auto') "模板的默认格式不是 auto"
    Assert-Ec ($byKey['history.enabled'].value -eq 'true') "模板把历史默认关掉了"
    Assert-Ec ($byKey['diagnostics.integrity_hints'].value -eq 'true') "模板把完整性解释默认关了"
    Assert-Ec ($byKey['query.window_list_limit'].value -eq '50') "模板的分页默认不是 50"
    # 模板旁边不许长出用户配置或历史目录：它是一份说明，不是一次部署。
    Assert-Ec (@(Get-EcDirLeafs -Dir $t1.Dir) -notcontains 'history') "读模板顺手建出了 history 目录"

    # =========================================================================
    Write-Host "`n=== 3) 位置判据、只读性质与排障入口（真机） ==="
    # =========================================================================
    if ($SkipReal) {
        Skip-Ec '配置的集成判据' '调用方给了 -SkipReal'
    } else {
    $t2 = New-EcExeCopy -Leaf 'loc'

    # 默认位置没有文件：absent 是正常状态，不报错、不写文件。
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 -Arguments @('--show-config')
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $j.config.state -eq 'absent') `
        "默认位置没文件时应报 absent（exit=$($r.Exit) state=$($j.config.state)）"
    Assert-Ec ($j.config.selection -eq 'default') "选择来源不对：$($j.config.selection)"
    Assert-Ec ([bool]$j.config.file) "absent 也要说清它去找的是哪一条路径"
    Assert-Ec (-not (Test-Path -LiteralPath $t2.Ini)) "只读查询把一份配置写了出来"
    Assert-Ec (@(Get-EcDirLeafs -Dir $t2.Dir) -notcontains 'history') "只读查询建出了历史目录"
    foreach ($s in @($j.settings)) { Assert-Ec ($s.source -eq 'builtin') "没读到时来源该全是 builtin：$($s.key)" }

    # --no-config：本次一条都不读。
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 -Arguments @('--show-config', '--no-config')
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $j.config.state -eq 'not_read' -and $j.config.selection -eq 'none') `
        "--no-config 应报 not_read / none（$($j.config.state) / $($j.config.selection)）"
    Assert-Ec (-not $j.config.file) "--no-config 不该报出一个文件路径"

    # 带着 --out 观察那一项偏好时，这份文档仍然恒在 stdout：这一路不产图，没有两条流可挤。
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 `
         -Arguments @('--show-config', '--out', '-')
    Assert-Ec ($r.Exit -eq 0 -and [bool]$r.Stdout.Trim()) "配置视图不许因为 --out - 就改道到 stderr"
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($j.contract -eq 'configview') "那份文档要能按 stdout 读到：$($r.Stderr)"
    $outItem = @($j.settings | Where-Object { $_.key -eq 'output.path' })[0]
    Assert-Ec ($outItem.value -eq '-' -and $outItem.source -eq 'cli') `
        "观察到的输出偏好要连来源一起报对：$($outItem | ConvertTo-Json -Compress)"

    # 点名的文件不在：报错、不回退，且结果在 stderr（stdout 留给图片那一路）。
    $missing = Join-Path $t2.Dir 'no-such\ECAPTURE.ini'
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 `
         -Arguments @('--show-config', '--config', $missing)
    Assert-Ec ($r.Exit -eq 8) "读不出那一类应是 I/O 失败 8（实得 $($r.Exit)）"
    Assert-Ec (-not $r.Stdout.Trim()) "结构化错误不许写进 stdout"
    $e = $null; try { $e = $r.Stderr | ConvertFrom-Json } catch { }
    Assert-Ec (@($e.errors | Where-Object { $_.code -eq 'config.not_found' }).Count -eq 1) `
        "点名的配置不在要报 config.not_found：$($r.Stderr)"

    # 默认位置那份坏了：读它才失败，而 help / version / --no-config 三条出路都还在。
    [void](Write-EcIni -Path $t2.Ini -Body "[general]`r`nlanguage = auto`r`noops`r`n")
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 -Arguments @('--show-config')
    Assert-Ec ($r.Exit -eq 1) "坏配置应是用法错 1（实得 $($r.Exit)）"
    $e = $null; try { $e = $r.Stderr | ConvertFrom-Json } catch { }
    $bad = @($e.errors | Where-Object { $_.code -eq 'config.syntax' })
    Assert-Ec ($bad.Count -eq 1 -and $bad[0].line -eq 3) "要报出坏在哪一行：$($r.Stderr)"
    Assert-Ec ([bool]$bad[0].option) "配置诊断要带文件位置"
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 -Arguments @('--help')
    Assert-Ec ($r.Exit -eq 3 -and $r.Stdout -match 'EvernightCapture') `
        "配置坏了 --help 也要照常给出（exit=$($r.Exit)）"
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 -Arguments @('--version')
    Assert-Ec ($r.Exit -eq 0 -and $r.Stdout -match 'EvernightCapture') "配置坏了 --version 也要照常"
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 `
         -Arguments @('--capabilities', '--no-config')
    Assert-Ec ($r.Exit -eq 0) "--no-config 要能在坏配置的环境里照常自检（exit=$($r.Exit)）"
    $r = Invoke-EcProcess -FilePath $t2.Exe -TimeoutMs 30000 -Arguments @('--class', 'Shell_TrayWnd')
    Assert-Ec ($r.Exit -eq 1) "坏的默认配置不许被'按默认值拍一张'绕过去（exit=$($r.Exit)）"

    # 裸运行不因为一份配置就变成截图：即使配置里写满了偏好也一样。
    [void](Write-EcIni -Path $t2.Ini -Body "[capture]`r`nmethod = wgc`r`n[output]`r`npath = bare.png`r`n")
    $r = Invoke-EcProcess -FilePath $t2.Exe -WorkingDirectory $t2.Dir -TimeoutMs 30000 -Arguments @()
    Assert-Ec ($r.Exit -eq 2 -or $r.Exit -eq 3) "无条件时必须是帮助而不是截图（exit=$($r.Exit)）"
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $t2.Dir 'bare.png'))) `
        "配置里的输出路径不该让裸运行长出一张图"

    # =========================================================================
    Write-Host "`n=== 4) 配置真的进生产链路：history 那一格与覆盖判据（fixture 窗口） ==="
    # =========================================================================
    $class = 'EcCfgWin' + ($run.Leaf -replace '[^a-z0-9]', '')
    $title = 'EC-CFG-' + ($run.Leaf -replace '[^a-z0-9]', '')
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '150,150,760,520' -Seed 5
    Write-Host ("窗口 PID={0} HWND={1}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd))
    Start-Sleep -Milliseconds 500

    $t3 = New-EcExeCopy -Leaf 'shot'
    $outDir = Join-Path $run.Path 'shots'
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    # 4a) 配置关掉历史：这一步没开始，于是那一格是 disabled，而且一棵 history 都不建。
    [void](Write-EcIni -Path $t3.Ini -Body "[history]`r`nenabled = false`r`n")
    $a = Join-Path $outDir 'off.png'
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 60000 `
         -Arguments @('--title', $title, '--capture', 'wgc', '--yes', $a)
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $j.captured -eq 1) "配置驱动的截图没出图（exit=$($r.Exit)）：$($r.Stderr)"
    Assert-Ec ($j.images[0].history.status -eq 'disabled') `
        "关闭那一档要报 disabled，不能是 failed 也不能假装 saved：$($j.images[0].history | ConvertTo-Json -Compress)"
    Assert-Ec (-not $j.images[0].history.code) "主动关闭没有'哪一步失败'可报"
    Assert-Ec (-not (Test-Path -LiteralPath $t3.History)) "关掉历史却建出了历史目录"

    # 4b) 命令行再覆盖回 on：CLI > INI 这件事要在同一次真截图上看得见。
    $b = Join-Path $outDir 'on.png'
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 60000 `
         -Arguments @('--title', $title, '--capture', 'wgc', '--yes', '--history', 'on', $b)
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $j.images[0].history.status -eq 'saved') `
        "--history on 要覆盖配置里的 false：$($j.images[0].history | ConvertTo-Json -Compress)"
    Assert-Ec (Test-Path -LiteralPath $j.images[0].history.file) "报了 saved 而磁盘上没有那一份"
    $h1 = (Get-FileHash -LiteralPath $b -Algorithm SHA256).Hash
    $h2 = (Get-FileHash -LiteralPath $j.images[0].history.file -Algorithm SHA256).Hash
    Assert-Ec ($h1 -eq $h2) '副本与主交付不是同一份字节'

    # 4c) 配置打开历史（不再给命令行开关）：默认那份 enabled=true 的语义同样由配置承担。
    [void](Write-EcIni -Path $t3.Ini -Body "[history]`r`nenabled = true`r`n")
    $c = Join-Path $outDir 'cfg-on.png'
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 60000 `
         -Arguments @('--title', $title, '--capture', 'wgc', '--yes', $c)
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($j.images[0].history.status -eq 'saved') "配置打开历史时副本要落地"

    # 4d) 配置里的覆盖保护是真的：目标已存在时不许盖掉，也不许"那就换个名字"。
    [void](Write-EcIni -Path $t3.Ini -Body "[output]`r`noverwrite = false`r`n")
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 60000 `
         -Arguments @('--title', $title, '--capture', 'wgc', '--yes', $c)
    Assert-Ec ($r.Exit -eq 8) "配置里 overwrite=false 时已存在的目标要挡住（exit=$($r.Exit)）"
    # 输出是文件时结果 JSON 恒在 stdout（--out 不是 - 就不许把文字混进图片那条流）
    $o = $null; try { $o = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec (@($o.errors | Where-Object { $_.code -eq 'io.file_exists' }).Count -eq 1) `
        "挡住的那一条应是 io.file_exists：$($r.Stdout)"

    # 4e) 配置里的缩放上限真的进了出图链路（不是只被读出来）。
    [void](Write-EcIni -Path $t3.Ini -Body "[scale]`r`nmax_width = 200`r`n[history]`r`nenabled = false`r`n")
    $d = Join-Path $outDir 'scaled.png'
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 60000 `
         -Arguments @('--title', $title, '--capture', 'wgc', '--yes', $d)
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $j.images[0].width -le 200) `
        "配置里的 max_width=200 要真的缩出来（实得 $($j.images[0].width)）"
    Assert-Ec ($j.images[0].history.status -eq 'disabled') '同一份配置里那条 history=false 也要一起生效'

    # 4f) 配置提出的光标要求是真的要求：换成兑现不了的通道时解析期就拒，不静默降级。
    [void](Write-EcIni -Path $t3.Ini -Body "[capture]`r`nmethod = printwindow`r`ncursor = include`r`n")
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 30000 `
         -Arguments @('--title', $title, '--yes', (Join-Path $outDir 'never.png'))
    Assert-Ec ($r.Exit -eq 1) "配置里的光标要求要参与解析期闸门（exit=$($r.Exit)）"
    $o = $null; try { $o = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec (@($o.errors | Where-Object { $_.code -eq 'capture.cursor_unsupported' }).Count -eq 1) `
        "这一条应是 capture.cursor_unsupported 而不是别的：$($r.Stdout)"
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $outDir 'never.png'))) '被拒的请求不许留下文件'

    # 4g) 只读查询不被配置里截图那一级污染，而 --show-config 仍报得出来源。
    [void](Write-EcIni -Path $t3.Ini -Body "[capture]`r`nmethod = bitblt`r`n[query]`r`nwindow_list_limit = 3`r`n")
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 30000 `
         -Arguments @('--config', $t3.Ini, '--show-config')
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    $capItem = @($j.settings | Where-Object { $_.key -eq 'capture.method' })[0]
    Assert-Ec ($capItem.source -eq 'config' -and $capItem.value -eq 'bitblt') `
        "配置视图要报出这一项来自配置文件：$($capItem | ConvertTo-Json -Compress)"
    $r = Invoke-EcProcess -FilePath $t3.Exe -TimeoutMs 60000 -Arguments @('--list')
    $j = $null; try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r.Exit -eq 0 -and $j.pagination.limit -eq 3) `
        "配置里的默认分页要进 --list，而这一条不被截图那一级判成冲突（exit=$($r.Exit)）"
    Assert-Ec ($j.pagination.limitSource -eq 'config') `
        "来源要说清这个条数来自配置：$($j.pagination.limitSource)"

    # =========================================================================
    Write-Host "`n=== 5) 模板与用户配置在构建与清理里共存 ==="
    # =========================================================================
    # 5a) 构建复制模板这件事不许覆盖已有配置：在 build 旁边放一份本次的哨兵，重跑一次构建。
    #     只在 build\ECAPTURE.ini 本来不存在时做这一条，绝不碰用户已经写好的那份。
    $devIni = Join-Path $root 'build\ECAPTURE.ini'
    $devTemplate = Join-Path $root 'build\ECAPTURE.example.ini'
    if (Test-Path -LiteralPath $devIni) {
        Skip-Ec '构建不覆盖已有开发配置' 'build\ECAPTURE.ini 已经存在（那是用户的开发配置，本判据不碰它）'
    } else {
        $sentinel = '; ECAPTURE-CONFIG-SENTINEL 本次哨兵，判据结束后删除'
        [void](Write-EcIni -Path $devIni -Body $sentinel)
        $bt = Join-Path $root 'build.ps1'
        $rb = Invoke-EcProcess -FilePath 'powershell.exe' -TimeoutMs 600000 `
             -Arguments @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $bt)
        Assert-Ec ($rb.Exit -eq 0) "重跑构建失败（exit=$($rb.Exit)）"
        Assert-Ec ((Get-Content -LiteralPath $devIni -Raw) -eq $sentinel) `
            '构建把已有开发配置覆盖了（模板复制必须只碰 example 那一份）'
        Assert-Ec (Test-Path -LiteralPath $devTemplate) '构建没把模板放到程序旁边'
        Remove-Item -LiteralPath $devIni -Force
    }

    # 5b) 清理同时保留历史与配置，且损坏的配置照样保留（判据只看名字）。
    Import-Module (Join-Path $root 'scripts\build-clean.psm1') -Force
    $cd = Join-Path $run.Path 'cleanme'
    New-Item -ItemType Directory -Force -Path (Join-Path $cd 'history\2026-01-01') | Out-Null
    Set-Content -LiteralPath (Join-Path $cd 'history\2026-01-01\a.png') -Value 'keep'
    Set-Content -LiteralPath (Join-Path $cd 'ECAPTURE.ini') -Value 'history = [[[ 读不通也保留'
    Set-Content -LiteralPath (Join-Path $cd 'build.ninja') -Value 'junk'
    New-Item -ItemType Directory -Force -Path (Join-Path $cd 'CMakeFiles') | Out-Null
    $v = Remove-EcBuildArtifacts -BuildDir $cd
    Assert-Ec ($v.Kept -like '*history*' -and $v.Kept -like '*config*') "清理该两样都留下：$($v.Kept)"
    Assert-Ec (Test-Path -LiteralPath (Join-Path $cd 'history\2026-01-01\a.png')) '清理把历史删了'
    Assert-Ec (Test-Path -LiteralPath (Join-Path $cd 'ECAPTURE.ini')) '清理把损坏的配置当成非配置删了'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $cd 'build.ninja'))) '构建产物没被清掉'

    # 5c) 卸载/清理永远不自动动历史与配置：这一条判的是"工具自己没有删它们的路径"，
    #     所以在本次目录里确认关掉历史也不删旧的那一棵。
    $t4 = New-EcExeCopy -Leaf 'keep'
    New-Item -ItemType Directory -Force -Path (Join-Path $t4.History '2026-01-01') | Out-Null
    $old = Join-Path $t4.History '2026-01-01\old.png'
    Set-Content -LiteralPath $old -Value 'old-history'
    [void](Write-EcIni -Path $t4.Ini -Body "[history]`r`nenabled = false`r`n")
    $r = Invoke-EcProcess -FilePath $t4.Exe -TimeoutMs 30000 `
         -Arguments @('--class', 'Shell_TrayWnd', '--dry-run', '--yes')
    Assert-Ec ($r.Exit -eq 0) "dry-run 失败（exit=$($r.Exit)）"
    Assert-Ec (Test-Path -LiteralPath $old) '关掉历史把已有的那一张删了'
    Assert-Ec ((Get-Content -LiteralPath $old -Raw) -match 'old-history') '已有历史被改写过'
    }
}
finally {
    Stop-EcOwnedWindows
    if ($Keep) { Write-Host "保留本次目录：$($run.Path)" -ForegroundColor Yellow }
    else { Remove-EcRunDir -Run $run }
}

exit (Complete-EcSuite -Title '用户偏好配置 ')
