cmake_minimum_required(VERSION 3.16)
project(PixelConnection LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # 源码统一按 UTF-8 解析：中文注释在默认代码页(936)下会被读成乱码，
    # 进而导致大量伪语法错误（C4819/C2001 等）
    add_compile_options(/utf-8)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

include(cmake/deps.cmake)

# 平台层：屏幕采集 / 视频编解码 / 键鼠注入
#   Windows: DXGI 采集 + Media Foundation H.264 编解码 + SendInput 注入
#   Linux:   X11 采集（可选，缺 X11 时用兜底桩）+ XTest 注入 + 编解码占位
set(PXC_PLATFORM_SOURCES "")
if(WIN32)
    list(APPEND PXC_PLATFORM_SOURCES
        src/platform/windows/screen_capturer_win.cpp
        src/platform/windows/video_encoder_mf.cpp
        src/platform/windows/video_decoder_mf.cpp
        src/platform/windows/input_injector_win.cpp
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND PXC_PLATFORM_SOURCES src/platform/linux/video_codec_stub.cpp)
    find_package(X11 QUIET)
    if(X11_FOUND AND X11_Xinerama_FOUND AND X11_XTest_FOUND)
        list(APPEND PXC_PLATFORM_SOURCES
            src/platform/linux/screen_capturer_x11.cpp
            src/platform/linux/input_injector_x11.cpp
        )
        set(PXC_HAVE_X11 TRUE)
    else()
        message(STATUS "pxc: X11/Xinerama/XTest 不可用，屏幕采集与注入使用兜底桩")
    endif()

    # 视频编解码：优先 FFmpeg libavcodec；缺失时退回占位桩
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET libavcodec libavutil)
    endif()
    if(FFMPEG_FOUND)
        list(REMOVE_ITEM PXC_PLATFORM_SOURCES src/platform/linux/video_codec_stub.cpp)
        list(APPEND PXC_PLATFORM_SOURCES
            src/platform/linux/video_encoder_ffmpeg.cpp
            src/platform/linux/video_decoder_ffmpeg.cpp
        )
        set(PXC_HAVE_FFMPEG TRUE)
        # libswscale：运行库随 ffmpeg 必装，但 dev 包（.pc/头文件）可能
        # 缺失。直接 find_library 链接 .so，头文件侧用最小原型替代；
        # 找不到时不启用 sws 缩放路径（编码器回退纯转换）。
        find_library(PXC_SWSCALE_LIB NAMES swscale libswscale.so.5 libswscale.so
                     HINTS ${FFMPEG_LIBRARY_DIRS}
                           /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu)
        if(PXC_SWSCALE_LIB)
            set(PXC_HAVE_SWSCALE TRUE)
            message(STATUS "pxc: libswscale 运行库 ${PXC_SWSCALE_LIB}")
        else()
            message(STATUS "pxc: 未找到 libswscale，编码器缩放路径禁用")
        endif()
        message(STATUS "pxc: 使用 FFmpeg libavcodec 编解码 (${FFMPEG_VERSION})")
    else()
        message(STATUS "pxc: 未找到 FFmpeg libavcodec，视频编解码使用占位桩")
    endif()

    # Wayland 采集：Mutter ScreenCast + PipeWire（GNOME 会话，免对话框）
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET libpipewire-0.3 libspa-0.2)
        pkg_check_modules(DBUS QUIET dbus-1)
    endif()
    if(PIPEWIRE_FOUND AND DBUS_FOUND)
        list(APPEND PXC_PLATFORM_SOURCES src/platform/linux/screen_capturer_pipewire.cpp)
        set(PXC_HAVE_PIPEWIRE TRUE)
        message(STATUS "pxc: Wayland 采集启用（Mutter ScreenCast + PipeWire）")
    else()
        message(STATUS "pxc: PipeWire/libdbus 不可用，Wayland 采集未启用")
    endif()
    # GNOME（mutter RemoteDesktop D-Bus）注入：仅需 libdbus。优先于 XTest
    # （XTest 到不了 Wayland 原生的 Shell 顶栏/Dock/概览）
    if(DBUS_FOUND)
        list(APPEND PXC_PLATFORM_SOURCES src/platform/linux/input_injector_gnome.cpp)
        set(PXC_HAVE_GNOME_RD TRUE)
        message(STATUS "pxc: GNOME RemoteDesktop 注入启用（libdbus）")
    endif()
    if(PXC_HAVE_X11 OR PXC_HAVE_PIPEWIRE)
        list(APPEND PXC_PLATFORM_SOURCES src/platform/linux/screen_capturer_linux.cpp)
    endif()
endif()
if(NOT PXC_PLATFORM_SOURCES)
    # 纯服务器环境（如 CI 容器）：只提供永远失败的桩实现
    list(APPEND PXC_PLATFORM_SOURCES src/platform/desktop_stub.cpp)
