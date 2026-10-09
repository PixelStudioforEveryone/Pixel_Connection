#include "video_receiver.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <QSettings>

#include "rx_log.h"

#include "pxc/video_decoder.h"

namespace pxc::gui {
namespace {

// 连续解码失败超过该次数则重建解码器（典型场景：中途加入错过关键帧）
constexpr int kDecoderRebuildThreshold = 10;

// 解码耗时分布（mutex_ 保护下使用）
class DecMs {
public:
    void push(double ms) {
        if (samples_.size() >= kCap) samples_.erase(samples_.begin());
        samples_.push_back(ms);
    }
    double at(double p) const {
        if (samples_.empty()) return 0.0;
        std::vector<double> sorted(samples_);
        std::sort(sorted.begin(), sorted.end());
        const size_t idx = std::min(sorted.size() - 1,
            static_cast<size_t>(p / 100.0 * sorted.size()));
        return sorted[idx];
    }
private:
    static constexpr size_t kCap = 2048;
    std::vector<double> samples_;
};

}  // namespace

VideoReceiver::VideoReceiver(QObject* parent) : QObject(parent) {
    const QString mode = QSettings().value(QStringLiteral("video/acceleration"),
                                            QStringLiteral("smart")).toString().toLower();
    if (mode == QStringLiteral("hardware")) acceleration_mode_ = pxc::VideoAccelerationMode::Hardware;
    else if (mode == QStringLiteral("software")) acceleration_mode_ = pxc::VideoAccelerationMode::Software;
}

VideoReceiver::~VideoReceiver() = default;

double VideoReceiver::DecMs::at(double p) const {
    if (samples_.empty()) return 0.0;
    std::vector<double> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());
    const size_t idx = std::min(sorted.size() - 1,
        static_cast<size_t>(p / 100.0 * sorted.size()));
    return sorted[idx];
}

void VideoReceiver::push(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++packets_in_;
    bytes_in_ += size;
    maybe_emit_stats();
    auto frame = reassembler_.push(data, size);
    // 丢帧检测：重组器丢弃了不完整帧 = 流上出现空洞。后续 P 帧会在
    // 陈旧参考上预测出脏画面且 MFT 不报错，必须立刻请求关键帧修复，
    // 否则实测卡画面 2-4 秒（只能等发送端空闲关键帧）。
    {
        const auto& st = reassembler_.stats();
        if (st.frames_dropped > keyframe_req_watermark_) {
            reference_.invalidate();
            keyframe_req_watermark_ = st.frames_dropped;
            request_keyframe_locked(
                QStringLiteral("丢帧(drop=%1)").arg(st.frames_dropped));
        }
    }
    if (!frame) {
        frame = try_starvation_recovery(data, size);
        if (reference_.needs_keyframe()) request_keyframe_locked(QStringLiteral("等待完整关键帧"));
        if (!frame) return;  // 不完整帧被静默丢弃，正常
    }
    last_completed_at_ = std::chrono::steady_clock::now();
    ++frames_in_;
    last_fid_ = frame->frame_id;
    const bool recovering = reference_.needs_keyframe();
    if (!reference_.accept(*frame)) {
        if (reference_.needs_keyframe()) request_keyframe_locked(QStringLiteral("参考帧缺失，等待关键帧"));
        return;
    }
    if (recovering && frame->keyframe) {
        // Discard delayed output from the old chain as well as its references.
        decoder_.reset();
        decoder_width_ = decoder_height_ = 0;
        prev_out_index_ = 0;
    }
    if (frame->reasm_first_us) {
        reasm_ms_.push(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() -
                           std::chrono::steady_clock::time_point(
                               std::chrono::microseconds(frame->reasm_first_us)))
                           .count());
    }

    QImage image;
    if (decode(*frame, image) && !image.isNull()) {
        fail_count_ = 0;
        ++frames_decoded_;
        if (frames_decoded_ == 1) {
            RxLog::instance().write(
                QString("首帧解码成功 %1x%2 (pkt=%3 frames=%4)")
                    .arg(image.width()).arg(image.height())
                    .arg(packets_in_).arg(frames_in_));
        }
        const uint64_t out_idx = decoder_->last_out_index();
        if (out_idx != 0 && out_idx != prev_out_index_) {
            prev_out_index_ = out_idx;
            dlag_frames_.push(static_cast<double>(
                static_cast<int64_t>(frames_in_) -
                static_cast<int64_t>(out_idx)));
        }
        emit frameDecoded(frame->frame_id, image);
    }
}

