# EvernightCapture

命令行窗口截图工具（`ECAPTURE.EXE`）：按条件筛出窗口，用 Windows.Graphics.Capture 把那个窗口画面存成图片文件。

当前版本 0.4.0：取图通道已实现 `--capture wgc`（Windows.Graphics.Capture，也是默认与 `auto` 的实际行为）；
`dwm` / `printwindow` / `bitblt` / `duplication` / `magnification` 参数已就位，运行时返回 `capture.unsupported`。

## 构建与测试

需要 Visual Studio（"使用 C++ 的桌面开发"工作负载）+ Windows SDK，脚本会自动定位。

```powershell
.\build.ps1                # Release，产物 build\ecapture.exe
.\build.ps1 -Config Debug
.\build.ps1 -Clean
.\tests\cli.ps1            # 输出契约回归测试（47 例，一律 --dry-run，不截图）
.\tests\smoke.ps1          # 真机冒烟：起记事本窗口截图，校验 PNG 尺寸与像素内容
```

产物是单文件：静态链接 CRT，目标机器不需要装 VC++ 运行时。

## 帮助

下面这段是 `ECAPTURE.EXE --help` 的原样输出，改动选项后运行 `.\scripts\mkreadme.ps1` 重新生成，不要手工编辑。

```text
EvernightCapture (ECAPTURE.EXE) —— 按条件窗口截图，基于 Windows.Graphics.Capture

用法: ECAPTURE.EXE [条件...] <输出路径>        不给任何条件 => 显示本帮助
      ECAPTURE.EXE [条件...] --out <路径>      路径写 - 表示把图片字节输出到标准输出

窗口匹配条件（同一选项多次出现取并集，不同选项必须同时命中）
  --hwnd <handle>             窗口句柄。纯数字按十进制，0x 前缀或含 a-f 按十六进制；推荐写 0x
  --pid <pid>                 进程 ID，十进制且大于 0
  --process, -p <image-name>  映像文件名（不含路径），忽略大小写；无扩展名时按 .exe 处理
  --exe <full-path>           映像完整路径，忽略大小写
  --title, -t <exact-title>   窗口标题精确匹配
  --title-contains, -T <text> 窗口标题包含子串
  --title-regex, -R <regex>   窗口标题正则匹配，ECMAScript 语法，解析期即校验
  --class, -c <class-name>    窗口类名，忽略大小写，如 Notepad / CabinetWClass

匹配到多个窗口时（互斥）
  --index, -i <n>             取第 n 个窗口，从 1 开始，按可见性/叠放次序排序
  --newest                    取最后创建的窗口
  --oldest                    取最早创建的窗口
  --all, -a                   每个匹配窗口各存一张

取图方式（默认 wgc；受系统版本或窗口性质限制时会失败）
  --capture, -C <method>      wgc(默认，被遮挡也能截) / dwm(DWM 缩略图) / printwindow(窗口自绘) / bitblt(拷屏幕可见像素) / duplication(DXGI 桌面复制) / magnification(放大镜 API) / auto(按 wgc-dwm-printwindow-bitblt 回退)

输出
  --out, -o <path|->          输出路径；特殊值 - 表示把图片字节写到标准输出。也可用位置参数
  --format, -f <name>         强制编码格式，默认由输出文件扩展名推断
  --quality <1-100>           JPEG 质量，默认 90
  --no-overwrite              目标已存在时不覆盖，报错退出

其它
  --dry-run, -d               只解析并列出候选窗口，不截图不写文件
  --json, -j                  已废弃的兼容开关，无副作用：成功与错误本来就输出 JSON
  --verbose, -v               JSON 中追加 input 段（规范化后的全部输入），并保留 notes
  --quiet, -q                 省略 notes；errors 无论如何都会返回
  --help, -h                  输出文本帮助（本段）
  --version                   输出版本与阶段

写法: --opt=value / -opt / /opt 都接受；取值本身以 - 开头时写成 --title=-x，或用 -- 结束选项解析
输出: 成功与错误都是 JSON，只含 captured / images（另有 errors / notes，--verbose 才有 input）
      --help / --version 以及不给条件时是文本
退出码: 0 成功 / 1 参数错 / 2 未给条件 / 3 --help / 4 无匹配窗口 / 5 匹配多个窗口 /
        6 目标受保护 / 7 截图失败 / 8 写文件失败 / 9 内部异常
当前构建: 已实现 --capture wgc（auto 同样走 wgc），其他方式返回 capture.unsupported；输出目录必须已存在

示例:
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title LocalSend --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 报告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --process notepad.exe --out - > snap.png
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