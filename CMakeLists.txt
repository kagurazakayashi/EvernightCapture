cmake_minimum_required(VERSION 3.25)

# 版本号只在 src/Version.h 写一次（C++ 的 --version 与 exe 文件属性都从它取），
# 这里把它读出来喂给 project(VERSION ...)，别再手抄一份数字到本文件里。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h" ECAPTURE_VERSION_HEADER)
foreach(part IN ITEMS MAJOR MINOR PATCH)
  string(REGEX MATCH "#define ECAPTURE_VERSION_${part} +([0-9]+)" _match
         "${ECAPTURE_VERSION_HEADER}")
  # 判"匹配到没有"要看整段匹配，别拿 CMAKE_MATCH_1 当真值：主版本号就是 0，
  # 用 if(NOT CMAKE_MATCH_1) 会把 0 当成没匹配上。
  if(_match STREQUAL "")
    message(FATAL_ERROR "src/Version.h 里找不到 #define ECAPTURE_VERSION_${part}")
  endif()
  set(ECAPTURE_${part}_VERSION "${CMAKE_MATCH_1}")
endforeach()

# LANGUAGES 里只有 CXX：RC 是 Windows 的资源编译器（rc.exe），Linux 上没有它，
# 在这里列出来会让 Linux 的配置阶段直接找不到编译器。Windows 分支再 enable_language(RC)。
project(EvernightCapture
        VERSION ${ECAPTURE_MAJOR_VERSION}.${ECAPTURE_MINOR_VERSION}.${ECAPTURE_PATCH_VERSION}
        DESCRIPTION "CLI screenshot tool built on Windows.Graphics.Capture"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 平台判据（2026-10：建立跨平台构建与 Linux 开发验证入口）
#
# Windows 这一支的限制一条都没放松：还是要 MSVC，还是要 rc.exe，还是链那一套系统库。
# Linux 这一支本步**只**开放已实测可移植的目标（见文件末尾《Linux 与共用目标》），
# 完整 CLI（ecapture）与那十几份要 Lang/.rc/取帧通道的测试程序仍然只在 Windows 分支里定义。
# 这里不接受"为了让 Linux 配置通过而删功能"：Linux 上没定义的目标就是没定义，
# 配置日志会明确列出缺哪一份、为什么，而不是假装全都编得过。
# ---------------------------------------------------------------------------
if(WIN32)
  if(NOT MSVC)
    message(FATAL_ERROR "Windows 构建需要 MSVC（Windows.Graphics.Capture / C++/WinRT 依赖 Windows SDK）。"
                        "在 Windows 上请用 .\\build.ps1；要在 Linux 上开发请用 ./build.sh。")
  endif()
  enable_language(RC)
elseif(UNIX AND NOT APPLE)
  if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    message(FATAL_ERROR "Linux 构建需要 GCC 或 Clang（本机实测 g++ 14.2.0 与 clang++ 19.1.7），"
                        "当前编译器是 ${CMAKE_CXX_COMPILER_ID}。请用 ./build.sh --compiler gcc|clang 指定。")
  endif()
else()
  message(FATAL_ERROR "EvernightCapture 只在 Windows 与 Linux 上做过构建验证；"
                      "macOS 与其它平台尚未列入计划，不做未验证的适配。")
endif()

# C++ 标准全仓一处：Windows 原来逐目标写 cxx_std_20，Linux 复用同一份，不再各抄一遍。
set(ECAPTURE_CXX_STANDARD 20)

# ---------------------------------------------------------------------------
# ecapture_apply_common(<target>)
#
# 每个目标原来都要写四行：include 路径、cxx 标准、编译器开关、CRT 运行时。
# 这里收成一处，**按编译器分派而不是按平台猜**：MSVC 那一套开关（/utf-8 /W4
# /permissive- /Zc:__cplusplus /EHsc 与 UNICODE/_UNICODE/NOMINMAX、静态 CRT）逐字保留，
# GCC/Clang 用等价的 -Wall -Wextra。共用目标（核心边界判据、WGC 几何判据）因此可以
# 在两个平台用同一份定义 —— 这正是 src/core/ 那道边界要的机器证明。
#
# 只给 `src` 加包含路径，不给 `resources`：共用目标一旦链上 .rc 与文案资源，就等于把
# 刚划开的边界又糊回去了。要读文案资源的那些目标仍然留在 Windows 分支里自己写自己的一份。
# ---------------------------------------------------------------------------
function(ecapture_apply_common target)
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

  target_compile_features(${target} PRIVATE cxx_std_${ECAPTURE_CXX_STANDARD})
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD ${ECAPTURE_CXX_STANDARD} CXX_STANDARD_REQUIRED ON)

  if(MSVC)
    target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE NOMINMAX)
    # CRT 静态链接：单文件交付，目标机器不必装 VC++ 运行时
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
                   "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
endfunction()

