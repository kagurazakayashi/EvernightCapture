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

project(EvernightCapture
        VERSION ${ECAPTURE_MAJOR_VERSION}.${ECAPTURE_MINOR_VERSION}.${ECAPTURE_PATCH_VERSION}
        DESCRIPTION "CLI screenshot tool built on Windows.Graphics.Capture"
        LANGUAGES CXX RC)

if(NOT MSVC)
  message(FATAL_ERROR "EvernightCapture 需要 MSVC（Windows.Graphics.Capture / C++/WinRT 依赖 Windows SDK）")
endif()

add_executable(ecapture
  src/Main.cpp
  src/CliOptions.cpp
  src/Lang.cpp
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
  src/WinrtApartment.cpp
  src/OutputPlan.cpp
  src/FileSave.cpp
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

# rc.exe 按当前目录解析 RCDATA 的文件名，而 Ninja 的工作目录是 build 目录：
# 把 resources 目录加进包含路径，文案文件按裸文件名即可命中。
# src 也在里面：ecapture.rc 用 #include "Version.h" 取版本号（那是个 C++ 头，不放 resources）。
target_include_directories(ecapture PRIVATE resources src)

# RC_DEPENDS 实测不会触发重编（改 .txt 后 ninja 说没活干），用 OBJECT_DEPENDS 才可靠：
# .rc 嵌进去的数据文件（四份文案 + 图标）与 #include 的头都必须列成 .rc 的依赖，
# 否则改完要 clean 重编才生效。
set(ECAPTURE_RC_INPUTS
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-zh-CN.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-zh-TW.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-en.txt"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/strings-ja.txt"
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
  src/Lang.cpp
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
# 隔离执行那套内部消息格式的离线判据（由 tests\worker.ps1 编译并运行）。
#
# 为什么单独一个可执行文件：格式层最要紧的判据是"畸形消息整条作废"——截断、超长、
# 保留段非 0、状态码没登记、像素字节数与行距不自洽。这些字节组合用真父子进程去造
# 既慢又造不全，而发布版 ECAPTURE.EXE 里绝不该有能喂自定义消息的入口。
# 期限那条判据要读 --lang 那套资源，所以把 Lang/CliOptions 与 .rc 一起编进来。
# 这里不链 Worker.cpp：它要起辅助进程，也要用到几条取图通道，而格式与期限这两层判据
# 都只需要纯数据的那几份源文件。异步 I/O 生命周期那层判据（tests\worker_io_state.cpp）
# 链的是 WorkerIo.cpp 本体与注入的假后端 —— 判的就是生产状态机，不是它的抄本；
# 连接时序那层（tests\worker_connect_state.cpp，F03）同样链 WorkerIo.cpp 本体，只是后端
# 换成真实的 Win32Backend + 真命名管道 —— "客户端抢先连上"这类顺序判不了假数据，
# 而发布版 ECAPTURE.EXE 里不许有控制连接顺序的开关，所以傀儡进程是本测试 exe 自己
# （--f03-peer 模式），不碰任何用户应用与桌面内容。
# ---------------------------------------------------------------------------
add_executable(ecapture-worker-tests
  tests/isolation_state.cpp
  tests/worker_io_state.cpp
  tests/worker_connect_state.cpp
  src/HdrColor.cpp
  src/WorkerIo.cpp
  src/WorkerProtocol.cpp
  src/Deadline.cpp
  src/WindowMatch.cpp
  src/ScreenMatch.cpp
  src/ScreenIdentity.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CliOptions.cpp
  src/Lang.cpp
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
  src/Lang.cpp
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
# 为什么单独一个可执行文件：这一批判据是"这一帧交回来的实际内容尺寸与纹理尺寸对不对得上、
# 该复制哪块矩形、要不要重建帧池、还来不来得及重建" —— 全是纯算术，而真机上要凑出
# ContentSize 大于 / 等于 / 小于纹理，以及"窗口恰好一直在变大把重建预算耗光"这几种形状，
# 既不稳定也做不到阴性对照。所以把判据本体（src/WgcGeometry.cpp）单独编一个测试程序逐条注入。
# 这里不碰 WinRT / D3D / 窗口，链接项一个图形库都不需要。
# ---------------------------------------------------------------------------
add_executable(ecapture-wgc-tests
  tests/wgc_state.cpp
  src/WgcGeometry.cpp)

target_include_directories(ecapture-wgc-tests PRIVATE src)
target_compile_features(ecapture-wgc-tests PRIVATE cxx_std_20)
set_target_properties(ecapture-wgc-tests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_compile_options(ecapture-wgc-tests PRIVATE /utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
target_compile_definitions(ecapture-wgc-tests PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(ecapture-wgc-tests PROPERTIES OUTPUT_NAME "ecapture-wgc-tests")
set_property(TARGET ecapture-wgc-tests PROPERTY MSVC_RUNTIME_LIBRARY
               "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
  src/Lang.cpp
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
  src/Lang.cpp
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
  src/CliOptions.cpp
  src/Lang.cpp
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
  src/CaptureScope.cpp
  src/CliOptions.cpp
  src/Lang.cpp
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
  src/Lang.cpp
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
  src/Lang.cpp
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
  src/Lang.cpp
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
  src/Lang.cpp
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
