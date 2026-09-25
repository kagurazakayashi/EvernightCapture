<#
.SYNOPSIS
    截图历史归档（主交付之外那一份独立副本）的回归判据：离线编排层 + 真机 WGC 那一路的落点、
    字节一致、失败归类与构建清理保留。
.DESCRIPTION
    默认开启的功能要判的是"每张完成主交付的图，在**实际运行的那个 exe 旁边**的 history\日期\ 里
    另存一份独立副本"，以及它失败时报告还能不能说清。判据分两层：

      1) 离线层：build\ecapture-history-tests.exe（源码 tests\history_state.cpp）。判生产的
         src\HistoryArchive.cpp + src\Delivery.cpp + src\FileSave.cpp：命名与本地日期同源、
         同刻两张 / 时钟回拨 / 跨午夜、同名独占提交与换名重试、"那个位置是个文件""父路径不存在"
         "路径名不合"的失败归类、主输出名字落在 history 树里时不自覆盖、主图成功而副本失败、
         主图失败与半段标准输出都不发布副本、主图落盘后预算才耗尽时副本不开始、归档抛出东西
         不抹掉交付事实、以及"送进归档的就是那一份编码缓冲"。
      2) 真机层：把 ECAPTURE.EXE 复制进本次自建的临时目录，用**自建、无隐私**的 fixture 窗口
         （--capture wgc + --yes：读的是那扇窗口自己的画面，不弹确认框、也不拍任何桌面像素）跑：
         落点跟着 exe 而不是工作目录或 --out 目录、主输出与副本逐字节 / SHA-256 相同、
         ROI+scale、两目标批次、--out - 二进制那一路、批次里一张成功一张失败、
         只读与参数失败一个历史都不建、重解析点与"位置是文件"时的部分成功、
         以及构建清理把历史留在原地。

    两条如实记未验证的：
      * 真人在确认框上答"否"那一路不建历史：本套件不弹真实确认框、也不替任何人答它（判"没交付
        就不归档"这件事由离线层与 tests\consent.ps1 各自负责）。
      * 真盘满 / 真没权限（要改用户的存储设置或 ACL 才能造）：由离线层的错误归类与真机那两条
        "位置被文件占着""重解析点"代表，不冒充成整机级验收。
.EXAMPLE
    .\tests\history.ps1
    .\tests\history.ps1 -SkipState        # 只跑真机层
    .\tests\history.ps1 -SkipReal         # 只跑离线判据层
    .\tests\history.ps1 -Keep             # 保留临时目录以便人眼看
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
$stateExe = Join-Path $root 'build\ecapture-history-tests.exe'

$run = New-EcRunDir -Tag 'history'
$tag = $run.Leaf -replace '[^a-z0-9]', ''
Write-Host "本次临时目录：$($run.Path)"

# 把被测 exe 复制进本次目录：归档跟着"实际运行的那一个 exe"走，所以要判落点就必须有一份
# 位置可控的副本。复制的是同一个文件（哈希相同），不是重新构建出来的别的东西。
function New-EcExeCopy {
    param([Parameter(Mandatory)][string]$Leaf)
    $dir = Join-Path $run.Path $Leaf
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $dest = Join-Path $dir 'ECAPTURE.EXE'
    Copy-Item -LiteralPath $Exe -Destination $dest -Force
    $a = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
    $b = (Get-FileHash -LiteralPath $dest -Algorithm SHA256).Hash
    Assert-Ec ($a -eq $b) "复制出来的 exe 与本次被测产物哈希不一致（$Leaf）"
    return [pscustomobject]@{ Dir = $dir; Exe = $dest; History = (Join-Path $dir 'history') }
}

function Get-EcHistoryFiles {
    param([Parameter(Mandatory)][string]$HistoryDir)
    if (-not (Test-Path -LiteralPath $HistoryDir)) { return @() }
    return @(Get-ChildItem -LiteralPath $HistoryDir -File -Recurse -Force)
}

# 一段内存字节的 SHA-256（ Compare-Object 去比十几万个字节会把判据自己拖死）
function Get-EcBytesHash {
    param([Parameter(Mandatory)][byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($Bytes)) -replace '-', '') }
    finally { $sha.Dispose() }
}

