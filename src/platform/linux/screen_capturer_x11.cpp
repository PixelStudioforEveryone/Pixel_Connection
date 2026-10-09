// Linux 屏幕采集：X11（Xinerama 多屏 + XGetImage）。
//
// 这是 Linux 侧的可用基线：不依赖 XShm/Damage，任何带 X 的环境都能跑。
// Wayland 会话下 XGetImage 通常拿不到其他窗口的内容，
// 后续应补 PipeWire/Desktop portal 实现，接口不变。
//
// 输出统一 BGRA（X11 ZPixmap 32bpp 在小端机器上内存布局即 B,G,R,A；
// 大端机器做字节序交换，虽然罕见但要正确）。

#if defined(__linux__) && defined(PXC_HAVE_X11)

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "pxc/screen_capturer.h"

namespace pxc {
namespace {

struct DisplayGuard {
    Display* dpy = nullptr;
    DisplayGuard() { dpy = XOpenDisplay(nullptr); }
    ~DisplayGuard() {
        if (dpy) XCloseDisplay(dpy);
    }
};

class ScreenCapturerX11 : public ScreenCapturer {
public:
    ~ScreenCapturerX11() override { stop(); }

    bool start(int output_index) override {
        stop();
        dpy_ = XOpenDisplay(nullptr);
        if (!dpy_) return fail("无法打开 X Display（是否在 Wayland 会话？）");

        if (!query_monitors()) return false;
        if (output_index < 0 || output_index >= static_cast<int>(monitors_.size())) {
            return fail("屏幕 " + std::to_string(output_index) + " 不存在");
        }
        current_ = output_index;
        started_ = true;
        return true;
    }

    void stop() override {
        if (dpy_) {
            XCloseDisplay(dpy_);
            dpy_ = nullptr;
        }
        started_ = false;
    }

    bool capture(RawFrame& out, std::chrono::milliseconds timeout) override {
        (void)timeout;  // XGetImage 是同步调用，无需超时
        if (!started_ || !dpy_) return fail("采集器未启动");

        const auto& mon = monitors_[static_cast<size_t>(current_)];
        if (!mon.x11_screen) return fail("采集器内部状态错误");

        // XGetImage 在 XWayland/Wayland 会话下会触发 BadMatch，
        // Xlib 默认错误处理会直接终止进程——必须用错误陷阱兜住，
        // 把失败转成普通返回值。
        x_error_flag_ = false;
        XSetErrorHandler(&ScreenCapturerX11::error_trap);

        XImage* image = XGetImage(dpy_, root_, mon.x, mon.y,
                                  static_cast<unsigned int>(mon.width),
                                  static_cast<unsigned int>(mon.height),
                                  AllPlanes, ZPixmap);

        XSetErrorHandler(nullptr);

        if (!image) {
            if (x_error_flag_) {
                return fail("X11 XGetImage 被拒绝（Wayland 会话下无法抓取桌面；"
                            "请使用 Xorg 会话或等待 PipeWire 支持）");
            }
            return fail("XGetImage 失败");
        }

        const uint32_t w = static_cast<uint32_t>(image->width);
        const uint32_t h = static_cast<uint32_t>(image->height);
        const size_t   stride = static_cast<size_t>(image->bytes_per_line);

        auto buffer = std::make_shared<std::vector<uint8_t>>();
        buffer->resize(stride * h);
        std::memcpy(buffer->data(), image->data, buffer->size());
        XDestroyImage(image);

        // 32bpp ZPixmap：小端机器已是 BGRA；大端机器需要交换 R/B
        if (image_byte_order_ == MSBFirst) {
            for (size_t i = 0; i + 3 < buffer->size(); i += 4) {
                std::swap((*buffer)[i], (*buffer)[i + 2]);
            }
        }

        RawFrame frame;
        frame.frame_id     = ++frame_id_;
        frame.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        frame.width  = w;
        frame.height = h;
        frame.stride = static_cast<uint32_t>(stride);
        frame.bgra   = std::move(buffer);
        out = std::move(frame);
        return true;
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(err_mutex_);
        return error_;
    }

private:
    bool query_monitors() {
        root_ = DefaultRootWindow(dpy_);
        image_byte_order_ = ImageByteOrder(dpy_);

        int events = 0, error = 0;
        if (XineramaQueryExtension(dpy_, &events, &error)) {
            int count = 0;
            XineramaScreenInfo* infos = XineramaQueryScreens(dpy_, &count);
            if (infos && count > 0) {
                for (int i = 0; i < count; ++i) {
                    Monitor mon;
                    mon.x      = infos[i].x_org;
                    mon.y      = infos[i].y_org;
                    mon.width  = infos[i].width;
                    mon.height = infos[i].height;
                    mon.primary = (infos[i].screen_number == 0);
                    monitors_.push_back(mon);
                }
                XFree(infos);
            }
        }
        if (monitors_.empty()) {
            XWindowAttributes attrs{};
            if (!XGetWindowAttributes(dpy_, root_, &attrs)) return fail("读取根窗口失败");
            Monitor mon;
            mon.x = 0;
            mon.y = 0;
            mon.width = attrs.width;
            mon.height = attrs.height;
            mon.primary = true;
            monitors_.push_back(mon);
        }
        return true;
    }

