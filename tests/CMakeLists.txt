add_executable(pxc-video-tests video_transport_test.cpp)
target_link_libraries(pxc-video-tests PRIVATE pxc_core)

add_executable(pxc-crypto-tests crypto_test.cpp)
target_link_libraries(pxc-crypto-tests PRIVATE pxc_core)

if(BUILD_TESTING)
    include(CTest)
    add_test(NAME pxc-video-tests COMMAND pxc-video-tests)
    add_test(NAME pxc-crypto-tests COMMAND pxc-crypto-tests)
endif()
