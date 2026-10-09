#pragma once

// 主控端视频接收引擎。
//
// 线程模型：push() 在 libdatachannel 的回调线程被调用；重组和解码都在
// 内部互斥锁保护下进行，解码结果以 QImage 经 Qt 信号（自动 queued）
// 投递到 UI 线程渲染。
//
// ch-video 不可靠无序：不完整帧由 VideoReassembler 静默丢弃；
// 解码器持续失败（如中途加入错过关键帧）时自动重建并等待下一个关键帧。

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QImage>
#include <QObject>

#include <rtc/rtc.hpp>

#include "pxc/video_decoder.h"
#include "pxc/video_transport.h"
#include "pxc/video_reference_guard.h"

namespace pxc::gui {

class VideoReceiver : public QObject {
    Q_OBJECT

public:
    explicit VideoReceiver(QObject* parent = nullptr);
    ~VideoReceiver() override;

    // ch-video 的 binary message 原样喂进来（任意线程）。
    void push(const uint8_t* data, size_t size);
    void push(const rtc::binary& data) {
        push(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    // 会话结束时清理（重建重组器与解码器）。
    void reset();
    void setAccelerationMode(pxc::VideoAccelerationMode mode);
    pxc::VideoAccelerationMode accelerationMode() const;

    QString last_error() const;

signals:
    // 解码出的一帧（UI 线程回调；image 已持有独立数据）。
    // frameId 用于显示层延迟定位（[vrx] fid 与 [vui] fid 对齐）。
    void frameDecoded(quint32 frameId, const QImage& image);
    // 可读的错误/状态提示（限频）。
    void videoNotice(const QString& message, bool is_error);
    // 检测到流上出现空洞（重组器丢弃不完整帧，或解码器重建）时发出。
    // 控制器应立即请求发送端补发关键帧：后续 P 帧会在陈旧参考上
    // 预测出脏画面，MFT 不会报错，只能等关键帧才能恢复。
    void keyframeRequested();

private:
    bool decode(const pxc::VideoFrame& frame, QImage& out);
    bool ensure_decoder();
    // mutex_ 保护下调用：每秒输出一行结构化统计
    void maybe_emit_stats();
    // mutex_ 保护下调用：长时间无完整帧且新包声明是关键帧时，重置
    // 重组器自愈（兼容 frame_id 回退的发送端，如旧版切档后从 0 重计）
    std::optional<pxc::VideoFrame> try_starvation_recovery(const uint8_t* data,
                                                           size_t size);
    // mutex_ 保护下调用：节流后发出 keyframeRequested()
    void request_keyframe_locked(const QString& reason);

    // 解码耗时分布（P50/P95，最近 2048 个样本）
    class DecMs {
    public:
        void push(double ms) {
            if (samples_.size() >= kCap) samples_.erase(samples_.begin());
            samples_.push_back(ms);
        }
        double at(double p) const;
    private:
        static constexpr size_t kCap = 2048;
        std::vector<double> samples_;
    };

    mutable std::mutex mutex_;
    pxc::VideoReassembler reassembler_;
    pxc::VideoReferenceGuard reference_;
    std::unique_ptr<pxc::VideoDecoder> decoder_;
    pxc::VideoAccelerationMode acceleration_mode_ = pxc::VideoAccelerationMode::Smart;
    uint32_t decoder_width_  = 0;
    uint32_t decoder_height_ = 0;
    int      fail_count_     = 0;
    QString  last_error_;
    // 累计计数（S1 统计口径见 docs/pixel_connection_plan/S0_baseline.md §6）
    uint64_t packets_in_     = 0;
    uint64_t bytes_in_       = 0;
    uint64_t frames_in_      = 0;
    uint64_t frames_decoded_ = 0;
    // 上一个统计窗口的快照
    uint64_t last_packets_in_     = 0;
    uint64_t last_bytes_in_       = 0;
    uint64_t last_frames_in_      = 0;
    uint64_t last_frames_decoded_ = 0;
    uint64_t last_reasm_received_ = 0;
    uint64_t last_reasm_rejected_ = 0;
    uint64_t last_reasm_dup_      = 0;
    uint64_t last_reasm_dropped_      = 0;
    std::chrono::steady_clock::time_point last_stats_at_ =
        std::chrono::steady_clock::now();
    // 最近一次得到完整帧的时刻（自愈判定基准）
    std::chrono::steady_clock::time_point last_completed_at_ =
        std::chrono::steady_clock::now();
    // 关键帧请求：丢帧水位（重组器 frames_dropped 快照）+ 节流
    uint64_t keyframe_req_watermark_ = 0;
    std::chrono::steady_clock::time_point last_keyframe_req_at_{};
    static constexpr std::chrono::milliseconds kKeyframeReqCooldown{400};
    DecMs   decode_ms_;
    // 延迟定位：最近完成帧号 + 首片到达→重组完成的等待分布
    uint32_t last_fid_ = 0;
    DecMs   reasm_ms_;
    // 解码器内部缓冲深度（帧）：已喂入序号 - 解码输出序号
    uint64_t prev_out_index_ = 0;
    DecMs   dlag_frames_;
};

}  // namespace pxc::gui
