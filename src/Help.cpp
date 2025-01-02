#include "CliOptions.h"

namespace ecapture {

const std::wstring& HelpText() {
    static const std::wstring text = LR"(
EvernightCapture (ECAPTURE.EXE) —— 基于 Windows.Graphics.Capture 的命令行截图工具

用法
  ECAPTURE.EXE [条件...] <输出路径>
  ECAPTURE.EXE [条件...] --out <输出路径>
  不给出任何条件、或带 --help 时显示本帮助。

匹配语义
  · 同一个选项写多次 = 并集（OR）
  · 不同选项同时出现 = 交集（AND），全部满足才开始截图
  · 条件全部作用在"同一个窗口"上，不会跨窗口拼接

窗口匹配条件
  --hwnd <值>            窗口句柄。十进制，或带 0x 前缀的十六进制（推荐加 0x）
  --pid <值>             进程 ID（十进制）
  --process <名称>       映像文件名，如 notepad.exe（忽略大小写，不含路径）
  --exe <完整路径>       映像完整路径，如 D:\App\Evernight.exe（忽略大小写）
  --title <标题>         窗口标题精确匹配
  --title-contains <字串> 窗口标题包含
  --title-regex <正则>   窗口标题正则匹配（ECMAScript 语法）
  --class <类名>         窗口类名，如 Notepad / CabinetWClass（忽略大小写）

目标不唯一时（匹配到多个窗口）
  默认：报错并列出候选，退出码 5，请补充条件或用下列选项消歧
  --index <N>            取第 N 个（从 1 开始，按 Z 序/可见性排序）
  --newest / --oldest    取最后创建 / 最早创建的窗口
  --all                  每个匹配窗口各存一张

输出
  <输出路径>             位置参数；扩展名决定编码（png/jpg/jpeg/bmp/tif/gif/webp/ico）
  --out <路径>           同上。特殊值 - 表示把图片字节写到标准输出
  --format <名称>        强制编码格式：auto png jpg bmp tiff gif webp ico（默认 auto）
  --quality <1-100>      JPEG 质量（默认 90）
  --no-overwrite         目标已存在时不覆盖（报错退出）
  文件名占位符：%d 日期 %t 时间 %h 句柄 %p 进程ID %i 序号 %n 窗口标题

其它
  --dry-run              只解析参数并打印匹配到的窗口信息，不写文件
  --json                 以 JSON 输出结果（窗口标题/句柄/尺寸/文件路径/耗时）
  -v, --verbose          打印详细过程      -q, --quiet  只输出错误
  -h, --help             显示本帮助        --version    显示版本
  短选项对应关系：
    -p=--process  -t=--title  -T=--title-contains  -R=--title-regex  -c=--class
    -i=--index  -a=--all  -o=--out  -f=--format  -d=--dry-run  -j=--json

取值写法
  · 选项名不区分大小写；支持 --opt=value、单横线 -opt、斜杠 /opt 三种写法
  · 短选项可合并：-vq
  · 值以 - 开头时用 --title=-abc，或把条件写在前面、用 -- 结束选项解析
  · 标题/正则等取值原样传给 Windows API，中文由控制台编码决定，建议 chcp 65001

示例
  ECAPTURE.EXE --process notepad.exe D:\shots\epad.png
  ECAPTURE.EXE --title "无期迷途" --class UnityWndClass --out D:\shots\game.png
  ECAPTURE.EXE --pid 12345 --title-contains 报告 --all D:\shots\rpt_%i.png
  ECAPTURE.EXE --hwnd 0x001A0B4C --format png --no-overwrite out.png
  ECAPTURE.EXE --exe "D:\Program Files\App\App.exe" --title-regex "^(App|应用).*" out.jpg
  ECAPTURE.EXE --process notepad.exe --out - > snap.png

退出码
  0 成功          1 参数错误        2 未指定任何条件（打印帮助）
  3 --help（打印帮助）
  4 无匹配窗口    5 匹配到多个窗口  6 目标受保护/被拒绝（DRM、安全窗口）
  7 截图失败      8 写文件失败

当前状态
  本版本只完成参数解析与校验（打印解析结果后按退出码 0 返回），
  窗口枚举与 Windows.Graphics.Capture 截图尚未实现。
)";
    return text;
}

}  // namespace ecapture
