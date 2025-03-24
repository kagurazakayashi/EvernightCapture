cmake_minimum_required(VERSION 3.25)

project(EvernightCapture
        VERSION 0.1.0
        DESCRIPTION "CLI screenshot tool built on Windows.Graphics.Capture"
        LANGUAGES CXX)

if(NOT MSVC)
  message(FATAL_ERROR "EvernightCapture 需要 MSVC（Windows.Graphics.Capture / C++/WinRT 依赖 Windows SDK）")
endif()

add_executable(ecapture
  src/Main.cpp
  src/CliOptions.cpp
  src/Json.cpp
  src/Report.cpp
  src/WindowMatch.cpp
  src/Capture.cpp
  src/CaptureCommon.cpp
  src/CaptureWgc.cpp
  src/CaptureDwm.cpp
  src/CapturePrintWindow.cpp
  src/CaptureBitBlt.cpp
  src/Encoder.cpp
)

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
