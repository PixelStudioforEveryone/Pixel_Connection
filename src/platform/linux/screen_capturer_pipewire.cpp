// Linux Wayland 屏幕采集：GNOME Mutter ScreenCast D-Bus + PipeWire。
//
// 为什么不用 xdg-desktop-portal：portal 的 SelectSources 会弹出交互式
// 显示器选择对话框，无人值守的被控端没人点。Mutter 的 D-Bus 接口
// （org.gnome.Mutter.ScreenCast，gnome-remote-desktop 同源）不弹对话框，
// 但 mutter 42 不通过 D-Bus 暴露 PipeWire 节点 id（属性/信号是 44+ 才加的）。
//
// 本实现的节点发现方式与版本无关：
//   1. 先连 PipeWire 注册表，记录「既有 Video/Source 节点」集合；
//   2. 再走 D-Bus CreateSession -> RecordArea(全屏) -> Start；
//   3. 注册表出现「新的 Video/Source 节点」即为 mutter 的生产者节点，
//      把消费者流连接到它。
//
// BGRx 与 BGRA 字节布局相同（[B,G,R,x]），帧数据零转换直接拷贝。
// 会话结束/析构时调用 Session.Stop() 并销毁 PipeWire 流。
//
// 运行要求：GUI 会话的 D-Bus（DBUS_SESSION_BUS_ADDRESS 可用）。

#if defined(__linux__) && defined(PXC_HAVE_PIPEWIRE)
#include "pxc/bgra_frame_copy.h"

extern "C" {
#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>
}

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "pxc/screen_capturer.h"
#include "screen_capturer_x11.h"
#include "gnome_remote_desktop.h"

namespace pxc {
namespace {

// PipeWire/D-Bus 事件日志：带时间戳写到 stderr（客户端的 stderr 由
// 启动脚本重定向到日志文件）。采集器位于 pxc 库层，不依赖 Qt；
// 采集停滞排查需要 pw_stream 状态变迁的精确时刻。
// 命名避开 PipeWire 自己的 pw_log 符号。
void pxc_pw_log(const char* fmt, ...) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(
                        now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[pw %s.%03d] ", stamp, static_cast<int>(ms));
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// ---------------------------------------------------------------- D-Bus 封装

class DbusConnection {
public:
    DbusConnection() {
        DBusError err;
        dbus_error_init(&err);
        conn_ = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
        owns_ = conn_ != nullptr;
        if (!conn_) {
            error_ = std::string("无法连接会话 D-Bus: ") +
                     (dbus_error_is_set(&err) ? err.message : "未知错误");
            dbus_error_free(&err);
        }
        if (conn_) dbus_connection_set_exit_on_disconnect(conn_, FALSE);
    }
    ~DbusConnection() {
        if (owns_ && conn_) {
            dbus_connection_close(conn_);
            dbus_connection_unref(conn_);
        }
    }
    DbusConnection(const DbusConnection&)            = delete;
    DbusConnection& operator=(const DbusConnection&) = delete;

    bool ok() const { return conn_ != nullptr; }
    const std::string& error() const { return error_; }

    DBusMessage* call(const std::string& service, const std::string& path,
                      const std::string& iface, const std::string& method,
                      const std::function<void(DBusMessage*)>& fill_args,
                      int timeout_ms) {
        DBusMessage* msg = dbus_message_new_method_call(
            service.c_str(), path.c_str(), iface.c_str(), method.c_str());
        if (!msg) {
            error_ = "dbus_message_new_method_call 失败";
            return nullptr;
        }
        if (fill_args) fill_args(msg);

        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            conn_, msg, timeout_ms, &err);
        dbus_message_unref(msg);
        if (!reply) {
            error_ = std::string(method) + " 失败: " +
                     (dbus_error_is_set(&err) ? err.message : "无响应");
            dbus_error_free(&err);
            return nullptr;
        }
        return reply;
    }

private:
    DBusConnection* conn_ = nullptr;
    bool            owns_ = false;
    std::string     error_;
};

void append_dict_u32(DBusMessageIter* dict, const char* key, uint32_t v) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    const char* sig = DBUS_TYPE_UINT32_AS_STRING;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &v);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void append_dict_bool(DBusMessageIter* dict, const char* key, bool v) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    const char* sig = DBUS_TYPE_BOOLEAN_AS_STRING;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
    dbus_bool_t value = v ? TRUE : FALSE;
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

void append_dict_string(DBusMessageIter* dict, const char* key, const char* v) {
    DBusMessageIter entry, variant;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    const char* sig = DBUS_TYPE_STRING_AS_STRING;
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &variant);
    dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &v);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

