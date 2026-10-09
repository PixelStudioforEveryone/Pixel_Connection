// Linux 键鼠注入：GNOME (mutter) RemoteDesktop D-Bus（Wayland 原生）。
//
// 与 input_injector_x11.cpp 的关系：XTest 只能注入 XWayland 世界，
// GNOME Shell 顶栏/Dock/概览不可达；本实现经 mutter 的 clutter 虚拟
// 设备注入，能到达整个桌面。工厂优先选择本实现，无 RD 会话时回退
// XTest。
//
// 与采集器（screen_capturer_pipewire.cpp）的接线：
//   采集器在 SC CreateSession 前 acquire_session_id()，把返回值作为
//   "remote-desktop-session-id" 选项传入建立关联；RecordArea 得到流
//   路径、格式协商得到流尺寸后推给本代理；注入用这两个信息发绝对坐标。
//   采集器重建（切屏/恢复）时重复以上流程。
//
// 运行要求：GUI 会话的 D-Bus 且 mutter 拥有 org.gnome.Mutter.RemoteDesktop
// 名字（GNOME 会话即满足）。

#if defined(__linux__) && defined(PXC_HAVE_GNOME_RD)

extern "C" {
#include <dbus/dbus.h>
}

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <functional>
#include <linux/input-event-codes.h>
#include <mutex>
#include <string>

#include "gnome_remote_desktop.h"
#include "pxc/input_injector.h"

namespace pxc {
namespace {

constexpr const char* kRdService      = "org.gnome.Mutter.RemoteDesktop";
constexpr const char* kRdRootPath     = "/org/gnome/Mutter/RemoteDesktop";
constexpr const char* kRdIface        = "org.gnome.Mutter.RemoteDesktop";
constexpr const char* kRdSessionIface = "org.gnome.Mutter.RemoteDesktop.Session";
constexpr const char* kPropsIface     = "org.freedesktop.DBus.Properties";

// 与采集器的 [pw] 同款：带毫秒时间戳写 stderr，便于与 PipeWire 日志对时。
void rd_log(const char* fmt, ...) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[rd %s.%03d] ", stamp, static_cast<int>(ms));
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// Qt::Key → X keysym。与 input_injector_x11.cpp 同一张表，
// 值直接写出（不依赖 X11 头文件）。
uint32_t qt_key_to_keysym(int key, const std::string& text) {
    switch (key) {
        case 0x01000000: return 0xff1b;  // Escape
        case 0x01000001: return 0xff09;  // Tab
        case 0x01000003: return 0xff08;  // Backspace
        case 0x01000004:                          // Return
        case 0x01000005: return 0xff0d;           // Enter
        case 0x01000006: return 0xff63;  // Insert
        case 0x01000007: return 0xffff;  // Delete
        case 0x01000008: return 0xff13;  // Pause
        case 0x01000009: return 0xff61;  // Print
        case 0x0100000b: return 0xff0b;  // Clear
        case 0x01000010: return 0xff50;  // Home
        case 0x01000011: return 0xff57;  // End
        case 0x01000012: return 0xff51;  // Left
        case 0x01000013: return 0xff52;  // Up
        case 0x01000014: return 0xff53;  // Right
        case 0x01000015: return 0xff54;  // Down
        case 0x01000016: return 0xff55;  // PageUp
        case 0x01000017: return 0xff56;  // PageDown
        case 0x01000020: return 0xffe1;  // Shift_L
        case 0x01000021: return 0xffe3;  // Control_L
        case 0x01000022: return 0xffeb;  // Super_L（Qt::Key_Meta）
        case 0x01000023: return 0xffe9;  // Alt_L
        case 0x01000024: return 0xffe5;  // Caps_Lock
        case 0x01000025: return 0xff7f;  // Num_Lock
        case 0x01000026: return 0xff14;  // Scroll_Lock
        case 0x01000054: return 0xff67;  // Menu
        default: break;
    }
    if (key >= 0x01000030 && key <= 0x0100004e) {  // F1..F31
        return 0xffbe + static_cast<uint32_t>(key - 0x01000030);
    }
    // Qt::Key_A..Z 不区分大小写，Shift 已由独立键事件传递。
    // Mutter 的 NotifyKeyboardKeysym 会给大写 keysym 自动补 Shift；
    // 直接发送 Qt::Key_C 会把 Ctrl+C 变成 Ctrl+Shift+C，并在 C 松开时
    // 干扰用户仍按住的 Shift。字母使用基础层小写 keysym。
    if (key >= 'A' && key <= 'Z') return static_cast<uint32_t>(key + ('a' - 'A'));
    // 其余 ASCII/Latin-1 区：Qt::Key 即 keysym 低 8 位
    if (key >= 0x20 && key < 0x100) return static_cast<uint32_t>(key);
    // Qt 的 Unicode 键与 X keysym 的 Unicode 映射同构（0x01000000|码点）
    if (key >= 0x01010000) return static_cast<uint32_t>(key);
    // 布局相关字符按文本兜底（单字节 ASCII 可靠）
    if (!text.empty()) {
        const unsigned char c = static_cast<unsigned char>(text[0]);
        if (c >= 0x20 && c < 0x7f) return c;
    }
    if (key > 0 && key < 0x1000) return static_cast<uint32_t>(key);
    return 0;
}

}  // namespace

// ------------------------------------------------------------------ 代理实现

struct GnomeRemoteDesktopBroker::State {
    DBusConnection* conn = nullptr;  // 专属连接，关闭即可清理未启动的会话
    std::string session_path;        // RD 会话对象路径
    std::string session_id;          // SessionId 属性（SC 关联用）
    std::string stream_path;         // SC 流对象路径（绝对坐标目标）
    uint32_t stream_width  = 0;      // 流像素尺寸（格式协商结果）
    uint32_t stream_height = 0;
    bool started = false;
    std::string last_error;
    mutable std::mutex mutex;