endif()

# 共享代码：信令协议 + 设备身份 + 信令客户端 + P2P 会话封装
add_library(pxc_core STATIC
    src/protocol.cpp
    src/identity.cpp
    src/crypto.cpp
    src/account_api.cpp
    src/session_auth.cpp
    src/signaling_client.cpp
    src/peer_session.cpp
    src/video_protocol.cpp
    src/video_transport.cpp
    src/video_frame_utils.cpp
    ${PXC_PLATFORM_SOURCES}
)
target_include_directories(pxc_core PUBLIC ${CMAKE_SOURCE_DIR}/include)
if(WIN32)
    target_compile_definitions(pxc_core PRIVATE
        NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00
    )
    target_link_libraries(pxc_core PUBLIC
        d3d11 dxgi dxguid mfplat mfuuid uuid ole32 user32 gdi32 advapi32
    )
endif()
if(PXC_HAVE_X11)
    target_compile_definitions(pxc_core PUBLIC PXC_HAVE_X11=1)
    target_include_directories(pxc_core PUBLIC ${X11_INCLUDE_DIR})
    target_link_libraries(pxc_core PUBLIC
        ${X11_LIBRARIES} ${X11_Xinerama_LIB} ${X11_XTest_LIB}
    )
endif()
if(PXC_HAVE_FFMPEG)
    target_compile_definitions(pxc_core PUBLIC PXC_HAVE_FFMPEG=1)
    target_include_directories(pxc_core SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(pxc_core PUBLIC ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(pxc_core PUBLIC ${FFMPEG_LIBRARIES})
    if(PXC_HAVE_SWSCALE)
        target_compile_definitions(pxc_core PUBLIC PXC_HAVE_SWSCALE=1)
        target_link_libraries(pxc_core PUBLIC ${PXC_SWSCALE_LIB})
    endif()
endif()
if(PXC_HAVE_PIPEWIRE)
    target_compile_definitions(pxc_core PUBLIC PXC_HAVE_PIPEWIRE=1)
    target_include_directories(pxc_core SYSTEM PUBLIC
        ${PIPEWIRE_INCLUDE_DIRS} ${DBUS_INCLUDE_DIRS})
    target_link_directories(pxc_core PUBLIC
        ${PIPEWIRE_LIBRARY_DIRS} ${DBUS_LIBRARY_DIRS})
    target_link_libraries(pxc_core PUBLIC ${PIPEWIRE_LIBRARIES} ${DBUS_LIBRARIES})
endif()
if(PXC_HAVE_GNOME_RD)
    target_compile_definitions(pxc_core PUBLIC PXC_HAVE_GNOME_RD=1)
    target_include_directories(pxc_core SYSTEM PUBLIC ${DBUS_INCLUDE_DIRS})
    target_link_directories(pxc_core PUBLIC ${DBUS_LIBRARY_DIRS})
    target_link_libraries(pxc_core PUBLIC ${DBUS_LIBRARIES})
endif()
target_link_libraries(pxc_core
    PUBLIC  LibDataChannel::LibDataChannel
            ixwebsocket
            nlohmann_json::nlohmann_json
            OpenSSL::SSL
            OpenSSL::Crypto
            Threads::Threads
)

# Qt 包仅生成客户端程序，服务运行时静态链接进客户端。
option(PXC_BUILD_QT_CLIENT "Build the Qt6 Widgets client" OFF)
if(PXC_BUILD_QT_CLIENT)
    set(PXC_STANDALONE_SERVER_DEFAULT OFF)
else()
    set(PXC_STANDALONE_SERVER_DEFAULT ON)
endif()
option(PXC_BUILD_STANDALONE_SERVER "Build a separate headless server executable"
       ${PXC_STANDALONE_SERVER_DEFAULT})
# 账号与信令服务器在 Windows / Linux 共用实现。
add_subdirectory(server)
add_subdirectory(apps/demo)

# Qt 客户端默认关闭：QLinux 构建环境通常没有 Qt6 开发包，
# 强制依赖会让整个工程无法配置。显式开启时才要求 Qt 存在。
if(PXC_BUILD_QT_CLIENT)
    find_package(Qt6 6.2 REQUIRED COMPONENTS Core Widgets Network)
    add_subdirectory(apps/client)
endif()

# 采集/编码链路诊断工具（部署排障用，体积小，始终构建）
add_executable(pxc-capture-test apps/capture_test.cpp)
target_link_libraries(pxc-capture-test PRIVATE pxc_core)

# 真实采集器活体压测（PipeWire 停滞复现用）
add_executable(pxc-capture-live-test apps/capture_live_test.cpp)
target_link_libraries(pxc-capture-live-test PRIVATE pxc_core)

option(BUILD_TESTING "Build PixelConnection tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "pxc: build type = ${CMAKE_BUILD_TYPE}")