# 归档名与它自己那个日期目录必须出自同一次取样：名字前 8 位 = 目录名去掉连字符。
# 这条判据不看"今天几号"，所以判据自己跨午夜也不会假失败。
function Test-EcHistoryDateAligned {
    param([Parameter(Mandatory)][string]$Path)
    $leaf = Split-Path -Leaf $Path
    $dir = Split-Path -Leaf (Split-Path -Parent $Path)
    if ($leaf -notmatch '^(\d{8})-\d{6}-') { return $false }
    return ($Matches[1] -eq ($dir -replace '-', ''))
}

# 判据在解析不出 JSON 时必须说清那一次跑成了什么样（起不来 / 超时 / 被谁结束），
# 不然只留一个 exit=-1 谁也分不出是产品没跑起来还是调用器没读到。
function Show-EcRunDiag {
    param($R, [string]$Label)
    $oneLine = { param($s) if (-not $s) { return '' } $t = ($s -replace "`r?`n", ' | '); if ($t.Length -gt 400) { return $t.Substring(0, 400) }; return $t }
    Write-Host ('  [{0}] exit={1} timedOut={2} killed={3} dur={4}ms truncated={5} startError={6}' -f `
                $Label, $R.Exit, $R.TimedOut, $R.KilledBy, $R.DurationMs, $R.Truncated, `
                (& $oneLine $R.StartError)) -ForegroundColor DarkYellow
    if ($R.Stdout) { Write-Host ('    stdout: ' + (& $oneLine $R.Stdout)) -ForegroundColor DarkYellow }
    if ($R.Stderr) { Write-Host ('    stderr: ' + (& $oneLine $R.Stderr)) -ForegroundColor DarkYellow }
}