void VideoReceiver::request_keyframe_locked(const QString& reason) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_keyframe_req_at_ < kKeyframeReqCooldown) return;
    last_keyframe_req_at_ = now;
    RxLog::instance().write(
        QString("[vrx] %1，请求关键帧").arg(reason));
    emit keyframeRequested();
}

void VideoReceiver::maybe_emit_stats() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_stats_at_ < std::chrono::seconds(1)) return;

    const auto& st = reassembler_.stats();
    RxLog::instance().write(
        QString("[vrx] pkt=%1 bytes=%2MB comp=%3 dec=%4 dup=%5 rej=%6 "
                "drop=%7 t_dec=%8/%9 fail=%10 err=%11 fid=%12 lat=%13/%14 "
                "dlag=%15/%16")
            .arg(packets_in_ - last_packets_in_)
            .arg((bytes_in_ - last_bytes_in_) / 1048576.0, 0, 'f', 2)
            .arg(frames_in_ - last_frames_in_)
            .arg(frames_decoded_ - last_frames_decoded_)
            .arg(st.duplicate_packets - last_reasm_dup_)
            .arg(st.packets_rejected - last_reasm_rejected_)
            .arg(st.frames_dropped - last_reasm_dropped_)
            .arg(decode_ms_.at(50), 0, 'f', 1)
            .arg(decode_ms_.at(95), 0, 'f', 1)
            .arg(fail_count_)
            .arg(last_error_.isEmpty() ? QStringLiteral("-") : last_error_)
            .arg(last_fid_)
            .arg(reasm_ms_.at(50), 0, 'f', 1)
            .arg(reasm_ms_.at(95), 0, 'f', 1)
            .arg(dlag_frames_.at(50), 0, 'f', 0)
            .arg(dlag_frames_.at(95), 0, 'f', 0));

    last_packets_in_   = packets_in_;
    last_bytes_in_     = bytes_in_;
    last_frames_in_    = frames_in_;
    last_frames_decoded_ = frames_decoded_;
    last_reasm_dup_    = st.duplicate_packets;
    last_reasm_rejected_ = st.packets_rejected;
    last_reasm_dropped_  = st.frames_dropped;
    last_stats_at_     = now;
}

std::optional<pxc::VideoFrame>
VideoReceiver::try_starvation_recovery(const uint8_t* data, size_t size) {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_completed_at_ < std::chrono::seconds(3)) return std::nullopt;
    auto packet = pxc::decode_video_packet(data, size);
    if (!packet || !packet->keyframe || packet->fragment_index != 0) {
        return std::nullopt;
    }
    RxLog::instance().write(
        QString("持续无完整帧且收到关键帧首片，重置重组器自愈 (pkt=%1 rej=%2)")
            .arg(packets_in_)
            .arg(reassembler_.stats().packets_rejected));
    reassembler_.reset();
    reference_.reset();
    // 重组器统计已清零，快照/水位同步归零，避免统计出现负数
    last_reasm_dup_      = 0;
    last_reasm_rejected_ = 0;
    last_reasm_dropped_  = 0;
    keyframe_req_watermark_ = 0;
    last_completed_at_ = now;
    return reassembler_.push(data, size);
}