    bool ensure_connection();
    void close_session();  // 调用方持有 mutex
    bool fail(const std::string& error);
    DBusMessage* call(const char* path, const char* iface, const char* method,
                      const std::function<void(DBusMessageIter*)>& fill,
                      int timeout_ms, std::string* error);

    std::string acquire_session_id();
    bool start_session();
    void set_stream_path(const std::string& path);
    void set_stream_size(uint32_t width, uint32_t height);
    void on_screencast_stopped(const std::string& owner_session_id);
    bool notify_pointer_move(float nx, float ny);
    bool notify_button(int button, bool pressed);
    bool notify_wheel(int steps);
    bool notify_keysym(uint32_t keysym, bool pressed);
};

bool GnomeRemoteDesktopBroker::State::ensure_connection() {
    if (conn) return true;
    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fail(std::string("无法连接会话 D-Bus: ") +
             (dbus_error_is_set(&err) ? err.message : "未知错误"));
        dbus_error_free(&err);
        return false;
    }
    dbus_error_free(&err);
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    return true;
}

void GnomeRemoteDesktopBroker::State::close_session() {
    if (started && !session_path.empty()) {
        std::string error;
        DBusMessage* reply = call(session_path.c_str(), kRdSessionIface,
                                  "Stop", nullptr, 2000, &error);
        if (reply) dbus_message_unref(reply);
    }
    // mutter 42 不允许 Stop 未启动的 RD 会话；释放专属连接使其自动关闭。
    if (conn) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        conn = nullptr;
    }
    started = false;
    session_path.clear();
    session_id.clear();
    stream_path.clear();
    stream_width = stream_height = 0;
}

bool GnomeRemoteDesktopBroker::State::fail(const std::string& error) {
    // 同一错误只记一次，避免鼠标事件风暴刷爆日志
    if (last_error != error) {
        rd_log("注入错误: %s", error.c_str());
        last_error = error;
    }
    return false;
}

DBusMessage* GnomeRemoteDesktopBroker::State::call(
    const char* path, const char* iface, const char* method,
    const std::function<void(DBusMessageIter*)>& fill, int timeout_ms,
    std::string* error) {
    if (!conn) {
        if (error) *error = "D-Bus 连接不可用";
        return nullptr;
    }
    DBusMessage* msg =
        dbus_message_new_method_call(kRdService, path, iface, method);
    if (!msg) {
        if (error) *error = "dbus_message_new_method_call 失败";
        return nullptr;
    }
    if (fill) {
        DBusMessageIter it;
        dbus_message_iter_init_append(msg, &it);
        fill(&it);
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, timeout_ms, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (error) {
            *error = std::string(method) + " 失败: " +
                     (dbus_error_is_set(&err) ? err.message : "无响应");
        }
        dbus_error_free(&err);
        return nullptr;
    }
    return reply;
}

