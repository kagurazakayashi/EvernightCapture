# 第三方依赖与许可证材料（Third-Party Notices）

EvernightCapture 本体以 **木兰宽松许可证 第 2 版（Mulan PSL v2）** 授权（见 `LICENSE`）。
本文件登记**不是**本项目编写、却被链接进产物或被构建流程使用的那一份份外部依赖，
按 AGENTS.md《结构与依赖边界》"只使用任务需要的依赖，记录依赖版本选择和许可证材料"的要求维护。

改任何一条依赖（版本、来源、许可、链接方式）都要同时改这里、`MEMORY.md` 与 README 的依赖小节。

---

## 1. Windows 侧：Windows SDK 与 C++/WinRT（不随本项目分发）

| 项目 | 用途 | 许可与分发说明 |
| --- | --- | --- |
| Windows SDK 头与导入库（`windowsapp.lib` / `d3d11.lib` / `dxgi.lib` / `dwmapi.lib` / `ole32.lib` / `user32.lib` / `gdi32.lib` / `kernel32.lib` / `advapi32.lib`） | WGC / DWM / PrintWindow / BitBlt / Desktop Duplication 取帧，`Windows.Graphics.Imaging.BitmapEncoder` 编码，D3D11 纹理拷回 CPU，系统通知 toast | 微软的操作系统组件。**不复制、不打包、不静态链接进仓库产物**：链接的是导入库，运行时由目标机器的 Windows 提供。构建产物只含本项目自己编译的目标文件与 MSVC 静态 CRT。 |
| C++/WinRT 投影头（`winrt/*.h`，随 Windows SDK 提供） | 调用上面那些 WinRT 接口 | 同上：MIT 许可的 SDK 组成部分（投影头是源码级模板，编译进本项目目标文件，不额外分发上游副本）。 |
| MSVC 运行时（`/MT` 静态 CRT） | 单文件交付 | Visual Studio 许可允许将再分发红线下的 CRT 静态链接进产物；不附带上游 DLL。 |
| Inno Setup（仅 `build-installer.ps1` 使用） | 制作安装程序 | 随 Windows 构建入口的外部工具，不在 Linux 侧使用，也不进入源码产物。 |

Windows 侧的编码器是 `Windows.Graphics.Imaging.BitmapEncoder`（`src/platform/windows/WinEncoder.cpp`）。
选择它而不是 WIC 的理由写在同一个文件头部：它与 WGC 共用 `windowsapp.lib`，不引入新的第三方依赖，
且 JPEG 质量接口明确。

---

## 2. Linux 侧：图片编码依赖（本步新增）

本项目**不**把下面任何一份源码复制进仓库，也**不**自动安装它们：构建期由 CMake 的
`find_package(PNG)` / `find_package(JPEG)` 发现发行版已安装的包（与 wxWidgets 同一套"发现而不假定路径"的
做法），缺失时如实报告并把编码判据记 `NOT RUN`（`./build.sh --codec` 可把缺依赖变成硬错误）。

| 依赖 | 本机实测版本 | 用途 | 许可证 | 链接方式 | 上游与许可原文 |
| --- | --- | --- | --- | --- | --- |
| libpng | 1.6.48 | PNG 编码（`png_write_row` + 自定义写出回调）；判据侧的像素回读 | **libpng License**（BSD/zlib 风格：允许使用与再分发，须保留版权声明，含一条"不得声称 endorse"的条款；非 copyleft） | 发行版共享库（`PNG::PNG`） | <https://github.com/pnggroup/libpng/blob/master/LICENSE> ；Debian 包里在 `/usr/share/doc/libpng-dev/copyright` |
| zlib | 1.3.1 | libpng 的 deflate 依赖（本项目不直接调用） | **zlib License**（与 libpng License 同源的那一份极简许可） | 发行版共享库（由 libpng 带上） | <https://zlib.net/zlib_license.html> ；`/usr/share/doc/zlib1g-dev/copyright` |
| libjpeg（由 libjpeg-turbo 提供 IJG 兼容接口） | libjpeg-turbo 2.1.5（Debian `libjpeg62-turbo`，SONAME 62） | JPEG 基线编码（自定义 destination manager）；判据侧的像素回读 | libjpeg-turbo 是**多份 BSD 风格许可的组合**：BSD-3-Clause（本项目自己的改动与 SIMD）、IJG 那一份 libjpeg 许可（继承自 JPEG Reference Software）、以及若干文件的 zlib 与 Expat(MIT)。均为宽松许可、非 copyleft | 发行版共享库（`JPEG::JPEG`） | <https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md> ；`/usr/share/doc/libjpeg62-turbo-dev/copyright` |

