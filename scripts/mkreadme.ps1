$ErrorActionPreference = 'Stop'
$root = (Get-Item -LiteralPath "$PSScriptRoot\..").FullName
$exe = Join-Path $root 'build\ecapture.exe'
# README 里的帮助段固定用简体中文：换一台英文系统的机器跑本脚本，产出也必须一模一样
$help = (& $exe --help --lang zh-CN 2>$null) -join "`r`n"
if (-not $help) { throw '取不到 --help 输出' }

$template = @'
# EvernightCapture

命令行窗口截图工具（`ECAPTURE.EXE`）：按条件筛出窗口，用 Windows.Graphics.Capture 把那个窗口画面存成图片文件。

当前版本 0.4.0：`--capture` 的取值全部可用——`wgc`（Windows.Graphics.Capture，默认，也是 `auto` 的首选）、
`dwm`（DWM 缩略图）、`printwindow`（窗口自绘）、`bitblt`（拷屏幕可见像素）、
`duplication`（DXGI 桌面复制整屏帧后按窗口矩形裁剪）、`auto`（按 wgc→dwm→printwindow→bitblt 回退）。
曾实现过的 `magnification` 已删除，理由见 AGENTS.md。

## 构建与测试

需要 Visual Studio（"使用 C++ 的桌面开发"工作负载）+ Windows SDK，脚本会自动定位。

```powershell
.\build.ps1                # Release，产物 build\ecapture.exe
.\build.ps1 -Config Debug
.\build.ps1 -Clean
.\tests\cli.ps1            # 输出契约回归测试（61 例 + 多语言检查，一律 --dry-run，不截图）
.\scripts\check-lang.ps1   # 四种语言文案的 key / 占位符对齐检查，并确认 exe 里真有四份资源
.\tests\smoke.ps1          # 真机冒烟：起记事本窗口截图，校验 PNG 尺寸与像素内容
.\tests\channels.ps1       # 真机通道对比：每条通道逐个截图 + 遮挡对照
.\tests\fontview_shot.bat  # 真机批处理冒烟：起字体查看器 -> 逐通道截图并校验画面 -> 打开截图目录 -> 结束进程
```

产物是单文件：静态链接 CRT，目标机器不需要装 VC++ 运行时。

## 文案语言

`--lang`（短形式 `-l`）选文案语言：`zh-CN` / `zh-TW` / `en` / `ja`，不给或给 `auto` 时跟随系统显示语言，
系统语言不支持时用 `en`。文案是 exe 自带的嵌入资源（`resources/strings-<语言>.txt` 按语言编成四份
`RCDATA`），包括 `--help` 全文与每条诊断的 message/hint；`code`、JSON 键名、取值枚举不随语言变化。

## 帮助

下面这段是 `ECAPTURE.EXE --help` 的原样输出，改动选项后运行 `.\scripts\mkreadme.ps1` 重新生成，不要手工编辑。

```text
@HELP@
```

## 输出形式

`--help`、`--version`、以及不给任何条件时是纯文本。其余一律 JSON，只装捕获到的窗口信息与保存的文件信息，不带工具名/版本/输入回显等元信息。

成功（真实输出的形状，数值为一次实际截取的例子）：

```json
{
  "captured": 1,
  "images": [
    {
      "file": "D:\\shots\\game.png",
      "bytes": 248193,
      "width": 2560,
      "height": 1440,
      "format": "png",
      "hwnd": "0x001A0B4C",
      "pid": 12345,
      "title": "LocalSend",
      "class": "UnrealWindow",
      "elapsedMs": 41
    }
  ]
}
```

出错：

```json
{
  "captured": 0,
  "images": [],
  "errors": [
    {
      "code": "cli.invalid_number",
      "message": "--hwnd 需要有效的句柄值（十进制，或带 0x 前缀的十六进制）",
      "option": "--hwnd",
      "value": "zzz",
      "hint": "纯数字按十进制解析；十六进制请写成 0x……，或含 a-f 时自动按十六进制"
    }
  ]
}
```

规则：`captured` 与 `images` 恒在，空时是 `[]`；`errors` 只要非空就一定出现（`--quiet` 也只抑制 `notes`，不会吞掉错误）；`notes` 是提示，仅非空且未 `--quiet` 时出现；`input` 段仅 `--verbose` 时出现，内容是规范化后的输入。诊断项里为空的字段整个键省略。

退出码与输出内容相互独立：`0` 成功 / `1` 参数错 / `2` 未给条件 / `3` `--help` / `4` 无匹配窗口 / `5` 匹配多个窗口 / `6` 目标受保护 / `7` 截图失败 / `8` 写文件失败 / `9` 内部异常。

输出通道：默认全部写 stdout，stderr 保持为空；`--out -` 时 stdout 留给图片字节，JSON 整体改走 stderr。

## 匹配语义

不同选项之间是 AND（"都满足才开始"），同一选项写多次是 OR。所有条件必须命中同一个窗口，不会跨窗口拼接。

```powershell
ECAPTURE.EXE --process notepad.exe --title-contains 报告 D:\shots\r.png
# 进程是 notepad.exe 且 标题含"报告" 的那些窗口
```
'@

$readme = $template.Replace('@HELP@', $help)
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText((Join-Path $root 'README.md'), $readme, $utf8NoBom)
Write-Host "README.md 已生成：$((Get-Item (Join-Path $root 'README.md')).Length) 字节"
