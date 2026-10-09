#pragma once

// 键鼠注入：跨平台纯虚接口（被控端使用）。
//
// 平台实现：
//   Windows  src/platform/windows/input_injector_win.cpp  (SendInput)
//   Linux    src/platform/linux/input_injector_x11.cpp    (XTest)
//
// 坐标约定：mouse_move 收到的是「目标屏幕内的归一化坐标 (0..1)」，
// 由注入端负责映射到该平台的多屏虚拟桌面绝对坐标。
// 屏幕列表必须来自 enumerate_screens()，两端顺序一致。
//
// 安全线：调用方必须先通过连接密钥认证，再创建/调用注入器。

#include <memory>
#include <string>

namespace pxc {

class InputInjector {
public:
    virtual ~InputInjector() = default;

    // 把光标移到 screen_index 号屏幕内的 (nx, ny)，nx/ny ∈ [0,1]。
    virtual bool mouse_move(int screen_index, float nx, float ny) = 0;
    // button: "left" | "right" | "middle"
    virtual bool mouse_button(const std::string& button, bool pressed) = 0;
    // 滚轮。dy 为格数，正值向上。
    virtual bool mouse_wheel(int dy) = 0;
    // qt_key 为 Qt::Key 值（两端都是 Qt 客户端，用它做统一键码）。
    // text 是事件附带的 UTF-8 文本（可为空），modifiers 是 Qt::KeyboardModifiers。
    virtual bool key_event(int qt_key, const std::string& text, bool pressed, int modifiers) = 0;

    virtual std::string last_error() const = 0;
};

// 平台工厂；不支持时返回 nullptr 并写入原因。
std::unique_ptr<InputInjector> create_input_injector(std::string* error = nullptr);

// GNOME（mutter RemoteDesktop D-Bus）注入：能到达 Wayland 原生组件
// （Shell 顶栏/Dock/概览）。要求屏幕录制已启动（采集器创建了关联的
// RD 会话）；无就绪会话时返回 nullptr（非 GNOME 会话或视频未启动），
// 调用方应回退平台默认实现（Linux 为 XTest）。仅 Linux 实现。
std::unique_ptr<InputInjector> create_input_injector_gnome(std::string* error = nullptr);

}  // namespace pxc
