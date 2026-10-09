#pragma once

// HOS 视频接收器：HosController 重组出的完整 H264 Annex-B 帧 →
// OH_VideoDecoder（video/avc）surface 模式硬解直显。
//
// 蓝本 apps/client/video_receiver.cpp 的关键机制保留：
//   - 解码器失败重建（fail_count 上限 kMaxFails，超限报致命错误）
//   - 重建/启动后主动请求远端关键帧（KeyframeRequester 注入）
//   - 每 1s 上报 videoStats 事件 {fps, pushed, dropped}
//
// 线程模型：
//   - onFrame 在 rtc 数据通道线程调用
//   - OH_AVCodec 四回调在媒体服务线程
//   - start/stop 在 JS 线程；全部经 mtx_ 保护

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include "pxc/video_reference_guard.h"

#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avbuffer.h>
#include <native_window/external_window.h>

namespace pxc::hos {

class HosVideoReceiver {
public:
    using EventSink        = std::function<void(const std::string& event, const std::string& json)>;
    // 解码器（重）启动成功后请求远端关键帧
    using KeyframeRequester = std::function<void()>;

    HosVideoReceiver(EventSink sink, KeyframeRequester requester);
    ~HosVideoReceiver();

    HosVideoReceiver(const HosVideoReceiver&)            = delete;
    HosVideoReceiver& operator=(const HosVideoReceiver&) = delete;

    // surface_id 来自 XComponent surface；width/height 为远端视频尺寸（仅作配置提示）
    bool start(uint64_t surface_id, int width, int height);
    void stop();

    // HosController::FrameSink 入口
    void onFrame(const uint8_t* data, size_t size, uint32_t frame_id,
                 bool keyframe, uint64_t timestamp_us);

private:
    struct InputItem {
        uint32_t     index  = 0;
        OH_AVBuffer* buffer = nullptr;
    };

    // OH_AVCodecCallback（媒体服务线程）
    static void onCodecError(OH_AVCodec* codec, int32_t code, void* user);
    static void onStreamChanged(OH_AVCodec* codec, OH_AVFormat* format, void* user);
    static void onNeedInputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* user);
    static void onNewOutputBuffer(OH_AVCodec* codec, uint32_t index, OH_AVBuffer* buffer, void* user);

    bool  setupLocked();
    void  teardownLocked();
    void  scheduleRestart();   // 解码器失败后异步重建
    void  emitNotice(const std::string& message, bool is_error);
    void  maybeEmitStats();

    EventSink         sink_;       // 构造后不变
    KeyframeRequester requester_;  // 构造后不变

    std::mutex        mtx_;
    OH_AVCodec*       codec_      = nullptr;
    OHNativeWindow*   window_     = nullptr;
    std::deque<InputItem> inputs_;
    pxc::VideoReferenceGuard reference_;
    uint64_t last_key_request_ms_ = 0;
    uint64_t          surface_id_ = 0;
    int               width_      = 0;
    int               height_     = 0;
    bool              running_    = false;
    std::atomic<bool> stopping_{false};
    std::thread       restart_thread_;

    // 统计（onFrame 线程更新，无需加锁；仅事件读取）
    uint64_t frames_in_          = 0;
    uint64_t frames_pushed_      = 0;
    uint64_t frames_dropped_     = 0;
    uint64_t stats_window_start_ = 0;
    uint64_t stats_window_count_ = 0;
    uint64_t last_stats_ms_      = 0;
    uint64_t fail_count_         = 0;
    bool     overflow_noticed_   = false;
};

}  // namespace pxc::hos
