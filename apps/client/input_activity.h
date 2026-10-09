#pragma once

// 远端输入活动打点（被控端）。
//
// 用途：视频发送线程的采集停滞检测。PipeWire/DXGI 都是损伤驱动的，
// 画面静止时不产帧是常态；但「远端输入持续到达却始终无新帧」只有
// 采集会话停摆一种解释（键鼠注入必然造成光标/画面变化）。被控端
// 在注入键鼠事件前打点，发送线程据此快速判定 Mutter ScreenCast
// 停摆并重建采集会话，而不是永远重发停摆瞬间的缓存帧。

#include <atomic>
#include <chrono>
#include <cstdint>

namespace pxc::gui {
namespace input_activity_detail {

inline std::atomic<int64_t>& shared_ms() {
    static std::atomic<int64_t> value{0};
    return value;
}

}  // namespace input_activity_detail

inline int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// 被控端注入键鼠事件前调用（任意线程，无锁）
inline void note_remote_input() {
    input_activity_detail::shared_ms().store(steady_now_ms(),
                                             std::memory_order_relaxed);
}

// 最近一次远端输入的时刻（steady_clock 毫秒）；0 表示本进程从未收到输入
inline int64_t last_remote_input_ms() {
    return input_activity_detail::shared_ms().load(std::memory_order_relaxed);
}

}  // namespace pxc::gui
