// 真实采集器活体压测：镜像 video_sender 的 tick 节奏驱动
// create_screen_capturer()（GNOME 下即 PipeWire/Mutter 路径），
// 逐秒输出出帧统计，停滞超 5 秒时导出 debug_dump()。
//
// 用法：
//   pxc-capture-live-test [时长秒=120] [tick毫秒=16]           单会话模式
//   pxc-capture-live-test torture [重建次数=5]                  重建拷打模式：
//     循环 {create→start→等出帧→destroy}，再最后跑 60s 验证会话仍健康。
//     镜像 video_sender 的停滞重建路径，验证重建是否毒化 mutter。

#include <pxc/screen_capturer.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

using namespace std::chrono;

namespace {
std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop = true; }

std::string wall_now() {
    const auto t = system_clock::now();
    const auto tt = system_clock::to_time_t(t);
    const auto ms = duration_cast<milliseconds>(t.time_since_epoch()).count() % 1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
    return buf;
}

// 在 tick 节奏下采集 until 秒，返回拿到的新帧总数
uint64_t pump(std::unique_ptr<pxc::ScreenCapturer>& capturer, double until_s,
              int tick_ms) {
    const auto t0 = steady_clock::now();
    const auto deadline = t0 + duration_cast<steady_clock::duration>(
                                   duration<double>(until_s));
    uint64_t frames = 0;
    while (!g_stop && steady_clock::now() < deadline) {
        const auto tick_begin = steady_clock::now();
        pxc::RawFrame frame;
        if (capturer->capture(frame, std::chrono::milliseconds(8))) {
            if (++frames <= 2) {
                std::printf("[%s]   frame#%llu: %ux%u bytes=%zu\n",
                            wall_now().c_str(),
                            static_cast<unsigned long long>(frames),
                            frame.width, frame.height,
                            frame.bgra ? frame.bgra->size() : 0);
            }
        } else {
            const std::string le = capturer->last_error();
            if (!le.empty()) {
                std::printf("[%s]   FATAL: %s\n%s\n", wall_now().c_str(),
                            le.c_str(), capturer->debug_dump().c_str());
                g_stop = true;
            }
        }
        const auto done = steady_clock::now();
        const auto elapsed = duration_cast<milliseconds>(done - tick_begin).count();
        if (elapsed < tick_ms) {
            std::this_thread::sleep_for(
                milliseconds(tick_ms - static_cast<int>(elapsed)));
        }
    }
    return frames;
}