补充说明（本步的依赖决定，逐条与代码里的事实对应）：

- **BMP 不引第三方**：32 位 BI_RGB 的容器由 `src/platform/linux/PosixEncoder.cpp` 自己写
  （14 字节文件头 + 40 字节 BITMAPINFOHEADER + 逐行 bottom-up 像素），
  判据也自己按格式规范解析回来，不借任何库。
- **TIFF 与 GIF 在 Linux 上明确不支持**（`src/core/CodecCapability.h` 登记为 `kNoEncoder`）：
  本机没有 libtiff，而 GIF 只能写 256 色调色板 —— 把真彩截图硬压成调色板是一条会静默改变画面的降级。
  将来要接 libtiff（BSD-3-Clause，`libtiff` 上游许可）时必须先补这张表、这里的账与 README 的格式表。
- **版本从哪来**：不是手工钉住的上游版本，而是**这台开发机上发行版实际提供的那一份**
  （Debian 13 / trixie）。CMake 不检查最低版本（`find_package(PNG QUIET)` / `find_package(JPEG QUIET)`），
  因为本项目用的接口（`png_write_row` / `png_set_write_fn` / `png_set_IHDR`、
  `jpeg_set_quality` / `jpeg_write_scanlines` 与自定义 destination manager）在 libpng 1.6 与
  libjpeg 6b 兼容接口上都存在多年；换发行版时按上面的实测方式重新记账即可。
- **本项目自己的许可证与这些依赖的关系**：三份都是宽松许可，动态链接不产生额外义务，
  也不与 Mulan PSL v2 冲突。它们都不要求本项目开源任何衍生二进制。
- **不引入的候选**：wxWidgets（GUI 那一步的依赖，已经记在下面第 3 节）、
  stb_image_write（single-header，Public Domain / MIT 双许可）——
  本步选择发行版的 libpng / libjpeg 而不是把上游源码复制进仓库，
  理由是"补丁与安全更新跟着发行版走，仓库里不留第二份副本要人工跟进"。

## 3. Linux 侧：wxWidgets（GUI 那一步的依赖，本步未改动）

| 依赖 | 本机实测版本 | 用途 | 许可证 | 状态 |
| --- | --- | --- | --- | --- |
| wxWidgets（wxGTK 工具集，组件 `core` + `base`） | 3.2.8 | 程序自有 GUI（确认、错误提示、通知）的统一实现入口 | **LGPL 2.1 with an exception**（动态链接无额外义务；静态链接需要按 wx 的那条例外提供可重新链接的材料） | 本步只有依赖探针 `ecapture-gui-probe`，没有 GUI 功能；共享库链接 |

---

## 4. 不属于依赖的那一类（写在这里免得被误登记）

- `resources/strings-*.txt`、`resources/icon.ico`、`resources/ECAPTURE.example.ini`：
  本项目自己编写并拥有的仓库文件。
- `scripts/*.cmake`、`scripts/*.sh`、`*.ps1`、`CMakeLists.txt`、`CMakePresets.json`：
  本项目的构建流程，CMake 只是使用它的外部工具（BSD-3-Clause，不分发、不链接进产物）。
- 测试用的合成图像数据：由 `tests/*.cpp` 运行时算出，不含任何外部素材文件。