std::string read_object_path(DBusMessage* reply) {
    const char* path = nullptr;
    if (!dbus_message_get_args(reply, nullptr, DBUS_TYPE_OBJECT_PATH, &path,
                               DBUS_TYPE_INVALID)) {
        return {};
    }
    return path ? path : std::string();
}

// ------------------------------------------------------------------ 采集器

class ScreenCapturerPipeWire : public ScreenCapturer {
public:
    ~ScreenCapturerPipeWire() override { stop(); }

    bool start(int output_index) override {
        // 逻辑桌面尺寸：XWayland 的根窗口始终等于 GNOME 的逻辑桌面布局
        std::string x11_error;
        auto screens = enumerate_screens_x11(&x11_error);
        if (screens.empty()) {
            return fail("无法从 XWayland 获取桌面尺寸: " + x11_error);
        }
        if (output_index < 0 || output_index >= static_cast<int>(screens.size())) {
            return fail("屏幕 " + std::to_string(output_index) + " 不存在");
        }
        screen_ = screens[static_cast<size_t>(output_index)];

        // 整体重试：会话快速重建时偶发连上垂死节点或在权限/链接
        // 就绪前发起连接（invalid message id / 永久 paused）。一次全新
        // 的 D-Bus 会话 + PipeWire 连接即可恢复。假成功比失败更糟，
        // 失败让上层重建。
        std::string last_err;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            stop();
            if (attempt > 1) {
                pxc_pw_log("start retry %d after: %s", attempt, last_err.c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }

            // ---- 1. PipeWire 图：loop/context/core + 注册表监听 ----
            if (!init_pipewire_graph()) {
                last_err = error_;
                continue;
            }

            // ---- 2. 先武装节点监听（必须在 D-Bus 之前，防竞态）----
            {
                std::lock_guard<std::mutex> lock(graph_mutex_);
                recording_started_ = true;
            }

            // ---- 3. D-Bus：CreateSession -> RecordArea -> Start ----
            if (!start_mutter_session()) {
                last_err = error_;
                continue;
            }

            // ---- 4. 确定生产者节点并连接 ----
            // 优先用 Start 回复给出的节点 id（无竞态）；旧版 mutter 没有
            // 回复载荷时回退到注册表扫描（回放已冲刷，扫到的一定是新节点）
            uint32_t node_id = session_node_id_;
            if (node_id == 0) {
                // 扫描 + 存活确认：垂死节点会在几百毫秒内收到 remove
                // 事件（registry_global_remove 清掉 new_node_id_）。
                // 等 300ms 仍存活的节点才可信。
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(12);
                while (std::chrono::steady_clock::now() < deadline) {
                    node_id = wait_new_source_node(2000);
                    if (node_id == 0) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    {
                        std::lock_guard<std::mutex> lock(graph_mutex_);
                        if (new_node_id_ == node_id) break;
                    }
                    // 期间被 remove：继续等下一个新节点
                    node_id = 0;
                }
                if (node_id == 0) {
                    std::lock_guard<std::mutex> lock(graph_mutex_);
                    last_err = "PipeWire 注册表未出现可用的存活新节点（已见事件 " +
                               std::to_string(registry_count_) + "，既有节点 " +
                               std::to_string(existing_nodes_.size()) +
                               "；确认会话为 GNOME 且 DBUS_SESSION_BUS_ADDRESS 可用）";
                    fail(last_err);
                    continue;
                }
            }

            // ---- 5. 消费者流连接到该节点 ----
            if (!connect_stream(node_id)) {
                last_err = error_;
                continue;
            }

            // 成功路径必须清掉重试过程中留下的错误，否则
            // last_error() 残留会让上层（video_sender）误判致命错误
            {
                std::lock_guard<std::mutex> lock(err_mutex_);
                error_.clear();
            }
            session_path_set_ = true;
            current_ = output_index;
            started_ = true;
            return true;
        }
        return fail("PipeWire 采集启动失败（已重试）：" + last_err);
    }

