// Windows 键鼠注入：统一 SendInput。
//
// 要点（与 docs/WINDOWS_TASKS.md 一致）：
//   - 绝对坐标必须用 MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK，
//     否则多显示器会错位；归一化范围是整个虚拟桌面（所有屏幕的包围盒）；
//   - 输入的 (screen_index, nx, ny) 由 enumerate_screens() 的同一份顺序
//     映射回具体显示器，两端顺序天然一致；
//   - 键码线格式是 Qt::Key（两端都是 Qt 客户端），这里映射回 Windows VK；
//     布局相关的标点用 VkKeyScanW 按当前布局兜底；
//   - 方向键/翻页/插入删除等需要 KEYEVENTF_EXTENDEDKEY。

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "pxc/input_injector.h"
#include "pxc/screen_capturer.h"

namespace pxc {
namespace {

// Qt::Key 值是 Qt 公开承诺稳定的 ABI 常量（qnamespace.h）。
// pxc_core 不链接 Qt，这里按值硬编码；新增键时对照 qnamespace.h。
constexpr int kQtKeyEscape    = 0x01000000;
constexpr int kQtKeyTab       = 0x01000001;
constexpr int kQtKeyBackspace = 0x01000003;
constexpr int kQtKeyReturn    = 0x01000004;
constexpr int kQtKeyEnter     = 0x01000005;
constexpr int kQtKeyInsert    = 0x01000006;
constexpr int kQtKeyDelete    = 0x01000007;
constexpr int kQtKeyPrint     = 0x01000009;
constexpr int kQtKeyClear     = 0x0100000b;
constexpr int kQtKeyPause     = 0x01000008;
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

WORD qt_key_to_vk(int key, const std::string& text) {
    // 功能键与方向键
    switch (key) {
        case kQtKeyEscape:    return VK_ESCAPE;
        case kQtKeyTab:       return VK_TAB;
        case kQtKeyBackspace: return VK_BACK;
        case kQtKeyReturn:
        case kQtKeyEnter:     return VK_RETURN;
        case kQtKeyInsert:    return VK_INSERT;
        case kQtKeyDelete:    return VK_DELETE;
        case kQtKeyPrint:     return VK_SNAPSHOT;
        case kQtKeyClear:     return VK_CLEAR;
        case kQtKeyPause:     return VK_PAUSE;
        case kQtKeyHome:      return VK_HOME;
        case kQtKeyEnd:       return VK_END;
        case kQtKeyLeft:      return VK_LEFT;
        case kQtKeyUp:        return VK_UP;
        case kQtKeyRight:     return VK_RIGHT;
        case kQtKeyDown:      return VK_DOWN;
        case kQtKeyPageUp:    return VK_PRIOR;
        case kQtKeyPageDown:  return VK_NEXT;
        case kQtKeyShift:     return VK_SHIFT;
        case kQtKeyControl:   return VK_CONTROL;
        case kQtKeyMeta:      return VK_LWIN;
        case kQtKeyAlt:       return VK_MENU;
        case kQtKeyCapsLock:  return VK_CAPITAL;
        case kQtKeyNumLock:   return VK_NUMLOCK;
        case kQtKeyScrollLock:return VK_SCROLL;
        case kQtKeyMenu:      return VK_APPS;
        default: break;
    }
    if (key >= 0x01000030 && key <= 0x0100004e) {  // F1..F31
        return static_cast<WORD>(VK_F1 + (key - 0x01000030));
    }

    // ASCII 区（字母/数字/常用标点/空格）：Qt::Key 与 ASCII、VK 同值
    if (key >= 0x20 && key <= 0x5a) return static_cast<WORD>(key);

    // 其余（含小键盘、布局相关标点）按文本映射到当前布局
    if (!text.empty()) {
        const wchar_t ch = static_cast<wchar_t>(text[0]);
        const SHORT vk = ::VkKeyScanW(ch);
        if (vk != -1) return static_cast<WORD>(vk & 0xff);
    }
    if (key > 0 && key < 0x100) return static_cast<WORD>(key);
    return 0;
}

bool vk_is_extended(WORD vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_INSERT: case VK_DELETE:
        case VK_RCONTROL: case VK_RMENU: case VK_APPS:
            return true;
        default:
            return false;
    }
}

struct MonitorRect {
    std::wstring device;
    RECT         rect{};
};

class InputInjectorWin : public InputInjector {
public:
    bool mouse_move(int screen_index, float nx, float ny) override {
        RECT rc{};
        if (!monitor_rect(screen_index, rc)) {
            // 屏幕信息缺失时退化为整个虚拟桌面
            rc.left   = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
            rc.top    = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
            rc.right  = rc.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
            rc.bottom = rc.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        }

        if (nx < 0) nx = 0;
        if (nx > 1) nx = 1;
        if (ny < 0) ny = 0;
        if (ny > 1) ny = 1;

        const long desk_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const long desk_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const long desk_w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const long desk_h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (desk_w <= 1 || desk_h <= 1) return fail("虚拟桌面尺寸异常");

        const long abs_x = rc.left + static_cast<long>(nx * (rc.right - rc.left));
        const long abs_y = rc.top + static_cast<long>(ny * (rc.bottom - rc.top));

        // 虚拟桌面归一化：0..65535 覆盖所有屏幕的包围盒
        const DWORD norm_x = static_cast<DWORD>((abs_x - desk_x) * 65535 / (desk_w - 1));
        const DWORD norm_y = static_cast<DWORD>((abs_y - desk_y) * 65535 / (desk_h - 1));

        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        input.mi.dx = norm_x;
        input.mi.dy = norm_y;
        return send(input);
    }