try {
    # =========================================================================
    Write-Host "`n=== 1) 离线层：归档本体与交付编排（真磁盘 + 注入时钟） ==="
    # =========================================================================
    if ($SkipState) {
        Skip-Ec '历史归档的离线判据' '调用方给了 -SkipState'
    } else {
        if (-not (Test-EcStateBinary -Root $root -Exe $stateExe)) {
            Assert-Ec $false "缺少离线判据程序 $stateExe：源码树里先跑一次 .\build.ps1；安装目录里它必须随包发出（说明打包的依赖闭包没兜住）"
        }
        $st = Invoke-EcProcess -FilePath $stateExe -TimeoutMs 120000
        $lines = @($st.Stdout -split "`r?`n" | Where-Object { $_ })
        $tail = [string]($lines | Select-Object -Last 1)
        Assert-Ec ($st.Exit -eq 0) "历史归档判据没全绿（exit=$($st.Exit)）：$tail"
        $m = [regex]::Match($tail, '共 (\d+) 项检查，失败 (\d+)')
        Assert-Ec $m.Success "读不出摘要：$tail"
        Assert-Ec ([int]$m.Groups[2].Value -eq 0) "历史归档判据有失败项：$tail"
        Assert-Ec ([int]$m.Groups[1].Value -ge 160) "历史归档判据条数不对劲（$($m.Groups[1].Value)），判据被删了？"
        Write-Host "  $tail"
        @($lines | Where-Object { $_ -match 'FAIL' }) | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    }

    if ($SkipReal) {
        Skip-Ec '真机归档与落点' '调用方给了 -SkipReal'
        Skip-Ec '构建清理保留历史' '调用方给了 -SkipReal'
        throw '__SKIP_REAL__'
    }

    # =========================================================================
    Write-Host "`n=== 2) 落点跟着实际运行的 exe；主输出与副本逐字节相同 ==="
    # =========================================================================
    $cap = New-EcExeCopy -Leaf 'ecap'
    $class = "ec-history-$tag"
    $title = "历史归档测试窗口 $class"
    $window = Start-EcWindow -RunDir $run -Class $class -Title $title -Rect '140,140,780,560' -Seed 3
    Write-Host ("窗口 PID={0} HWND={1}" -f $window.Pid, (Get-EcHwndHex $window.Hwnd))
    Start-Sleep -Milliseconds 500

    $outDir = Join-Path $run.Path 'outdir'
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $cwdDir = Join-Path $run.Path 'cwd'
    New-Item -ItemType Directory -Force -Path $cwdDir | Out-Null
    $main = Join-Path $outDir 'shot.png'

    $r = Invoke-EcProcess -FilePath $cap.Exe -WorkingDirectory $cwdDir -TimeoutMs 60000 `
        -Arguments @('--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes', $main)
    $j = $null
    try { $j = $r.Stdout | ConvertFrom-Json } catch { }
    if ($null -eq $j) { Show-EcRunDiag -R $r -Label '2) 第一次截图' }
    Assert-Ec ($null -ne $j) "读不出结果 JSON（exit=$($r.Exit) stderr=$($r.Stderr.Trim())）"
    Assert-Ec ($r.Exit -eq 0) "一张窗口截图应干净完工（exit=$($r.Exit)）"
    Assert-Ec ($j.captured -eq 1) "captured=1"
    $img = @($j.images)[0]
    Assert-Ec ($img.file -eq $main) 'images[].file 还是主输出的那个名字'
    Assert-Ec ($img.history.status -eq 'saved') "images[].history.status 是 saved（实测 $($img.history.status)）"
    Assert-Ec ([bool] $img.history.file) 'saved 那一张有副本文件名'
    Assert-Ec (Test-Path -LiteralPath $img.history.file) '副本真在磁盘上'
    Assert-Ec ((Split-Path -Parent (Split-Path -Parent $img.history.file)) -eq $cap.History) `
        '副本就在实际运行的 exe 旁边那棵 history 里'
    Assert-Ec ([bool](Test-EcHistoryDateAligned $img.history.file)) '归档名与它自己那个日期目录出自同一次本地时间取样'
    Assert-Ec ((Get-FileHash -LiteralPath $main -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $img.history.file -Algorithm SHA256).Hash) `
        '主输出与历史副本逐字节相同（同一份编码字节，不重拍也不重编码）'
    Assert-Ec ($img.history.file -like '*.png') '副本扩展名跟着真实编码（png）'
    $mainBytes = [IO.File]::ReadAllBytes($main)
    $histBytes = [IO.File]::ReadAllBytes($img.history.file)
    Assert-Ec ($mainBytes[0] -eq 0x89 -and $histBytes[0] -eq 0x89) '两边都是 PNG 签名开头'
    $dims = Get-EcImageStats -Path $img.history.file
    Assert-Ec ($dims.Width -eq $img.width -and $dims.Height -eq $img.height) `
        "副本尺寸与 JSON 报的一致（$($dims.Width)x$($dims.Height) vs $($img.width)x$($img.height)）"
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $outDir 'history'))) `
        '没有在主输出目录里顺手建 history'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $cwdDir 'history'))) `
        '没有在调用方的当前目录里建 history'
    Assert-Ec (@(Get-ChildItem -LiteralPath $outDir -Force).Count -eq 1) '输出目录里只有那一张主输出'

    # =========================================================================
    Write-Host "`n=== 3) ROI + scale 那一张也走同一个归档入口 ==="
    # =========================================================================
    $roiMain = Join-Path $outDir 'roi.png'
    $r2 = Invoke-Ecapture -Exe $cap.Exe -TimeoutMs 60000 `
        -Arguments @('--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes',
                    '--roi', '10,10,300,200', '--scale', 'max-width=160', $roiMain)
    $j2 = $null
    try { $j2 = $r2.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r2.Exit -eq 0 -and $j2.captured -eq 1) "ROI+scale 那张应交付（exit=$($r2.Exit)）"
    $img2 = @($j2.images)[0]
    Assert-Ec ($img2.history.status -eq 'saved') '裁剪并缩放那一张同样有历史副本'
    Assert-Ec ((Get-FileHash -LiteralPath $roiMain -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $img2.history.file -Algorithm SHA256).Hash) `
        '副本是**变换之后**那一张（裁过、缩过的字节，不是原始整窗帧）'
    Assert-Ec ($img2.width -le 160) "缩放后的宽度落在天花板之内（实测 $($img2.width)）"
    $dims2 = Get-EcImageStats -Path $img2.history.file
    Assert-Ec ($dims2.Width -eq $img2.width -and $dims2.Height -eq $img2.height) '副本尺寸就是交付尺寸'

    # =========================================================================
    Write-Host "`n=== 4) 两个目标各存各的副本 ==="
    # =========================================================================
    $class2 = "ec-history2-$tag"
    $title2 = "历史归档第二窗口 $class2"
    $window2 = Start-EcWindow -RunDir $run -Class $class2 -Title $title2 -Rect '900,200,1400,620' -Seed 7
    Start-Sleep -Milliseconds 400
    $batchDir = Join-Path $outDir 'batch'
    New-Item -ItemType Directory -Force -Path $batchDir | Out-Null
    $r3 = Invoke-EcProcess -FilePath $cap.Exe -TimeoutMs 60000 -Arguments @(
        '--lang', 'zh-CN', '--class', $class, '--class', $class2, '--all',
        '--capture', 'wgc', '--yes', (Join-Path $batchDir 'pair_%i.png'))
    $j3 = $null
    try { $j3 = $r3.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r3.Exit -eq 0 -and $j3.captured -eq 2) "两扇窗口各交付一张（exit=$($r3.Exit) captured=$($j3.captured)）"
    $files3 = @($j3.images | ForEach-Object { $_.history.file })
    Assert-Ec (@($files3 | Where-Object { $_ }).Count -eq 2) '两张各自都有副本文件名'
    Assert-Ec (($files3[0]) -ne ($files3[1])) '批次里两张的副本名字不同'
    $pairOk = $true
    foreach ($im in @($j3.images)) {
        if ((Get-FileHash -LiteralPath $im.file -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $im.history.file -Algorithm SHA256).Hash) { $pairOk = $false }
    }
    Assert-Ec $pairOk '批次里每一张的主输出都与自己的副本逐字节相同'
    Assert-Ec (@(Get-EcHistoryFiles -HistoryDir $cap.History).Count -eq 4) `
        '到目前为止副本共 4 张（2+1+批次 2 里已存的都算上）'

    # =========================================================================
    Write-Host "`n=== 5) 批次里一张交付、一张写不下去：失败那张不进 images 也不进历史 ==="
    # =========================================================================
    # --all 那两个序号是按命中顺序排的，而命中顺序（Z 序）不是本判据要赌的东西。所以两次各占住
    # 一个名字：无论哪一扇窗口拿到那个序号，那一次都必然是"一张成交、一张撞名失败"。
    function Invoke-EcMixRun {
        param([Parameter(Mandatory)][string]$Sub, [Parameter(Mandatory)][int]$BlockedOrdinal)
        $mixDir = Join-Path $outDir $Sub
        New-Item -ItemType Directory -Force -Path $mixDir | Out-Null
        $blocked = Join-Path $mixDir ("mix_{0}.png" -f $BlockedOrdinal)
        Set-Content -LiteralPath $blocked -Value 'occupy' -Encoding ASCII
        $before = @(Get-EcHistoryFiles -HistoryDir $cap.History).Count
        $rr = Invoke-EcProcess -FilePath $cap.Exe -TimeoutMs 60000 -Arguments @(
            '--lang', 'zh-CN', '--class', $class, '--class', $class2, '--all', '--no-overwrite',
            '--capture', 'wgc', '--yes', (Join-Path $mixDir 'mix_%i.png'))
        $jj = $null
        try { $jj = $rr.Stdout | ConvertFrom-Json } catch { }
        Assert-Ec ($null -ne $jj) "读不出批次结果 JSON（$Sub，exit=$($rr.Exit)）"
        Assert-Ec ($jj.captured -eq 1) "只有一张真交付（$Sub：captured=$($jj.captured)）"
        Assert-Ec ($rr.Exit -eq 7) "已交付 + 有错误 = 部分成功 7（$Sub：实测 $($rr.Exit)）"
        $codes = @($jj.errors | ForEach-Object { $_.code })
        Assert-Ec ($codes -contains 'io.file_exists') `
            "失败那张留的是它自己那条写失败（$Sub：$($codes -join ',')）"
        Assert-Ec ((Get-Content -LiteralPath $blocked -Raw -Encoding ASCII).Trim() -eq 'occupy') `
            "被占住的那个名字没有被改写（$Sub）"
        Assert-Ec (@(Get-EcHistoryFiles -HistoryDir $cap.History).Count -eq ($before + 1)) `
            "失败那一张不发布副本：$Sub 那一次副本只多了一张"
        $one = @($jj.images)[0]
        Assert-Ec ($one.history.status -eq 'saved') "成交那一张有自己的副本（$Sub）"
    }
    Invoke-EcMixRun -Sub 'mix-a' -BlockedOrdinal 1
    Invoke-EcMixRun -Sub 'mix-b' -BlockedOrdinal 2

    # =========================================================================
    Write-Host "`n=== 6) --out - 二进制那一路：stdout 只有图片字节，副本另存 ==="
    # =========================================================================
    $r5 = Invoke-EcProcess -FilePath $cap.Exe -TimeoutMs 60000 -Arguments @(
        '--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes', '--format', 'jpeg',
        '--out', '-')
    $j5 = $null
    try { $j5 = $r5.Stderr | ConvertFrom-Json } catch { }
    Assert-Ec ($null -ne $j5) "stdout 被图片占了，JSON 该整份走 stderr（exit=$($r5.Exit)）"
    Assert-Ec ($r5.Exit -eq 0) "标准输出那一路干净完工（exit=$($r5.Exit)）"
    $img5 = @($j5.images)[0]
    Assert-Ec ($img5.file -eq '-') 'images[].file 仍是那条流'
    Assert-Ec ($img5.history.status -eq 'saved') '标准输出一路也经过同一个归档入口'
    Assert-Ec ($r5.StdoutBytes.Count -eq $img5.bytes) 'stdout 的字节数就是 JSON 报的那一个数'
    Assert-Ec ($r5.StdoutBytes[0] -eq 0xFF -and $r5.StdoutBytes[1] -eq 0xD8) 'stdout 是 JPEG（首两字节 FFD8）'
    Assert-Ec ($img5.history.file -like '*.jpg') '副本扩展名跟着真实编码（jpeg）'
    $hist5 = [IO.File]::ReadAllBytes($img5.history.file)
    Assert-Ec ((Get-EcBytesHash -Bytes $r5.StdoutBytes) -eq (Get-EcBytesHash -Bytes $hist5)) `
        '副本字节与管道那边收到的完全相同'

    # =========================================================================
    Write-Host "`n=== 7) 只读查询与参数失败：一个历史都不建 ==="
    # =========================================================================
    $ro = New-EcExeCopy -Leaf 'readonly'
    $queries = @(
        @('--capabilities'), @('--diagnostics'), @('--screens'), @('--version'),
        @('--help'), @('--class', "no-such-class-$tag", '--dry-run', 'x.png'),
        @('--class', "no-such-class-$tag", 'x.png'),          # 无匹配：一张都没截
        @('--monitor', 'id:nosuchid', 'x.png'),               # 屏幕标识不在桌面上
        @('--roi', '99999,0,10,10', '--class', $class, 'x.png'),   # 参数不合
        @('--list', '--class', 'Shell_TrayWnd')             # 只读窗口发现（成功那一路）
    )
    foreach ($q in $queries) {
        # 工作目录就用这个副本自己的目录：万一哪条路真的写出了文件，下一步的"目录里只剩 exe"
        # 就会当场抓到，而不是留在仓库里没人看见。退出码这里不判（各条本来不同），
        # 要判的是"没有因此多出一次截图、也就没有多出历史"。
        $qr = Invoke-EcProcess -FilePath $ro.Exe -WorkingDirectory $ro.Dir -TimeoutMs 60000 -Arguments $q
        if ($null -eq $qr) { continue }
    }
    Assert-Ec (-not (Test-Path -LiteralPath $ro.History)) `
        '只读查询、帮助、参数错与无匹配都不该建出 history 目录'
    Assert-Ec (@(Get-ChildItem -LiteralPath $ro.Dir -Force).Count -eq 1) `
        '那次运行除了 exe 自己什么都没留下'

    # =========================================================================
    Write-Host "`n=== 8) 归档那一路说不通：主图保住，副本如实报失败（部分成功 7）==="
    # =========================================================================
    # 8a) history 那个位置是一个重解析点（junction）。跟着它写等于把截图放进谁也没批准过的别处，
    #     所以产品必须停下并如实报，而不是"那就顺着写"或换个目录。
    $rp = New-EcExeCopy -Leaf 'reparse'
    $elsewhere = Join-Path $run.Path 'reparse-target'
    New-Item -ItemType Directory -Force -Path $elsewhere | Out-Null
    $junctionBuilt = $true
    try {
        New-Item -ItemType Junction -Path $rp.History -Target $elsewhere -ErrorAction Stop | Out-Null
    } catch {
        $junctionBuilt = $false
        # 本机不给非管理员建 junction 时如实 SKIP，不拿"没跑成"冒充"判过了"。
        Skip-Ec '重解析点那一档' "建 junction 失败：$($_.Exception.Message)"
    }
    if ($junctionBuilt) {
        $rpMain = Join-Path $outDir 'reparse.png'
        $r6 = Invoke-EcProcess -FilePath $rp.Exe -TimeoutMs 60000 -Arguments @(
            '--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes', $rpMain)
        $j6 = $null
        try { $j6 = $r6.Stdout | ConvertFrom-Json } catch { }
        Assert-Ec ($r6.Exit -eq 7) "主图已交付而副本写不下去 = 部分成功 7（实测 $($r6.Exit)）"
        Assert-Ec ($j6.captured -eq 1) '主图那张仍然算交付'
        $img6 = @($j6.images)[0]
        Assert-Ec ($img6.history.status -eq 'failed') "副本如实报 failed（实测 $($img6.history.status)）"
        Assert-Ec ($img6.history.code -eq 'history.unavailable') `
            "码是 history.unavailable（实测 $($img6.history.code)）"
        Assert-Ec (-not $img6.history.file) '失败时不给一个副本名字'
        $codes6 = @($j6.errors | ForEach-Object { $_.code })
        Assert-Ec ($codes6 -contains 'history.unavailable') "errors 里有同码那一条（$($codes6 -join ',')）"
        $herr = @($j6.errors | Where-Object { $_.code -eq 'history.unavailable' })[0]
        Assert-Ec ($herr.stage -eq 'history') 'stage 说的是归档那一段'
        Assert-Ec ($herr.message -match 'reparse') 'ASCII 原因 token 说得出是哪一路走不通'
        Assert-Ec (@(Get-ChildItem -LiteralPath $elsewhere -Force).Count -eq 0) `
            '没有穿过那个重解析点往别处写任何一个文件'
        Assert-Ec (Test-Path -LiteralPath $rpMain) '主输出那张图原样留着'
        # -q 也不许把副本失败藏起来（errors 与 images 段本来就不被抑制）
        $r6q = Invoke-EcProcess -FilePath $rp.Exe -TimeoutMs 60000 -Arguments @(
            '--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes', '-q', $rpMain)
        $j6q = $null
        try { $j6q = $r6q.Stdout | ConvertFrom-Json } catch { }
        Assert-Ec ($j6q.images[0].history.status -eq 'failed') '--quiet 藏不住历史失败的 status'
        Assert-Ec (@($j6q.errors | ForEach-Object { $_.code }) -contains 'history.unavailable') `
            '--quiet 藏不住历史失败那条 errors'
    }

    # 8b) history 那个位置是一个文件：不删它、不改它，也不换个地方存
    $af = New-EcExeCopy -Leaf 'asfile'
    [IO.File]::WriteAllText($af.History, 'NOT-A-DIRECTORY')   # 逐字节写，不留 Set-Content 的行尾
    $afMain = Join-Path $outDir 'asfile.png'
    $r7 = Invoke-EcProcess -FilePath $af.Exe -TimeoutMs 60000 -Arguments @(
        '--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes', $afMain)
    $j7 = $null
    try { $j7 = $r7.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r7.Exit -eq 7) "位置被文件占住时也是部分成功 7（实测 $($r7.Exit)）"
    $img7 = @($j7.images)[0]
    Assert-Ec ($img7.history.status -eq 'failed' -and $img7.history.code -eq 'history.unavailable') `
        "副本如实报 history.unavailable（$($img7.history.status) / $($img7.history.code)）"
    Assert-Ec ([IO.File]::ReadAllText($af.History) -eq 'NOT-A-DIRECTORY') `
        '占位那个文件没有被删、也没有被改'

    # =========================================================================
    Write-Host "`n=== 9) 主输出名字就写在 history 树里：不自覆盖 ==="
    # =========================================================================
    # 让主输出直接落在归档那棵树的同一个日期目录里（名字由用户挑），副本必须另存一个名字，
    # 而主输出那一份不被改写；两张都在，各是各的字节。
    $dayDir = @(Get-ChildItem -LiteralPath $cap.History -Directory -Force)[0].FullName
    $manual = Join-Path $dayDir 'manual-target.png'
    $r8 = Invoke-EcProcess -FilePath $cap.Exe -TimeoutMs 60000 -Arguments @(
        '--lang', 'zh-CN', '--title', $title, '--capture', 'wgc', '--yes', $manual)
    $j8 = $null
    try { $j8 = $r8.Stdout | ConvertFrom-Json } catch { }
    Assert-Ec ($r8.Exit -eq 0 -and $j8.captured -eq 1) "写进 history 树里那一张照样交付（exit=$($r8.Exit)）"
    $img8 = @($j8.images)[0]
    Assert-Ec ($img8.file -eq $manual) 'images[].file 就是用户写的那个名字'
    Assert-Ec ('saved skipped' -match [regex]::Escape($img8.history.status)) `
        "副本要么另存一份要么如实跳过，两种都说得清（实测 $($img8.history.status)）"
    if ($img8.history.status -eq 'saved') {
        Assert-Ec ($img8.history.file -ne $manual) '副本没有顶掉用户那张主输出的名字'
    }
    Assert-Ec (Test-Path -LiteralPath $manual) '主输出那一份还在'
    $dims8 = Get-EcImageStats -Path $manual
    Assert-Ec ($dims8.Width -eq $img8.width) '主输出内容仍然是那一张（没有被副本改写）'

    # =========================================================================
    Write-Host "`n=== 10) 构建清理保留历史（生产判据本体）==="
    # =========================================================================
    # 这一节判的是源码开发期的清理策略（build\ 与 build\history 都在源码树里），需要
    # scripts\build-clean.psm1。安装目录里既不发构建脚本、也没有 build\ 要清，所以那里
    # 如实记 SKIP —— 不是"这条也验过了"。离线模式（-SkipReal）本来就不会走到这里。
    $cleanModule = Join-Path $root 'scripts\build-clean.psm1'
    if (-not (Test-Path -LiteralPath $cleanModule)) {
        Skip-Ec '构建清理保留历史' '这里没有 scripts\build-clean.psm1（安装目录不发构建脚本，那是源码开发期检查）'
        throw '__SKIP_CLEAN__'
    }
    Import-Module $cleanModule -Force

    $fake = Join-Path $run.Path 'fakebuild'
    $fakeHist = Join-Path $fake 'history\2026-01-02'
    New-Item -ItemType Directory -Force -Path (Join-Path $fake 'CMakeFiles\x') | Out-Null
    New-Item -ItemType Directory -Force -Path $fakeHist | Out-Null
    Set-Content -LiteralPath (Join-Path $fake 'ecapture.exe') -Value 'artifact' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $fake 'CMakeCache.txt') -Value 'cache' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $fake 'CMakeFiles\x.o') -Value 'obj' -Encoding ASCII
    $sentinel = Join-Path $fakeHist 'kept.png'
    $sentinelBody = 'HISTORY-SENTINEL-' + [Guid]::NewGuid().ToString('N')
    Set-Content -LiteralPath $sentinel -Value $sentinelBody -Encoding ASCII

    $v = Remove-EcBuildArtifacts -BuildDir $fake
    Assert-Ec $v.Removed '清理没做成'
    Assert-Ec ($v.Kept -eq 'history') '清理没有把历史认出来（Kept 不是 history）'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $fake 'ecapture.exe'))) '清理后构建产物还在'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $fake 'CMakeCache.txt'))) '清理后 CMake 缓存还在'
    Assert-Ec (-not (Test-Path -LiteralPath (Join-Path $fake 'CMakeFiles'))) '清理后 CMakeFiles 还在'
    Assert-Ec (Test-Path -LiteralPath $fake) 'build 目录本身被删掉了（历史还在里面，必须留着）'
    Assert-Ec (Test-Path -LiteralPath $sentinel) '清理把截图历史删掉了'
    Assert-Ec ((Get-Content -LiteralPath $sentinel -Raw -Encoding ASCII).Trim() -eq $sentinelBody.Trim()) `
        '清理改写了历史里的内容'

    # 不能安全保留时明确拒绝，且不静默删除 / 不静默搬移
    $rej = Join-Path $run.Path 'rejbuild'
    New-Item -ItemType Directory -Force -Path $rej | Out-Null
    Set-Content -LiteralPath (Join-Path $rej 'a.obj') -Value 'x' -Encoding ASCII
    [IO.File]::WriteAllText((Join-Path $rej 'history'), 'a-file-occupies-the-name')
    $v2 = Remove-EcBuildArtifacts -BuildDir $rej
    Assert-Ec (-not $v2.Removed) '那个位置是文件时清理居然动手删了'
    Assert-Ec (Test-Path -LiteralPath (Join-Path $rej 'a.obj')) '拒绝清理时把别的东西也带走了'
    Assert-Ec ([IO.File]::ReadAllText((Join-Path $rej 'history')) -eq 'a-file-occupies-the-name') `
        '拒绝清理时还是改了那个名字'
    Assert-Ec ([bool] $v2.Reason) '拒绝时没给出为什么'

    $rej2 = Join-Path $run.Path 'rejbuild2'
    $rej2Target = Join-Path $run.Path 'rejbuild2-target'
    New-Item -ItemType Directory -Force -Path $rej2, $rej2Target | Out-Null
    Set-Content -LiteralPath (Join-Path $rej2 'b.obj') -Value 'x' -Encoding ASCII
    $rej2Hist = Join-Path $rej2 'history'
    $rej2Built = $true
    try { New-Item -ItemType Junction -Path $rej2Hist -Target $rej2Target -ErrorAction Stop | Out-Null }
    catch { $rej2Built = $false; Skip-Ec '清理遇到重解析历史目录那一档' "建 junction 失败：$($_.Exception.Message)" }
    if ($rej2Built) {
        $v3 = Remove-EcBuildArtifacts -BuildDir $rej2
        Assert-Ec (-not $v3.Removed) '历史目录是重解析点时清理居然动手删了'
        Assert-Ec (Test-Path -LiteralPath (Join-Path $rej2 'b.obj')) '拒绝时把别的东西也带走了'
        Assert-Ec (@(Get-ChildItem -LiteralPath $rej2Target -Force).Count -eq 0) '清理穿过了那个链接'
    }

    # 没有历史时行为照旧（整棵删掉），不因为加了这一层就留下空目录
    $plain = Join-Path $run.Path 'plainbuild'
    New-Item -ItemType Directory -Force -Path $plain | Out-Null
    Set-Content -LiteralPath (Join-Path $plain 'c.obj') -Value 'x' -Encoding ASCII
    $v4 = Remove-EcBuildArtifacts -BuildDir $plain
    Assert-Ec ($v4.Removed -and -not $v4.BuildDirKept) '没有历史要保留时应当把 build 整棵删掉'

    # 全新检出（还没有 build\）上跑 -Clean 是**正常空操作**，不是"清理被拒绝"：这一条抓的是
    # "把没做成与被拒绝混成一格，于是构建脚本在干净树上直接中断"那个现场（build-path 抓到过）。
    $v5 = Remove-EcBuildArtifacts -BuildDir (Join-Path $run.Path 'never-existed')
    Assert-Ec $v5.NothingToDo '"build\ 本来就不存在"被当成了失败（NothingToDo 没为真）'
    Assert-Ec (-not $v5.Removed) '空操作不该报告"已经删过东西"'
    Assert-Ec ([bool] $v5.Reason) '空操作也要说清它做了什么'

    Stop-EcWindow -Window $window2
    Stop-EcWindow -Window $window
} catch {
    # 两个哨兵都是"提前收尾"的正常路径（只跑离线层 / 安装目录里没有构建清理脚本），
    # 窗口交给 finally 里那份 Stop-EcOwnedWindows 统一收，不留没人认领的进程。
    if ($_.Exception.Message -notin @('__SKIP_REAL__', '__SKIP_CLEAN__')) { throw }
} finally {
    if (-not $Keep) { Remove-EcRunDir $run } else { Write-Host "保留临时目录：$($run.Path)" }
    Stop-EcOwnedWindows
}

exit (Complete-EcSuite -Title '截图历史归档 ')
