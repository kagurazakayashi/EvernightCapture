<#
.SYNOPSIS
    桌面测试的共享基础设施：进程调用器、唯一临时目录、自有测试窗口程序、图像度量与断言套件。
.DESCRIPTION
    两件事是这个模块负责的：

    1. 进程调用器（Invoke-EcProcess）：参数按 Windows argv 引号规则传递（支持 ArgumentList 的
       .NET 版本直接用它），stdout / stderr 同时消费，二进制字节不经文本转码，等待有明确期限，
       超时只结束本次调用拥有的子进程树并保留可诊断的部分输出。

    2. 资源隔离：每次运行建一个唯一临时目录（只删自己那个），桌面测试的目标窗口一律由
       Build-EcHelper 编出来的自有程序 ecwindow.exe 建立，调用方持有本次的 PID / HWND /
       Process 实例。任何地方都不再按进程名去找目标或批量收尾，使用者本来开着的同名进程
       既不会被当成目标，也不会被结束。

    本模块不改变 ECAPTURE 的输出契约，只是把测试原来各自手写的调用代码收拢成一份。
.EXAMPLE
    Import-Module "$PSScriptRoot\harness.psm1" -Force
    Initialize-EcHarness -Exe $Exe
    $run = New-EcRunDir -Tag 'channels'
    $r = Invoke-Ecapture @('--help')
    Remove-EcRunDir $run
#>
Set-StrictMode -Off

$script:EcExe = $null
$script:EcFailures = New-Object System.Collections.Generic.List[string]
$script:EcSkips = New-Object System.Collections.Generic.List[string]
$script:EcPassCount = 0
$script:EcArgumentListSupported = $null
$script:EcFoundWindow = [IntPtr]::Zero
$script:EcFoundDialog = [IntPtr]::Zero
# 本次运行建立过的窗口：异常退出时由 Stop-EcOwnedWindows 兜底收尾，只收尾这一份清单。
$script:EcOwnedWindows = New-Object System.Collections.Generic.List[object]