# Linux 上没显式给配置时按 Debug 走：带 -g 才有符号，debug.sh 直接能下断点。
# 入口脚本（build.sh / test-all.sh）总是显式传配置，这条只是给手敲 cmake 的人兜底。
# 只在 Windows 分支之外生效，不改动 Windows 的既有配置约定。
if(NOT WIN32 AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建配置（Debug/Release/RelWithDebInfo）" FORCE)
  message(STATUS "未指定 CMAKE_BUILD_TYPE，按 Debug 配置（带调试符号）")
endif()

# ---------------------------------------------------------------------------
# 多语言文案的跨平台嵌入（2026-10：迁移命令行文本处理与多语言资源）
#
# 四份 resources/strings-*.txt 过去只由 rc.exe 按 RCDATA 嵌进 exe，共用语言层读它们就得
# #include <windows.h>，Linux 上根本没有这份载荷。现在改由构建期生成一份 C++ 源文件，
# 两个平台编译同一份嵌入物；自定义命令带 DEPENDS，**改文案必然触发重生成与重编译**
# （当年 .rc 那条要靠 OBJECT_DEPENDS 才勉强办到，实测 RC_DEPENDS 不生效）。
# ---------------------------------------------------------------------------
set(ECAPTURE_LANG_DATA "${CMAKE_BINARY_DIR}/generated/ec_lang_data.cpp")
add_custom_command(
  OUTPUT "${ECAPTURE_LANG_DATA}"
  # VERBATIM 负责 quoting，这里不再自己加引号（加了会把引号当路径的一部分传下去）。
  COMMAND ${CMAKE_COMMAND}
          -DECP_RESOURCES_DIR=${CMAKE_CURRENT_SOURCE_DIR}/resources
          -DECP_OUTPUT=${ECAPTURE_LANG_DATA}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-lang-data.cmake
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/gen-lang-data.cmake"
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-zh-CN.txt"
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-zh-TW.txt"
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-en.txt"
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-ja.txt"
  COMMENT "从 resources/strings-*.txt 生成嵌入文案源文件"
  VERBATIM)

# ---------------------------------------------------------------------------
# 平台服务：一处登记，各目标不要再各抄一份文件名
#
# 共用层（解析、配置、文案、JSON、文本出口）声明判据，动作在各平台的源文件里。两套实现
# 提供的是同一组函数名，所以**换平台就是换这几行文件名**，共用源文件一条都不改。
#   * 语言探测（DetectSystemLanguage）跟着文案走：要读文案的目标必须同时链上它。
#   * 路径 / 文件 / 目录 / 原子保存 / 标准流这几个服务跟着 Config.cpp 与交付那一路走：
#     读配置、建历史目录与写输出都靠它们。平台换一份实现，共用调用点一行都不改。
# ---------------------------------------------------------------------------
if(WIN32)
  set(ECAPTURE_LOCALE_SOURCE src/platform/windows/WinLocale.cpp)
  set(ECAPTURE_HOST_SOURCES
    src/Config.cpp
    src/platform/windows/WinHostPath.cpp
    src/platform/windows/WinHostFile.cpp
    src/platform/windows/WinHostDirectory.cpp
    src/platform/windows/WinOutputStream.cpp
    src/platform/windows/WinFileSave.cpp)
else()
  set(ECAPTURE_LOCALE_SOURCE src/platform/linux/PosixLocale.cpp)
  set(ECAPTURE_HOST_SOURCES
    src/Config.cpp
    src/platform/linux/PosixHostPath.cpp
    src/platform/linux/PosixHostFile.cpp
    src/platform/linux/PosixHostDirectory.cpp
    src/platform/linux/PosixOutputStream.cpp
    src/platform/linux/PosixFileSave.cpp)
endif()

set(ECAPTURE_LANG_SOURCES
  src/Lang.cpp
  "${ECAPTURE_LANG_DATA}"
  ${ECAPTURE_LOCALE_SOURCE})

# ===========================================================================
# 【平台层 · Windows】以下整段只在 Windows 定义
#
# 这一段里的东西全都依赖 MSVC + Windows SDK + WinRT + rc.exe：主程序 ecapture 与
# 那十几份要 Lang.cpp/.rc 文案资源或取帧通道的测试程序。Linux 分支在文件末尾，
# 两边共用同一份 C++ 标准与同一套开关分派。
# 这一段正文刻意**没有**因为包进 if(WIN32) 而重新缩进 —— 重新缩进会让这次改动无法逐行审查。
# ===========================================================================
if(WIN32)

add_executable(ecapture
  src/platform/windows/EntryWin.cpp
  src/platform/windows/WinCliArgs.cpp
  src/Main.cpp
  src/CliText.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/Json.cpp
  src/Report.cpp
  src/WindowMatch.cpp
  src/WindowIdentity.cpp
  src/WindowQuery.cpp
  src/WindowQueryRun.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/ScreenQuery.cpp
  src/SystemCompat.cpp
  src/ProcessIntegrity.cpp
  src/EnvReport.cpp
  src/Capture.cpp
  src/CaptureScope.cpp
  src/CropGeometry.cpp
  src/CursorControl.cpp
  src/HdrColor.cpp
  src/HdrDetect.cpp
  src/Deadline.cpp
  src/Worker.cpp
  src/WorkerIo.cpp
  src/WorkerProtocol.cpp
  src/WorkerTiming.cpp
  src/WinrtApartment.cpp
  src/OutputPlan.cpp
  src/Delivery.cpp
  src/HistoryArchive.cpp
  # 截图后的桌面通知。两个源文件分开是责任边界：Notification.cpp 只管"这条通知写什么"（纯函数，
  # 离线判据链它一份就够），NotificationToast.cpp 才碰 WinRT 与系统的通知平台（链 windowsapp.lib）。
  src/Notification.cpp
  src/NotificationToast.cpp
  src/Consent.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/D3dDevice.cpp
  src/WgcGeometry.cpp
  src/DupGeometry.cpp
  src/CaptureWgc.cpp
  src/CaptureDwm.cpp
  src/CapturePrintWindow.cpp
  src/CaptureBitBlt.cpp
  src/CaptureDuplication.cpp
  src/Encoder.cpp
  resources/ecapture.rc
)

# rc.exe 按当前目录解析它要读的文件名，而 Ninja 的工作目录是 build 目录：
# 把 resources 目录加进包含路径，图标按裸文件名即可命中。
# src 也在里面：ecapture.rc 用 #include "Version.h" 取版本号（那是个 C++ 头，不放 resources）。
target_include_directories(ecapture PRIVATE resources src)

# RC_DEPENDS 实测不会触发重编（ninja 说没活干），用 OBJECT_DEPENDS 才可靠：
# .rc 嵌进去的数据文件与 #include 的头都必须列成 .rc 的依赖，否则改完要 clean 重编才生效。
# 四份 strings-*.txt 已经从 .rc 里搬走（见上面 ECAPTURE_LANG_DATA 那条自定义命令），
# 这里只剩 .rc 真正还要读的两样：图标与 Version.h 那批宏。
set(ECAPTURE_RC_INPUTS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.ico"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/Version.h")
set_source_files_properties(resources/ecapture.rc PROPERTIES OBJECT_DEPENDS "${ECAPTURE_RC_INPUTS}")

target_compile_features(ecapture PRIVATE cxx_std_20)
set_target_properties(ecapture PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# 源码与执行字符集都是 UTF-8；/W4 便于早期发现隐式转换问题
target_compile_options(ecapture PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus)
target_compile_definitions(ecapture PRIVATE UNICODE _UNICODE NOMINMAX WINRT_LEAN_AND_MEAN)

# 交付名固定为 ECAPTURE.EXE（Windows 文件系统不区分大小写）
set_target_properties(ecapture PROPERTIES OUTPUT_NAME "ecapture" SUFFIX ".exe")

# 配置模板跟着程序一起落到它旁边：默认位置读的就是这一目录里的 ECAPTURE.ini，
# 所以"程序旁边要有一份可对照的模板"是同一件事的另一半（安装版靠安装器交付同一份）。
#   * copy_if_different —— 模板本身是受管产物，内容变了就跟上。
#   * 只复制这一份 example，**永不碰 ECAPTURE.ini**：那是用户配置，构建与清理都不许覆盖它，
#     也不做"首次构建顺手生成一份配置"这种事（判据见 src/Config.h 与 README《配置文件》）。
#   * 这里用一条**增量**规则而不是 ecapture 的 POST_BUILD：POST_BUILD 只在**重新链接**那一次跑，
#     而模板是可以单独被改的仓库文件——只改模板时那一版复制根本不会执行，build 旁边就留下一份
#     陈旧副本（实测踩过：源码 9580 字节、build 里还是旧的 9137 字节）。
#   * 也不能用"每次都算过期"的裸 add_custom_target：tests\installer-package.ps1 有一条前提判的是
#     "没有东西变了就得是干净的空操作"，一条恒跑的步骤会让那次构建图不干净（实测报
#     "[1/1] Refreshing ECAPTURE.example.ini ..."）。所以这里给出 OUTPUT + DEPENDS 做时间戳判定，
#     并在复制之后 touch 那一份产物：copy_if_different 遇到同内容不写文件、时间戳不动，
#     不 touch 就会被判成"还没生成"而每次重跑。touch 的对象是本规则自己生成的那一份副本，
#     不碰仓库里那份模板，也不碰 ECAPTURE.ini。
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/ECAPTURE.example.ini"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/resources/ECAPTURE.example.ini"
          "${CMAKE_BINARY_DIR}/ECAPTURE.example.ini"
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/ECAPTURE.example.ini"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/ECAPTURE.example.ini"
  COMMENT "Refreshing ECAPTURE.example.ini next to the built executable (never touches ECAPTURE.ini)")
add_custom_target(ecapture-example-ini ALL
  DEPENDS "${CMAKE_BINARY_DIR}/ECAPTURE.example.ini")
add_dependencies(ecapture ecapture-example-ini)

# ---------------------------------------------------------------------------
# Windows.Graphics.Capture 阶段（C++/WinRT）所需组件。已实测：
#   * winrt/Windows.Graphics.Capture.h 等头文件由 Windows SDK 自动进入 MSVC 包含路径
#   * 所需符号全部由 windowsapp.lib 提供
#   * 新版 C++/WinRT 的 winrt::init_apartment() 返回 void，不要再套 check_hresult()
# ---------------------------------------------------------------------------
target_link_libraries(ecapture PRIVATE
  windowsapp.lib         # WinRT：Graphics.Capture / Graphics.Imaging / Storage.Streams
  d3d11.lib              # D3D11 设备与纹理拷贝
  dxgi.lib               # IDXGIDevice / IDXGISurface 互操作
  ole32.lib              # COM 套间与流
  dwmapi.lib             # DwmRegisterThumbnail 通道 + DWMWA_EXTENDED_FRAME_BOUNDS
  user32.lib kernel32.lib advapi32.lib gdi32.lib)

# WinRT 异常需要 /EHsc；CRT 静态链接，避免目标机器缺少 VC++ 运行时（单文件交付）
target_compile_options(ecapture PRIVATE /EHsc)
set_property(TARGET ecapture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
  set_property(TARGET ecapture PROPERTY LINK_FLAGS "/OPT:REF /OPT:ICF")
endif()

# ---------------------------------------------------------------------------
# 截图授权状态机的离线判据（由 tests\consent.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：授权这件事必须用"注入假的应答器 + 假的屏幕布局"来跑完整状态机
# （升级要重新确认、拒绝之后不再换后端、目标挪位置作废凭证……），而这些判据一条都不能靠
# 发布版里的测试开关实现 —— ECAPTURE.EXE 里不许有能跳过确认的后门。所以这里把纯逻辑那几份
# 源文件单独编一个测试程序，假应答器与假拓扑从测试代码里注入。
# 不碰 WinRT / D3D / 编码，所以链接项里一个图形相关的库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-consent-tests
  tests/consent_state.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/WindowMatch.cpp
  resources/ecapture.rc)

target_include_directories(ecapture-consent-tests PRIVATE resources src)
target_compile_features(ecapture-consent-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-consent-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-consent-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-consent-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-consent-tests PRIVATE
  user32.lib kernel32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-consent-tests PROPERTIES OUTPUT_NAME "ecapture-consent-tests")
set_property(TARGET ecapture-consent-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 隔离执行那套内部消息格式与期限换算的离线判据（由 tests\timeout.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：格式层最要紧的判据是"畸形消息整条作废"——截断、超长、
# 保留段非 0、状态码没登记、像素字节数与行距不自洽。这些字节组合用真父子进程去造
# 既慢又造不全，而发布版 ECAPTURE.EXE 里绝不该有能喂自定义消息的入口。
# 期限那条判据要读 --lang 那套资源，所以把 Lang/CliOptions 与 .rc 一起编进来。
# 这里不链 Worker.cpp：它要起辅助进程，也要用到几条取图通道，而格式与期限这两层判据
# 都只需要纯数据的那几份源文件 —— 隔离调用领多少、连管肯等多久、随任务交下去的那一笔
# 预算怎么判界、辅助进程自己的三段期限什么时候该退出，全在 WorkerTiming.cpp，
# 链它本体来判（判的不是测试里抄的一份算式）。真父子进程那一段在 tests\timeout.ps1。
# 异步 I/O 生命周期那层判据（tests\worker_io_state.cpp）链的是 WorkerIo.cpp 本体与注入的
# 假后端 —— 判的就是生产状态机，不是它的抄本；连接时序那层（tests\worker_connect_state.cpp，
# F03）同样链 WorkerIo.cpp 本体，只是后端换成真实的 Win32Backend + 真命名管道 ——
# "客户端抢先连上"这类顺序判不了假数据，而发布版 ECAPTURE.EXE 里不许有控制连接顺序的开关，
# 所以傀儡进程是本测试 exe 自己（--f03-peer 模式），不碰任何用户应用与桌面内容。
# ---------------------------------------------------------------------------
add_executable(ecapture-worker-tests
  tests/isolation_state.cpp
  tests/worker_io_state.cpp
  tests/worker_connect_state.cpp
  src/HdrColor.cpp
  src/WorkerIo.cpp
  src/WorkerProtocol.cpp
  src/WorkerTiming.cpp
  src/Deadline.cpp
  src/WindowMatch.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-worker-tests PRIVATE resources src)
target_compile_features(ecapture-worker-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-worker-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-worker-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-worker-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-worker-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_source_files_properties(resources/ecapture.rc PROPERTIES OBJECT_DEPENDS "${ECAPTURE_RC_INPUTS}")
set_target_properties(ecapture-worker-tests PROPERTIES OUTPUT_NAME "ecapture-worker-tests")
set_property(TARGET ecapture-worker-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 帧内存形状与单色判定的离线判据（由 tests\image.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这两件事的判据都要**手工摆出特定的像素排布**（竖条纹、棋盘、
# 只在某个采样点之外变化、行末带填充、缓冲区短一个字节……），真机截图造不出这些形状，
# 而"另在测试里抄一份算法当判据"正是这次要修的错 —— 所以直接测生产函数本体（ImageOps.cpp，
# 连同它调用的 CaptureCommon.cpp 那套诊断）。这里不碰 D3D 取帧，链接项里一个图形库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-image-tests
  tests/image_state.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/ImageOps.cpp
  src/CaptureCommon.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/WindowMatch.cpp
  resources/ecapture.rc)

target_include_directories(ecapture-image-tests PRIVATE resources src)
target_compile_features(ecapture-image-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-image-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-image-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-image-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-image-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-image-tests PROPERTIES OUTPUT_NAME "ecapture-image-tests")
set_property(TARGET ecapture-image-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# WGC 动态尺寸与帧池生命周期的离线判据（由 tests\wgc.ps1 编译并运行）。
#
# 这一份的定义已经搬到文件末尾《Linux 与共用目标》那一段：它只需要纯算术的
# tests/wgc_state.cpp + src/WgcGeometry.cpp，实测在 g++ 14.2 上编译并跑通全部判据，
# 所以 Windows 与 Linux 共用同一条定义（本段不再重复一份，避免两边各改一处）。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 目标身份复核的离线判据（由 tests\identity.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的是"同一个 HWND 值现在属于另一个对象"这种现场 ——
# 句柄与 PID 何时被系统复用完全不由测试决定，在同一次运行里既造不出来也没法安排阴性对照
#（要造就得杀掉别人的进程，而仓库规矩是不碰用户真实应用）。所以把判据本体
# (src/WindowIdentity.cpp) 单独编一个测试程序，注入**假查询层**逐条判：
# IsWindow 为假、PID 变了、PID 没变但进程创建时间变了（PID 复用）、类名变了、
# 标题变了而条件仍成立 / 不再成立、以及每一道判据"问不出来"。
# 这里不碰取帧、不碰窗口，链接项一个图形库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-identity-tests
  tests/identity_state.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/WindowIdentity.cpp
  src/WindowMatch.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-identity-tests PRIVATE resources src)
target_compile_features(ecapture-identity-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-identity-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-identity-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-identity-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-identity-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-identity-tests PROPERTIES OUTPUT_NAME "ecapture-identity-tests")
set_property(TARGET ecapture-identity-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

#
# 为什么单独一个可执行文件：这三件事在真机上要么造不出稳定现场（把用户的显示方向改掉、
# 拔显示器、必须两块显卡），要么根本没有阴性对照（旋转过的屏幕上"尺寸对但画面横躺"的图
# 看起来就是一次成功）。判据本体是纯算术：
#   * src/DupGeometry.cpp —— 桌面坐标 -> 纹理坐标 -> 交付图像坐标的换算、旋转与尺寸的对照、
#     adapter/output 的定位（假枚举器逐条注入）。
#   * src/ImageOps.cpp 的 RotateCropFrame —— 四角不同颜色的合成帧逐点核对。
#   * src/ScreenMatch.cpp 的 CompareScreen —— 负坐标、跨屏、部分离屏、编号变化、尺寸变化、热拔出。
# 这里不碰 DXGI / D3D / 窗口，链接项一个图形库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-dup-tests
  tests/dup_state.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/DupGeometry.cpp
  src/ImageOps.cpp
  src/CaptureCommon.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/WindowMatch.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-dup-tests PRIVATE resources src)
target_compile_features(ecapture-dup-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-dup-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-dup-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-dup-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-dup-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-dup-tests PROPERTIES OUTPUT_NAME "ecapture-dup-tests")
set_property(TARGET ecapture-dup-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 运行环境能力判据的离线判据（由 tests\compat.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的是"这台机器的 Windows 内部版本是 X"那个现场，
# 而降级一台机器既做不到也不该做。判据本体（src/SystemCompat.cpp 那三个纯函数）不碰窗口、
# 不碰取帧、不弹框，注入假版本就能逐条判边界；只有文案那一段要读 .rc 里编进的四份资源。
# ---------------------------------------------------------------------------
add_executable(ecapture-compat-tests
  tests/compat_state.cpp
  src/SystemCompat.cpp
  src/ProcessIntegrity.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-compat-tests PRIVATE resources src)
target_compile_features(ecapture-compat-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-compat-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-compat-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-compat-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-compat-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-compat-tests PROPERTIES OUTPUT_NAME "ecapture-compat-tests")
set_property(TARGET ecapture-compat-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 能力与诊断查询的离线判据（由 tests\capabilities.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的是"这台机器不是那一台开发机"的现场 ——
# 无图形会话（一块屏都没有）、版本低于某条通道的下限、版本根本问不出来、某个编码器没登记。
# 本机既降不了级也没有第二块显卡，也绝不能为了看"没有屏幕拓扑时会怎么判"而把显示器拔掉。
# 判据本体（src/EnvReport.cpp 的 BuildEnvReport）是纯函数，注入一份假的 EnvProbe 就能
# 逐条判；只有格式名与通道名那两套机器名要读 CliOptions，而它连带文案资源（.rc）一起编进来。
# 这里不碰 WinRT / D3D / 窗口 / 弹框，链接项一个图形库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-capabilities-tests
  tests/capabilities_state.cpp
  src/EnvReport.cpp
  src/SystemCompat.cpp
  src/ProcessIntegrity.cpp
  src/CaptureScope.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/Json.cpp
  resources/ecapture.rc)

target_include_directories(ecapture-capabilities-tests PRIVATE resources src)
target_compile_features(ecapture-capabilities-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-capabilities-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-capabilities-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-capabilities-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-capabilities-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib)
set_target_properties(ecapture-capabilities-tests PROPERTIES OUTPUT_NAME "ecapture-capabilities-tests")
set_property(TARGET ecapture-capabilities-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 结构化窗口发现与检查（--list / --inspect）的离线判据（由 tests\windows.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的是"某一问读不到""命中 100 扇而只交回 3 扇"
# "同一个条件命中两扇而没人消歧""归属进程是受限进程"这些现场 —— 真机上要么安排不出来
# （不能为了造"读不到"去杀别人的进程，也不能把窗口尺寸改成 0 还指望它可见），
# 要么没有阴性对照。判据本体（src/WindowQuery.cpp 的 BuildWindowQueryResult /
# MakeWindowRecord / RenderWindowQuery）全是纯函数，注入一条假候选就能逐条判，
# 连渲染都判得到"字段齐不齐、路径有没有漏出去"。
# 这里不链 Worker / 取帧通道：条件求值那条辅助进程路线只在真机入口（WindowQueryRun.cpp）用，
# 而离线判据判的是"拿到快照之后怎么算、怎么写"。链接项一个图形库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-windows-tests
  tests/windows_state.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/WindowQuery.cpp
  src/WindowIdentity.cpp
  src/WindowMatch.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/Json.cpp
  resources/ecapture.rc)

target_include_directories(ecapture-windows-tests PRIVATE resources src)
target_compile_features(ecapture-windows-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-windows-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-windows-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-windows-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-windows-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-windows-tests PROPERTIES OUTPUT_NAME "ecapture-windows-tests")
set_property(TARGET ecapture-windows-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 只读屏幕枚举与按标识选屏（--screens / --monitor=device: / --monitor=id:）的离线判据
#（由 tests\screens.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的现场是"这台机器只接了一块屏"永远造不出来的 ——
# 副屏在负坐标、竖屏旋转、同一个设备名被系统重新发给另一块面板、同一个标识命中两块屏、
# 显示配置整路问不出来、复制模式下两条路径共享一个桌面。真机上要判这些要么得插第二台显示器、
# 要么得改用户的显示设置，两条都不允许（见 ScreenIdentity.h 顶部第 1 条）。
# 判据本体（src/ScreenIdentity.cpp 的 SelectScreenCandidates / CompareScreenIdentity 与
# src/ScreenQuery.cpp 的 BuildScreenQuery / RenderScreenQuery）全是纯函数：候选表注进来就能逐条判，
# 连渲染都判得到"读不到的那一项有没有被写成 0"与"选择器有没有凭空多出一条"。
# 这里不链取帧通道也不链 Worker：一次像素都不取的那一路用不着它们。
# ---------------------------------------------------------------------------
add_executable(ecapture-screens-tests
  tests/screens_state.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/ScreenIdentity.cpp
  src/ScreenQuery.cpp
  src/WindowQuery.cpp
  src/WindowMatch.cpp
  src/WindowIdentity.cpp
  src/ScreenMatch.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/Json.cpp
  resources/ecapture.rc)

target_include_directories(ecapture-screens-tests PRIVATE resources src)
target_compile_features(ecapture-screens-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-screens-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-screens-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-screens-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-screens-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-screens-tests PROPERTIES OUTPUT_NAME "ecapture-screens-tests")
set_property(TARGET ecapture-screens-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 窗口内部裁剪（--roi / --client-area）的离线判据（由 tests\crop.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的现场是"一张比请求小的交付图像"与"客户区落在图像之外"
# —— 真机上要么得把窗口正好改到某个尺寸、要么得把窗口挂出屏幕边缘某条线上，而且都要在
# "选定之后、取帧之前"那几毫秒里发生（identity 复核与授权复核都拦在前面）。所以把判据本体
#（src/CropGeometry.cpp 的 RoiFitsTarget / DescribeDeliveredImage / ResolveWindowCrop）单独编一个
# 测试程序，交付图像的尺寸、它的屏幕矩形问没问到、客户区那块矩形问没问到，全部注入进来逐条判：
# 负值、相加绕回、越界一条边、零尺寸、图像原点核实不出来、复制模式下客户区整块在外面。
# 纯算术，不链任何图形库，也不链 Lang（这一层不产生文案）。
# ---------------------------------------------------------------------------
add_executable(ecapture-crop-tests
  tests/crop_state.cpp
  src/CropGeometry.cpp)

target_include_directories(ecapture-crop-tests PRIVATE src)
target_compile_features(ecapture-crop-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-crop-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-crop-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-crop-tests PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(ecapture-crop-tests PROPERTIES OUTPUT_NAME "ecapture-crop-tests")
set_property(TARGET ecapture-crop-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 光标包含与排除（--cursor）的离线判据（由 tests\cursor.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的现场是"这台机器的 Windows 版本正好卡在那道门槛两侧"
# "某条路径没登记进光标表""设完读回来是相反的那一件" —— 本机只有一台 19045 的开发机，降不了级，
# 也不许为了看"没有开关时会怎样"去动真机的 WGC。判据本体（src/CursorControl.cpp 的
# FilterChainForCursor / GateCaptureChain 与 src/CursorControl.h 那张按路径登记的表）是纯函数：
# 假版本与假通道链注进来就能逐条判。文案那一段要读 .rc 里编进的四份资源，所以连着
# CliOptions/Lang 一起编进来；另外链上 CaptureScope.cpp，用来现场核对"两条表说的是同一批路径"。
# GateCaptureChain 现在串的是**三道**闸门（版本 / 光标 / HDR），所以那一道要的 HdrColor.cpp 与
# 它自己的依赖（CaptureCommon / ImageOps / Deadline / Consent / 那两条枚举件）一并链进来 ——
# 这一份判据仍然不碰 WinRT / D3D / 窗口 / 弹框，链接项里没有图形库。
# ---------------------------------------------------------------------------
add_executable(ecapture-cursor-tests
  tests/cursor_state.cpp
  src/CursorControl.cpp
  src/HdrColor.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/Deadline.cpp
  src/Consent.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/WindowMatch.cpp
  src/CaptureScope.cpp
  src/SystemCompat.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-cursor-tests PRIVATE resources src)
target_compile_features(ecapture-cursor-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-cursor-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-cursor-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-cursor-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-cursor-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-cursor-tests PROPERTIES OUTPUT_NAME "ecapture-cursor-tests")
set_property(TARGET ecapture-cursor-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# HDR 色彩处理（--hdr）的离线判据（由 tests\hdr.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：本机没有能开 HDR 的显示器，"真带回一幅 FP16 / 10 位帧"这件事在这里
# 造不出来 —— 而 tone mapping 的数学、来源格式与色彩空间的分类、登记表一致性、以及那组结果键的
# 合成本身全是纯算术，用"已知色块 + 亮度梯度"注入就能逐条判（真机那条在 tests\hdr.ps1 记未验证）。
# 判据本体是 src/HdrColor.cpp（映射函数、分类、half 解码）与 src/HdrColor.h 那张按路径登记的表；
# 连着 CaptureCommon / ImageOps / CaptureScope 编进来，用来现场核对两张表说的是同一批路径。
# 这里链的是 HdrColor.cpp（纯算术那半），不是 HdrDetect.cpp（问显示 HDR 状态那半要 dxgi，只有
# 主目标里的 CaptureWgc 用），所以链接项一个图形库都不需要。文案要读 .rc 里编进的四份资源。
#
# 2026-10 加进来的一份：策略筛选与回退控制（F07）。同一份判据还要逐条判"显式 tonemap/refuse 时
# 链里只剩兑现得了的那几条"（src/HdrColor.cpp 的 FilterChainForHdr 与串三道闸门的
# GateCaptureChain，所以连着 CursorControl.cpp / SystemCompat.cpp），以及"后端交回
# capture.hdr_refused 之后其余后端一次都不调用、原码原样交出"（src/FallbackChain.h 那个模板，
# 假后端在这里注入）。这两件都造不出真实现场：本机没有 HDR 显示器，也没有一条真通道会被测试
# 允许去试 —— 所以判的是生产函数本体，不是测试里另抄的一份规则。
# ---------------------------------------------------------------------------
add_executable(ecapture-hdr-tests
  tests/hdr_state.cpp
  src/HdrColor.cpp
  src/CursorControl.cpp
  src/SystemCompat.cpp
  src/Deadline.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/WindowMatch.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-hdr-tests PRIVATE resources src)
target_compile_features(ecapture-hdr-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-hdr-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-hdr-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-hdr-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-hdr-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-hdr-tests PROPERTIES OUTPUT_NAME "ecapture-hdr-tests")
set_property(TARGET ecapture-hdr-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 交付那一步与"期限合规"分开记账的离线判据（由 tests\delivery.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：这一批判据要的现场是"最后一张图的磁盘写跨过了时限""标准输出的
# 字节全部到达之后预算才用尽""半段流留在管道里""改名那一瞬间撞了 --no-overwrite"，还有
# "图已经收不回来了而记账要用的内存刚好不够"—— 时间点与故障都排在一次真实写入的两端，真机上
# 要么安排不出来（本机没有一块能被测试变慢的盘，也不许为了造慢盘去改用户的存储设置；真把内存
# 抽干更是整机级故障），要么根本要拿别人的应用当管道对面。所以把交付那一步的编排本体
#（src/Delivery.cpp 的 DeliverImage / OutcomeExitCode）连同**生产的原子写**
#（src/platform/<平台>/*FileSave.cpp，经 src/FileSave.h 的同一份声明）
# 一起链进来，出口、时钟与一道分配闸门（测试里替换全局 operator new）从测试注入：文件走真磁盘
#（于是"磁盘上到底有没有这张图、多少字节、旧文件有没有被破坏"是现场量出来的，不是判据里抄的），
# 标准输出走内存管道（半段流就是半段）。
# 这里不链取帧通道、不链编码器、不弹框：进来的已经是编好的字节。链接项一个图形库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-delivery-tests
  tests/delivery_state.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/Delivery.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/WindowMatch.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-delivery-tests PRIVATE resources src)
target_compile_features(ecapture-delivery-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-delivery-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-delivery-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-delivery-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-delivery-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-delivery-tests PROPERTIES OUTPUT_NAME "ecapture-delivery-tests")
set_property(TARGET ecapture-delivery-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 截图历史归档（主交付之外那一份独立副本）的离线判据：tests/history_state.cpp。
# 判的是生产那三份本体：src/HistoryArchive.cpp（归档根解析、本地日期目录、命名、独占提交与失败
# 归类）+ src/Delivery.cpp（交付那一步把它排在主交付之后、期限核对之前，以及三种下场怎么入账）
# + src/platform/<平台>/*FileSave.cpp（与主输出共用的那一次"不许替换"的原子提交）。落点、本地时钟与抗冲突标识从
# 测试注入（生产的 HistoryArchive 也照样真用，那些"副本字节与主交付相同""同名不覆盖"判的都是真
# 磁盘上的文件），于是"同刻两张""时钟回拨""跨午夜""那个位置是个文件""名字已被占满"这些现场
# 排得出来 —— 真机上它们要么要改用户的存储设置/ACL/完整性标签，要么根本等不到那一刻。
# 这里不链取帧通道、不链编码器、不弹框：进来的已经是编好的字节。
# ---------------------------------------------------------------------------
add_executable(ecapture-history-tests
  tests/history_state.cpp
  src/HistoryArchive.cpp
  src/Delivery.cpp
  src/OutputPlan.cpp
  src/Deadline.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-history-tests PRIVATE resources src)
target_compile_features(ecapture-history-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-history-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-history-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-history-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-history-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-history-tests PROPERTIES OUTPUT_NAME "ecapture-history-tests")
set_property(TARGET ecapture-history-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 截图后桌面通知（src/Notification.cpp）的离线判据：tests/notification_state.cpp。
#
# 判的是生产本体：三档 detail 到底把哪些字段交出去、落点与附图是不是只认"真已提交的那一份副本"、
# --purpose 占哪一格、合并模式列不下时数量怎么说、RenderToastXml 的 XML 转义（窗口标题是外部数据，
# 那是注入面），以及 SendCaptureNotifications 对"全成功 / 部分失败 / 全失败有码 / 全失败没码 /
# 零交付"四种下场的归类，并核对通知失败**不回头改交付事实**。
#
# 发送那一次 WinRT 调用不在这里链、也不在这里判（src/NotificationToast.cpp 要真问系统的通知平台，
# 判到的是"这台机器让不让"而不是"我们写对了没有"）：测试注入 ToastSender，被 judged 的仍是生产
# 那份编排函数。所以这里一个 windowsapp.lib 都不链。
# ---------------------------------------------------------------------------
add_executable(ecapture-notification-tests
  tests/notification_state.cpp
  src/Notification.cpp
  src/HdrColor.cpp
  src/Deadline.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  resources/ecapture.rc)

target_include_directories(ecapture-notification-tests PRIVATE resources src)
target_compile_features(ecapture-notification-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-notification-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-notification-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-notification-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-notification-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-notification-tests PROPERTIES OUTPUT_NAME "ecapture-notification-tests")
set_property(TARGET ecapture-notification-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 用户偏好配置（ECAPTURE.ini）的离线判据：tests/config_state.cpp。
#
# 判的是生产本体，不是测试里另抄的一份规则：
#   * src/Config.cpp 的语法层（UTF-8/BOM/CRLF、整行注释、成对引号、重复节与键、未知节与键、
#     缺等号、不闭合引号、NUL、行与文件的上限）与逐键判定（版本/布尔/枚举/严格十进制/区间）；
#   * src/CliOptions.cpp 的合并那一步 —— 一律通过真实入口 ParseCommandLine 跑，因为它才拿得到
#     "本次命令行写过哪些项"，而那正是整条优先级（CLI > INI > 内置）与分组覆盖的判据所在；
#   * 输出格式那条链（显式 --format / 命令行输出名扩展名 / 配置的非 auto format / 配置路径
#     扩展名 / png）与 --show-config 那份来源表；
#   * 只读查询不被配置里截图那一级的默认污染、坏配置时 --help/--version/--no-config 仍可用。
# 链接项与 consent 那一份同构（CliOptions + Lang + .rc：解析期的报错文案要读资源），
# 不链取帧通道、不弹框、不建目录 —— 临时 INI 全部写在本次自建的 %TEMP% 目录里，结束整棵删掉。
# ---------------------------------------------------------------------------
add_executable(ecapture-config-tests
  tests/config_state.cpp
  ${ECAPTURE_HOST_SOURCES}
  src/HdrColor.cpp
  src/Deadline.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CliOptions.cpp
  ${ECAPTURE_LANG_SOURCES}
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/WindowMatch.cpp
  resources/ecapture.rc)

target_include_directories(ecapture-config-tests PRIVATE resources src)
target_compile_features(ecapture-config-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-config-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-config-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-config-tests PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(ecapture-config-tests PRIVATE
  kernel32.lib user32.lib advapi32.lib gdi32.lib dwmapi.lib)
set_target_properties(ecapture-config-tests PROPERTIES OUTPUT_NAME "ecapture-config-tests")
set_property(TARGET ecapture-config-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# 跨平台核心边界的离线判据（Windows 由 tests\core_boundary.ps1 编译并运行，
# Linux 由 ./test-all.sh 或 scripts\core-portability-check.sh 跑同一份判据）。
#
# 这一份的定义搬到下面的《Linux 与共用目标》段：它的源文件一条都不 include windows.h，
# 在两个平台用同一条定义才说明边界是真的（原来那些 MSVC 专属开关由 ecapture_apply_common
# 按编译器分派，Windows 侧取值一字未改）。
# ---------------------------------------------------------------------------

endif()   # 【平台层 · Windows】以上目标只在 Windows 定义

# ===========================================================================
# Linux 与共用目标（本步开放的范围，按实测可移植程度给）
#
# 实测记录（本机 Debian 13，g++ 14.2.0 / clang++ 19.1.7，cmake 3.31.6）：
#   * src/ 下能不带 Windows 头编过的生产源文件只有 7 份：CaptureScope.cpp、CropGeometry.cpp、
#     Delivery.cpp、DupGeometry.cpp、ImageOps.cpp、Json.cpp、WgcGeometry.cpp。
#   * tests/*_state.cpp 里能单独编过的有 7 份，但只有判据本体也在上面那 7 份里的才链得起来：
#       - core_boundary_state.cpp + CaptureScope.cpp      -> 已跑通（141 条判据）
#       - wgc_state.cpp         + WgcGeometry.cpp         -> 已跑通（21 条判据）
#     其余五份（cursor / identity / notification / screens / windows）判的生产源文件仍然要
#     windows.h（CursorControl.cpp、WindowIdentity.cpp、Notification.cpp、ScreenQuery.cpp、
#     WindowQuery.cpp），Linux 上链不起来 —— 那是后端步骤的活，本步不为了凑数把它们搬空。
#   * crop_state.cpp 单独编不过：它 include src/CaptureCommon.h（Dib/HDC 那一份），
#     而 CropGeometry.cpp 本体其实是中立的。要让这一套也能在 Linux 跑，得先把
#     CaptureCommon.h 的中立部分继续搬进 src/core/，属于独立步骤。
#   * Lang.cpp/.rc（四语文案）与 CliOptions/Config 仍然要 Win32 资源，所以凡是要读文案的
#     测试程序都留在 Windows 分支。
# ===========================================================================

# ---- 共用：两个平台同一条定义，同一份判据 ----
#
# ecapture-core-tests —— 为什么单独一个可执行文件：这一步交付的是"共用核心 / 平台实现"的分界本身，
# 而分界最容易在下一次改动里悄悄漏掉。这个程序的存在就是那道门 —— 它的源文件
# tests/core_boundary_state.cpp 一条都不 include windows.h，只用共用模型（src/core/ 与那几份共用
# 数据模型头）。所以 Windows 这边编得过 = 搬走原生类型之后模型与判据仍然自洽；
# 同一份文件在 Linux 上编得过 = 那一层真的平台无关。
# 这里**不**链任何 Windows 库，也不链 .rc 与资源：一链就把边界又糊回去了。
# 链进来的生产源文件只有 CaptureScope.cpp（路径登记表与 --yes 那条例外的本体）。
add_executable(ecapture-core-tests
  tests/core_boundary_state.cpp
  src/CaptureScope.cpp)
ecapture_apply_common(ecapture-core-tests)
set_target_properties(ecapture-core-tests PROPERTIES OUTPUT_NAME "ecapture-core-tests")

# ecapture-wgc-tests —— 为什么单独一个可执行文件：这一批判据是"这一帧交回来的实际内容尺寸与
# 纹理尺寸对不对得上、该复制哪块矩形、要不要重建帧池、还来不来得及重建" —— 全是纯算术，而真机上
# 要凑出 ContentSize 大于 / 等于 / 小于纹理，以及"窗口恰好一直在变大把重建预算耗光"这几种形状，
# 既不稳定也做不到阴性对照。所以把判据本体（src/WgcGeometry.cpp）单独编一个测试程序逐条注入。
# 这里不碰 WinRT / D3D / 窗口，链接项一个图形库都不需要 —— 也正因为它只需要纯算术，
# 这一份判据在 Linux 上同样跑得起来（本机 21 条全过）。
add_executable(ecapture-wgc-tests
  tests/wgc_state.cpp
  src/WgcGeometry.cpp)
ecapture_apply_common(ecapture-wgc-tests)
set_target_properties(ecapture-wgc-tests PROPERTIES OUTPUT_NAME "ecapture-wgc-tests")

# ecapture-cli-text-tests —— 命令行文本处理与多语言资源的离线判据（两个平台同一条定义）。
#
# 为什么这一批可以在 Linux 上编、也能链：这一步把"读文案 / 定路径 / 读文件 / 写标准流"这四件
# 平台动作收进了平台服务（src/HostPath.h、src/HostFile.h、src/OutputStream.h、语言探测），
# 共用那几份源文件（CliOptions / Config / Lang / Json / CliText / core）因此一条都不再
# include 平台头。链的是生产本体，不是测试里另抄的一份规则。
#
# 不链进来的：ecapture 主程序那一份响应组装（src/Report.cpp）与取帧、窗口/屏幕枚举、
# 辅助进程管道 —— 那些仍然只在 Windows 分支，所以这个程序判的是"文本与参数这一层"，
# 不假装整条 CLI 已经在 Linux 上跑通。
if(WIN32)
  # 判的那一条就是生产入口本体（DecodeWindowsArgv），所以它自己也要一起链进来。
  set(ECAPTURE_CLI_ENTRY_CHECKS
    tests/platform/cli_text_win_checks.cpp
    src/platform/windows/WinCliArgs.cpp)
else()
  set(ECAPTURE_CLI_ENTRY_CHECKS
    tests/platform/cli_text_posix_checks.cpp
    src/platform/linux/PosixCliArgs.cpp)
endif()

add_executable(ecapture-cli-text-tests
  tests/cli_text_state.cpp
  ${ECAPTURE_CLI_ENTRY_CHECKS}
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/CliText.cpp
  src/Json.cpp
  src/CaptureScope.cpp
  src/core/TextCoding.cpp)
ecapture_apply_common(ecapture-cli-text-tests)
set_target_properties(ecapture-cli-text-tests PROPERTIES OUTPUT_NAME "ecapture-cli-text-tests")
# 判据文件里写的是 UTF-8 中文用例，MSVC 上源码与执行字符集都要按 UTF-8 读
# （ecapture_apply_common 已经给了 /utf-8，这里只补 Windows 侧那份文案表与诊断需要的系统库）。
if(WIN32)
  target_link_libraries(ecapture-cli-text-tests PRIVATE kernel32.lib user32.lib advapi32.lib)
endif()

# ecapture-save-tests —— 跨平台文件 / 目录 / 原子保存 / 历史 / 配置位置的离线判据。
#
# 这一步把 OutputPlan / FileSave / HistoryArchive / Delivery 从"只在 Windows 头文件下编得过"
# 变成共用层：它们的平台动作已经拆到 HostPath / HostFile / HostDirectory / *FileSave 两套实现里。
# 所以这一份判据在 Linux 上链生产本体，逐条判真实磁盘现场：正常保存、不覆盖、并发同名、
# 权限失败、符号链接、中文路径、历史部分成功、配置位置与 --portable。判据只用本次自建的
# 临时目录与合成字节，不碰用户历史，也不取一个像素。
find_package(Threads REQUIRED)
add_executable(ecapture-save-tests
  tests/save_state.cpp
  src/CliOptions.cpp
  ${ECAPTURE_HOST_SOURCES}
  ${ECAPTURE_LANG_SOURCES}
  src/OutputPlan.cpp
  src/HistoryArchive.cpp
  src/Delivery.cpp
  src/Deadline.cpp
  src/CaptureScope.cpp
  src/core/TextCoding.cpp)
ecapture_apply_common(ecapture-save-tests)
target_link_libraries(ecapture-save-tests PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(ecapture-save-tests PRIVATE kernel32.lib user32.lib advapi32.lib)
endif()
set_target_properties(ecapture-save-tests PROPERTIES OUTPUT_NAME "ecapture-save-tests")

# ---- 只在 Windows 定义的那批目标：Linux 上如实报告，不静默消失 ----
#
# 这份名单由 Windows 分支自己复核（下面 get_property 那一条）：名单与真实定义对不上，
# Windows 配置阶段直接失败。这样 Linux 报出来的"还建不了"不会有一天变成过期清单。
set(ECAPTURE_WINDOWS_ONLY_TARGETS
  ecapture
  ecapture-example-ini
  ecapture-consent-tests
  ecapture-worker-tests
  ecapture-image-tests
  ecapture-identity-tests
  ecapture-dup-tests
  ecapture-compat-tests
  ecapture-capabilities-tests
  ecapture-windows-tests
  ecapture-screens-tests
  ecapture-crop-tests
  ecapture-cursor-tests
  ecapture-hdr-tests
  ecapture-delivery-tests
  ecapture-history-tests
  ecapture-notification-tests
  ecapture-config-tests)

if(WIN32)
  get_property(_ec_defined_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  set(_ec_list_missing "")
  foreach(_ec_t IN LISTS ECAPTURE_WINDOWS_ONLY_TARGETS)
    if(NOT _ec_t IN_LIST _ec_defined_targets)
      list(APPEND _ec_list_missing "${_ec_t}")
    endif()
  endforeach()
  if(_ec_list_missing)
    list(JOIN _ec_list_missing ", " _ec_list_missing_text)
    message(FATAL_ERROR "ECAPTURE_WINDOWS_ONLY_TARGETS 与 Windows 分支实际定义的目标对不上，"
                        "缺这些：${_ec_list_missing_text}。名单是给 Linux 报告的口径，改了目标要同步改名单。")
  endif()
endif()
# Linux 侧"哪些目标还建不了"那句话放在文件末尾的 wx 判定之后一起说（要带上 GUI 探针的有无）。

# ===========================================================================
# wxWidgets 依赖接线（本步只做这一件：找得到就建一个探针目标，链上 core+base）
#
# 确认弹窗与通知的 wxWidgets 实现是后续步骤，这里**不**重写 Consent.cpp / Notification*.cpp。
# 探针存在的意义是把"依赖发现 + 头/宏/库三样都传给了目标"钉成一条可重复验证的构建事实，
# 而不是等到写 GUI 那天才发现 find_package 的组件与链接顺序不对。
#
# 选用版本与来源（也写进 README 与 MEMORY.md）：
#   * 目标版本 wxWidgets 3.2.x（当前上游稳定分支）。Linux 上用发行版包，Debian/Ubuntu 是
#     libwxgtk3.2-dev（工具集 wxGTK 3.0 那一档太旧，本项目不要）。
#   * 发现方式：CMake 的 MODULE 模式 FindwxWidgets —— 它调 wx-config 问版本、cflags 与 libs，
#     因此跟着发行版装的 wx 走，不写死安装路径；多版本共存时用 -DECAPTURE_WX_CONFIG=/path/to/wx-config 点名。
#   * 链接策略：共享库（不用 wx 自带的 static 配置，避免与发行版 GTK 依赖打架）；
#     组件只要 core + base —— core 自己带上工具集依赖，这里不硬编 GTK 版本号。
#     头、宏、附加编译选项、库四样都由 target_* 显式挂到目标上，不用
#     include(${wxWidgets_USE_FILE})，那会污染整个目录的作用域。
#   * 许可证：wxWidgets 是 LGPL 2.1 with an exception（动态链接下无额外义务），本项目静态 CRT 的
#     Windows 交付方式需要把它一并记账，属于 GUI 实现那一步的依赖材料。
#
# 默认值按平台分岔是刻意的：Windows 构建入口（build.ps1 + installer 那套判据）要保持原样，
# 不能因为某台机器恰好装了 wx 就多出一个目标、把打包判据的产物清单打乱。
# ===========================================================================
if(WIN32)
  set(_ec_gui_default OFF)
else()
  set(_ec_gui_default ON)
endif()
option(ECAPTURE_ENABLE_GUI "构建 wxWidgets GUI 目标（本步只有依赖探针，没有确认弹窗）" ${_ec_gui_default})
option(ECAPTURE_REQUIRE_WX "找不到 wxWidgets 就报错退出（./build.sh --gui 用它把缺依赖变成硬错误）" OFF)
set(ECAPTURE_WX_CONFIG "" CACHE FILEPATH "指定 wx-config 可执行文件；留空则由 FindwxWidgets 在 PATH 里找")
set(ECAPTURE_WX_MIN_VERSION "" CACHE STRING "wxWidgets 版本下限，例如 3.2.0；留空表示找到即用")

set(_ec_gui_target_note "")
if(ECAPTURE_ENABLE_GUI)
  if(ECAPTURE_WX_CONFIG)
    set(wxWidgets_CONFIG_EXECUTABLE "${ECAPTURE_WX_CONFIG}")
  endif()
  set(_ec_wx_req "${ECAPTURE_WX_MIN_VERSION}")
  find_package(wxWidgets ${_ec_wx_req} COMPONENTS core base QUIET)

  if(wxWidgets_FOUND)
    set(_ec_wx_ver "${wxWidgets_VERSION_STRING}")
    if(NOT _ec_wx_ver)
      set(_ec_wx_ver "版本见 wx-config")
    endif()
    message(STATUS "wxWidgets：找到 ${_ec_wx_ver}（组件 core;base），库：${wxWidgets_LIBRARIES}")
    add_executable(ecapture-gui-probe src/gui/WxGuiProbe.cpp)
    ecapture_apply_common(ecapture-gui-probe)
    target_include_directories(ecapture-gui-probe PRIVATE ${wxWidgets_INCLUDE_DIRS})
    target_compile_definitions(ecapture-gui-probe PRIVATE ${wxWidgets_DEFINITIONS})
    target_compile_options(ecapture-gui-probe PRIVATE ${wxWidgets_CXX_FLAGS})
    target_link_libraries(ecapture-gui-probe PRIVATE ${wxWidgets_LIBRARIES})
    set_target_properties(ecapture-gui-probe PROPERTIES OUTPUT_NAME "ecapture-gui-probe")
    set(_ec_gui_target_note ", ecapture-gui-probe")
  elseif(ECAPTURE_REQUIRE_WX)
    message(FATAL_ERROR
      "ECAPTURE_REQUIRE_WX=ON 但没找到 wxWidgets（FindwxWidgets 走 wx-config 与常见安装前缀都没命中）。\n"
      "  * Linux 建议安装发行版包（本项目不自动装依赖，见 AGENTS.md）：Debian/Ubuntu "
      "apt install libwxgtk3.2-dev；Fedora dnf install wxGTK3.2-devel\n"
      "  * 已经装了但 wx-config 不在 PATH 上：-DECAPTURE_WX_CONFIG=/全路径/wx-config 再配置一次\n"
      "  * 版本下限卡住了：-DECAPTURE_WX_MIN_VERSION=3.2.0（留空表示找到即用）\n"
      "  * 只要核心离线判据：去掉 --gui（默认是可选依赖，不阻塞构建）")
  else()
    message(STATUS "wxWidgets：未找到，跳过 GUI 探针目标（ECAPTURE_ENABLE_GUI=ON 但依赖缺失）。"
                   "装好后重跑 ./build.sh --gui 可看到硬失败诊断。")
  endif()
else()
  message(STATUS "wxWidgets：ECAPTURE_ENABLE_GUI=OFF，不构建 GUI 目标。")
endif()

# Linux 这一句要等 wx 判定完才写得出完整清单，所以放在文件末尾。
if(NOT WIN32)
  list(JOIN ECAPTURE_WINDOWS_ONLY_TARGETS ", " _ec_win_only_text)
  message(STATUS "以下目标只在 Windows 分支定义，Linux 上暂不可构建（截图后端与文案资源尚未迁移）：${_ec_win_only_text}")
  message(STATUS "Linux 本步可构建目标：ecapture-core-tests, ecapture-wgc-tests, ecapture-cli-text-tests, ecapture-save-tests${_ec_gui_target_note}")

  # 给入口脚本读的那一份清单（./build.sh --list-targets 与 ./test-all.sh 的"这个目标本平台到底有没有"）。
  # 由 CMake 生成，而不是在 shell 里再抄一份名单：两边各写一处迟早对不上。
  set(_ec_buildable_report "")
  foreach(_ec_t IN ITEMS ecapture-core-tests ecapture-wgc-tests ecapture-cli-text-tests ecapture-save-tests ecapture-gui-probe)
    if(TARGET ${_ec_t})
      string(APPEND _ec_buildable_report "target=${_ec_t} artifact=$<TARGET_FILE:${_ec_t}>\n")
    endif()
  endforeach()
  list(JOIN ECAPTURE_WINDOWS_ONLY_TARGETS "," _ec_win_only_semi)
  string(APPEND _ec_buildable_report "windows-only=${_ec_win_only_semi}\n")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/ecapture-buildable-targets.txt" CONTENT
"# 由 CMakeLists.txt 生成，不要手改：这是本平台当前真正可构建的目标清单。
# windows-only= 那一行列出仍然只在 Windows 分支定义的目标（Linux 上暂不可构建）。
${_ec_buildable_report}")
endif()