std::string GnomeRemoteDesktopBroker::State::acquire_session_id() {
    std::lock_guard<std::mutex> lock(mutex);

    close_session();

    if (!ensure_connection()) return {};

    // 服务可用性：非 GNOME 会话没有 org.gnome.Mutter.RemoteDesktop
    DBusError err;
    dbus_error_init(&err);
    const dbus_bool_t owned = dbus_bus_name_has_owner(conn, kRdService, &err);
    const bool name_error   = dbus_error_is_set(&err) != FALSE;
    char name_error_text[256] = {0};
    if (name_error && err.message) {
        std::snprintf(name_error_text, sizeof(name_error_text), "%s", err.message);
    }
    dbus_error_free(&err);
    if (!owned) {
        fail(name_error
                 ? std::string("查询服务失败: ") + name_error_text
                 : "org.gnome.Mutter.RemoteDesktop 不可用（非 GNOME 会话？）");
        return {};
    }

    // 1) CreateSession → 对象路径
    std::string error;
    DBusMessage* reply =
        call(kRdRootPath, kRdIface, "CreateSession", nullptr, 5000, &error);
    if (!reply) {
        fail(error);
        return {};
    }
    {
        const char* path = nullptr;
        if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_OBJECT_PATH, &path,
                                  DBUS_TYPE_INVALID) &&
            path) {
            session_path = path;
        }
        dbus_message_unref(reply);
    }
    if (session_path.empty()) {
        fail("RemoteDesktop CreateSession 返回无效");
        return {};
    }

    // 2) 读 SessionId 属性（SC 关联时用它，不是对象路径）
    reply = call(session_path.c_str(), kPropsIface, "Get",
                 [&](DBusMessageIter* it) {
                     const char* iface = kRdSessionIface;
                     const char* prop  = "SessionId";
                     dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &iface);
                     dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &prop);
                 },
                 5000, &error);
    if (!reply) {
        fail(error);
        return {};
    }
    {
        DBusMessageIter it, v;
        if (dbus_message_iter_init(reply, &it) &&
            dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&it, &v);
            if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
                const char* sid = nullptr;
                dbus_message_iter_get_basic(&v, &sid);
                if (sid) session_id = sid;
            }
        }
        dbus_message_unref(reply);
    }
    if (session_id.empty()) {
        fail("读取 SessionId 属性失败");
        return {};
    }

    // 先关联 SC 并 RecordArea，再由 RD Start 启动两者。
    rd_log("RD 会话已创建: %s（SessionId %zu 字节）", session_path.c_str(),
           session_id.size());
    last_error.clear();
    return session_id;
}

bool GnomeRemoteDesktopBroker::State::start_session() {
    std::lock_guard<std::mutex> lock(mutex);
    if (session_path.empty() || session_id.empty()) return fail("RD 会话不存在");
    if (started) return true;
    if (stream_path.empty()) return fail("ScreenCast 流未就绪");
    std::string error;
    DBusMessage* reply = call(session_path.c_str(), kRdSessionIface,
                              "Start", nullptr, 5000, &error);
    if (!reply) return fail(error);
    dbus_message_unref(reply);
    started = true;
    last_error.clear();
    rd_log("RD 会话已启动（连带启动 SC）");
    return true;
}

void GnomeRemoteDesktopBroker::State::set_stream_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stream_path == path) return;
    stream_path = path;
    rd_log("SC 流路径: %s", path.c_str());
}

void GnomeRemoteDesktopBroker::State::set_stream_size(uint32_t width,
                                                      uint32_t height) {
    std::lock_guard<std::mutex> lock(mutex);
    if (stream_width == width && stream_height == height) return;
    stream_width  = width;
    stream_height = height;
    rd_log("SC 流尺寸: %ux%u", width, height);
}

