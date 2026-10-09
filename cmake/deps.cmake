# 第三方依赖统一在这里拉取。
# 所有依赖都从源码构建，避免 Ubuntu 22.04 仓库里没有对应包的问题。

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(PXC_DEPS_DIR ${CMAKE_SOURCE_DIR}/third_party)

# 依赖一律静态链接。libdatachannel 的 CMakeLists 里有
# option(BUILD_SHARED_LIBS ... ON)，会把这个缓存变量污染成 ON，
# 连带把 ixwebsocket 也编成 DLL——其类静态成员不在导出表里，
# 最终表现为 LNK2019。这里强制 OFF 并放在 MakeAvailable 之前。
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# 可选的本地依赖缓存根目录。网络受限时，调用方可传入已有的
# build/_deps，避免每个构建目录重新 clone GitHub；默认仍走 FetchContent。
set(PXC_DEPS_ROOT "" CACHE PATH "Reuse an existing _deps cache")
if(PXC_DEPS_ROOT)
    foreach(_pxc_dep nlohmann_json libdatachannel ixwebsocket sqlite_amalgamation)
        if(EXISTS "${PXC_DEPS_ROOT}/${_pxc_dep}-src")
            set(FETCHCONTENT_SOURCE_DIR_${_pxc_dep}
                "${PXC_DEPS_ROOT}/${_pxc_dep}-src" CACHE PATH "" FORCE)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------- nlohmann/json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)

# ------------------------------------------------------------- libdatachannel
# P2P 核心：内含 libjuice (ICE/STUN/TURN) + usrsctp (SCTP over DTLS) + OpenSSL
set(NO_WEBSOCKET     ON  CACHE BOOL "" FORCE)   # 信令自己用 IXWebSocket，不需要它的 ws
set(NO_MEDIA         ON  CACHE BOOL "" FORCE)   # 我们只走 DataChannel，不用 RTP 媒体栈
set(NO_EXAMPLES      ON  CACHE BOOL "" FORCE)
set(NO_TESTS         ON  CACHE BOOL "" FORCE)
set(USE_SYSTEM_SRTP  OFF CACHE BOOL "" FORCE)
set(USE_NICE         OFF CACHE BOOL "" FORCE)   # 用内置 libjuice，不依赖 libnice

FetchContent_Declare(
    libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        v0.24.6
    GIT_SHALLOW    TRUE
)

# ------------------------------------------------------------------ IXWebSocket
# 信令通道：同一个库同时提供 server 和 client，服务端与客户端代码可以共用
set(USE_TLS          ON  CACHE BOOL "" FORCE)
set(USE_OPEN_SSL     ON  CACHE BOOL "" FORCE)
set(USE_ZLIB         ON  CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG        v12.0.1
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(nlohmann_json libdatachannel ixwebsocket)
include("${CMAKE_CURRENT_LIST_DIR}/ixwebsocket_ipv6.cmake")

# ----------------------------------------------------------------------- SQLite
# 只有账号服务器需要。用官方 amalgamation 源码直接编译，避免依赖系统 libsqlite3-dev，
# Aliyun Linux / Windows 上都能同样构建。哈希已与 sqlite.org 公布的 SHA3-256 核对过。
FetchContent_Declare(
    sqlite_amalgamation
    URL      https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
)
FetchContent_GetProperties(sqlite_amalgamation)
if(NOT sqlite_amalgamation_POPULATED)
    FetchContent_Populate(sqlite_amalgamation)
endif()

enable_language(C)
add_library(pxc_sqlite3 STATIC ${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c)
target_include_directories(pxc_sqlite3 PUBLIC ${sqlite_amalgamation_SOURCE_DIR})
target_compile_definitions(pxc_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DQS=0
)
target_link_libraries(pxc_sqlite3 PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

message(STATUS "pxc: libdatachannel ${libdatachannel_SOURCE_DIR}")
message(STATUS "pxc: ixwebsocket    ${ixwebsocket_SOURCE_DIR}")
