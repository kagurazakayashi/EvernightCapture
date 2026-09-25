<#
.SYNOPSIS
    构建目录的安全清理：删掉构建产物，但**保留开发版 ECAPTURE.EXE 旁边的截图历史**。
.DESCRIPTION
    为什么要有这一份，而不是各自写一句 Remove-Item -Recurse：

      * 默认开启的截图历史归档住在"实际运行的那个 exe 所在目录\history\日期\"里。开发版的 exe 就
        躺在 build\ 里，所以它的历史也在 build\history\。历史是**持续保留的截图数据**，不是可以
        随手重建的缓存，也不是构建产物：一次"清理构建目录"不该顺手把用户截过的图删掉。
      * clean.ps1 与 .\build.ps1 -Clean 是同一条约定的两个入口，必须共用同一份判据。只修一处
        而让另一处仍然整目录删除，等于没修。
      * 搬移不算保留：把 build\history 挪出去再挪回来，中途断电或被杀就留下一个没人认领的目录。
        这里的做法是**原地不动**——先删 build\ 里除 history 之外的每一项，然后 build\ 因为里面
        还剩历史而保留下来（里面只剩历史）。CMake 的缓存被删干净了，下一次配置仍是全新配置。

    安全边界（三条都判，不"看着像目录就删"）：
      1. build\ 本身是重解析点（junction / 符号链接）时**拒绝**：跟着它删会删到别处去。
      2. build\history 是重解析点、或者根本不是目录（一个文件占了这个名字）时**拒绝**：
         这时"保留历史"这件事没法安全承诺，就如实停下来让用户先看一眼，而不是静默删掉或搬走。
      3. 递归删除时遇到任何重解析点，只删那个链接本身，绝不跟着链接进目标目录。

    返回一份结论（不 throw，由调用方决定怎么报错）：
      Removed      build\ 里的产物删掉了没有（false = 一条都没动）
      NothingToDo  "本来就没有 build\ 目录"这一种**正常空操作**：既不 Removed 也不算被拒绝。
                   少了这一格，"在全新检出上跑 .\build.ps1 -Clean"会被调用方当成失败而中断构建。
      Kept         被保留下来的东西：'history' / 'nothing'
      BuildDirKept build\ 目录本身是否还在（历史在里面时必然为真）
      HistoryPath  历史目录的路径（不存在时为空）
      Reason       为什么没删 / 为什么留下了什么（给人看的一句话）
.EXAMPLE
    Import-Module .\scripts\build-clean.psm1
    Remove-EcBuildArtifacts -BuildDir .\build
#>
Set-StrictMode -Version 3.0

function Remove-EcTreeSafe {
    <#
        递归删一棵子树，遇到重解析点只删链接本身（PowerShell 5.1 的 Remove-Item -Recurse
        会跟着 junction 进目标目录去删，那正是这里不能做的事）。
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Path)

    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        # 链接本身删掉就够了：目标在别处，不归这次清理管。
        if ($item.PSIsContainer) { [IO.Directory]::Delete($Path) } else { [IO.File]::Delete($Path) }
        return
    }
    if ($item.PSIsContainer) {
        foreach ($child in @(Get-ChildItem -LiteralPath $Path -Force)) {
            Remove-EcTreeSafe -Path $child.FullName
        }
        [IO.Directory]::Delete($Path)
        return
    }
    [IO.File]::Delete($Path)
}

function Remove-EcBuildArtifacts {
    <#
        清掉 BuildDir 里的构建产物，保留 BuildDir\history。判据见文件头那三条安全边界。
        一切都不确定时宁可不删：返回 Removed=false 与那句为什么。
    #>
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$BuildDir)

    $result = [pscustomobject]@{
        Removed      = $false
        NothingToDo  = $false
        Kept         = 'nothing'
        BuildDirKept = $false
        HistoryPath  = ''
        Reason       = ''
    }

    if (-not (Test-Path -LiteralPath $BuildDir)) {
        # 全新检出、或上一次已经清过：这是正常空操作，不是拒绝。调用方据此继续构建。
        $result.NothingToDo = $true
        $result.Reason = '没有 build 目录，无需清理。'
        $result.BuildDirKept = $false
        return $result
    }

    $buildItem = Get-Item -LiteralPath $BuildDir -Force
    if (-not $buildItem.PSIsContainer) {
        $result.Reason = "$BuildDir 不是一个目录，没有动它。"
        $result.BuildDirKept = $true
        return $result
    }
    if (($buildItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        $result.Reason = "$BuildDir 是一个重解析点（junction 或符号链接）。跟着它递归删除会删到" +
                         '它指向的那个目录去，那不在本次清理的范围里，所以一条都没动。' +
                         '请先确认它指向哪里；要清的话对真实目录名再来一次。'
        $result.BuildDirKept = $true
        return $result
    }

    $history = Join-Path $BuildDir 'history'
    $historyKept = $false
    if (Test-Path -LiteralPath $history) {
        $hItem = Get-Item -LiteralPath $history -Force
        if (($hItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            $result.Reason = "$history 是一个重解析点，不是这个程序自己建的那棵历史目录。" +
                             '这里不删、不搬、也不改它的标签与指向：请先人工确认它到底是什么，' +
                             '再把 build 里别的东西清掉。'
            $result.BuildDirKept = $true
            return $result
        }
        if (-not $hItem.PSIsContainer) {
            $result.Reason = "$history 这个名字被一个文件占着，而截图历史应当是一棵目录树。" +
                             '这里不覆盖、不删除那个文件，也不动 build 里的别的东西：' +
                             '请先看一眼那是什么，再决定怎么清。'
            $result.BuildDirKept = $true
            return $result
        }
        $historyKept = $true
        $result.HistoryPath = $hItem.FullName
    }

    try {
        if (-not $historyKept) {
            # 没有历史要保留：整棵目录删掉，与旧行为一致（仍然走同一份不跟链接的递归）。
            Remove-EcTreeSafe -Path $BuildDir
            $result.Removed = $true
            $result.Kept = 'nothing'
            $result.BuildDirKept = Test-Path -LiteralPath $BuildDir
            $result.Reason = '已删除 build 目录（本次没有截图历史需要保留）。'
            return $result
        }

        foreach ($child in @(Get-ChildItem -LiteralPath $BuildDir -Force)) {
            if ($child.Name -eq 'history') { continue }
            Remove-EcTreeSafe -Path $child.FullName
        }
        # build\ 本身留着：里面只剩那棵历史目录。缓存与产物已经清干净，下一次 cmake 配置
        # 是全新的一次；历史一个字节都没被搬动过。
        $result.Removed = $true
        $result.Kept = 'history'
        $result.BuildDirKept = $true
        $result.Reason = '已清掉 build 里的构建产物，保留截图历史：' + $result.HistoryPath
        return $result
    } catch {
        $result.Reason = "清理中途失败：$($_.Exception.Message)。" +
                         '已经删掉的不回来，剩下的原样留着（不重试、也不换个目录继续删）。'
        $result.BuildDirKept = Test-Path -LiteralPath $BuildDir
        return $result
    }
}

Export-ModuleMember -Function Remove-EcBuildArtifacts, Remove-EcTreeSafe