    bool mouse_button(const std::string& button, bool pressed) override {
        INPUT input{};
        input.type = INPUT_MOUSE;
        if (button == "left") {
            input.mi.dwFlags = pressed ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        } else if (button == "right") {
            input.mi.dwFlags = pressed ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        } else if (button == "middle") {
            input.mi.dwFlags = pressed ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        } else {
            return fail("未知鼠标按键: " + button);
        }
        return send(input);
    }

    bool mouse_wheel(int dy) override {
        if (dy == 0) return true;
        INPUT input{};
        input.type    = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(dy * WHEEL_DELTA);  // 正值向上
        return send(input);
    }

    bool key_event(int qt_key, const std::string& text, bool pressed, int modifiers) override {
        (void)modifiers;  // 修饰键会以独立按键事件到达，无需在此展开

        WORD vk = qt_key_to_vk(qt_key, text);
        if (vk == 0) return fail("无法映射键码: " + std::to_string(qt_key));

        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = vk;
        if (pressed && vk_is_extended(vk)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (!pressed) input.ki.dwFlags |= KEYEVENTF_KEYUP;
        return send(input);
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(err_mutex_);
        return error_;
    }

private:
    bool send(INPUT input) {
        if (::SendInput(1, &input, sizeof(INPUT)) != 1) {
            return fail("SendInput 被拒绝（可能处于安全桌面或权限不足）");
        }
        std::lock_guard<std::mutex> lock(err_mutex_);
        error_.clear();
        return true;
    }

    bool fail(const std::string& reason) {
        std::lock_guard<std::mutex> lock(err_mutex_);
        error_ = reason;
        return false;
    }

    bool monitor_rect(int screen_index, RECT& rc) {
        refresh_monitors_if_needed();
        {
            std::lock_guard<std::mutex> lock(monitors_mutex_);
            if (screen_index < 0 || screen_index >= static_cast<int>(monitors_.size())) {
                return false;
            }
            rc = monitors_[static_cast<size_t>(screen_index)].rect;
            return true;
        }
    }

    void refresh_monitors_if_needed() {
        std::lock_guard<std::mutex> lock(monitors_mutex_);
        if (!monitors_.empty()) return;

        // enumerate_screens() 决定屏幕顺序；这里只负责把它的 gdi_device
        // 映射到 GDI 显示器矩形。
        std::string error;
        const auto screens = enumerate_screens(&error);
        if (screens.empty()) return;

        std::vector<MonitorRect> gdi_monitors;
        ::EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM lparam) {
            auto* list = reinterpret_cast<std::vector<MonitorRect>*>(lparam);
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (::GetMonitorInfoW(monitor, &mi)) {
                list->push_back({mi.szDevice, mi.rcMonitor});
            } else {
                list->push_back({L"", mi.rcMonitor});
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&gdi_monitors));

        for (const auto& screen : screens) {
            const std::wstring want(screen.gdi_device.begin(), screen.gdi_device.end());
            bool matched = false;
            for (const auto& mon : gdi_monitors) {
                if (mon.device == want) {
                    monitors_.push_back(mon);
                    matched = true;
                    break;
                }
            }
            if (!matched && !gdi_monitors.empty()) {
                // 名称对不上（罕见的驱动差异）时按顺序兜底
                monitors_.push_back(
                    gdi_monitors[std::min<size_t>(monitors_.size(),
                                                  gdi_monitors.size() - 1)]);
            }
        }
    }

    std::mutex               monitors_mutex_;
    std::vector<MonitorRect> monitors_;
    mutable std::mutex       err_mutex_;
    std::string              error_;
};

}  // namespace

std::unique_ptr<InputInjector> create_input_injector(std::string* error) {
    (void)error;
    return std::make_unique<InputInjectorWin>();
}

}  // namespace pxc

#endif  // _WIN32
