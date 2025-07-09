<#
.SYNOPSIS
    EvernightCapture 构建脚本：自动定位 Visual Studio 的 C++ 工具链，再用 CMake + Ninja 构建。
.DESCRIPTION
    路径编码规矩（改之前先看，真机判据在 tests\build-path.ps1）：

    交给 cmd 的那份临时批处理**只允许 ASCII**，仓库目录、build 目录、vcvars64.bat 的路径一律
    通过进程环境块交给子进程，批处理正文里只出现 %ECP_xxx% 引用；批处理自己放在哪里执行也由
    %ECP_BAT% 给出，所以递给 cmd.exe 的命令行文本永远是固定 ASCII。原因都是实测踩过的：
      * 批处理正文要按代码页解码，中文路径先坏一次（旧实现用 ASCII 写，直接变成一串问号）；
        换 UTF-8 写也不解决——本机 OEM 码页是 65001 才恰好读对，换台 GBK 机器又是乱码。
        环境块是 UTF-16，全程没有代码页。
      * 正文里的单个 % 会启动变量展开：旧写法遇到 `项目 (100% &x)` 这种目录名时，
        两个参数被黏成一条报错路径（实测过）。展开出来的值不会再扫一遍，所以路径里的
        % ! & ( ) ^ 全部原样送达。
    另外显式 /v:off 关掉延迟展开，免得某些机器上路径里的 ! 被吃掉。
.EXAMPLE
    .\build.ps1                # Release
    .\build.ps1 -Config Debug
    .\build.ps1 -Clean         # 删除 build 目录后重新配置
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

# ----------------------------------------------------------------------------
# 批处理正文：纯 ASCII，绝对路径只以 %ECP_*% 出现
# ----------------------------------------------------------------------------
function New-BuildScriptText {
    <#
        生成临时批处理正文。环境取值：ECP_VCVARS / ECP_PATHADD / ECP_ROOT / ECP_BUILDDIR / ECP_CONFIG。
        退出码分得很细，是为了让「VS 环境没导进来」和「代码编不过」在报错里一眼分得开；
        环境导入的任何一步失败都在跑 cmake 之前 exit，不会拿着半套环境去配置。
    #>
    $cr = [char]13
    $lf = [char]10
    $lines = @(
        '@echo off',
        'setlocal disabledelayedexpansion',
        'call "%ECP_VCVARS%" >nul 2>&1',
        'if errorlevel 1 exit /b 11',
        'if not defined VCINSTALLDIR exit /b 12',
        'where cl.exe >nul 2>&1',
        'if errorlevel 1 exit /b 13',
        'if not defined WindowsSDKDir exit /b 14',
        'where cmake.exe >nul 2>&1',
        'if errorlevel 1 exit /b 15',
        'if defined ECP_PATHADD set "PATH=%ECP_PATHADD%;%PATH%"',
        'cmake -S "%ECP_ROOT%" -B "%ECP_BUILDDIR%" -G Ninja -DCMAKE_BUILD_TYPE=%ECP_CONFIG%',
        'if errorlevel 1 exit /b 20',
        'cmake --build "%ECP_BUILDDIR%"',
        'if errorlevel 1 exit /b 21',
        'exit /b 0'
    )
    return (($lines -join "$cr$lf") + "$cr$lf")
}

function Get-BuildScriptFailureText {
    <# 批处理退出码 -> 人能读懂的一句话。只增不改号，与工具的退出码同一套规矩。 #>
    param([int]$Code)

    switch ($Code) {
        11 { return "Visual Studio 环境导入失败：调用 vcvars64.bat 返回非零" }
        12 { return "Visual Studio 环境导入失败：跑完 vcvars64.bat 却没定义 VCINSTALLDIR" }
        13 { return "Visual Studio 环境导入失败：导入后 PATH 里找不到 cl.exe（C++ 工具链不可用）" }
        14 { return "Visual Studio 环境导入失败：没定义 WindowsSDKDir（缺 Windows SDK，rc.exe 与 winres.h 会找不到）" }
        15 { return "找不到 cmake.exe：VS 没自带 CMake，PATH 里也没有（装 VS 的 C++ CMake 组件，或把 cmake 放进 PATH）" }
        20 { return "cmake 配置阶段失败，见上面 cmake 的输出" }
        21 { return "编译阶段失败，见上面构建日志" }
        default { return "构建批处理返回未预期的退出码 $Code" }
    }
}

