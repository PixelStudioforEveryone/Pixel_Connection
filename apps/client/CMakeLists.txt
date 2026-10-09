# Qt 的 Q_OBJECT 类需要 moc 生成元对象代码。
# 不开启 AUTOMOC 会出现 "undefined reference to vtable" 之类的链接错误。
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(pxc-client
    main.cpp
    client_controller.cpp
    desktop_wallpaper.cpp
    local_server_manager.cpp
    login_page.cpp
    file_transfer_dialog.cpp
    device_page.cpp
    remote_control_view.cpp
    control_center_panel.cpp
    floating_control_button.cpp
    video_sender.cpp
    video_receiver.cpp
    session_wire.cpp
    theme.cpp
    settings_page.cpp
    toggle_switch.cpp
    icons.qrc
)

target_link_libraries(pxc-client PRIVATE pxc_core pxc_server Qt6::Core Qt6::Widgets Qt6::Network)

# Windows 上不要弹出控制台窗口；调试时可临时注释掉这一行
if(WIN32)
    target_sources(pxc-client PRIVATE app_icon.rc)
    target_include_directories(pxc-client PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_source_files_properties(app_icon.rc PROPERTIES OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/src/icon.ico")
    set_target_properties(pxc-client PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
