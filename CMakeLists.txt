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
  src/ScreenMatch.cpp
  src/Capture.cpp
  src/CaptureScope.cpp
  src/Deadline.cpp
  src/Worker.cpp
  src/WorkerProtocol.cpp
  src/WinrtApartment.cpp
  src/OutputPlan.cpp
  src/FileSave.cpp
  src/Consent.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/D3dDevice.cpp
  src/WgcGeometry.cpp
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
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/CliOptions.cpp
  src/Lang.cpp
  src/ScreenMatch.cpp
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
# 都只需要纯数据的那几份源文件。
# ---------------------------------------------------------------------------
add_executable(ecapture-worker-tests
  tests/isolation_state.cpp
  src/WorkerProtocol.cpp
  src/Deadline.cpp
  src/WindowMatch.cpp
  src/ScreenMatch.cpp
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
  src/ImageOps.cpp
  src/CaptureCommon.cpp
  src/CaptureScope.cpp
  src/Consent.cpp
  src/CliOptions.cpp
  src/Lang.cpp
  src/ScreenMatch.cpp
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