function Assert-BatchAsciiText {
    <# 写盘前的守卫：正文一旦混进非 ASCII，就说明有人又把路径直接写进批处理了，当场拦下。 #>
    param([Parameter(Mandatory)][string]$Text)

    $m = [regex]::Match($Text, "[^\u0009\u000A\u000D\u0020-\u007E]")
    if (-not $m.Success) { return }
    $msg = ('临时批处理正文必须全是 ASCII：遇到 U+{0:X4} 就会被 cmd 按代码页读坏。' -f [int][char]$m.Value) +
           '别把绝对路径写进正文，改成 %ECP_xxx% 环境变量传进去。'
    throw $msg
}

function Invoke-BuildScript {
    <#
        把正文写进唯一命名的临时 .bat，用 cmd 执行，退出码非 0 一律 throw；
        无论成败只删自己建的那一个文件。构建日志不重定向，直接进当前控制台。
    #>
    param(
        [Parameter(Mandatory)][string]$Text,
        [hashtable]$Env = @{},
        [string]$BatDir
    )

    Assert-BatchAsciiText -Text $Text
    $dir = $BatDir
    if (-not $dir) { $dir = $env:TEMP }
    if (-not $dir) { throw '没有可用的临时目录（%TEMP% 为空且没给 -BatDir）' }

    $leaf = 'ecapture-build-{0}-{1}-{2}.bat' -f $PID, (Get-Date -Format 'yyyyMMdd-HHmmss'),
            ([Guid]::NewGuid().ToString('N')).Substring(0, 8)
    $bat = Join-Path $dir $leaf
    try {
        # 正文已经被守卫成纯 ASCII，这里的 ASCII 编码不会再改坏任何字符
        [IO.File]::WriteAllBytes($bat, [Text.Encoding]::ASCII.GetBytes($Text))

        # 子进程输出的是 UTF-8 字节（cmake / ninja / cl），控制台码页不切到 UTF-8 的话
        # 日志里的中文路径会花屏。只动当前会话，跑完还原；没有输出码页可问时整段跳过。
        $prevEncoding = $null
        try {
            $prevEncoding = [Console]::OutputEncoding
            [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false, $false)
        } catch { $prevEncoding = $null }

        try {
            $psi = New-Object Diagnostics.ProcessStartInfo
            $psi.FileName = $env:ComSpec
            $psi.UseShellExecute = $false
            $psi.CreateNoWindow = $false
            # 固定 ASCII：批处理路径自己走环境块，命令行里只有 %ECP_BAT%；
            # /d 跳 autorun，/s 让首尾那对引号按 cmd 的规则剥掉，/v:off 关延迟展开。
            $psi.Arguments = '/d /s /v:off /c ""%ECP_BAT%""'
            $psi.EnvironmentVariables['ECP_BAT'] = $bat
            foreach ($k in $Env.Keys) { $psi.EnvironmentVariables[[string]$k] = [string]$Env[$k] }

            $p = [Diagnostics.Process]::Start($psi)
            try {
                $p.WaitForExit()
                $code = $p.ExitCode
            } finally { $p.Dispose() }
        } finally {
            if ($null -ne $prevEncoding) { try { [Console]::OutputEncoding = $prevEncoding } catch { } }
        }
        if ($code -ne 0) { throw (Get-BuildScriptFailureText -Code $code) }
    } finally {
        Remove-Item -LiteralPath $bat -Force -ErrorAction SilentlyContinue
    }
}

# ----------------------------------------------------------------------------
# 工具链定位
# ----------------------------------------------------------------------------
function ConvertFrom-ToolBytes {
    <#
        外部工具的 stdout 字节 -> 文本：先按 UTF-8 严格解，解不动再按系统 ANSI 解。
        vswhere 这类原生工具在不同版本/不同机器上分别会吐 UTF-8 或 ACP，安装路径含中文时
        两条都得上；纯 ASCII 路径两种走法结果一样。
    #>
    param([byte[]]$Bytes)

    if (-not $Bytes -or -not $Bytes.Length) { return '' }
    try {
        return (New-Object Text.UTF8Encoding($false, $true)).GetString($Bytes)
    } catch {
        return [Text.Encoding]::Default.GetString($Bytes)
    }
}

