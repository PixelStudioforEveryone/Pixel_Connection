// Linux 键鼠注入：XTest 扩展。
//
// 坐标约定与 Windows 侧一致：mouse_move 收到 (screen_index, nx, ny)，
// 由 Xinerama 几何映射回根窗口全局坐标（Xinerama/RandR 下根窗口
// 覆盖所有显示器，坐标天然是全局的）。
// 键码线格式是 Qt::Key，映射到 X keysym 后经 XKeysymToKeycode 注入。

#if defined(__linux__) && defined(PXC_HAVE_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xinerama.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <string>

#include "pxc/input_injector.h"
#include "pxc/screen_capturer.h"
#include "gnome_remote_desktop.h"

namespace pxc {
namespace {

// 与 Windows 侧同一张 Qt::Key 常量表（Qt 承诺这些值稳定）
constexpr int kQtKeyEscape    = 0x01000000;
constexpr int kQtKeyTab       = 0x01000001;
constexpr int kQtKeyBackspace = 0x01000003;
constexpr int kQtKeyReturn    = 0x01000004;
constexpr int kQtKeyEnter     = 0x01000005;
constexpr int kQtKeyInsert    = 0x01000006;
constexpr int kQtKeyDelete    = 0x01000007;
constexpr int kQtKeyPause     = 0x01000008;
constexpr int kQtKeyPrint     = 0x01000009;
constexpr int kQtKeyClear     = 0x0100000b;
constexpr int kQtKeyHome      = 0x01000010;
constexpr int kQtKeyEnd       = 0x01000011;
constexpr int kQtKeyLeft      = 0x01000012;
constexpr int kQtKeyUp        = 0x01000013;
constexpr int kQtKeyRight     = 0x01000014;
constexpr int kQtKeyDown      = 0x01000015;
constexpr int kQtKeyPageUp    = 0x01000016;
constexpr int kQtKeyPageDown  = 0x01000017;
constexpr int kQtKeyShift     = 0x01000020;
constexpr int kQtKeyControl   = 0x01000021;
constexpr int kQtKeyMeta      = 0x01000022;
constexpr int kQtKeyAlt       = 0x01000023;
constexpr int kQtKeyCapsLock  = 0x01000024;
constexpr int kQtKeyNumLock   = 0x01000025;
constexpr int kQtKeyScrollLock= 0x01000026;
constexpr int kQtKeyMenu      = 0x01000054;

KeySym qt_key_to_keysym(int key, const std::string& text) {
    switch (key) {
        case kQtKeyEscape:    return XK_Escape;
        case kQtKeyTab:       return XK_Tab;
        case kQtKeyBackspace: return XK_BackSpace;
        case kQtKeyReturn:
        case kQtKeyEnter:     return XK_Return;
        case kQtKeyInsert:    return XK_Insert;
        case kQtKeyDelete:    return XK_Delete;
        case kQtKeyPause:     return XK_Pause;
        case kQtKeyPrint:     return XK_Print;
        case kQtKeyClear:     return XK_Clear;
        case kQtKeyHome:      return XK_Home;
        case kQtKeyEnd:       return XK_End;
        case kQtKeyLeft:      return XK_Left;
        case kQtKeyUp:        return XK_Up;
        case kQtKeyRight:     return XK_Right;
        case kQtKeyDown:      return XK_Down;
        case kQtKeyPageUp:    return XK_Page_Up;
        case kQtKeyPageDown:  return XK_Page_Down;
        case kQtKeyShift:     return XK_Shift_L;
        case kQtKeyControl:   return XK_Control_L;
        case kQtKeyMeta:      return XK_Super_L;
        case kQtKeyAlt:       return XK_Alt_L;
        case kQtKeyCapsLock:  return XK_Caps_Lock;
        case kQtKeyNumLock:   return XK_Num_Lock;
        case kQtKeyScrollLock:return XK_Scroll_Lock;
        case kQtKeyMenu:      return XK_Menu;
        default: break;
    }
    if (key >= 0x01000030 && key <= 0x0100004e) {  // F1..F31
        return XK_F1 + (key - 0x01000030);
    }
    // ASCII 区（字母/数字/常用标点/空格）：Qt::Key 即 keysym 低 8 位
    if (key >= 0x20 && key < 0x100) return static_cast<KeySym>(key);
    // 布局相关字符按文本兜底
    if (!text.empty()) {
        const KeySym sym = XStringToKeysym(text.substr(0, 1).c_str());
        if (sym != NoSymbol) return sym;
    }
    if (key > 0 && key < 0x1000) return static_cast<KeySym>(key);
    return NoSymbol;
}

class InputInjectorX11 : public InputInjector {
public:
    ~InputInjectorX11() override {
        if (dpy_) XCloseDisplay(dpy_);
    }

    bool mouse_move(int screen_index, float nx, float ny) override {
#if defined(PXC_HAVE_GNOME_RD)
        if (auto* g = gnome_for_event()) return g->mouse_move(screen_index, nx, ny);
#endif
        Display* dpy = display();
        if (!dpy) return fail("无法打开 X Display");

        int x = 0, y = 0, w = 0, h = 0;
        if (!monitor_geometry(screen_index, x, y, w, h)) return false;

        if (nx < 0) nx = 0;
        if (nx > 1) nx = 1;
        if (ny < 0) ny = 0;
        if (ny > 1) ny = 1;

        const int px = x + static_cast<int>(nx * w);
        const int py = y + static_cast<int>(ny * h);
        if (!XTestFakeMotionEvent(dpy, DefaultScreen(dpy), px, py, CurrentTime)) {
            return fail("XTestFakeMotionEvent 失败");
        }
        XFlush(dpy);
        return true;
    }

