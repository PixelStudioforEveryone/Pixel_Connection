# HOS 端第三方依赖（在主 CMakeLists 的 project() 之后 include）。
#
#   - OpenSSL：WSL 交叉编译的 prebuilt 静态库，按 ABI 存放于 third_party/openssl/<abi>/
#     （构建脚本见 third_party/openssl/build_openssl_wsl.sh）
#   - libdatachannel / ixwebsocket / nlohmann_json：与原仓库 build-qt/_deps 同源拷贝，
#     经 FETCHCONTENT_SOURCE_DIR_* 就地构建，不联网
#
# 注意：hvigor 对每个 ABI（arm64-v8a / x86_64）分别跑一次 CMake 配置，
# OHOS_ARCH 由 ohos.toolchain.cmake 提供，本文件所有路径都按当前 ABI 解析。

if(NOT DEFINED OHOS_ARCH)
    message(FATAL_ERROR "pxc: 缺少 OHOS_ARCH（应由 hvigor 的 ohos.toolchain.cmake 提供）")
endif()

# 工程根 third_party：cpp -> main -> src -> pixel_connection -> Pixel_Connection_HOS
set(PXC_TP "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party")

# ------------------------------------------------------------- OpenSSL prebuilt
set(PXC_OPENSSL_ROOT "${PXC_TP}/openssl/${OHOS_ARCH}")
if(NOT EXISTS "${PXC_OPENSSL_ROOT}/lib/libcrypto.a")
    message(FATAL_ERROR
        "pxc: 未找到 OpenSSL prebuilt：${PXC_OPENSSL_ROOT}\n"
        "pxc: 先运行 third_party/openssl/build_openssl_linux.sh 生成")
endif()
set(OPENSSL_ROOT_DIR "${PXC_OPENSSL_ROOT}" CACHE PATH "" FORCE)
set(OPENSSL_INCLUDE_DIR "${PXC_OPENSSL_ROOT}/include" CACHE PATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${PXC_OPENSSL_ROOT}/lib/libcrypto.a" CACHE FILEPATH "" FORCE)
set(OPENSSL_SSL_LIBRARY "${PXC_OPENSSL_ROOT}/lib/libssl.a" CACHE FILEPATH "" FORCE)
set(OPENSSL_USE_STATIC_LIBS TRUE CACHE BOOL "" FORCE)
find_package(OpenSSL REQUIRED)
message(STATUS "pxc: OpenSSL ${OPENSSL_VERSION} prebuilt (${OHOS_ARCH})")

# --------------------------------------------------------------------- 通用选项
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# 一律静态链接；libdatachannel 会试图把 BUILD_SHARED_LIBS 污染成 ON，
# 必须在 MakeAvailable 之前强制 OFF（同原仓库 cmake/deps.cmake 的处理）。
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# ----------------------------------------------------------------- libdatachannel
# P2P 核心：内含 libjuice (ICE/STUN/TURN) + usrsctp (SCTP over DTLS)
# 选项与原仓库 cmake/deps.cmake 完全一致，TLS 层用上面找到的 OpenSSL。
set(NO_WEBSOCKET     ON  CACHE BOOL "" FORCE)
set(NO_MEDIA         ON  CACHE BOOL "" FORCE)
set(NO_EXAMPLES      ON  CACHE BOOL "" FORCE)
set(NO_TESTS         ON  CACHE BOOL "" FORCE)
set(USE_SYSTEM_SRTP  OFF CACHE BOOL "" FORCE)
set(USE_NICE         OFF CACHE BOOL "" FORCE)

if(EXISTS "${PXC_TP}/libdatachannel/CMakeLists.txt")
    FetchContent_Declare(libdatachannel SOURCE_DIR "${PXC_TP}/libdatachannel")
else()
    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG v0.24.6 GIT_SHALLOW TRUE)
endif()

# -------------------------------------------------------------------- ixwebsocket
# 账号与信令公网入口使用 HTTPS/WSS，复用已交叉编译的 OpenSSL。
# 注意：USE_WS 只控制 ws 命令行工具（拖 spdlog），库内 WS 客户端始终编译；原仓库也未开启。
set(USE_TLS          ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL     ON CACHE BOOL "" FORCE)
set(USE_MBED_TLS     OFF CACHE BOOL "" FORCE)
set(USE_ZLIB         OFF CACHE BOOL "" FORCE)
set(USE_WS           OFF CACHE BOOL "" FORCE)
set(USE_TEST         OFF CACHE BOOL "" FORCE)
set(BUILD_DEMO       OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

if(EXISTS "${PXC_TP}/ixwebsocket/CMakeLists.txt")
    FetchContent_Declare(ixwebsocket SOURCE_DIR "${PXC_TP}/ixwebsocket")
else()
    FetchContent_Declare(ixwebsocket
        GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
        GIT_TAG v12.0.1 GIT_SHALLOW TRUE)
endif()

# ------------------------------------------------------------------ nlohmann/json
if(EXISTS "${PXC_TP}/nlohmann_json/CMakeLists.txt")
    FetchContent_Declare(nlohmann_json SOURCE_DIR "${PXC_TP}/nlohmann_json")
else()
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
endif()

FetchContent_MakeAvailable(libdatachannel ixwebsocket nlohmann_json)

# usrsctp 使用 u_long/u_int 等 BSD 类型，musl 仅在 _GNU_SOURCE 下提供（sys/types.h）。
foreach(_pxc_t usrsctp usrsctp-static)
    if(TARGET ${_pxc_t})
        target_compile_definitions(${_pxc_t} PRIVATE _GNU_SOURCE)
    endif()
endforeach()

message(STATUS "pxc: libdatachannel=${libdatachannel_SOURCE_DIR}")
message(STATUS "pxc: ixwebsocket=${ixwebsocket_SOURCE_DIR}")