    void stop() override {
        // 在线程循环持有锁的状态下清理流与注册表，并做一次 roundtrip，
        // 等服务器确认代理移除（否则 pw_core_disconnect 会命中 refcount 断言）
        if (loop_) pw_thread_loop_lock(loop_.get());
        stream_.reset();
        if (registry_) registry_->cleanup();
        if (loop_ && core_) {
            struct SyncData {
                int done = 0;
                int seq  = 0;
            };
            static const pw_core_events core_events = [] {
                pw_core_events ev{};
                ev.version = PW_VERSION_CORE_EVENTS;
                ev.done    = [](void* data, uint32_t id, int seq) {
                    auto* sd = static_cast<SyncData*>(data);
                    if (id == PW_ID_CORE && seq == sd->seq) sd->done = 1;
                };
                return ev;
            }();

            SyncData sync;
            spa_hook hook{};
            sync.seq = pw_core_sync(core_.get(), PW_ID_CORE, 0);
            pw_core_add_listener(core_.get(), &hook, &core_events, &sync);
            // 限时等待：done 事件丢失时（核心连接异常）绝不能无限挂起，
            // 否则上层 stop()/析构会卡死所在线程（UI 或发送线程）。
            for (int waited_sec = 0; !sync.done && waited_sec < 10; ++waited_sec) {
                pw_thread_loop_timed_wait(loop_.get(), 1);
            }
            spa_hook_remove(&hook);
        }
        if (loop_) pw_thread_loop_unlock(loop_.get());
        if (loop_) pw_thread_loop_stop(loop_.get());
        // 每个对象只能销毁一次：绝不能先手动 pw_context_destroy /
        // pw_thread_loop_destroy 再交给 unique_ptr 的 deleter 调第二遍，
        // 那是对已释放内存的二次销毁（gdb 栈：pw_context_destroy →
        // pw_data_loop_destroy → pw_loop_destroy 段错误）。
        // pw_core_disconnect 会把自己从 context->core_list 摘除，
        // 因此先断 core 再销毁 context 是安全的。
        core_.reset();      // pw_core_disconnect（deleter 执行）
        context_.reset();   // pw_context_destroy（deleter 执行，仅一次）
        loop_.reset();      // pw_thread_loop_destroy（deleter 执行，仅一次）
        pw_init_.reset();

#if defined(PXC_HAVE_GNOME_RD)
        // 关联型 SC 的 Stop 也被 mutter 拒绝，必须由 RD Stop 关闭两者。
        if (!rd_session_id_.empty()) {
            GnomeRemoteDesktopBroker::instance().on_screencast_stopped(rd_session_id_);
        }
#endif
        // 独立 SC 则直接 Stop，再断开创建它的 D-Bus 连接。
        if (!rd_linked_ && session_path_set_ && !session_path_.empty() && dbus_) {
            DBusMessage* reply = dbus_->call(
                "org.gnome.Mutter.ScreenCast", session_path_,
                "org.gnome.Mutter.ScreenCast.Session", "Stop", nullptr, 3000);
            if (reply) dbus_message_unref(reply);
        }
        dbus_.reset();
        rd_linked_ = false;
        rd_session_id_.clear();
        session_path_.clear();
        session_path_set_ = false;
        started_ = false;
    }

    bool capture(RawFrame& out, std::chrono::milliseconds timeout) override {
        if (!started_) return fail("采集器未启动");

        std::unique_lock<std::mutex> lock(frame_mutex_);
        if (frame_seq_ == last_delivered_seq_) {
            // 无新帧：等到超时（画面静止是常态）
            if (!frame_cv_.wait_for(lock, timeout, [this] {
                    return frame_seq_ != last_delivered_seq_;
                })) {
                return false;
            }
        }

        RawFrame frame;
        frame.frame_id     = ++frame_id_;
        // 内容生产时刻（on_process 拷贝现场），供发送端测“采集→发出”age；
        // 0（未知）时退化为取帧时刻
        frame.timestamp_us = frame_produced_us_ ? frame_produced_us_
            : static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
        frame.width  = frame_width_;
        frame.height = frame_height_;
        frame.stride = frame_stride_;
        frame.bgra   = std::make_shared<std::vector<uint8_t>>(frame_data_);
        last_delivered_seq_ = frame_seq_;
        lock.unlock();

        if (frame.bgra->empty() || frame.width == 0) return false;
        out = std::move(frame);
        return true;
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(err_mutex_);
        return error_;
    }

    // 采集停滞重建时由发送端导出：完整的状态/信号历史（无时间戳，
    // 精确时刻看 stderr 的 [pw] 实时日志）
    std::string debug_dump() const override {
        std::lock_guard<std::mutex> lock(debug_mutex_);
        std::string out;
        for (const auto& sig : debug_signals_) {
            if (!out.empty()) out += '\n';
            out += sig;
        }
        return out;
    }

private:
    // -------------------------------------------------- 类型守卫

    struct PwInitGuard {
        PwInitGuard() { pw_init(nullptr, nullptr); }
        // 故意不调 pw_deinit：PipeWire 全局状态与动态加载的模块
        // （protocol-native 等）在长生命周期进程里无法安全拆除。
        // 采集器重建时新旧实例并存，旧实例 deinit 会让新实例的
        // PipeWire 连接段错误（实测 libpipewire-module-protocol-native
        // 内 segfault/GPF，会话反复建立后必现）。进程退出由 OS
        // 回收；业界（Chrome 等）同样只 init 不 deinit。
        ~PwInitGuard() = default;
    };

