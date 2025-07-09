<#
.SYNOPSIS
    构建路径判据：中文 / 空格 / 括号 / 百分号 / & 的目录与 %TEMP% 下跑 build.ps1，并验证产物可用。
.DESCRIPTION
    旧 build.ps1 把仓库与工具链的绝对路径用 ASCII 写进临时批处理，中文在执行前就变成问号，
    路径里的单个 % 还会启动 cmd 的变量展开、把后面那条参数黏进来（本判据实测复现过）。
    两层判据：
      1. 离线（不需要 MSVC）：批处理正文必须全是 ASCII 且不含任何绝对路径；把路径写进正文的
         改动要被守卫当场拦下；VS 环境导入失败要在跑 cmake 之前就报错；路径经环境块交给
         cmd 后逐字符保真（含 % ! & ( ) 与中文）；临时批处理用完一个都不留；
         外部工具 stdout 的 UTF-8 / 系统 ANSI 两路解码。
      2. 真机：把仓库最小集复制进怪路径，Release / Debug / RelWithDebInfo 与 -Clean 各构建一次，
         %TEMP% 换成中文目录再构建一次；断言退出码、CMakeCache 里回写的源码目录一字不差、
         CMAKE_BUILD_TYPE 是要求的那个、产物的 --version / --help 契约正常。
    判据里的非 ASCII 目录名一律用码点拼出来：不能依赖 .ps1 本身被读成哪种编码。
    全程不截图、不碰桌面上任何别人的窗口。没有 MSVC / SDK 时第 2 层整层记 SKIP（未验证）。
.EXAMPLE
    .\tests\build-path.ps1
    .\tests\build-path.ps1 -OfflineOnly   # 只跑第 1 层（离线判据，秒级）
#>
param(
    [switch]$OfflineOnly
)

$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'harness.psm1') -Force -DisableNameChecking
Initialize-EcHarness -Exe $null -AllowMissing | Out-Null

$repo = Get-EcRepoRoot
$buildScript = Join-Path $repo 'build.ps1'
if (-not (Test-Path -LiteralPath $buildScript)) { throw "找不到 $buildScript" }
# dot-source：只取 build.ps1 的内部函数，这一步不构建（脚本尾部认 InvocationName 为 '.' 就直接返回）
. $buildScript

function EcChars {
    <# 用码点拼非 ASCII 文本，绕开「这个 .ps1 被按哪种编码读」这个问题。 #>
    param([int[]]$Codes)
    return (-join ($Codes | ForEach-Object { [char]$_ }))
}

function New-RepoCopy {
    <# 只复制构建用得上的最小集，且绝不带上 build\ 与 .git\（旧缓存会把构建带到别的目录去）。 #>
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Dest)

    if (Test-Path -LiteralPath $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    foreach ($leaf in @('CMakeLists.txt', 'build.ps1', 'src', 'resources')) {
        Copy-Item -LiteralPath (Join-Path $Source $leaf) -Destination $Dest -Recurse -Force
    }
    return (Join-Path $Dest 'build.ps1')
}

$run = New-EcRunDir -Tag 'buildpath'
Write-Host ("  本次临时目录：{0}" -f $run.Path)
Reset-EcSuite
$savedTemp = $env:TEMP
$powershell = (Get-Command powershell.exe).Source
$CR = [char]13
$LF = [char]10
$Eol = "$CR$LF"

$CJK_PROJECT = EcChars @(0x65E0, 0x671F, 0x8FF7, 0x9014)      # 无期迷途
$CJK_DIR = EcChars @(0x9879, 0x76EE)                          # 项目
$CJK_USER = EcChars @(0x7528, 0x6237, 0x9648, 0x5927, 0x660E)  # 用户陈大明
$CJK_TEMP = EcChars @(0x4E34, 0x65F6, 0x76EE, 0x5F55)        # 临时目录
$WEIRD_LEAF = $CJK_USER + '\' + $CJK_PROJECT + ' ' + $CJK_DIR + ' (100% &x)'

