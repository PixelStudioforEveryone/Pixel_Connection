cmake_minimum_required(VERSION 3.16)
project(pxchos)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/deps_ohos.cmake)

# 原仓库根目录：共享源码引用（只读，绝不修改）
# cpp -> main -> src -> pixel_connection -> Pixel_Connection_HOS -> Pixel_Connection
set(PXC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." CACHE PATH
    "Pixel Connection 源码根目录")
if(NOT EXISTS "${PXC_ROOT}/include/pxc/protocol.h")
    message(FATAL_ERROR "pxc: 未找到原仓库源码（PXC_ROOT=${PXC_ROOT}）")
endif()

# ---------------------------------------------------------------------------
# pxc_core_hos：原仓库可移植核心（主控端所需），无平台采集/注入/解码文件。
# session_wire.cpp（ch-control JSON 线格式）Qt-free，直接复用。
# ---------------------------------------------------------------------------
add_library(pxc_core_hos STATIC
    ${PXC_ROOT}/src/protocol.cpp
    ${PXC_ROOT}/src/identity.cpp
    ${PXC_ROOT}/src/crypto.cpp
    ${PXC_ROOT}/src/account_api.cpp
    ${PXC_ROOT}/src/session_auth.cpp
    ${PXC_ROOT}/src/signaling_client.cpp
    ${PXC_ROOT}/src/peer_session.cpp
    ${PXC_ROOT}/src/video_protocol.cpp
    ${PXC_ROOT}/src/video_transport.cpp
    ${PXC_ROOT}/src/video_frame_utils.cpp
    ${PXC_ROOT}/apps/client/session_wire.cpp
)
target_include_directories(pxc_core_hos PUBLIC
    ${PXC_ROOT}/include
    ${PXC_ROOT}/apps/client
)
find_package(Threads REQUIRED)
target_link_libraries(pxc_core_hos
    PUBLIC LibDataChannel::LibDataChannel
           ixwebsocket
           nlohmann_json::nlohmann_json
           OpenSSL::SSL
           OpenSSL::Crypto
           Threads::Threads
)

# ---------------------------------------------------------------------------
# libpxc_hos.so：NAPI 模块（Phase 3 起承载 HosSession / HosVideoReceiver）
# ---------------------------------------------------------------------------
add_library(pxc_hos SHARED
    pxc_napi/napi_init.cpp
    pxc_napi/hos_controller.cpp
    pxc_napi/hos_file_transfer.cpp
    pxc_napi/hos_video_receiver.cpp
)

target_link_libraries(pxc_hos PUBLIC pxc_core_hos libace_napi.z.so
    libnative_media_vdec.so
    libnative_media_core.so
    libnative_media_codecbase.so
    libnative_window.so
    libhilog_ndk.z.so)
