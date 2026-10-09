#pragma once

// 被控端视频发送引擎。
//
// 线程模型：start() 只绑定通道并启动发送线程，立即返回；采集器的
// 建立、重建与销毁全部发生在发送线程内（PipeWire/Mutter 的 D-Bus
// 初始化最坏可达数十秒，落在 UI 线程会造成界面「未响应」）。
// 发送线程按固定发送时钟运行（目标帧率由 settings 决定，而不是
// 「有帧就编」）；所有外部调用的设置接口都线程安全，命令（画质/
// 帧率/切屏）在下一个 tick 生效。采集器用 shared_ptr 持有：切屏时
// 在线程内替换指针，正在进行的采集由旧对象的 shared 拷贝保活。
//
// 拥塞纪律（与 docs/WINDOWS_TASKS.md 一致）：
//   - 发送前检查 bufferedAmount()，拥塞时整帧丢弃非关键帧；
//   - ch-video 不可靠无序，丢帧是预期行为，绝不重传。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rtc/rtc.hpp>

#include "pxc/screen_capturer.h"
#include "pxc/video_encoder.h"

namespace pxc::gui {

class VideoSender {
public:
    VideoSender(std::shared_ptr<pxc::ScreenCapturer> capturer);
    ~VideoSender();

    VideoSender(const VideoSender&)            = delete;
    VideoSender& operator=(const VideoSender&) = delete;

    // 绑定发送通道并启动发送线程。立即返回 true；采集启动失败会在
    // 发送线程内以 2 秒间隔重试，错误经 last_error()/video_state 上报。
    bool start(std::shared_ptr<rtc::DataChannel> channel,
               const pxc::VideoEncoderConfig& config);
    void stop();

    // ---- 以下接口线程安全，可在任意线程调用 ----
    void set_config(const pxc::VideoEncoderConfig& config);
    pxc::VideoEncoderConfig config() const;
    // 异步切屏：仅投递请求，由发送线程执行；结果经 video_state 上报。
    bool switch_screen(int index);
    void force_keyframe();

    int      screen_index() const;
    uint32_t actual_width() const;   // 实际编码尺寸（受采集分辨率限制，只缩不放）
    uint32_t actual_height() const;
    uint32_t actual_fps() const;
    std::string encoder_name() const;
    std::string last_error() const;

private:
    void run();
    // 发送线程内建立/重建采集器；running_ 置 false 时返回 false
    bool ensure_capturer(int screen);
    // 可被 stop() 打断的睡眠；返回 false 表示已停止
    bool sleep_interruptible(int ms);
    // 发送一帧的所有分片；packets_sent/bytes_sent 可选输出本帧实际入队量
    bool send_frame(const pxc::VideoFrame& frame,
                    uint64_t* packets_sent = nullptr,
                    uint64_t* bytes_sent = nullptr);
    void set_error(const std::string& reason) const;

    std::shared_ptr<rtc::DataChannel> channel_;

    mutable std::mutex                       settings_mutex_;
    std::shared_ptr<pxc::ScreenCapturer>     capturer_;
    std::unique_ptr<pxc::VideoEncoder>       encoder_;
    pxc::VideoEncoderConfig                  settings_;
    int      current_screen_ = 0;
    bool     force_key_      = true;
    bool     config_dirty_   = false;
    uint32_t actual_width_   = 0;
    uint32_t actual_height_  = 0;
    std::string encoder_name_;

    std::atomic<int>        pending_screen_{-1};
    std::thread             thread_;
    std::condition_variable cv_;
    std::mutex              cv_mutex_;
    std::atomic<bool>       running_{false};

    mutable std::mutex err_mutex_;
    mutable std::string error_;
};

}  // namespace pxc::gui