int run_torture(int cycles) {
    std::unique_ptr<pxc::ScreenCapturer> previous;
    for (int i = 0; i < cycles && !g_stop; ++i) {
        std::printf("[%s] === cycle %d/%d: create + start ===\n",
                    wall_now().c_str(), i + 1, cycles);
        std::string err;
        auto capturer = pxc::create_screen_capturer(&err);
        if (!capturer) {
            std::printf("[%s] create failed: %s\n", wall_now().c_str(),
                        err.c_str());
            return 1;
        }
        if (!capturer->start(0)) {
            std::printf("[%s] start failed: %s\n%s\n", wall_now().c_str(),
                        capturer->last_error().c_str(),
                        capturer->debug_dump().c_str());
            return 1;
        }
        // 镜像切屏替换：新会话启动后才释放旧对象，旧析构不能关闭新会话。
        if (previous) {
            previous->stop();
            previous.reset();
        }
        // 先等 8 秒看有没有帧（damage 活跃时应 ~2-4fps）
        const uint64_t frames = pump(capturer, 8.0, 16);
        std::printf("[%s] cycle %d: frames=%llu %s\n", wall_now().c_str(),
                    i + 1, static_cast<unsigned long long>(frames),
                    frames > 0 ? "OK" : "** ZERO FRAMES **");
        if (frames == 0) {
            std::printf("%s\n", capturer->debug_dump().c_str());
            return 2;
        }
        if (g_stop) return 2;
        // 留到下轮新会话启动后销毁，覆盖 shared_ptr 延迟析构的路径。
        previous = std::move(capturer);
        std::this_thread::sleep_for(milliseconds(1000));
    }
    if (previous) previous->stop();
    previous.reset();
    std::printf("[%s] === final long run: 60s ===\n", wall_now().c_str());
    std::string err;
    auto capturer = pxc::create_screen_capturer(&err);
    if (!capturer || !capturer->start(0)) {
        std::printf("[%s] final create/start failed: %s\n", wall_now().c_str(),
                    capturer ? capturer->last_error().c_str() : err.c_str());
        return 1;
    }
    const uint64_t frames = pump(capturer, 60.0, 16);
    std::printf("[%s] final run: frames=%llu %s\n", wall_now().c_str(),
                static_cast<unsigned long long>(frames),
                frames > 0 ? "OK" : "** ZERO FRAMES **");
    if (frames == 0) std::printf("%s\n", capturer->debug_dump().c_str());
    capturer->stop();
    std::printf("[%s] torture done\n", wall_now().c_str());
    return frames > 0 && !g_stop ? 0 : 2;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "torture") == 0) {
        const int cycles = argc > 2 ? std::atoi(argv[2]) : 5;
        std::signal(SIGINT, on_sigint);
        return run_torture(cycles);
    }
    const int duration_s = argc > 1 ? std::atoi(argv[1]) : 120;
    const int tick_ms    = argc > 2 ? std::atoi(argv[2]) : 16;

    std::string err;
    auto capturer = pxc::create_screen_capturer(&err);
    if (!capturer) {
        std::fprintf(stderr, "[%s] create failed: %s\n", wall_now().c_str(),
                     err.c_str());
        return 1;
    }
    if (!capturer->start(0)) {
        std::fprintf(stderr, "[%s] start(0) failed: %s\n", wall_now().c_str(),
                     capturer->last_error().c_str());
        return 1;
    }
    std::signal(SIGINT, on_sigint);
    std::printf("[%s] started, duration=%ds tick=%dms\n", wall_now().c_str(),
                duration_s, tick_ms);

    steady_clock::time_point t0 = steady_clock::now();
    steady_clock::time_point last_frame = t0;
    steady_clock::time_point last_report = t0;
    uint64_t total_frames = 0;
    uint64_t sec_frames = 0;
    uint64_t total_timeouts = 0;
    bool stall_reported = false;
    bool first_frame_logged = false;

    while (!g_stop) {
        const auto now = steady_clock::now();
        if (duration_cast<seconds>(now - t0).count() >= duration_s) break;

        pxc::RawFrame frame;
        const auto tick_begin = steady_clock::now();
        if (capturer->capture(frame, std::chrono::milliseconds(8))) {
            ++total_frames;
            ++sec_frames;
            last_frame = now;
            if (!first_frame_logged) {
                first_frame_logged = true;
                std::printf("[%s] first frame: %ux%u stride=%u bytes=%zu id=%llu\n",
                            wall_now().c_str(), frame.width, frame.height,
                            frame.stride, frame.bgra ? frame.bgra->size() : 0,
                            static_cast<unsigned long long>(frame.frame_id));
            }
        } else {
            ++total_timeouts;
            const std::string le = capturer->last_error();
            if (!le.empty()) {
                std::printf("[%s] FATAL error: %s\n%s\n", wall_now().c_str(),
                            le.c_str(), capturer->debug_dump().c_str());
                return 2;
            }
        }

        // 停滞检测：有帧历史但 5 秒无新帧
        if (!stall_reported && total_frames > 0 &&
            duration_cast<milliseconds>(now - last_frame).count() > 5000) {
            stall_reported = true;
            std::printf("[%s] === STALL: no frame for %lldms ===\n%s\n",
                        wall_now().c_str(),
                        static_cast<long long>(
                            duration_cast<milliseconds>(now - last_frame).count()),
                        capturer->debug_dump().c_str());
        }
        if (stall_reported &&
            duration_cast<milliseconds>(now - last_frame).count() < 5000) {
            stall_reported = false;
        }

        // 每秒统计
        if (duration_cast<milliseconds>(now - last_report).count() >= 1000) {
            std::printf("[%s] sec: frames=%llu total=%llu timeouts=%llu\n",
                        wall_now().c_str(),
                        static_cast<unsigned long long>(sec_frames),
                        static_cast<unsigned long long>(total_frames),
                        static_cast<unsigned long long>(total_timeouts));
            sec_frames = 0;
            last_report = now;
        }

        // tick 节奏（镜像 video_sender：从 tick 起点算剩余时间）
        const auto done = steady_clock::now();
        const auto elapsed = duration_cast<milliseconds>(done - tick_begin).count();
        if (elapsed < tick_ms) {
            std::this_thread::sleep_for(milliseconds(tick_ms -
                                                     static_cast<int>(elapsed)));
        }
    }

    std::printf("[%s] done: total_frames=%llu timeouts=%llu\n",
                wall_now().c_str(),
                static_cast<unsigned long long>(total_frames),
                static_cast<unsigned long long>(total_timeouts));
    capturer->stop();
    return total_frames > 0 && !g_stop ? 0 : 2;
}