    bool mouse_button(const std::string& button, bool pressed) override {
#if defined(PXC_HAVE_GNOME_RD)
        if (auto* g = gnome_for_event()) return g->mouse_button(button, pressed);
#endif
        Display* dpy = display();
        if (!dpy) return fail("无法打开 X Display");

        unsigned int b = 0;
        if (button == "left")        b = 1;
        else if (button == "middle") b = 2;
        else if (button == "right")  b = 3;
        else return fail("未知鼠标按键: " + button);

        if (!XTestFakeButtonEvent(dpy, b, pressed ? True : False, CurrentTime)) {
            return fail("XTestFakeButtonEvent 失败");
        }
        XFlush(dpy);
        return true;
    }

    bool mouse_wheel(int dy) override {
#if defined(PXC_HAVE_GNOME_RD)
        if (auto* g = gnome_for_event()) return g->mouse_wheel(dy);
#endif
        Display* dpy = display();
        if (!dpy) return fail("无法打开 X Display");
        // 传统 wheel 按钮：4=上 5=下
        const unsigned int button = dy > 0 ? 4 : 5;
        const int times = std::min(std::abs(dy), 10);
        for (int i = 0; i < times; ++i) {
            if (!XTestFakeButtonEvent(dpy, button, True, CurrentTime) ||
                !XTestFakeButtonEvent(dpy, button, False, CurrentTime)) {
                return fail("XTestFakeButtonEvent 失败");
            }
        }
        XFlush(dpy);
        return true;
    }

    bool key_event(int qt_key, const std::string& text, bool pressed, int modifiers) override {
#if defined(PXC_HAVE_GNOME_RD)
        if (auto* g = gnome_for_event()) return g->key_event(qt_key, text, pressed, modifiers);
#endif
        (void)modifiers;
        Display* dpy = display();
        if (!dpy) return fail("无法打开 X Display");

        const KeySym sym = qt_key_to_keysym(qt_key, text);
        if (sym == NoSymbol) return fail("无法映射键码: " + std::to_string(qt_key));
        const KeyCode code = XKeysymToKeycode(dpy, sym);
        if (code == 0) return fail("当前键盘布局没有该键");

        if (!XTestFakeKeyEvent(dpy, code, pressed ? True : False, CurrentTime)) {
            return fail("XTestFakeKeyEvent 失败");
        }
        XFlush(dpy);
        return true;
    }

    std::string last_error() const override {
#if defined(PXC_HAVE_GNOME_RD)
        if (gnome_ && GnomeRemoteDesktopBroker::instance().has_session()) {
            return gnome_->last_error();
        }
#endif
        std::lock_guard<std::mutex> lock(err_mutex_);
        return error_;
    }

private:
#if defined(PXC_HAVE_GNOME_RD)
    InputInjector* gnome_for_event() {
        // 首个输入可能早于异步采集启动；会话就绪后升级，避免永久停在 XTest。
        if (!GnomeRemoteDesktopBroker::instance().has_session()) return nullptr;
        if (!gnome_) gnome_ = create_input_injector_gnome(nullptr);
        return gnome_.get();
    }
    std::unique_ptr<InputInjector> gnome_;
#endif
    Display* display() {
        if (!dpy_) dpy_ = XOpenDisplay(nullptr);
        return dpy_;
    }

    bool monitor_geometry(int screen_index, int& x, int& y, int& w, int& h) {
        std::string error;
        const auto screens = enumerate_screens(&error);
        if (screens.empty()) return fail(error.empty() ? "没有可用的屏幕" : error);
        if (screen_index < 0 || screen_index >= static_cast<int>(screens.size())) {
            return fail("屏幕 " + std::to_string(screen_index) + " 不存在");
        }
        // enumerate_screens 只有尺寸没有原点，重新查一次几何
        int events = 0, err = 0;
        if (XineramaQueryExtension(dpy_, &events, &err)) {
            int count = 0;
            XineramaScreenInfo* infos = XineramaQueryScreens(dpy_, &count);
            if (infos && count > 0 &&
                screen_index < count) {
                x = infos[screen_index].x_org;
                y = infos[screen_index].y_org;
                w = infos[screen_index].width;
                h = infos[screen_index].height;
                XFree(infos);
                return true;
            }
            if (infos) XFree(infos);
        }
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(dpy_, DefaultRootWindow(dpy_), &attrs)) {
            return fail("读取根窗口失败");
        }
        x = 0;
        y = 0;
        w = attrs.width;
        h = attrs.height;
        return true;
    }

    bool fail(const std::string& reason) {
        std::lock_guard<std::mutex> lock(err_mutex_);
        error_ = reason;
        return false;
    }

    Display* dpy_ = nullptr;
    mutable std::mutex err_mutex_;
    std::string error_;
};

}  // namespace

std::unique_ptr<InputInjector> create_input_injector(std::string* error) {
#if defined(PXC_HAVE_GNOME_RD)
    {
        // GNOME 注入能到达 Wayland 原生组件（Shell 顶栏/Dock/概览），
        // XTest 只能到达 XWayland 世界——有就绪会话时优先 GNOME。
        // 无会话（视频未启动/非 GNOME）时静默回退 X11。
        std::string gnome_error;
        auto gnome = create_input_injector_gnome(&gnome_error);
        if (gnome) return gnome;
    }
#endif
    (void)error;
    return std::make_unique<InputInjectorX11>();
}

}  // namespace pxc

#endif  // __linux__ && PXC_HAVE_X11
