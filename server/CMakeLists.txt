add_library(pxc_server STATIC
    main.cpp
    registry.cpp
    store.cpp
    auth_service.cpp
    http_api.cpp
)
target_include_directories(pxc_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pxc_server PUBLIC pxc_core pxc_sqlite3)
if(WIN32)
    target_compile_definitions(pxc_server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(PXC_BUILD_STANDALONE_SERVER)
    add_executable(pxc-server standalone_main.cpp)
    target_link_libraries(pxc-server PRIVATE pxc_server)
    if(WIN32)
        target_compile_definitions(pxc-server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endif()