# ----------------------------------------------------------------------------
# 本机 API：窗口枚举 / 类名 / 对话框按钮。桌面测试自己去看屏幕，不借工具的结论
# ----------------------------------------------------------------------------
if (-not ('EcHarnessWin' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EcHarnessWin {
  [StructLayout(LayoutKind.Sequential)]
  public struct RECT { public int Left, Top, Right, Bottom; }
  public delegate bool EnumWinProc(IntPtr hwnd, IntPtr data);
  [DllImport("user32")] public static extern bool EnumWindows(EnumWinProc proc, IntPtr data);
  [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, char[] cls, int max);
  [DllImport("user32", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, char[] text, int max);
  [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32")] public static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
  [DllImport("user32")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  [DllImport("user32")] public static extern bool SetProcessDPIAware();
}
'@
}

function Set-EcDpiAware {
    <# 让本测试进程与工具看到同一套物理像素坐标（per-monitor v2，老系统退化成系统 DPI 感知）。 #>
    [void][EcHarnessWin]::SetProcessDpiAwarenessContext([IntPtr](-4))
    [void][EcHarnessWin]::SetProcessDPIAware()
}

# ----------------------------------------------------------------------------
# 路径与运行目录
# ----------------------------------------------------------------------------
function Get-EcRepoRoot {
    return (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
}

function Initialize-EcHarness {
    <# 解析并缓存被测可执行文件路径；$Exe 为空时用 build\ecapture.exe。 #>
    param([string]$Exe, [switch]$AllowMissing)

    if (-not $Exe) { $Exe = Join-Path (Get-EcRepoRoot) 'build\ecapture.exe' }
    if (-not (Test-Path -LiteralPath $Exe)) {
        if ($AllowMissing) { $script:EcExe = $null; return $null }
        throw "找不到可执行文件：$Exe（先运行 .\build.ps1）"
    }
    $script:EcExe = (Get-Item -LiteralPath $Exe).FullName
    return $script:EcExe
}

function Get-EcExePath {
    if (-not $script:EcExe) { throw '先调用 Initialize-EcHarness -Exe <路径>' }
    return $script:EcExe
}

function New-EcRunDir {
    <# 本次运行专属的临时目录。名字里带时间戳 + PID + 随机段，两轮并发各自一份。 #>
    param([string]$Tag = 'run')

    $root = Join-Path ([IO.Path]::GetTempPath()) 'ecapture-tests'
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $salt = ([Guid]::NewGuid().ToString('N')).Substring(0, 8)
    $leaf = 'run-{0}-{1}-{2}-{3}' -f ($Tag -replace '[^A-Za-z0-9_-]', ''), $stamp, $PID, $salt
    $path = Join-Path $root $leaf
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    return [pscustomobject]@{ Path = (Get-Item -LiteralPath $path).FullName; Root = $root; Leaf = $leaf }
}

function Test-EcOwnRunDir {
    <# 目录形状判定：必须是 %TEMP%\ecapture-tests\run-<tag>-<时间>-<pid>-<随机> 这个样式。 #>
    param([Parameter(Mandatory)]$RunDir)

    if (-not $RunDir.Path) { return $false }
    $underRoot = $RunDir.Path.StartsWith($RunDir.Root + [IO.Path]::DirectorySeparatorChar,
                                        [StringComparison]::OrdinalIgnoreCase)
    $ownShape = $RunDir.Leaf -match '^run-[A-Za-z0-9_-]*-\d{8}-\d{6}-\d+-[0-9a-f]{8}$'
    return [bool]($underRoot -and $ownShape)
}

function Remove-EcRunDir {
    <# 只删自己建的那个目录：形状不对就拒绝，绝不 recurse 到别人的产物里。 #>
    param($RunDir, [switch]$Quiet)

    if (-not $RunDir -or -not $RunDir.Path) { return }
    if (-not (Test-EcOwnRunDir $RunDir)) {
        Write-Host "  !! 拒绝删除不是本次建立的目录：$($RunDir.Path)" -ForegroundColor DarkYellow
        return
    }
    if (-not $Quiet) {
        $left = @(Get-ChildItem -LiteralPath $RunDir.Path -Recurse -File -ErrorAction SilentlyContinue).Count
        Write-Host ("  清理本次临时目录：{0}（{1} 个文件）" -f $RunDir.Path, $left) -ForegroundColor DarkGray
    }
    Remove-Item -LiteralPath $RunDir.Path -Recurse -Force -ErrorAction SilentlyContinue
}

function Get-EcRunFile {
    param($RunDir, [Parameter(Mandatory)][string]$Name)
    if (-not $RunDir) { return $Name }
    return Join-Path $RunDir.Path $Name
}

# ----------------------------------------------------------------------------
# argv 引号：Windows CRT / CommandLineToArgvW 那套规则
# ----------------------------------------------------------------------------
function Format-EcOneArg {
    param([AllowNull()][string]$Value)

    $s = ''
    if ($null -ne $Value) { $s = $Value }
    # 只有空串、含空白或含引号时才需要包起来；其余原样，少一层转义面。
    if ($s.Length -gt 0 -and $s -notmatch '[\s"]') { return $s }

    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $backslashes = 0
    foreach ($ch in $s.ToCharArray()) {
        if ($ch -eq '\') { $backslashes++; [void]$sb.Append('\'); continue }
        if ($ch -eq '"') {
            # 引号前的反斜杠要加倍：2n 个反斜杠 + 转义引号 才表示 n 个反斜杠 + 一个字面引号
            if ($backslashes) { [void]$sb.Append('\' * $backslashes) }
            $backslashes = 0
            [void]$sb.Append('\"')
            continue
        }
        # 普通字符会中断反斜杠串：不歸零就會在下一個引號前多補一位，
        # 于是收尾引号被当成转义引号，后面的参数全部黏成一条（实测踩过）。
        $backslashes = 0
        [void]$sb.Append($ch)
    }
    if ($backslashes) { [void]$sb.Append('\' * $backslashes) }   # 结尾反斜杠加倍，否则会吃掉收尾引号
    [void]$sb.Append('"')
    return $sb.ToString()
}

function Format-EcWindowsArgv {
    param([string[]]$Arguments)

    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($a in @($Arguments)) { $parts.Add((Format-EcOneArg ([string]$a))) }
    return ($parts -join ' ')
}

function Test-EcArgumentListSupported {
    <# .NET Framework 的 ProcessStartInfo 没有 ArgumentList（Windows PowerShell 5.1 就是这种）。 #>
    if ($null -eq $script:EcArgumentListSupported) {
        $script:EcArgumentListSupported = ($null -ne [Diagnostics.ProcessStartInfo].GetProperty('ArgumentList'))
    }
    return [bool]$script:EcArgumentListSupported
}

function Get-EcArgumentListMode {
    <# 本运行时实际会用的传参方式，报告与断言都用它，别在测试里各自反射一遍。 #>
    if (Test-EcArgumentListSupported) { return 'ArgumentList' }
    return 'argv-quoting'
}

function Parse-EcArgEcho {
    <#
        ecwindow.exe --mode args 的协议：每条参数一行「序号<TAB>UTF-8字节数<TAB>原文」，
        最后一条 END<TAB>条数。原文可以含 TAB 与换行，所以只能按字节数走，不能按行切。
    #>
    param([Parameter(Mandatory)][byte[]]$Bytes)

    $items = New-Object System.Collections.Generic.List[string]
    $enc = New-Object System.Text.UTF8Encoding($false, $false)
    $i = 0
    $n = $Bytes.Length
    while ($i -lt $n) {
        $value = @()
        foreach ($field in 1, 2) {
            $start = $i
            while ($i -lt $n -and $Bytes[$i] -ne 9) { $i++ }
            if ($i -ge $n) { return $items }
            $value += $enc.GetString($Bytes, $start, $i - $start)
            $i++
        }
        if ($value[0] -eq 'END') { return $items }
        $len = [int]::Parse($value[1], [Globalization.NumberStyles]::Integer,
                            [Globalization.CultureInfo]::InvariantCulture)
        if ($i + $len -gt $n) { return $items }
        $items.Add($enc.GetString($Bytes, $i, $len))
        $i += $len
        if ($i -lt $n -and $Bytes[$i] -eq 10) { $i++ }
    }
    return $items
}

function Test-EcBytePattern {
    <#
        校验假后端的输出模式：第 i 个字节必须是 i % 251。长度必须完全相等（少一个字节就是被
        转码或截断了），再按 251 的整周期抽查若干块，避免在 PowerShell 里循环几百万次。
    #>
    param(
        [Parameter(Mandatory)][byte[]]$Bytes,
        [int]$Total = -1,
        [int]$Blocks = 96
    )

    if ($Total -ge 0 -and $Bytes.Length -ne $Total) { return $false }
    $period = 251
    if ($Bytes.Length -lt $period) {
        for ($i = 0; $i -lt $Bytes.Length; $i++) { if ($Bytes[$i] -ne [byte]($i % $period)) { return $false } }
        return $true
    }
    $checks = New-Object System.Collections.Generic.List[int]
    $checks.Add(0)
    $checks.Add($Bytes.Length - $period)
    for ($k = 1; $k -le $Blocks; $k++) { $checks.Add([int](($k * 7919) % ($Bytes.Length - $period))) }
    foreach ($offset in ($checks | Sort-Object -Unique)) {
        for ($j = 0; $j -lt $period; $j++) {
            if ($Bytes[$offset + $j] -ne [byte](($offset + $j) % $period)) { return $false }
        }
    }
    return $true
}

# ----------------------------------------------------------------------------
# 进程树收尾：只针对本次调用亲手起的那个 PID
# ----------------------------------------------------------------------------
function Stop-EcProcessTree {
    <#
    .DESCRIPTION
        结束一棵进程树，而且只结束这一棵：根进程必须是本次调用亲手起的（所以拿得到 Process 实例）。
        先 taskkill /T（树还在时最有效），没杀干净再退回 Process.Kill。
    #>
    param(
        [Parameter(Mandatory)][Diagnostics.Process]$Process,
        [int]$WaitMs = 5000
    )

    if ($Process.HasExited) { return 'already-exited' }
    $how = 'none'
    $target = Join-Path $env:SystemRoot 'System32\taskkill.exe'
    if (Test-Path -LiteralPath $target) {
        try {
            [void](Start-Process -FilePath $target -ArgumentList @('/T', '/F', '/PID', [string]$Process.Id) `
                    -WindowStyle Hidden -Wait)
            $how = 'taskkill'
        } catch { }
    }
    if (-not $Process.HasExited) {
        try {
            $killTree = $Process.GetType().GetMethod('Kill', [type[]]@([bool]))
            if ($killTree) { $Process.Kill($true) } else { $Process.Kill() }
            $how = if ($how -eq 'none') { 'Kill' } else { "$how+Kill" }
        } catch { }
    }
    try { [void]$Process.WaitForExit($WaitMs) } catch { }
    return $how
}

function Stop-EcOwnProcess {
    <# 收尾一个本次拥有的进程：附带可执行文件路径核对，PID 被复用时会拒绝动手。 #>
    param(
        [Parameter(Mandatory)][Diagnostics.Process]$Process,
        [string]$ExpectedPath
    )

    if ($ExpectedPath) {
        $actual = $null
        try { $actual = $Process.MainModule.FileName } catch { }
        if ($actual -and ($actual -ne $ExpectedPath)) {
            Write-Host ("  !! PID {0} 已不是 {1}（现在是 {2}），不动它" -f `
                       $Process.Id, $ExpectedPath, $actual) -ForegroundColor DarkYellow
            return 'refused'
        }
    }
    return (Stop-EcProcessTree -Process $Process)
}

function Test-EcProcessAlive {
    param([Parameter(Mandatory)][int]$ProcessId)
    return [bool](Get-Process -Id $ProcessId -ErrorAction SilentlyContinue)
}

# ----------------------------------------------------------------------------
# 共享进程调用器
# ----------------------------------------------------------------------------
function Invoke-EcProcess {
    <#
    .SYNOPSIS
        起一个子进程，同时消费两条标准流，带期限等待，返回字节 + 解码文本 + 诊断信息。
    .DESCRIPTION
        * 参数：支持 ArgumentList 的运行时走 ArgumentList，否则走 Format-EcWindowsArgv（同一套
          Windows argv 规则，两边对空串、空白、引号、结尾反斜杠的处理一致）。
        * 两条流都用异步读并发消费，任何一条都不会因为另一条塞满管道而互相等死；
          原始字节留在 StdoutBytes / StderrBytes 里，二进制输出不经文本转码。
        * -Probe 在等待期间按间隔回调（参数是 Process 实例），返回 $true 就停止探测。
          需要「进程跑到一半时点掉一个对话框」的测试用它，不必往调用器里塞业务逻辑。
        * 超时只结束本次拥有的子进程树，已经拿到的部分输出照样留着给人看。
    #>
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [int]$TimeoutMs = 60000,
        [scriptblock]$Probe,
        [int]$ProbeIntervalMs = 60,
        [int]$ProbeTimeoutMs = 15000,
        [string]$WorkingDirectory,
        [switch]$NoTextDecode
    )

    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    if ($WorkingDirectory) { $psi.WorkingDirectory = $WorkingDirectory }

    $argumentMode = 'argv-quoting'
    if (Test-EcArgumentListSupported) {
        $argumentMode = 'ArgumentList'
        foreach ($a in @($Arguments)) { $psi.ArgumentList.Add([string]$a) }
    } else {
        $psi.Arguments = Format-EcWindowsArgv -Arguments $Arguments
    }

    $started = [Diagnostics.Stopwatch]::StartNew()
    try {
        $p = [Diagnostics.Process]::Start($psi)
    } catch {
        return [pscustomobject]@{
            Exit = -1; StdoutBytes = [byte[]]@(); StderrBytes = [byte[]]@()
            Stdout = ''; Stderr = ''; TimedOut = $false; KilledBy = 'none'; Truncated = $false
            DurationMs = [int]$started.ElapsedMilliseconds; StartError = $_.Exception.Message
            ProbeResult = $null; ArgumentMode = $argumentMode
        }
    }

    $stdoutStream = $p.StandardOutput.BaseStream
    $stderrStream = $p.StandardError.BaseStream
    $stdoutMs = New-Object IO.MemoryStream
    $stderrMs = New-Object IO.MemoryStream
    $stdoutBuf = New-Object byte[] 65536
    $stderrBuf = New-Object byte[] 65536
    $stdoutDone = $false
    $stderrDone = $false
    $truncated = $false
    $timedOut = $false
    $killedBy = 'none'

    $readOut = $stdoutStream.BeginRead($stdoutBuf, 0, $stdoutBuf.Length, $null, $null)
    $readErr = $stderrStream.BeginRead($stderrBuf, 0, $stderrBuf.Length, $null, $null)
    $probeDone = (-not $Probe)
    $probeResult = $null
    $nextProbe = Get-Date

    while ($true) {
        if (-not $stdoutDone -and $readOut.AsyncWaitHandle.WaitOne(0)) {
            try {
                $n = $stdoutStream.EndRead($readOut)
                if ($n -le 0) { $stdoutDone = $true }
                else {
                    $stdoutMs.Write($stdoutBuf, 0, $n)
                    $readOut = $stdoutStream.BeginRead($stdoutBuf, 0, $stdoutBuf.Length, $null, $null)
                }
            } catch { $stdoutDone = $true; $truncated = $true }
        }
        if (-not $stderrDone -and $readErr.AsyncWaitHandle.WaitOne(0)) {
            try {
                $n = $stderrStream.EndRead($readErr)
                if ($n -le 0) { $stderrDone = $true }
                else {
                    $stderrMs.Write($stderrBuf, 0, $n)
                    $readErr = $stderrStream.BeginRead($stderrBuf, 0, $stderrBuf.Length, $null, $null)
                }
            } catch { $stderrDone = $true; $truncated = $true }
        }
        if ($stdoutDone -and $stderrDone) { break }

        if (-not $probeDone) {
            $now = Get-Date
            if ($now -ge $nextProbe) {
                $probeResult = $null
                try { $probeResult = & $Probe $p }
                catch { Write-Host "  !! 探测回调异常：$_" -ForegroundColor DarkYellow; $probeDone = $true }
                if ([bool]$probeResult -or $started.ElapsedMilliseconds -gt $ProbeTimeoutMs) { $probeDone = $true }
                $nextProbe = $now.AddMilliseconds($ProbeIntervalMs)
            }
        }

        if ($started.ElapsedMilliseconds -gt $TimeoutMs) {
            $timedOut = $true
            $killedBy = Stop-EcProcessTree -Process $p
            break
        }
        [Threading.Thread]::Sleep(5)
    }

    if (-not $timedOut) {
        # 两条流都读完不代表进程已经退出：再给它一个有期限的等待，超时同样只结束自己这一棵。
        $exited = $false
        try { $exited = $p.WaitForExit([Math]::Min(10000, [Math]::Max(1000, $TimeoutMs))) } catch { $exited = $true }
        if (-not $exited) {
            $timedOut = $true
            $killedBy = Stop-EcProcessTree -Process $p
        }
    } else {
        # 已经下手了，管道里可能还剩最后一批字节，再收一小会儿就放弃
        $grace = (Get-Date).AddMilliseconds(500)
        while (((Get-Date) -lt $grace) -and (-not ($stdoutDone -and $stderrDone))) { [Threading.Thread]::Sleep(10) }
    }

    $exitCode = -1
    try { if ($p.HasExited) { $exitCode = $p.ExitCode } } catch { }
    $stdoutBytes = $stdoutMs.ToArray()
    $stderrBytes = $stderrMs.ToArray()
    $stdoutText = ConvertFrom-EcBytes -Bytes $stdoutBytes
    $stderrText = ConvertFrom-EcBytes -Bytes $stderrBytes
    if ($NoTextDecode) { $stdoutText = ''; $stderrText = '' }
    $started.Stop()

    return [pscustomobject]@{
        Exit = $exitCode
        StdoutBytes = $stdoutBytes
        StderrBytes = $stderrBytes
        Stdout = $stdoutText
        Stderr = $stderrText
        TimedOut = $timedOut
        KilledBy = $killedBy
        Truncated = $truncated
        DurationMs = [int]$started.ElapsedMilliseconds
        StartError = $null
        ProbeResult = $probeResult
        ArgumentMode = $argumentMode
    }
}

function ConvertFrom-EcBytes {
    <# UTF-8 解码（工具的输出固定 UTF-8）。BOM 若存在就吃掉，免得 ConvertFrom-Json 被顶一下。 #>
    param([byte[]]$Bytes)
    if (-not $Bytes -or -not $Bytes.Length) { return '' }
    $text = (New-Object System.Text.UTF8Encoding($false, $false)).GetString($Bytes)
    if ($text.Length -gt 0 -and [int][char]$text[0] -eq 0xFEFF) { $text = $text.Substring(1) }
    return $text
}

function Show-EcProcessDiag {
    <# 把一次调用的可诊断信息打出来（超时 / 失败时用），文本按长度截断。 #>
    param(
        [Parameter(Mandatory)]$Result,
        [string]$Label = '',
        [int]$MaxChars = 1200
    )

    $head = 'exit={0} 用时={1}ms 参数方式={2}' -f $Result.Exit, $Result.DurationMs, $Result.ArgumentMode
    if ($Result.TimedOut) { $head += " 超时（已结束进程树：$($Result.KilledBy)）" }
    if ($Result.StartError) { $head += " 起进程失败：$($Result.StartError)" }
    Write-Host ("    [{0}] {1}" -f $Label, $head) -ForegroundColor DarkGray
    foreach ($name in @('Stdout', 'Stderr')) {
        $text = [string]$Result.$name
        if (-not $text) { continue }
        if ($text.Length -gt $MaxChars) { $text = $text.Substring(0, $MaxChars) + ' …（截断）' }
        Write-Host ("      {0}> {1}" -f $name.ToLower(), ($text -replace '[\r\n]+', ' | ')) -ForegroundColor DarkGray
    }
}

function Invoke-Ecapture {
    <# 被测 CLI 的薄封装：参数一律以 string[] 传，不在这里做字符串拼接。 #>
    param(
        [string[]]$Arguments = @(),
        [string]$Exe,
        [int]$TimeoutMs = 60000,
        [scriptblock]$Probe,
        [int]$ProbeIntervalMs = 60,
        [int]$ProbeTimeoutMs = 15000
    )

    $file = $Exe
    if (-not $file) { $file = Get-EcExePath }
    return (Invoke-EcProcess -FilePath $file -Arguments $Arguments -TimeoutMs $TimeoutMs `
            -Probe $Probe -ProbeIntervalMs $ProbeIntervalMs -ProbeTimeoutMs $ProbeTimeoutMs)
}

# ----------------------------------------------------------------------------
# 自有测试窗口程序
# ----------------------------------------------------------------------------
function Get-EcCscPath {
    foreach ($bit in @('Framework64', 'Framework')) {
        $candidate = Join-Path $env:WINDIR "Microsoft.NET\${bit}\v4.0.30319\csc.exe"
        if (Test-Path -LiteralPath $candidate) { return (Get-Item -LiteralPath $candidate).FullName }
    }
    throw '找不到 .NET Framework 的 csc.exe（%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe）'
}

function Build-EcHelper {
    <#
        把 tests\helper\ec_window.cs 编进本次运行目录，返回 exe 路径。
        产物落在唯一临时目录里，随目录一起删；不写进仓库，也不与其它运行共用文件。
    #>
    param([Parameter(Mandatory)]$RunDir)

    $exe = Get-EcRunFile -RunDir $RunDir -Name 'ecwindow.exe'
    if (Test-Path -LiteralPath $exe) { return $exe }
    $source = Join-Path $PSScriptRoot 'helper\ec_window.cs'
    if (-not (Test-Path -LiteralPath $source)) { throw "测试窗口源码不存在：$source" }

    $csc = Get-EcCscPath
    $r = Invoke-EcProcess -FilePath $csc -TimeoutMs 120000 -Arguments @(
        '/nologo', '/target:exe', '/optimize+', '/r:System.Drawing.dll', ('/out:' + $exe), $source)
    if ($r.Exit -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
        Write-Host "  !! 编译测试窗口失败（csc 退出码 $($r.Exit)）" -ForegroundColor Red
        Show-EcProcessDiag -Result $r -Label 'csc'
        throw "编译 $source 失败"
    }
    return $exe
}

function Start-EcWindow {
    <#
    .SYNOPSIS
        起一个自有测试窗口，返回本次拥有的 Process / PID / HWND / 类名 / 标题 / 矩形。
    .PARAMETER Mode
        window = 多色内容（截图目标）；solid = 单色（遮挡物或屏幕标记）。
    .PARAMETER Rect
        物理像素的 'L,T,R,B'。窗口是 WS_POPUP，所以请求矩形就是它真正占的位置。
    .PARAMETER Windows
        同一个进程建几扇窗口（默认 1）。多扇时类名是 <class>、<class>-2 …，标题全都一样，
        用来造「同一个 PID 的两个目标」—— %p / %n 的撞名检测只有这么造才验得到。
    .OUTPUTPROPERTY
        Proc Diagnostics.Process，Pid Int32，Hwnd IntPtr，Class String，Title String，Rect Int[]
        Hwnds / Classes：本次建的全部窗口（多扇时按 -2、-3 的顺序），Hwnd / Class 就是它们的第一个
    #>
    param(
        [Parameter(Mandatory)]$RunDir,
        [Parameter(Mandatory)][string]$Class,
        [string]$Title = '',
        [string]$Rect = '120,120,540,420',
        [ValidateSet('window', 'solid')][string]$Mode = 'window',
        [int]$Seed = 1,
        [string]$Color = 'FF0000',   # RRGGBB：遮挡物/标记默认纯红
        [switch]$TopMost,
        [int]$MaxLifeSeconds = 300,
        [string]$PidFile = '',
        [int]$Windows = 1,
        [int]$TimeoutMs = 15000
    )

    $helper = Build-EcHelper -RunDir $RunDir
    if (-not $Title) { $Title = $Class }

    $arguments = @('--mode', $Mode, '--class', $Class, '--title', $Title, '--rect', $Rect,
                  '--seed', [string]$Seed, '--color', $Color, '--max-life', [string]$MaxLifeSeconds,
                  '--watch-pid', [string]$PID)
    if ($PidFile) { $arguments += @('--pid-file', $PidFile) }
    if ($TopMost) { $arguments += '--topmost' }
    if ($Windows -gt 1) { $arguments += @('--windows', [string]$Windows) }

    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $helper
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    if (Test-EcArgumentListSupported) {
        foreach ($a in $arguments) { $psi.ArgumentList.Add($a) }
    } else {
        $psi.Arguments = Format-EcWindowsArgv -Arguments $arguments
    }

    try { $proc = [Diagnostics.Process]::Start($psi) }
    catch { throw "起测试窗口失败：$helper —— $($_.Exception.Message)" }

    $window = [pscustomobject]@{
        Proc = $proc; Pid = [int]$proc.Id; Hwnd = [IntPtr]::Zero; Class = $Class
        Title = $Title; Helper = $helper; Rect = ($Rect.Split(',') | ForEach-Object { [int]$_ })
        Hwnds = @(); Classes = @($Class)
    }
    [void]$script:EcOwnedWindows.Add($window)
    try {
        $window.Hwnd = Wait-EcWindow -ProcessId $window.Pid -Class $Class -TimeoutMs $TimeoutMs
        $window.Hwnds = @($window.Hwnd)
        # --windows N 时其余几扇各自有类名 <class>-2、<class>-3 …：一扇一扇等到，
        # 少一扇就是这次的目标不完整，宁可直接抛而不拿半套去截图。
        for ($k = 2; $k -le $Windows; $k++) {
            $extra = "$Class-$k"
            $extraHwnd = Wait-EcWindow -ProcessId $window.Pid -Class $extra -TimeoutMs $TimeoutMs
            $window.Hwnds += $extraHwnd
            $window.Classes += $extra
        }
    } catch {
        Stop-EcWindow -Window $window
        throw
    }
    return $window
}

function Wait-EcWindow {
    <# 按 PID + 类名等自己的窗口出现：不看进程名，也不接受「第一个同名进程」。 #>
    param(
        [Parameter(Mandatory)][int]$ProcessId,
        [Parameter(Mandatory)][string]$Class,
        [int]$TimeoutMs = 15000
    )

    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $hwnd = Find-EcWindow -ProcessId $ProcessId -Class $Class
        if ($hwnd -ne [IntPtr]::Zero) { return $hwnd }
        Start-Sleep -Milliseconds 100
    }
    throw "没等到窗口：class=$Class pid=$ProcessId（$TimeoutMs ms 内）"
}

function Find-EcWindow {
    <# 返回匹配的 HWND，找不到给 IntPtr::Zero。委托先存进变量，否则会被 GC 掉。 #>
    param([int]$ProcessId, [string]$Class)

    $script:EcFoundWindow = [IntPtr]::Zero
    $callback = [EcHarnessWin+EnumWinProc] {
        param($h, $data)
        $sameProcess = $true
        if ($ProcessId) { $sameProcess = Test-EcWindowBelongsTo -Hwnd $h -ProcessId $ProcessId }
        if ($sameProcess -and ((-not $Class) -or ((Get-EcClassName -Hwnd $h) -eq $Class))) {
            $script:EcFoundWindow = $h
            return $false
        }
        return $true
    }
    [void][EcHarnessWin]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:EcFoundWindow
}

function Test-EcWindowBelongsTo {
    param([IntPtr]$Hwnd, [int]$ProcessId)
    $pid_ = [uint32]0
    [void][EcHarnessWin]::GetWindowThreadProcessId($Hwnd, [ref]$pid_)
    return ([int]$pid_ -eq $ProcessId)
}

function Get-EcClassName {
    param([IntPtr]$Hwnd)
    $buffer = New-Object char[] 256
    $n = [EcHarnessWin]::GetClassName($Hwnd, $buffer, $buffer.Length)
    if ($n -le 0) { return '' }
    return (-join ($buffer[0..($n - 1)]))
}

function Get-EcWindowText {
    param([IntPtr]$Hwnd)
    $buffer = New-Object char[] 512
    $n = [EcHarnessWin]::GetWindowTextW($Hwnd, $buffer, $buffer.Length)
    if ($n -le 0) { return '' }
    return (-join ($buffer[0..($n - 1)]))
}

function Get-EcWindowRect {
    param([IntPtr]$Hwnd)
    $r = New-Object EcHarnessWin+RECT
    if (-not [EcHarnessWin]::GetWindowRect($Hwnd, [ref]$r)) { return $null }
    return [pscustomobject]@{ Left = $r.Left; Top = $r.Top; Right = $r.Right; Bottom = $r.Bottom }
}

function Get-EcHwndHex {
    <# 与工具的 images[].hwnd 同一个写法（0x + 至少 8 位大写十六进制），用来比对归属。 #>
    param([Parameter(Mandatory)]$Hwnd)
    return ('0x{0:X8}' -f [int64]$Hwnd)
}

function Stop-EcWindow {
    <# 收尾本次建立的一个窗口：只碰这个 Process 实例，并按 exe 路径核对，PID 被复用就不动。 #>
    param([Parameter(Mandatory)]$Window)

    if (-not $Window -or -not $Window.Proc) { return }
    Stop-EcOwnProcess -Process $Window.Proc -ExpectedPath $Window.Helper | Out-Null
    [void]$script:EcOwnedWindows.Remove($Window)
}

function Stop-EcOwnedWindows {
    <# 兜底收尾：异常退出时（finally 里）调用，只清本次登记过的那些窗口。 #>
    foreach ($window in @($script:EcOwnedWindows.ToArray())) {
        try { Stop-EcWindow -Window $window } catch { }
    }
    $script:EcOwnedWindows.Clear()
}

function Get-EcOwnedWindowCount {
    return $script:EcOwnedWindows.Count
}

# ----------------------------------------------------------------------------
# 对话框（整屏确认框）：测试侧按控件 ID 点，与文案语言无关
# ----------------------------------------------------------------------------
function Find-EcDialog {
    <# 只在该进程的窗口里找 #32770，不去碰别人弹出的对话框。 #>
    param([Parameter(Mandatory)][int]$ProcessId)

    $script:EcFoundDialog = [IntPtr]::Zero
    $callback = [EcHarnessWin+EnumWinProc] {
        param($h, $data)
        if (((Get-EcClassName -Hwnd $h) -eq '#32770') -and (Test-EcWindowBelongsTo -Hwnd $h -ProcessId $ProcessId)) {
            $script:EcFoundDialog = $h
            return $false
        }
        return $true
    }
    [void][EcHarnessWin]::EnumWindows($callback, [IntPtr]::Zero)
    return $script:EcFoundDialog
}

function Wait-EcDialog {
    param([Parameter(Mandatory)][int]$ProcessId, [int]$TimeoutMs = 8000)

    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $h = Find-EcDialog -ProcessId $ProcessId
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 60
    }
    return [IntPtr]::Zero
}

function Click-EcDialogButton {
    <# IDYES=6 / IDNO=7：按钮不存在时返回 $false，让调用方决定这算不算失败。 #>
    param([Parameter(Mandatory)][IntPtr]$Dialog, [Parameter(Mandatory)][int]$ButtonId)

    if (-not [EcHarnessWin]::IsWindow($Dialog)) { return $false }
    $button = [EcHarnessWin]::GetDlgItem($Dialog, $ButtonId)
    if ($button -eq [IntPtr]::Zero) { return $false }
    [void][EcHarnessWin]::SendMessage($button, 0xF5, [IntPtr]::Zero, [IntPtr]::Zero)   # BM_CLICK
    return $true
}

# ----------------------------------------------------------------------------
# 图像度量：尺寸 + 颜色种数 + 红色占比 + 上半部主色
# ----------------------------------------------------------------------------
function Invoke-EcConsentShot {
    <#
    .SYNOPSIS
        起一次 ECAPTURE 截图，并把"确认框到底弹没弹"作为结果一起交回来。
    .DESCRIPTION
        截图授权分级之后，真机判据必须能区分三种情况：
          * 压根不该弹框（带 --yes 的窗口内容路径、--dry-run、参数错、无匹配、输出预检失败）
            —— 用 -ExpectNoDialog：探测一发现 #32770 就算失败，且不再等；
          * 该弹框、人自己点（默认）—— 不带 -Answer，函数只报告"弹过了"，不替人决定；
          * 该弹框、测试代答（只在专门腾出来的无隐私桌面上用 -Answer）——
            IDYES=6 放行、IDNO=7 拒绝。代人点"是"等于替人同意，默认不做。
        弹框与否由本进程窗口枚举判断（只查这个 PID 的窗口），不去碰别人弹出的对话框。
        返回对象：Exit / Stdout / Stderr / StdoutBytes / Dialog（弹过）/ Clicked（点到了）。
    #>
    param(
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$Arguments = @(),
        [int]$Answer = 0,                       # 0 = 只看不点；6 = 是；7 = 否
        [switch]$ExpectNoDialog,
        [int]$DialogWaitMs = 8000,
        [int]$TimeoutMs = 120000
    )

    $probe = {
        param($p)
        # Find-EcDialog 交回来的是 IntPtr，而 PowerShell 不把 [IntPtr]::Zero 当假值看：
        # 必须显式比一次，否则"根本没弹框"也会被读成"弹过了"（踩过一次）。
        $h = Find-EcDialog -ProcessId ([int]$p.Id)
        if ($h -eq [IntPtr]::Zero) { return '' }
        $script:EcConsentSeen = $true
        if ($Answer -gt 0) {
            $script:EcConsentClicked = (Click-EcDialogButton -Dialog $h -ButtonId $Answer)
        }
        return 'seen'
    }
    $script:EcConsentSeen = $false
    $script:EcConsentClicked = $false
    # 不弹框的用例只探测一小会儿（弹了才算失败），否则整个超时会白等在那里
    $wait = if ($ExpectNoDialog) { 1200 } else { $DialogWaitMs }
    $r = Invoke-EcProcess -FilePath $Exe -Arguments $Arguments -TimeoutMs $TimeoutMs `
        -Probe $probe -ProbeIntervalMs 60 -ProbeTimeoutMs $wait
    return [pscustomobject]@{
        Exit = $r.Exit; Stdout = $r.Stdout; Stderr = $r.Stderr; StdoutBytes = $r.StdoutBytes
        TimedOut = $r.TimedOut
        Dialog = ($script:EcConsentSeen -eq $true)
        Clicked = ($script:EcConsentClicked -eq $true)
        Raw = $r
    }
}

function Get-EcSignatureRgb {
    <#
        与 tests\helper\ec_window.cs 里 SignatureColor() 同一套公式，两边各自实现一遍是有意的：
        判据必须能独立于被测程序成立（这里也独立于那个辅助程序）。改公式时两边一起改。
    #>
    param([int]$Seed)
    $r = 40 + (($Seed * 53) % 180)
    $g = 120 + (($Seed * 97) % 110)
    $b = 130 + (($Seed * 149) % 110)
    return @($r, $g, $b)
}

function New-EcSignatureKey {
    param([int]$Seed)
    $rgb = Get-EcSignatureRgb -Seed $Seed
    return '{0},{1},{2}' -f $rgb[0], $rgb[1], $rgb[2]
}

function Get-EcImageStats {
    param(
        [Parameter(Mandatory)][string]$Path,
        [int]$Step = 5
    )

    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    Add-Type -AssemblyName System.Drawing | Out-Null
    $bmp = New-Object System.Drawing.Bitmap($Path)
    try {
        $colors = @{}
        $topTally = @{}
        $red = 0
        $samples = 0
        $half = [int]($bmp.Height / 2)
        for ($y = 0; $y -lt $bmp.Height; $y += $Step) {
            for ($x = 0; $x -lt $bmp.Width; $x += $Step) {
                $c = $bmp.GetPixel($x, $y)
                $samples++
                $key = '{0},{1},{2}' -f $c.R, $c.G, $c.B
                $colors[$key] = 1
                if ($c.R -gt 190 -and $c.G -lt 70 -and $c.B -lt 70) { $red++ }
                if ($y -lt $half) {
                    if ($topTally.ContainsKey($key)) { $topTally[$key] = $topTally[$key] + 1 }
                    else { $topTally[$key] = 1 }
                }
            }
        }
        $topDominant = ''
        $best = -1
        foreach ($k in $topTally.Keys) {
            if ($topTally[$k] -gt $best) { $best = $topTally[$k]; $topDominant = $k }
        }
        return [pscustomobject]@{
            Width = $bmp.Width
            Height = $bmp.Height
            Colors = $colors.Count
            ColorKeys = @($colors.Keys)
            Samples = $samples
            RedRatio = $(if ($samples) { [math]::Round($red / $samples, 3) } else { 0 })
            TopDominant = $topDominant
            Bytes = (Get-Item -LiteralPath $Path).Length
            Path = $Path
        }
    } finally {
        $bmp.Dispose()
    }
}

function Get-EcColorDistance {
    <# 两个 "r,g,b" 的曼哈顿距离；用来证明「这张图的颜色更像我自己的窗口」。 #>
    param([string]$A, [string]$B)
    if (-not $A -or -not $B) { return 9999 }
    $pa = $A.Split(',') | ForEach-Object { [int]$_ }
    $pb = $B.Split(',') | ForEach-Object { [int]$_ }
    return ([Math]::Abs($pa[0] - $pb[0]) + [Math]::Abs($pa[1] - $pb[1]) + [Math]::Abs($pa[2] - $pb[2]))
}

# ----------------------------------------------------------------------------
# 断言套件
# ----------------------------------------------------------------------------
function Reset-EcSuite {
    $script:EcFailures = New-Object System.Collections.Generic.List[string]
    $script:EcSkips = New-Object System.Collections.Generic.List[string]
    $script:EcPassCount = 0
}

function Assert-Ec {
    param(
        [Parameter(Mandatory)][bool]$Condition,
        [Parameter(Mandatory)][string]$Message,
        [switch]$Quiet
    )

    if ($Condition) {
        $script:EcPassCount++
        if (-not $Quiet) { Write-Host "  PASS  $Message" -ForegroundColor DarkGreen }
        return
    }
    [void]$script:EcFailures.Add($Message)
    Write-Host "  FAIL  $Message" -ForegroundColor Red
}

function Skip-Ec {
    <# 环境不满足时如实记 SKIP：既不算通过，也不算失败。 #>
    param(
        [Parameter(Mandatory)][string]$Message,
        [string]$Reason = ''
    )

    $text = $Message
    if ($Reason) { $text = "$Message（$Reason）" }
    [void]$script:EcSkips.Add($text)
    Write-Host "  SKIP  $text" -ForegroundColor DarkYellow
}

function Complete-EcSuite {
    <# 打印汇总并给出退出码：有 FAIL 就是 1，否则 0（SKIP 写在摘要里，不判失败）。 #>
    param([Parameter(Mandatory)][string]$Title)

    Write-Host ''
    if ($script:EcFailures.Count) {
        Write-Host ("{0}失败：{1} 项，通过 {2} 项" -f $Title, $script:EcFailures.Count, $script:EcPassCount) -ForegroundColor Red
        foreach ($f in $script:EcFailures) { Write-Host "  - $f" }
    } else {
        $extra = ''
        if ($script:EcSkips.Count) { $extra = "，跳过 $($script:EcSkips.Count) 项" }
        Write-Host ("{0}全部通过：{1} 项{2}" -f $Title, $script:EcPassCount, $extra) -ForegroundColor Green
    }
    if ($script:EcSkips.Count -and -not $script:EcFailures.Count) {
        foreach ($s in $script:EcSkips) { Write-Host "  - 未验证：$s" -ForegroundColor DarkYellow }
    }
    return [int][bool]$script:EcFailures.Count
}

Export-ModuleMember -Function *-*