function Invoke-ToolText {
    <# 起一个工具并把 stdout 字节收回来自己解码（不显示），参数是固定 ASCII，不含路径拼接。 #>
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string]$Arguments = '',
        [int]$TimeoutMs = 60000
    )

    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = $Arguments
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $p = [Diagnostics.Process]::Start($psi)
    try {
        # 先读到 EOF 再等退出：边等边读会互相等死
        $ms = New-Object IO.MemoryStream
        $src = $p.StandardOutput.BaseStream
        $buf = New-Object byte[] 4096
        while ($true) {
            $n = $src.Read($buf, 0, $buf.Length)
            if ($n -le 0) { break }
            $ms.Write($buf, 0, $n)
        }
        if (-not $p.WaitForExit($TimeoutMs)) { throw "$FilePath 没有按时退出" }
        return (ConvertFrom-ToolBytes -Bytes $ms.ToArray())
    } finally {
        $p.Dispose()
    }
}

function Resolve-VcVarsPath {
    <# vswhere 定位带 C++ 桌面工作负载的 VS，返回 vcvars64.bat 路径；找不到就 throw。 #>
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "找不到 vswhere.exe：$vswhere" }

    $text = Invoke-ToolText -FilePath $vswhere -Arguments `
        '-latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath'
    $vs = (@($text -split "[`r`n]+" | Where-Object { $_ }) | Select-Object -First 1)
    if (-not $vs) { throw '未找到安装 "使用 C++ 的桌面开发" 工作负载的 Visual Studio' }

    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) { throw "找不到 vcvars64.bat：$vcvars" }
    return [pscustomobject]@{ Vs = $vs; VcVars = $vcvars }
}

function Resolve-ToolDirs {
    <# VS 自带的 cmake / ninja 目录（MSYS 版 cmake 会把盘符路径当相对路径改写，必须避开）。 #>
    param([Parameter(Mandatory)][string]$Vs)

    $dirs = @(
        (Join-Path $Vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
        (Join-Path $Vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
    ) | Where-Object { Test-Path -LiteralPath $_ }
    return ($dirs -join ';')
}

function Start-Build {
    param(
        [Parameter(Mandatory)][string]$Config,
        [switch]$Clean
    )

    # 结尾反斜杠会跟命令行引号拼成转义引号（"D:\x\" 里的 \"），这里统一去掉
    $root = (Get-Item -LiteralPath $PSScriptRoot).FullName.TrimEnd('\')
    $buildDir = (Join-Path $root 'build').TrimEnd('\')

    $tool = Resolve-VcVarsPath
    $pathAdd = Resolve-ToolDirs -Vs $tool.Vs

    if ($Clean -and (Test-Path -LiteralPath $buildDir)) {
        Write-Host "清理 $buildDir" -ForegroundColor Yellow
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }

    Write-Host "配置/构建 ($Config) ..." -ForegroundColor Cyan
    Write-Host "  MSVC: $($tool.Vs)"
    Write-Host ("  cmake/ninja: {0}" -f $(if ($pathAdd) { $pathAdd } else { 'PATH 中的版本' }))

    $batEnv = @{
        ECP_VCVARS   = $tool.VcVars
        ECP_PATHADD  = $pathAdd
        ECP_ROOT     = $root
        ECP_BUILDDIR = $buildDir
        ECP_CONFIG   = $Config
    }
    Invoke-BuildScript -Text (New-BuildScriptText) -Env $batEnv

    $exe = Join-Path $buildDir 'ecapture.exe'
    if (-not (Test-Path -LiteralPath $exe)) { throw "构建完成但找不到产物：$exe" }
    Write-Host "`n产物: $exe" -ForegroundColor Green
}

# 被 dot-source 时只定义函数：tests\build-path.ps1 单独验正文编码与报错分派，不真构建
if ($MyInvocation.InvocationName -eq '.') { return }

Start-Build -Config $Config -Clean:$Clean