void GnomeRemoteDesktopBroker::State::on_screencast_stopped(
    const std::string& owner_session_id) {
    std::lock_guard<std::mutex> lock(mutex);
    // 替换采集器时旧对象可能晚于新会话析构，不能关闭新会话。
    if (owner_session_id != session_id) return;
    close_session();
}

bool GnomeRemoteDesktopBroker::State::notify_pointer_move(float nx, float ny) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!started) return fail("RD 会话未启动");
    if (stream_path.empty())
        return fail("ScreenCast 流未就绪");
    if (stream_width == 0 || stream_height == 0)
        return fail("流尺寸未知（格式未协商）");

    // mutter Area 流坐标 = 流像素（area.x + round(px/scale)），
    // 流像素尺寸即协商出的帧尺寸，归一化 × 帧尺寸即正确落点
    const double x = static_cast<double>(nx) * stream_width;
    const double y = static_cast<double>(ny) * stream_height;
    std::string error;
    DBusMessage* reply = call(
        session_path.c_str(), kRdSessionIface, "NotifyPointerMotionAbsolute",
        [&](DBusMessageIter* it) {
            const char* sp = stream_path.c_str();
            dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &sp);
            dbus_message_iter_append_basic(it, DBUS_TYPE_DOUBLE, &x);
            dbus_message_iter_append_basic(it, DBUS_TYPE_DOUBLE, &y);
        },
        2000, &error);
    if (!reply) return fail(error);
    dbus_message_unref(reply);
    last_error.clear();
    return true;
}

bool GnomeRemoteDesktopBroker::State::notify_button(int button, bool pressed) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!started) return fail("RD 会话未启动");
    if (stream_path.empty()) return fail("ScreenCast 流未就绪");

    // 协议使用 X11 风格的 1/2/3；mutter 接口要求 evdev BTN_*。
    dbus_int32_t b = 0;
    switch (button) {
        case 1: b = BTN_LEFT; break;
        case 2: b = BTN_MIDDLE; break;
        case 3: b = BTN_RIGHT; break;
        default: return fail("未知鼠标按钮");
    }
    const dbus_bool_t st = pressed ? TRUE : FALSE;
    std::string error;
    DBusMessage* reply = call(
        session_path.c_str(), kRdSessionIface, "NotifyPointerButton",
        [&](DBusMessageIter* it) {
            dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &b);
            dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &st);
        },
        2000, &error);
    if (!reply) return fail(error);
    dbus_message_unref(reply);
    last_error.clear();
    return true;
}

bool GnomeRemoteDesktopBroker::State::notify_wheel(int steps) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!started) return fail("RD 会话未启动");

    // mutter 42: axis 0 = 垂直，steps<0 = 向上；本协议 steps>0 = 向上
    const uint32_t axis = 0;
    const int clamped   = std::max(-10, std::min(10, steps));
    if (clamped == 0) return true;
    const dbus_int32_t msteps = static_cast<dbus_int32_t>(-clamped);
    std::string error;
    DBusMessage* reply = call(
        session_path.c_str(), kRdSessionIface, "NotifyPointerAxisDiscrete",
        [&](DBusMessageIter* it) {
            dbus_message_iter_append_basic(it, DBUS_TYPE_UINT32, &axis);
            dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &msteps);
        },
        2000, &error);
    if (!reply) return fail(error);
    dbus_message_unref(reply);
    last_error.clear();
    return true;
}

bool GnomeRemoteDesktopBroker::State::notify_keysym(uint32_t keysym,
                                                    bool pressed) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!started) return fail("RD 会话未启动");

    const uint32_t ks = keysym;
    const dbus_bool_t st = pressed ? TRUE : FALSE;
    std::string error;
    DBusMessage* reply = call(
        session_path.c_str(), kRdSessionIface, "NotifyKeyboardKeysym",
        [&](DBusMessageIter* it) {
            dbus_message_iter_append_basic(it, DBUS_TYPE_UINT32, &ks);
            dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN, &st);
        },
        2000, &error);
    if (!reply) return fail(error);
    dbus_message_unref(reply);
    last_error.clear();
    return true;
}

// ------------------------------------------------------------ 对外薄封装

GnomeRemoteDesktopBroker& GnomeRemoteDesktopBroker::instance() {
    static GnomeRemoteDesktopBroker broker;
    return broker;
}

