#pragma once

// GNOME (mutter) RemoteDesktop D-Bus 会话代理：采集与注入共享。
//
// XTest 只能到达 XWayland 世界；GNOME Shell 的顶栏/底部 Dock/概览是
// Wayland 原生组件，XTest 事件永远到不了。mutter 的 RemoteDesktop
// D-Bus 接口（gnome-remote-desktop / ToDesk 同款机制）经 clutter 虚拟
// 设备注入，能到达整个桌面（含 Shell）。
//
// mutter 42 语义（对照 gnome-42 分支源码逐条确认）：
//  * ScreenCast CreateSession 通过 "remote-desktop-session-id" 选项关联
//    RD 会话，值是 RD 会话的 SessionId 属性（字符串），不是对象路径；
//  * RD 会话的全部 Notify* 方法有 peer 校验——必须与创建它的 D-Bus
//    连接是同一发送者。因此本代理持有专属连接，RD 会话生命周期与
//    所有注入调用都走它；采集器的 SC 会话用采集器自己的连接创建
//    （注册关联时无 peer 校验）；
//  * SC 会话关闭时 mutter 自动关闭关联的 RD 会话
//    （on_screen_cast_session_closed → close），因此采集器每次重建都
//    必须重新创建并关联 RD 会话；
//  * 绝对坐标是「流像素」：Area 流经 area.x + round(px/scale) 映射，
//    流像素尺寸就是采集器协商出的帧尺寸；
//  * NotifyPointerAxisDiscrete(axis=0) 是垂直轴，steps<0 为向上。
//
// 可用性：RD 会话不可用（非 GNOME 会话）时采集器照常建独立 SC 会话，
// 注入回退 X11（input_injector_x11.cpp），无功能回退。

#if defined(__linux__) && defined(PXC_HAVE_GNOME_RD)

#include <cstdint>
#include <string>

namespace pxc {

class GnomeRemoteDesktopBroker {
public:
    static GnomeRemoteDesktopBroker& instance();

    // ---- 采集侧（screen_capturer_pipewire 调用）----

    // 在 SC CreateSession 之前调用：确保 RD 会话已创建（不 Start），
    // 返回 "remote-desktop-session-id" 的取值。失败返回空串
    // （采集器照常建独立 SC 会话，注入回退 X11）。
    std::string acquire_session_id();
    // RecordArea 成功后调用：mutter 42 拒绝对关联型 SC 会话直接 Start
    // （"Must be started from remote desktop session"），必须由 RD Start
    // 拉起（内部连带 meta_screen_cast_session_start）。
    bool start_session();
    // RecordArea 成功后调用：流对象路径（绝对坐标的目标）。
    void set_stream_path(const std::string& path);
    // 流像素尺寸已知/变化时调用（格式协商结果）。
    void set_stream_size(uint32_t width, uint32_t height);
    // 采集停止时调用：由 RD Stop 关闭关联 SC，再释放专属连接；
    // 未启动/启动失败的 RD 也通过连接断开清理。可重复调用。
    void on_screencast_stopped(const std::string& session_id);

    // ---- 注入侧 ----

    bool has_session() const;
    // nx/ny ∈ [0,1]（录制屏幕内归一化），内部换算成流像素。
    bool notify_pointer_move(float nx, float ny);
    bool notify_button(int button, bool pressed);  // 1=左 2=中 3=右
    bool notify_wheel(int steps);                  // steps>0 向上
    bool notify_keysym(uint32_t keysym, bool pressed);
    std::string last_error() const;

private:
    GnomeRemoteDesktopBroker();
    ~GnomeRemoteDesktopBroker();
    GnomeRemoteDesktopBroker(const GnomeRemoteDesktopBroker&)            = delete;
    GnomeRemoteDesktopBroker& operator=(const GnomeRemoteDesktopBroker&) = delete;

    // 隐藏 libdbus 类型，头文件不依赖 dbus.h
    struct State;
    State* s_;
};

}  // namespace pxc

#endif  // __linux__ && PXC_HAVE_GNOME_RD