void VideoReceiver::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    RxLog::instance().write(
        QString("reset (pkt=%1 frames=%2 decoded=%3 dropped=%4)")
            .arg(packets_in_).arg(frames_in_).arg(frames_decoded_)
            .arg(reassembler_.stats().frames_dropped));
    reassembler_.reset();
    reference_.reset();
    decoder_.reset();
    decoder_width_  = 0;
    decoder_height_ = 0;
    fail_count_     = 0;
    last_completed_at_ = std::chrono::steady_clock::now();
    // 重组器统计已清零，快照/水位同步归零，避免统计出现负数
    last_reasm_dup_      = 0;
    last_reasm_rejected_ = 0;
    last_reasm_dropped_  = 0;
    keyframe_req_watermark_ = 0;
}

void VideoReceiver::setAccelerationMode(pxc::VideoAccelerationMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (acceleration_mode_ == mode) return;
    acceleration_mode_ = mode;
    reference_.invalidate();
    decoder_.reset();
    decoder_width_ = decoder_height_ = 0;
    fail_count_ = 0;
    last_error_.clear();
}

pxc::VideoAccelerationMode VideoReceiver::accelerationMode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return acceleration_mode_;
}

QString VideoReceiver::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

bool VideoReceiver::ensure_decoder() {
    if (decoder_) return true;

    std::string error;
    decoder_ = pxc::create_video_decoder(&error, acceleration_mode_);
    if (!decoder_) {
        last_error_ = QString::fromStdString(
            error.empty() ? "当前平台没有视频解码器" : error);
        return false;
    }
    decoder_width_  = 0;
    decoder_height_ = 0;
    return true;
}

bool VideoReceiver::decode(const pxc::VideoFrame& frame, QImage& out) {
    if (frame.codec != pxc::VideoCodec::H264) {
        // 第一版只协商 H.264；Unknown 编码是测试数据
        last_error_ = QStringLiteral("收到未知编码的视频帧");
        return false;
    }
    if (!ensure_decoder()) return false;

    pxc::DecodedFrame decoded;
    const auto dec_t0 = std::chrono::steady_clock::now();
    const bool dec_ok =
        decoder_->decode(frame.payload.data(), frame.payload.size(), decoded);
    decode_ms_.push(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - dec_t0)
                        .count());
    if (!dec_ok) {
        const std::string err = decoder_->last_error();
        if (err.empty()) {
            // 解码器还在缓冲预热（如 MF 同步 MFT 需要先喂几帧才吐
            // 画面），不是错误；屏幕静止时帧稀疏，若计入失败计数
            // 会在 10 帧后误杀解码器导致永久黑屏。
            return false;
        }
        ++fail_count_;
        reference_.invalidate();
        request_keyframe_locked(QStringLiteral("解码失败，等待关键帧"));
        last_error_ = QString::fromStdString(err);
        if (fail_count_ == 1 || fail_count_ % 10 == 0) {
            RxLog::instance().write(
                QString("解码失败 #%1: %2").arg(fail_count_).arg(last_error_));
        }
        if (fail_count_ >= kDecoderRebuildThreshold) {
            // 大概率错过关键帧/流中断：重建解码器等下一个关键帧
            decoder_.reset();
            fail_count_ = 0;
            RxLog::instance().write("连续失败达到阈值，重建解码器");
            emit videoNotice(QStringLiteral("视频流中断，等待下一个关键帧..."), false);
            // 重建后的解码器必须吃到关键帧才能出画；发送端并不知道
            // 这边重建了，主动请求（运动期间可能长期没有关键帧）
            request_keyframe_locked(QStringLiteral("解码器重建"));
        }
        return false;
    }
    if (decoded.width == 0 || decoded.height == 0 || decoded.bgra.empty()) {
        // 解码器还没输出画面（等后续帧），正常
        return false;
    }

    // BGRA 与 QImage::Format_ARGB32 内存布局一致；构造 + copy 一次拿到独立数据
    QImage image(decoded.bgra.data(), static_cast<int>(decoded.width),
                 static_cast<int>(decoded.height), static_cast<int>(decoded.stride),
                 QImage::Format_ARGB32);
    out = image.copy();
    return !out.isNull();
}

}  // namespace pxc::gui