    // 进程级单例：所有采集器实例共享同一份全局初始化，永不拆除
    static std::shared_ptr<PwInitGuard> acquire_pw_init() {
        static const std::shared_ptr<PwInitGuard> g =
            std::make_shared<PwInitGuard>();
        return g;
    }
    struct PwCoreDeleter {
        void operator()(pw_core* c) const { if (c) pw_core_disconnect(c); }
    };
    struct PwContextDeleter {
        void operator()(pw_context* c) const { if (c) pw_context_destroy(c); }
    };
    struct PwLoopDeleter {
        void operator()(pw_thread_loop* l) const { if (l) pw_thread_loop_destroy(l); }
    };
    struct PwStreamDeleter {
        void operator()(pw_stream* s) const { if (s) pw_stream_destroy(s); }
    };
    struct PwRegistryGuard {
        pw_registry* r = nullptr;
        spa_hook     hook_{};
        ~PwRegistryGuard() { cleanup(); }
        void cleanup() {
            if (!r) return;
            spa_hook_remove(&hook_);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(r));
            r = nullptr;
        }
    };

    // -------------------------------------------------- PipeWire 注册表

    static void registry_global(void* data, uint32_t id, uint32_t permissions,
                                const char* type, uint32_t version,
                                const struct spa_dict* props) {
        auto* self = static_cast<ScreenCapturerPipeWire*>(data);
        ++self->registry_count_;
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

        // mutter 的生产者节点特征：media.category=Capture / media.role=Screen。
        // 记录候选节点的属性到调试日志，便于排查连错对象的情况。
        const char* media_class =
            props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : nullptr;
        const char* media_category =
            props ? spa_dict_lookup(props, PW_KEY_MEDIA_CATEGORY) : nullptr;
        const char* media_role =
            props ? spa_dict_lookup(props, PW_KEY_MEDIA_ROLE) : nullptr;
        const char* object_serial =
            props ? spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL) : nullptr;

        {
            std::lock_guard<std::mutex> lock(self->debug_mutex_);
            self->debug_signals_.push_back(
                "node " + std::to_string(id) + " serial=" +
                (object_serial ? object_serial : "-") + " class=" +
                (media_class ? media_class : "-") + " category=" +
                (media_category ? media_category : "-") + " role=" +
                (media_role ? media_role : "-"));
        }

        std::lock_guard<std::mutex> lock(self->graph_mutex_);
        if (!self->recording_started_) {
            self->existing_nodes_.insert(id);  // 录制开始前已存在
        } else if (self->new_node_id_ == 0) {
            // mutter 的生产者节点是 Stream/Output/Video；
            // 我们自己的消费者流是 Stream/Input/Video，必须排除
            const bool is_output =
                media_class && std::strstr(media_class, "Output") != nullptr;
            if (is_output) {
                self->new_node_id_ = id;
                self->node_cv_.notify_all();
            }
        }
    }

    static void registry_global_remove(void* data, uint32_t id) {
        auto* self = static_cast<ScreenCapturerPipeWire*>(data);
        std::lock_guard<std::mutex> lock(self->graph_mutex_);
        self->existing_nodes_.erase(id);
        // 我们选中的节点被销毁（会话快速重建时 mutter 会复用全局 id，
        // 垂死节点先于我们的 connect 消失）：清掉候选，让扫描继续等
        // 下一个新节点，绝不能把消费者流连向已死节点
        // （invalid message id / 永久 paused 零帧的根因）。
        if (self->new_node_id_ == id) {
            self->new_node_id_ = 0;
            self->node_cv_.notify_all();
        }
    }

    bool init_pipewire_graph() {
        // 每次 init 都是新连接：图状态必须归零。否则重试/重建时
        // new_node_id_ 残留旧值，扫描瞬间返回陈旧（可能已死的）节点。
        {
            std::lock_guard<std::mutex> lock(graph_mutex_);
            existing_nodes_.clear();
            new_node_id_ = 0;
            registry_count_ = 0;
            recording_started_ = false;
        }
        pw_init_ = acquire_pw_init();
        loop_    = std::unique_ptr<pw_thread_loop, PwLoopDeleter>(
            pw_thread_loop_new("pxc-capture", nullptr));
        if (!loop_) return fail("创建 PipeWire 线程循环失败");

        // 先启动事件循环线程：registry 事件靠它分发
        if (pw_thread_loop_start(loop_.get()) < 0) {
            return fail("启动 PipeWire 线程循环失败");
        }

        pw_thread_loop_lock(loop_.get());
        context_ = std::unique_ptr<pw_context, PwContextDeleter>(
            pw_context_new(pw_thread_loop_get_loop(loop_.get()), nullptr, 0));
        if (!context_) {
            pw_thread_loop_unlock(loop_.get());
            return fail("创建 PipeWire context 失败");
        }
        core_ = std::unique_ptr<pw_core, PwCoreDeleter>(
            pw_context_connect(context_.get(), nullptr, 0));
        if (!core_) {
            pw_thread_loop_unlock(loop_.get());
            return fail("连接 PipeWire 核心失败");
        }

        static const pw_registry_events registry_events = [] {
            pw_registry_events ev{};
            ev.version       = PW_VERSION_REGISTRY_EVENTS;
            ev.global        = &registry_global;
            ev.global_remove = &registry_global_remove;
            return ev;
        }();
        registry_ = std::make_unique<PwRegistryGuard>();
        registry_->r = reinterpret_cast<pw_registry*>(pw_core_get_registry(
            core_.get(), PW_VERSION_REGISTRY, 0));
        pw_registry_add_listener(registry_->r, &registry_->hook_,
                                  &registry_events, this);
        pw_thread_loop_unlock(loop_.get());

        // 等服务器把注册表回放（既有全局对象）投递完毕：回放事件在
        // loop 线程异步到达，靠 sleep 猜时机不可靠（原实现 300ms 仍
        // 偶发迟到）。冲刷完成前绝不置 recording_started_，否则会话
        // 重建时旧生产者节点会因回放迟到被误判为「新节点」——连上
        // 垂死节点（invalid message id / 永久 paused）的根因。
        {
            struct SyncData {
                int done = 0;
                int seq  = 0;
            };
            static const pw_core_events flush_events = [] {
                pw_core_events ev{};
                ev.version = PW_VERSION_CORE_EVENTS;
                ev.done    = [](void* data, uint32_t id, int seq) {
                    auto* sd = static_cast<SyncData*>(data);
                    if (id == PW_ID_CORE && seq == sd->seq) sd->done = 1;
                };
                return ev;
            }();
            pw_thread_loop_lock(loop_.get());
            SyncData sync;
            spa_hook hook{};
            sync.seq = pw_core_sync(core_.get(), PW_ID_CORE, 0);
            pw_core_add_listener(core_.get(), &hook, &flush_events, &sync);
            for (int waited_sec = 0; !sync.done && waited_sec < 10; ++waited_sec) {
                pw_thread_loop_timed_wait(loop_.get(), 1);
            }
            spa_hook_remove(&hook);
            pw_thread_loop_unlock(loop_.get());
            if (!sync.done) return fail("PipeWire 注册表同步超时");
        }
        return true;
    }

    uint32_t wait_new_source_node(int timeout_ms) {
        std::unique_lock<std::mutex> lock(graph_mutex_);
        node_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return new_node_id_ != 0; });
        return new_node_id_;
    }

    // -------------------------------------------------- D-Bus 会话

    bool start_mutter_session() {
        // 连接必须在整个录制期间保持存活：mutter 的会话对象绑定在
        // 创建它的 D-Bus 连接上，连接断开 = 会话被销毁
        dbus_ = std::make_unique<DbusConnection>();
        if (!dbus_->ok()) return fail(dbus_->error());
        DbusConnection& dbus = *dbus_;

        // RemoteDesktop 会话关联（GNOME 注入用）：必须在 SC CreateSession
        // 之前创建。失败时照常建独立 SC 会话（注入回退 X11），采集不受影响。
        std::string rd_session_id;
#if defined(PXC_HAVE_GNOME_RD)
        rd_session_id = GnomeRemoteDesktopBroker::instance().acquire_session_id();
        if (rd_session_id.empty()) {
            pxc_pw_log("RemoteDesktop 会话不可用，注入将回退 X11: %s",
                       GnomeRemoteDesktopBroker::instance().last_error().c_str());
        }
#endif

        rd_linked_ = !rd_session_id.empty();
        rd_session_id_ = rd_session_id;
        DBusMessage* reply = dbus.call(
            "org.gnome.Mutter.ScreenCast", "/org/gnome/Mutter/ScreenCast",
            "org.gnome.Mutter.ScreenCast", "CreateSession",
            [&](DBusMessage* msg) {
                DBusMessageIter it;
                dbus_message_iter_init_append(msg, &it);
                DBusMessageIter dict;
                dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
                append_dict_u32(&dict, "cursor_mode", 2);  // embedded：光标画进帧
                append_dict_bool(&dict, "remote", true);
                if (!rd_session_id.empty()) {
                    // 关联后 NotifyPointerMotionAbsolute 等注入方法可用；
                    // mutter 42 的选项键是 "remote-desktop-session-id"（字符串）
                    append_dict_string(&dict, "remote-desktop-session-id",
                                       rd_session_id.c_str());
                }
                dbus_message_iter_close_container(&it, &dict);
            },
            5000);
        if (!reply) return fail(dbus.error());
        session_path_ = read_object_path(reply);
        dbus_message_unref(reply);
        if (session_path_.empty()) return fail("CreateSession 返回无效");
        session_path_set_ = true;

        // RecordArea(逻辑全屏, 签名 iiiia{sv})
        reply = dbus.call(
            "org.gnome.Mutter.ScreenCast", session_path_,
            "org.gnome.Mutter.ScreenCast.Session", "RecordArea",
            [&](DBusMessage* msg) {
                DBusMessageIter it;
                dbus_message_iter_init_append(msg, &it);
                const dbus_int32_t x = static_cast<dbus_int32_t>(screen_.x);
                const dbus_int32_t y = static_cast<dbus_int32_t>(screen_.y);
                const dbus_int32_t w = static_cast<dbus_int32_t>(screen_.width);
                const dbus_int32_t h = static_cast<dbus_int32_t>(screen_.height);
                dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &x);
                dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &y);
                dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &w);
                dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &h);
                DBusMessageIter dict;
                dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
                dbus_message_iter_close_container(&it, &dict);
            },
            5000);
        if (!reply) {
            return fail("RecordArea 失败 (" + std::to_string(screen_.width) + "x" +
                        std::to_string(screen_.height) + "): " + dbus.error());
        }
        // 流对象路径：GNOME 注入的绝对坐标目标（RecordArea 回复 o）
        const std::string stream_path = read_object_path(reply);
        dbus_message_unref(reply);
        if (stream_path.empty()) return fail("RecordArea 返回无效流路径");
