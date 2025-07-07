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
  src/Consent.cpp
  src/CaptureCommon.cpp
  src/ImageOps.cpp
  src/D3dDevice.cpp
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