GnomeRemoteDesktopBroker::GnomeRemoteDesktopBroker() : s_(new State) {}

GnomeRemoteDesktopBroker::~GnomeRemoteDesktopBroker() {
    if (s_) s_->close_session();
    delete s_;
}

std::string GnomeRemoteDesktopBroker::acquire_session_id() {
    return s_->acquire_session_id();
}
bool GnomeRemoteDesktopBroker::start_session() {
    return s_->start_session();
}
void GnomeRemoteDesktopBroker::set_stream_path(const std::string& path) {
    s_->set_stream_path(path);
}
void GnomeRemoteDesktopBroker::set_stream_size(uint32_t width, uint32_t height) {
    s_->set_stream_size(width, height);
}
void GnomeRemoteDesktopBroker::on_screencast_stopped(const std::string& session_id) {
    s_->on_screencast_stopped(session_id);
}
bool GnomeRemoteDesktopBroker::has_session() const {
    std::lock_guard<std::mutex> lock(s_->mutex);
    return s_->started && !s_->session_path.empty();
}
bool GnomeRemoteDesktopBroker::notify_pointer_move(float nx, float ny) {
    return s_->notify_pointer_move(nx, ny);
}
bool GnomeRemoteDesktopBroker::notify_button(int button, bool pressed) {
    return s_->notify_button(button, pressed);
}
bool GnomeRemoteDesktopBroker::notify_wheel(int steps) {
    return s_->notify_wheel(steps);
}
bool GnomeRemoteDesktopBroker::notify_keysym(uint32_t keysym, bool pressed) {
    return s_->notify_keysym(keysym, pressed);
}
std::string GnomeRemoteDesktopBroker::last_error() const {
    std::lock_guard<std::mutex> lock(s_->mutex);
    return s_->last_error;
}

// ------------------------------------------------------------------ 注入器

namespace {

class InputInjectorGnome : public InputInjector {
public:
    bool mouse_move(int screen_index, float nx, float ny) override {
        // 流始终对应「当前正在录制的屏幕」，归一化坐标直接用；
        // screen_index 无需参与映射。
        (void)screen_index;
        return GnomeRemoteDesktopBroker::instance().notify_pointer_move(nx, ny);
    }

    bool mouse_button(const std::string& button, bool pressed) override {
        int b = 0;
        if (button == "left")        b = 1;
        else if (button == "middle") b = 2;
        else if (button == "right")  b = 3;
        else return false;
        return GnomeRemoteDesktopBroker::instance().notify_button(b, pressed);
    }

    bool mouse_wheel(int dy) override {
        if (dy == 0) return true;
        return GnomeRemoteDesktopBroker::instance().notify_wheel(dy);
    }

    bool key_event(int qt_key, const std::string& text, bool pressed,
                   int modifiers) override {
        (void)modifiers;
        const uint32_t keysym = qt_key_to_keysym(qt_key, text);
        if (keysym == 0) return false;
        return GnomeRemoteDesktopBroker::instance().notify_keysym(keysym,
                                                                  pressed);
    }

    std::string last_error() const override {
        return GnomeRemoteDesktopBroker::instance().last_error();
    }
};

}  // namespace

std::unique_ptr<InputInjector> create_input_injector_gnome(std::string* error) {
    auto& broker = GnomeRemoteDesktopBroker::instance();
    if (!broker.has_session()) {
        if (error) {
            *error = "GNOME RemoteDesktop 会话未就绪（屏幕录制未启动或非 GNOME 会话）";
        }
        return nullptr;
    }
    return std::make_unique<InputInjectorGnome>();
}

#if !defined(PXC_HAVE_X11)
// 没有 X11 的环境：GNOME 注入器兼任注入工厂
std::unique_ptr<InputInjector> create_input_injector(std::string* error) {
    auto injector = create_input_injector_gnome(error);
    if (injector) return injector;
    if (error && error->empty()) {
        *error = "无可用键鼠注入实现（缺 X11 且无 GNOME 会话）";
    }
    return nullptr;
}
#endif  // !PXC_HAVE_X11

}  // namespace pxc

#endif  // __linux__ && PXC_HAVE_GNOME_RD
