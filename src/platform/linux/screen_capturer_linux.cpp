// Linux 屏幕采集工厂：统一分发。
//
// 优先级：
//   1. PipeWire（Mutter ScreenCast）：Wayland/Xorg GNOME 会话都可采集，
//      无人值守友好（无对话框）；需要 GUI 会话的 D-Bus。
//   2. X11（XWayland/Xorg）：PipeWire 不可用时的兜底；Wayland 会话下
//      抓不到完整桌面（XGetImage 被拒），会给出明确错误提示。
//
// enumerate_screens 走 XWayland（无论哪种采集方式，逻辑屏幕布局都一致）。

#include <string>
#include <vector>

#include "pxc/screen_capturer.h"
#include "screen_capturer_x11.h"

#if !defined(PXC_HAVE_X11) && !defined(PXC_HAVE_PIPEWIRE)
#error "Linux 屏幕采集需要 PXC_HAVE_X11 或 PXC_HAVE_PIPEWIRE 之一"
#endif

namespace pxc {

std::vector<ScreenInfo> enumerate_screens(std::string* error) {
#if defined(PXC_HAVE_X11)
    return enumerate_screens_x11(error);
#else
    if (error) *error = "缺少 X11（XWayland）用于屏幕枚举";
    return {};
#endif
}

std::unique_ptr<ScreenCapturer> create_screen_capturer(std::string* error) {
#if defined(PXC_HAVE_PIPEWIRE)
    // 有会话 D-Bus（GUI 会话）时用 Mutter ScreenCast + PipeWire
    if (pipewire_environment_available()) {
        auto capturer = create_screen_capturer_pipewire(error);
        if (capturer) return capturer;
        // PipeWire 建流失败（如非 GNOME 会话），退回 X11
    }
#endif
#if defined(PXC_HAVE_X11)
    return create_screen_capturer_x11(error);
#else
    if (error) *error = "当前环境没有可用的屏幕采集实现";
    return nullptr;
#endif
}

}  // namespace pxc