    bool fail(const std::string& reason) {
        std::lock_guard<std::mutex> lock(err_mutex_);
        error_ = reason;
        return false;
    }

    // Xlib 错误陷阱：只置标志，不终止进程
    static int error_trap(Display*, XErrorEvent*) {
        x_error_flag_ = true;
        return 0;
    }

    static bool x_error_flag_;

    struct Monitor {
        int  x = 0, y = 0, width = 0, height = 0;
        bool primary = false;
        bool x11_screen = true;
    };

    Display* dpy_    = nullptr;
    Window   root_   = 0;
    int      image_byte_order_ = LSBFirst;
    std::vector<Monitor> monitors_;
    int      current_ = 0;
    bool     started_ = false;
    uint64_t frame_id_ = 0;
    mutable std::mutex err_mutex_;
    std::string error_;
};

bool ScreenCapturerX11::x_error_flag_ = false;

}  // namespace

// 供 Linux 工厂调度的 X11 实现入口（screen_capturer_linux.cpp 统一分发：
// PipeWire 可用时优先用 Mutter ScreenCast，这里作为 Xorg 会话兜底）。
std::vector<ScreenInfo> enumerate_screens_x11(std::string* error) {
    std::vector<ScreenInfo> screens;
    DisplayGuard guard;
    if (!guard.dpy) {
        if (error) *error = "无法打开 X Display";
        return screens;
    }

    std::vector<ScreenInfo> found;
    int events = 0, err = 0;
    if (XineramaQueryExtension(guard.dpy, &events, &err)) {
        int count = 0;
        XineramaScreenInfo* infos = XineramaQueryScreens(guard.dpy, &count);
        if (infos && count > 0) {
            for (int i = 0; i < count; ++i) {
                ScreenInfo info;
                info.index   = i;
                info.name    = "Screen " + std::to_string(i + 1);
                info.x      = static_cast<uint32_t>(infos[i].x_org);
                info.y      = static_cast<uint32_t>(infos[i].y_org);
                info.width   = static_cast<uint32_t>(infos[i].width);
                info.height  = static_cast<uint32_t>(infos[i].height);
                info.primary = (infos[i].screen_number == 0);
                found.push_back(info);
            }
        }
        if (infos) XFree(infos);
    }
    if (found.empty()) {
        XWindowAttributes attrs{};
        if (XGetWindowAttributes(guard.dpy, DefaultRootWindow(guard.dpy), &attrs)) {
            ScreenInfo info;
            info.index = 0;
            info.name = "Screen 1";
            info.width = static_cast<uint32_t>(attrs.width);
            info.height = static_cast<uint32_t>(attrs.height);
            info.primary = true;
            found.push_back(info);
        }
    }
    if (found.empty() && error) *error = "没有可采集的屏幕";
    return found;
}

std::unique_ptr<ScreenCapturer> create_screen_capturer_x11(std::string* error) {
    (void)error;
    return std::make_unique<ScreenCapturerX11>();
}

}  // namespace pxc

#endif  // __linux__ && PXC_HAVE_X11