#if defined(PXC_HAVE_GNOME_RD)
        if (!stream_path.empty()) {
            GnomeRemoteDesktopBroker::instance().set_stream_path(stream_path);
        }
#endif

        // 关联型 SC 必须在 RecordArea 后由 RD Start 拉起；mutter 42
        // 拒绝直接调用 SC Start。独立 SC 保留原来的启动及回复解析。
        reply = nullptr;
#if defined(PXC_HAVE_GNOME_RD)
        if (!rd_session_id.empty()) {
            if (!GnomeRemoteDesktopBroker::instance().start_session()) {
                return fail("RemoteDesktop Start 失败: " +
                            GnomeRemoteDesktopBroker::instance().last_error());
            }
        } else
#endif
        {
            reply = dbus.call("org.gnome.Mutter.ScreenCast", session_path_,
                          "org.gnome.Mutter.ScreenCast.Session", "Start",
                          nullptr, 5000);
            if (!reply) return fail("Session.Start 失败: " + dbus.error());
        }
        session_node_id_ = 0;
        if (reply) {
            DBusMessageIter it;
            if (dbus_message_iter_init(reply, &it) &&
                dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                DBusMessageIter entries;
                dbus_message_iter_recurse(&it, &entries);
                if (dbus_message_iter_get_arg_type(&entries) ==
                        DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter entry;
                    dbus_message_iter_recurse(&entries, &entry);
                    if (dbus_message_iter_get_arg_type(&entry) ==
                            DBUS_TYPE_UINT32) {
                        dbus_message_iter_get_basic(&entry, &session_node_id_);
                    }
                }
            }
            dbus_message_unref(reply);
        }
        pxc_pw_log(session_node_id_
                       ? "mutter Start: producer node %u"
                       : "mutter Start: reply carries no node id, "
                         "fallback to registry scan",
                   session_node_id_);
        return true;
    }

    // -------------------------------------------------- 消费者流

    static void on_process(void* data) {
        auto* self = static_cast<ScreenCapturerPipeWire*>(data);
        self->pull_frame();
    }

    static void on_param_changed(void* data, uint32_t id, const struct spa_pod* param) {
        auto* self = static_cast<ScreenCapturerPipeWire*>(data);
        {
            std::lock_guard<std::mutex> lock(self->debug_mutex_);
            self->debug_signals_.push_back(
                "param_changed id=" + std::to_string(id) +
                (param ? std::string(" pod=yes") : std::string(" pod=null")));
        }
        if (param == nullptr || id != SPA_PARAM_Format) return;

        struct spa_video_info_raw info;
        if (spa_format_video_raw_parse(param, &info) < 0) {
            std::lock_guard<std::mutex> lock(self->debug_mutex_);
            self->debug_signals_.push_back("format parse failed");
            return;
        }

        std::lock_guard<std::mutex> lock(self->frame_mutex_);
        self->frame_width_  = static_cast<uint32_t>(info.size.width);
        self->frame_height_ = static_cast<uint32_t>(info.size.height);
        self->frame_stride_ = self->frame_width_ * 4;
        self->frame_format_ = static_cast<int>(info.format);
#if defined(PXC_HAVE_GNOME_RD)
        // 流像素尺寸 = 注入绝对坐标的取值空间，格式变化时同步给代理
        if (self->frame_width_ > 0 && self->frame_height_ > 0) {
            GnomeRemoteDesktopBroker::instance().set_stream_size(self->frame_width_,
                                                                 self->frame_height_);
        }
#endif
    }

    void pull_frame() {
        pw_buffer* buffer = pw_stream_dequeue_buffer(stream_.get());
        if (!buffer) {
            // on_process 触发却取不到缓冲：消费者侧缓冲耗尽或驱动
            // 尚未就绪——采集停滞排查的关键信号
            pxc_pw_log("process event without buffer (dequeue failed)");
            return;
        }

        spa_buffer* spa = buffer->buffer;
        if (spa && spa->n_datas > 0 && spa->datas[0].data && spa->datas[0].chunk) {
            const auto& data = spa->datas[0];
            std::lock_guard<std::mutex> lock(frame_mutex_);
            // mutter 的格式事件可能不带尺寸（0x0），首帧时回退到逻辑桌面尺寸
            if (frame_width_ == 0) {
                frame_width_  = screen_.width;
                frame_height_ = screen_.height;
                frame_stride_ = frame_width_ * 4;
            }
            if (copy_bgra_frame(static_cast<const uint8_t*>(data.data), data.maxsize,
                                data.chunk->offset, data.chunk->size, data.chunk->stride,
                                frame_width_, frame_height_, frame_data_)) {
                frame_stride_ = frame_width_ * 4;
                // RGBx/RGBA 与 BGRA 的 R/B 位置相反，做交换
                if (frame_format_ == SPA_VIDEO_FORMAT_RGBx ||
                    frame_format_ == SPA_VIDEO_FORMAT_RGBA) {
                    for (size_t i = 0; i + 3 < frame_data_.size(); i += 4) {
                        std::swap(frame_data_[i], frame_data_[i + 2]);
                    }
                }
                ++frame_seq_;
                frame_produced_us_ = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
                frame_cv_.notify_all();
            }
        } else {
            // mutter 送来的 buffer 无有效数据——此前是静默路径，停滞排查盲区
            pxc_pw_log("process event with EMPTY buffer (n_datas=%u)",
                       spa ? spa->n_datas : 0u);
        }
        pw_stream_queue_buffer(stream_.get(), buffer);
    }

    bool connect_stream(uint32_t node_id) {
        auto* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                        PW_KEY_MEDIA_CATEGORY, "Capture",
                                        PW_KEY_MEDIA_ROLE, "Screen", nullptr);
        stream_ = std::unique_ptr<pw_stream, PwStreamDeleter>(
            pw_stream_new(core_.get(), "pxc-screen-capture", props));
        if (!stream_) return fail("创建 PipeWire 流失败");

        static const pw_stream_events stream_events = [] {
            pw_stream_events ev{};
            ev.version       = PW_VERSION_STREAM_EVENTS;
            ev.state_changed = [](void* data, pw_stream_state old,
                                   pw_stream_state state, const char* error) {
                auto* self = static_cast<ScreenCapturerPipeWire*>(data);
                {
                    std::lock_guard<std::mutex> lock(self->debug_mutex_);
                    self->debug_signals_.push_back(
                        std::string("state:") + pw_stream_state_as_string(state) +
                        (error ? std::string(" err=") + error : std::string()));
                    if (error) {
                        self->debug_signals_.push_back(std::string("stream_error:") + error);
                    }
                }
                pxc_pw_log("stream state: %s -> %s%s%s",
                           pw_stream_state_as_string(old),
                           pw_stream_state_as_string(state),
                           error ? " err=" : "", error ? error : "");
            };
            ev.param_changed = &on_param_changed;
            ev.process       = &on_process;
            return ev;
        }();
        pw_stream_add_listener(stream_.get(), &stream_hook_, &stream_events, this);

        uint8_t pod_buffer[1024];
        struct spa_pod_builder builder;
        spa_pod_builder_init(&builder, pod_buffer, sizeof(pod_buffer));
        const struct spa_pod* params[2];
        struct spa_fraction zero_fps = SPA_FRACTION(0, 1);
        params[0] = static_cast<const struct spa_pod*>(
            spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType,     SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,  SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,  SPA_POD_CHOICE_ENUM_Id(
                    5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                    SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                    SPA_VIDEO_FORMAT_RGBA),
                SPA_FORMAT_VIDEO_framerate,
                    SPA_POD_Fraction(&zero_fps),
                0));
        // 强制 memfd 内存（不用 DMA-BUF，保证 datas[0].data 是可读指针）
        params[1] = static_cast<const struct spa_pod*>(
            spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_MemPtr),
                0));

        if (pw_stream_connect(stream_.get(), PW_DIRECTION_INPUT, node_id,
                              static_cast<pw_stream_flags>(
                                  PW_STREAM_FLAG_AUTOCONNECT |
                                  PW_STREAM_FLAG_MAP_BUFFERS),
                              params, 2) < 0) {
            return fail("PipeWire 流连接失败（节点 " + std::to_string(node_id) + "）");
        }
        pxc_pw_log("consumer stream connecting to node %u", node_id);

        // 必须等到 STREAMING 才算成功：连到垂死节点时 pw 流会走
        // connecting -> error(invalid message id) -> paused 然后永久
        // 挂起——paused 且零流量的会话是「假成功」，会让上层以为
        // 重建完成而画面永久冻结。
        for (int i = 0; i < 120; ++i) {
            const char* e = nullptr;
            const pw_stream_state state =
                pw_stream_get_state(stream_.get(), &e);
            if (state == PW_STREAM_STATE_ERROR) {
                std::string err = "PipeWire 流错误（节点 " +
                                  std::to_string(node_id) + "）";
                if (e) err += std::string("：") + e;
                return fail(err);
            }
            if (state == PW_STREAM_STATE_STREAMING) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return fail("PipeWire 流 " + std::to_string(node_id) +
                    " 未进入 streaming（垂死节点或权限未就绪），拒绝假成功");
    }

    bool fail(const std::string& reason) {
        std::lock_guard<std::mutex> lock(err_mutex_);
        error_ = reason;
        return false;
    }

    // -------------------------------------------------- 成员

    std::shared_ptr<PwInitGuard>                    pw_init_;
    std::unique_ptr<pw_thread_loop, PwLoopDeleter>  loop_;
    std::unique_ptr<pw_context, PwContextDeleter>   context_;
    std::unique_ptr<pw_core, PwCoreDeleter>         core_;
    std::unique_ptr<PwRegistryGuard>                registry_;
    std::unique_ptr<pw_stream, PwStreamDeleter>     stream_;
    spa_hook                                        stream_hook_{};

    // 图状态（PipeWire 线程回调写）
    std::mutex         graph_mutex_;
    std::set<uint32_t> existing_nodes_;
    uint32_t           new_node_id_       = 0;
    int                registry_count_ = 0;
    bool               recording_started_ = false;
    std::condition_variable node_cv_;

    // D-Bus 会话
    std::string session_path_;
    bool        session_path_set_ = false;
    // Start 回复给出的生产者节点 id（0 = 旧版 mutter 未提供）
    uint32_t    session_node_id_ = 0;
    bool        rd_linked_ = false;
    std::string rd_session_id_;

    // 帧状态（PipeWire 回调线程写，capture 读）
    std::mutex              frame_mutex_;
    std::condition_variable frame_cv_;
    std::vector<uint8_t>    frame_data_;
    uint32_t frame_width_  = 0;
    uint32_t frame_height_ = 0;
    uint32_t frame_stride_ = 0;
    int      frame_format_ = 0;
    uint64_t frame_seq_ = 0;
    // 最近一次 on_process 拷贝完成的时刻（steady_clock us）
    uint64_t frame_produced_us_ = 0;
    uint64_t last_delivered_seq_ = 0;
    uint64_t frame_id_ = 0;

    ScreenInfo screen_;

    // mutter 会话绑定在创建连接上，整个录制期间必须保持存活
    std::unique_ptr<DbusConnection> dbus_;

    // 调试：状态与信号日志（debug_dump 为 const 读，需要 mutable）
    mutable std::mutex       debug_mutex_;
    std::vector<std::string> debug_signals_;

    int      current_ = 0;
    bool     started_ = false;
    mutable std::mutex err_mutex_;
    std::string        error_;
};

}  // namespace

std::unique_ptr<ScreenCapturer> create_screen_capturer_pipewire(std::string* error) {
    (void)error;
    return std::make_unique<ScreenCapturerPipeWire>();
}

bool pipewire_environment_available() {
    DbusConnection dbus;
    return dbus.ok();
}

}  // namespace pxc

#endif  // __linux__ && PXC_HAVE_PIPEWIRE