try {
    # ==================================================================
    # 第 1 层：离线判据
    # ==================================================================
    Write-Host "`n=== 1) 交给 cmd 的批处理正文：纯 ASCII、不含绝对路径 ==="
    $text = New-BuildScriptText
    $bad = [regex]::Match($text, "[^\u0009\u000A\u000D\u0020-\u007E]")
    $firstBad = '无'
    if ($bad.Success) { $firstBad = 'U+{0:X4}' -f [int][char]$bad.Value }
    Assert-Ec (-not $bad.Success) "正文全是 ASCII（首个非 ASCII 字符 $firstBad）"
    Assert-Ec (-not ($text -match '[A-Za-z]:[\\/]')) '正文里没有任何盘符绝对路径'
    foreach ($var in @('ECP_VCVARS', 'ECP_ROOT', 'ECP_BUILDDIR', 'ECP_CONFIG')) {
        Assert-Ec ($text -match ('%{0}%' -f $var)) "路径与取值经 %$var% 环境变量交给 cmd" -Quiet
    }
    Assert-Ec ($text -match '(?s)exit /b 11.*cmake -S') '环境导入的守卫排在 cmake 之前'

    Write-Host "`n=== 2) 把路径写进正文的改动要被当场拦下 ==="
    $injected = $text -replace '%ECP_ROOT%', (Join-Path $run.Path ($CJK_TEMP + ' (100%)'))
    $guardMsg = ''
    try { Assert-BatchAsciiText -Text $injected } catch { $guardMsg = $_.Exception.Message }
    Assert-Ec ($guardMsg -like '*ASCII*') "Assert-BatchAsciiText 拦住了非 ASCII 正文：$guardMsg"
    # 判据一律针对本次自己建的目录：别处可能正有另一轮构建在写它自己的批处理
    $weirdBatDir = Join-Path $run.Path ("temp bat (100% &x) " + $CJK_TEMP)
    New-Item -ItemType Directory -Force -Path $weirdBatDir | Out-Null
    $writeMsg = ''
    try { Invoke-BuildScript -Text $injected -BatDir $weirdBatDir -Env @{ ECP_VCVARS = 'C:\none\vcvars64.bat' } }
    catch { $writeMsg = $_.Exception.Message }
    Assert-Ec ($writeMsg -like '*ASCII*') 'Invoke-BuildScript 在写盘之前就拒收非 ASCII 正文'
    Assert-Ec (@(Get-ChildItem -LiteralPath $weirdBatDir -File -EA SilentlyContinue).Count -eq 0) '拒收时一个文件都没落地'

    Write-Host "`n=== 3) VS 环境导入失败必须立刻报错，且不会去跑 cmake ==="
    $emptyVcvars = Join-Path $run.Path 'fake-vcvars.bat'
    [IO.File]::WriteAllBytes($emptyVcvars, [Text.Encoding]::ASCII.GetBytes(
        ('@echo off', 'set VCINSTALLDIR=', 'set WindowsSDKDir=', 'set PATH=C:\only-path-here') -join $Eol))
    foreach ($case in @(
            @{ Name = 'vcvars64.bat 不存在'; VcVars = 'C:\no-such-dir\vcvars64.bat'; Want = 'vcvars64.bat 返回非零' },
            @{ Name = 'vcvars64.bat 没导入环境'; VcVars = $emptyVcvars; Want = 'VCINSTALLDIR' })) {
        $msg = ''
        try {
            Invoke-BuildScript -Text $text -BatDir $weirdBatDir -Env @{
                ECP_VCVARS = $case.VcVars; ECP_PATHADD = ''; ECP_ROOT = 'C:\none'
                ECP_BUILDDIR = 'C:\none\build'; ECP_CONFIG = 'Release'
            }
        } catch { $msg = $_.Exception.Message }
        $nm = [string]$case.Name
        $want = [string]$case.Want
        Assert-Ec ($msg -like '*Visual Studio 环境导入失败*') "$nm : 报的是环境导入失败（$msg）"
        Assert-Ec ($msg -like ('*' + $want + '*')) "$nm : 消息里点名原因 [$want]"
        Assert-Ec (-not ($msg -match 'cmake (配置|编译)')) "$nm : 没有拿着半套环境去跑 cmake"
        Assert-Ec ($msg -notlike '*C:\none*') "$nm : 报错里没有把内部临时路径当判据"
    }

    Write-Host "`n=== 4) 路径经环境块交给 cmd：逐字符保真（% ! & ( ) 与中文） ==="
    $probeText = ('@echo off', 'md "%ECP_TARGET%"', 'if errorlevel 1 exit /b 3', 'exit /b 0') -join $Eol
    $CJK_LINE = EcChars @(0x4E3B, 0x7EBF)                        # 主线
    $CJK_TRAIL = EcChars @(0x5C3E, 0x53CD, 0x659C, 0x6760)      # 尾反斜杠
    foreach ($leaf in @('a 100%!b (c) & d', ($CJK_PROJECT + ' ' + $CJK_LINE + ' 12-3'), 'x^y_z~q', ($CJK_TRAIL + [char]92))) {
        $target = Join-Path (Join-Path $run.Path 'made') $leaf
        $threw = ''
        try {
            # 批处理自己落在中文 + 空格 + 括号 + 百分号 + & 的目录里：命令行那一层一并验到
            Invoke-BuildScript -Text $probeText -Env @{ ECP_TARGET = $target } -BatDir $weirdBatDir
        } catch { $threw = $_.Exception.Message }
        $exact = $false
        if (Test-Path -LiteralPath $target) {
            $exact = ((Get-Item -LiteralPath $target).Name -ceq ([IO.Path]::GetFileName($target.TrimEnd([char]92))))
        }
        Assert-Ec ($exact -and -not $threw) "目录名逐字符送达：[$leaf] $threw"
    }
    $left = @(Get-ChildItem -LiteralPath $weirdBatDir -File -EA SilentlyContinue).Count
    Assert-Ec ($left -eq 0) "临时批处理用完就删，$weirdBatDir 里剩 $left 个文件"

    Write-Host "`n=== 5) 外部工具 stdout 解码：UTF-8 优先，解不动退系统 ANSI ==="
    $utf8 = [Text.Encoding]::UTF8.GetBytes($CJK_PROJECT)
    Assert-Ec ((ConvertFrom-ToolBytes -Bytes $utf8) -ceq $CJK_PROJECT) 'UTF-8 字节按 UTF-8 解出中文'
    $invalid = [byte[]]@(0xC0, 0xC1, 0xFE, 0xFF)
    Assert-Ec ((ConvertFrom-ToolBytes -Bytes $invalid) -ceq [Text.Encoding]::Default.GetString($invalid)) `
        '非法 UTF-8 字节退到系统 ANSI 解码且不抛'
    Assert-Ec ((ConvertFrom-ToolBytes -Bytes ([byte[]]@())) -ceq '') '空字节 -> 空文本'

    # ==================================================================
    # 第 2 层：真机构建矩阵
    # ==================================================================
    if ($OfflineOnly) {
        Skip-Ec -Message '真机构建矩阵（-OfflineOnly）' -Reason '只跑了离线判据'
    } else {
        $vsError = ''
        try { [void](Resolve-VcVarsPath) } catch { $vsError = $_.Exception.Message }
        if ($vsError) {
            Write-Host "  本机没有可用的 MSVC 工具链：$vsError" -ForegroundColor DarkYellow
            Skip-Ec -Message '真机构建矩阵（中文/空格/括号/百分号路径、Release/Debug/RelWithDebInfo、-Clean）' `
                     -Reason $vsError
        } else {
            $versionText = [IO.File]::ReadAllText((Join-Path $repo 'src\Version.h'))
            $parts = @()
            foreach ($p in @('MAJOR', 'MINOR', 'PATCH')) {
                if ($versionText -match ('ECAPTURE_VERSION_' + $p + '\s+([0-9]+)')) { $parts += $Matches[1] }
                else { throw "src\Version.h 里找不到 ECAPTURE_VERSION_$p" }
            }
            $expectVersion = $parts -join '.'
            Write-Host ("`n=== 6) 真机构建矩阵（版本判据 {0}） ===" -f $expectVersion)

            # 每一例都给自己的 %TEMP% 换一个新目录：批处理残留判据只认本次这个目录，
            # 另一轮并发构建（或上一轮被中途杀掉）留下的批处理不该把这一例判成失败
            $cases = @(
                @{ Name = '中文+空格+括号+百分号+& Release'; Root = $WEIRD_LEAF; Config = 'Release'; Clean = $false; Tail = 'plain' },
                @{ Name = '同一棵树 Debug + -Clean'; Root = $WEIRD_LEAF; Config = 'Debug'; Clean = $true; Tail = 'plain' },
                @{ Name = '%TEMP% 为中文目录 Release + -Clean'; Root = $WEIRD_LEAF; Config = 'Release'; Clean = $true; Tail = $CJK_TEMP },
                @{ Name = '纯 ASCII 路径 RelWithDebInfo + -Clean'; Root = 'plain-ascii'; Config = 'RelWithDebInfo'; Clean = $true; Tail = 'plain' }
            )
            $i = 0
            foreach ($case in $cases) {
                $i++
                $repoDir = Join-Path (Join-Path $run.Path $case.Root) 'repo'
                $newTemp = Join-Path (Join-Path $run.Path ('temp-' + $i)) $case.Tail
                New-Item -ItemType Directory -Force -Path $newTemp | Out-Null
                Write-Host ("  [{0}] 配置={1} Clean={2} 仓库={3}" -f $case.Name, $case.Config, $case.Clean, $repoDir) -ForegroundColor DarkGray
                $buildPs1 = New-RepoCopy -Source $repo -Dest $repoDir
                $env:TEMP = $newTemp
                try {
                    $childArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $buildPs1, '-Config', $case.Config)
                    if ($case.Clean) { $childArgs += '-Clean' }
                    # 先确认 %TEMP% 真的换掉了，否则「批处理落在中文 TEMP 下」这条判据是空的。
                    # 走文件不回显：判据不能挂在控制台码页上。
                    $tempProbe = Join-Path $run.Path ('temp-probe-' + $i + '.txt')
                    $q = [char]39
                    $probeCmd = '[IO.File]::WriteAllText(' + $q + $tempProbe + $q + ', $env:TEMP)'
                    [void](Invoke-EcProcess -FilePath $powershell -Arguments @('-NoProfile', '-Command', $probeCmd) -TimeoutMs 60000)
                    $seenTemp = ''
                    if (Test-Path -LiteralPath $tempProbe) {
                        $seenTemp = [IO.File]::ReadAllText($tempProbe, [Text.Encoding]::UTF8)
                    }
                    Assert-Ec ($seenTemp -ceq $newTemp) "  子进程的 %TEMP% 确实是 $newTemp（实际 $seenTemp）" -Quiet
                    $r = Invoke-EcProcess -FilePath $powershell -Arguments $childArgs -TimeoutMs 900000
                    Assert-Ec ($r.Exit -eq 0) "  $($case.Name)：build.ps1 退出码 $($r.Exit)（应为 0）"
                    if ($r.Exit -ne 0) { Show-EcProcessDiag -Result $r -Label $case.Name }

                    $cache = Join-Path $repoDir 'build\CMakeCache.txt'
                    $exe = Join-Path $repoDir 'build\ecapture.exe'
                    Assert-Ec (Test-Path -LiteralPath $cache) "  配置真的跑了：$cache" -Quiet
                    Assert-Ec (Test-Path -LiteralPath $exe) "  产物存在：$exe" -Quiet
                    if (Test-Path -LiteralPath $cache) {
                        $cacheText = [IO.File]::ReadAllText($cache, [Text.Encoding]::UTF8)
                        $wantHome = ($repoDir -replace '\\', '/')
                        Assert-Ec ($cacheText -match ('CMAKE_HOME_DIRECTORY:INTERNAL=' + [regex]::Escape($wantHome) + $CR)) `
                            "  CMakeCache 里的源码目录一字不差：$wantHome"
                        Assert-Ec ($cacheText -match ('CMAKE_BUILD_TYPE:STRING=' + $case.Config + $CR)) `
                            "  CMAKE_BUILD_TYPE = $($case.Config) 真的传到了 cmake"
                    }
                    if (Test-Path -LiteralPath $exe) {
                        $v = Invoke-EcProcess -FilePath $exe -Arguments @('--version') -TimeoutMs 30000
                        Assert-Ec ($v.Exit -eq 0 -and $v.Stdout -like ('*' + $expectVersion + '*')) `
                            "  --version 退出码 $($v.Exit) 且含 $expectVersion"
                        $h = Invoke-EcProcess -FilePath $exe -Arguments @('--help') -TimeoutMs 30000
                        Assert-Ec ($h.Exit -eq 3 -and $h.Stdout.Trim().Length -gt 200) `
                            "  --help 退出码 $($h.Exit)（应为 3）且不是空文本"
                    }
                    $stray = @(Get-ChildItem -LiteralPath $newTemp -Filter 'ecapture-build-*.bat' -File -EA SilentlyContinue).Count
                    Assert-Ec ($stray -eq 0) "  %TEMP%（$newTemp）里没有残留批处理"
                } finally {
                    $env:TEMP = $savedTemp
                }
            }
        }
    }
} finally {
    $env:TEMP = $savedTemp
    Remove-EcRunDir -RunDir $run
}

exit (Complete-EcSuite -Title '构建路径')
