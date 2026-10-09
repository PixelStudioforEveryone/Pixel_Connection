#include "video_sender.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <QDebug>
#include <QString>
#include <QtGlobal>

#include "input_activity.h"
#include "pxc/frame_utils.h"
#include "pxc/video_protocol.h"

namespace pxc::gui {
namespace {

// 拥塞阈值：发送队列超过 2MB 视为拥塞，丢非关键帧保实时性
constexpr size_t kCongestBytes = 2 * 1024 * 1024;
// 关键帧的硬上限：关键帧不允许在 2MB 处拦腰截断（截断的关键帧在
// 接收端永远拼不完整，后续 P 帧会在陈旧参考上预测出脏画面，实测
// 卡画面 2-4 秒直到下一个关键帧）；但也要有兜底，链路彻底堵死时
// 仍然放弃，避免发送缓冲无限膨胀。
constexpr size_t kKeyframeCongestBytes = 8 * 1024 * 1024;

// 空闲保帧：画面静止时采集器（PipeWire/DXGI）不再产帧，而接收端
// 解码器（MF 同步 MFT）需要持续喂帧才能吐出缓冲中的画面。超时后
// 每隔这么久重发一次缓存的最后一帧。
constexpr int kIdleResendMs = 250;
// 空闲持续超过该时长后，重发改为强制关键帧（让中途重建的解码器
// 能立即出画，无需等下一个 GOP）
constexpr int kIdleKeyframeMs = 2000;
// Keep a request pending through capture timeouts, congestion and encoder delay.
struct PendingKeyframe {
    VideoSender& sender;
    bool& pending;
    ~PendingKeyframe() { if (pending) sender.force_keyframe(); }
};
// 发送端统计日志间隔（S1: 每秒一行结构化摘要）
constexpr int kStatsIntervalMs = 1000;

// 采集停滞检测阈值（无新帧多久后重建采集会话）：
// - 本会话从未出帧：Mutter 连续重建 ScreenCast 会话后停摆（S1 实测）
// - 出过帧且「输入仍在持续到达」：光标内嵌时持续键鼠必然产生
//   damage，无帧即停摆实锤。只看「最后一次输入是否新近」——
//   桌面静止后残留的单次输入打点不能证明停摆（damage 驱动采集：
//   静止 = 零帧是常态，误判会拆毁健康会话并触发重建竞态）
// - 出过帧且无持续输入：正常静止，长兜底重建即可；真停摆会在
//   用户回来操作时立刻落入上一档快速恢复
constexpr int kStallNeverMs     = 10000;
constexpr int kStallInputMs     = 5000;
constexpr int kStallIdleMs      = 600000;
constexpr int kInputRecentMs    = 1500;  // 「输入仍在持续」的判定窗口
// 停滞重建后仍无新帧时按 10s→20s→40s→60s 指数退避，避免重建风暴
constexpr int kStallBackoffMaxMs = 60000;

size_t packet_payload_limit(const std::shared_ptr<rtc::DataChannel>& channel) {
    const size_t max_message = channel->maxMessageSize();
    if (max_message <= pxc::kVideoPacketHeaderSize + 8) {
        return pxc::kVideoPacketDefaultPayload;
    }
    return std::min(pxc::kVideoPacketDefaultPayload,
                    max_message - pxc::kVideoPacketHeaderSize);
}

// 各阶段耗时的 P50/P95 跟踪（发送线程私有，无锁）
class StageMs {
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

VideoSender::VideoSender(std::shared_ptr<pxc::ScreenCapturer> capturer)
    : capturer_(std::move(capturer)) {}

VideoSender::~VideoSender() {
    stop();
}

bool VideoSender::start(std::shared_ptr<rtc::DataChannel> channel,
                        const pxc::VideoEncoderConfig& config) {
    if (running_.load()) return true;
    channel_ = std::move(channel);
    if (!channel_) {
        set_error("视频通道不可用");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        settings_       = config;
        force_key_      = true;
        config_dirty_   = true;
        current_screen_ = 0;
    }
    pending_screen_.store(-1);
    encoder_.reset();

    // 采集器的建立（PipeWire/D-Bus，最坏数十秒）全部留在发送线程，
    // 绝不阻塞调用方（GUI 线程）——否则被控端界面会「未响应」。
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
}

void VideoSender::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    // 采集器/编码器已由发送线程在退出前清理；这里兜底 start() 未被
    // 调用过、线程未运行过的场景（此时析构不含任何重活）。
    std::lock_guard<std::mutex> lock(settings_mutex_);
    capturer_.reset();
    encoder_.reset();
}

pxc::VideoEncoderConfig VideoSender::config() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return settings_;
}

void VideoSender::set_config(const pxc::VideoEncoderConfig& config) {
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        settings_     = config;
        config_dirty_ = true;
        force_key_    = true;
    }
    cv_.notify_all();
}

bool VideoSender::switch_screen(int index) {
    // 异步切屏：新采集器的建立（D-Bus + PipeWire，数秒到数十秒）
    // 在发送线程内完成，这里只投递请求。
    pending_screen_.store(index);
    cv_.notify_all();
    return true;
}

void VideoSender::force_keyframe() {
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        force_key_ = true;
    }
    cv_.notify_all();
}

int VideoSender::screen_index() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return current_screen_;
}

uint32_t VideoSender::actual_width() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return actual_width_;
}

uint32_t VideoSender::actual_height() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return actual_height_;
}

uint32_t VideoSender::actual_fps() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return settings_.fps;
}

std::string VideoSender::encoder_name() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return encoder_name_;
}

std::string VideoSender::last_error() const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    return error_;
}

void VideoSender::set_error(const std::string& reason) const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    error_ = reason;
}

bool VideoSender::send_frame(const pxc::VideoFrame& frame,
                             uint64_t* packets_sent, uint64_t* bytes_sent) {
    if (!channel_ || !channel_->isOpen()) return false;

    // 拥塞控制：队列太满时整帧丢弃非关键帧（关键帧例外，见下）。
    // 丢帧后流上出现空洞，后续 P 帧会解码成漂移画面，必须让下一个
    // 编码帧变关键帧来修复。
    if (channel_->bufferedAmount() > kCongestBytes && !frame.keyframe) {
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            force_key_ = true;
        }
        return false;
    }

    const size_t payload_limit = packet_payload_limit(channel_);
    auto packets = pxc::packetize_video_frame(frame, payload_limit);
    if (packets.empty()) { force_keyframe(); return false; }

    // 关键帧用更宽的硬上限：绝不轻易截断（截断的关键帧在接收端永远
    // 拼不完整，后续 P 帧在陈旧参考上预测出脏画面，实测卡画面 2-4
    // 秒直到下一个关键帧）；P 帧维持原拥塞纪律。
    const size_t congest_limit =
        frame.keyframe ? kKeyframeCongestBytes : kCongestBytes;
    size_t frame_bytes = 0;
    for (const auto& packet : packets) frame_bytes += packet.size();
    const size_t queued = channel_->bufferedAmount();
    if (queued > congest_limit || frame_bytes > congest_limit - queued) {
        force_keyframe();
        return false;
    }
    for (auto& packet : packets) {
        if (!channel_->isOpen() || channel_->bufferedAmount() > congest_limit) {
            // 本帧剩余分片不再发送，接收端会丢弃不完整帧——同样留下
            // 空洞，标记下一帧为关键帧
            {
                std::lock_guard<std::mutex> lock(settings_mutex_);
                force_key_ = true;
            }
            return false;
        }
        rtc::binary binary(packet.size());
        std::transform(packet.begin(), packet.end(), binary.begin(),
                       [](uint8_t value) { return static_cast<std::byte>(value); });
        try {
            // libdatachannel returns false when accepted into its queue, not rejected.
            channel_->send(std::move(binary));
        } catch (const std::exception&) {
            force_keyframe();
            return false;
        }
        if (packets_sent) ++*packets_sent;
        if (bytes_sent) *bytes_sent += packet.size();
    }
    return true;
}

bool VideoSender::ensure_capturer(int screen) {
    // 只在发送线程调用。不持有 settings_mutex_ 的情况下做重活：
    // capturer->start() 内部是 D-Bus 同步调用 + PipeWire 协商。
    std::shared_ptr<pxc::ScreenCapturer> capturer;
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        capturer = capturer_;
    }
    while (running_.load()) {
        if (!capturer) capturer = pxc::create_screen_capturer();
        if (!capturer) {
            set_error("无法创建屏幕采集器");
            qInfo().noquote()
                << "[video-sender] 创建采集器失败，2s 后重试";
            if (!sleep_interruptible(2000)) return false;
            continue;
        }
        if (capturer->start(screen)) break;
        const std::string start_err = capturer->last_error().empty()
                                          ? "屏幕采集启动失败"
                                          : capturer->last_error();
        set_error(start_err);
        // 必须留痕：此循环可能长时间空转，无日志会表现成发送线程
        // “无故静默”（实测卡在这里数分钟无任何输出）
        qInfo().noquote()
            << "[video-sender] 采集启动失败(重试中): "
            << QString::fromStdString(start_err);
        // PipeWire 图可能处于坏状态：换一个新对象重试
        capturer = pxc::create_screen_capturer();
        if (!sleep_interruptible(2000)) return false;
    }
    if (!running_.load()) return false;
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        capturer_ = std::move(capturer);
    }
    set_error("");
    return true;
}

bool VideoSender::sleep_interruptible(int ms) {
    std::unique_lock<std::mutex> lock(cv_mutex_);
    return !cv_.wait_for(lock, std::chrono::milliseconds(ms),
                         [this] { return !running_.load(); });
}

void VideoSender::run() {
    std::vector<uint8_t> scaled;
    uint32_t scaled_w = 0, scaled_h = 0;

    // 空闲保帧缓存（发送线程私有）：编码输入的 BGRA 拷贝
    std::shared_ptr<std::vector<uint8_t>> idle_bgra;
    uint32_t idle_w = 0, idle_h = 0;
    size_t   idle_stride = 0;   // 原始帧行距可能带 padding，不能按 w*4 推
    // 编码器当前配置尺寸（发送线程跟踪）。画质切换后编码器先按上限
    // 重配，空闲路径再按流尺寸对齐——用跟踪值判断是否需要重配，
    // 比用 actual_width_ 推导更可靠（sws 编码器配置的是实际编码尺寸）
    uint32_t enc_cfg_w = 0, enc_cfg_h = 0;
    // 当前编码器是否内置高效缩放（创建后不变）
    bool enc_scales = false;
    // 采集停滞检测（见 kStall* 常量注释）：last_capture_at 追踪最近
    // 一次真实采到的帧；空闲重发会让接收端持续出画，从而掩盖停摆
    // （用户看到的是停摆瞬间的旧画面），所以停滞判定不看缓存是否
    // 为空，只看「多久没有新帧」。
    auto stall_since = std::chrono::steady_clock::time_point{};
    auto last_capture_at  = std::chrono::steady_clock::time_point{};
    int   stall_backoff_ms = 0;
    auto last_encode_at = std::chrono::steady_clock::now() -
                          std::chrono::milliseconds(kIdleKeyframeMs);
    auto last_key_at    = last_encode_at;

    // 1 秒窗口统计 + 各阶段耗时分布
    uint64_t stat_captured = 0, stat_encoded = 0, stat_sent = 0;
    uint64_t stat_idle = 0, stat_timeouts = 0, stat_send_fail = 0;
    uint64_t stat_pkts = 0, stat_bytes = 0, stat_key = 0;
    size_t   stat_buf_peak = 0;
    double   stat_window_fps = 0;
    StageMs  t_cap, t_cvt, t_enc;
    // 延迟定位探针：pipe=取帧→发出；age=内容生产(pw 交付)→发出
    uint32_t stat_last_fid = 0;
    StageMs  t_pipe, t_age;
    auto stat_window_at = std::chrono::steady_clock::now();

    // 采集器建立留在发送线程（见 start() 注释）
    // 默认取主屏，避免多屏设备按枚举顺序把副屏当作首页桌面预览。
    // 枚举放在采集线程，启动远控时不阻塞 UI。
    {
        const auto screens = pxc::enumerate_screens();
        const auto primary = std::find_if(screens.begin(), screens.end(),
                                          [](const pxc::ScreenInfo& screen) {
                                              return screen.primary;
                                          });
        if (primary != screens.end()) {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            current_screen_ = primary->index;
        }
    }
    if (!ensure_capturer(current_screen_)) {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        capturer_.reset();
        encoder_.reset();
        return;
    }

    auto next_tick = std::chrono::steady_clock::now();

    while (running_.load()) {
        // 固定发送时钟：不管前面是否丢帧都按整点推进，落后就重新对齐
        uint32_t fps = 30;
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            fps = std::max<uint32_t>(settings_.fps, 1);
        }
        next_tick += std::chrono::milliseconds(1000 / fps);
        const auto now = std::chrono::steady_clock::now();
        if (next_tick < now) next_tick = now;

        {
            std::unique_lock<std::mutex> lock(cv_mutex_);
            cv_.wait_until(lock, next_tick, [this] { return !running_.load(); });
        }
        if (!running_.load()) break;

        // 统计日志（1 秒窗口，S1 结构化摘要）
        if (now - stat_window_at >= std::chrono::milliseconds(kStatsIntervalMs)) {
            const QString err = QString::fromStdString(last_error());
            const double win_s = std::chrono::duration<double>(
                                     now - stat_window_at).count();
            qInfo().noquote()
                << QString("[vtx] cap=%1 to=%2 enc=%3 key=%4 sent=%5 idle=%6 "
                           "sfail=%7 pkt=%8 out=%9MB buf=%10k bufpk=%11k "
                           "t_cap=%12/%13 t_cvt=%14/%15 t_enc=%16/%17 fps=%18 "
                           "fid=%19 pipe=%20/%21 age=%22/%23 err=%24")
                       .arg(stat_captured).arg(stat_timeouts)
                       .arg(stat_encoded).arg(stat_key).arg(stat_sent)
                       .arg(stat_idle).arg(stat_send_fail).arg(stat_pkts)
                       .arg(stat_bytes / 1048576.0, 0, 'f', 2)
                       .arg((channel_ ? channel_->bufferedAmount() : 0) / 1024)
                       .arg(stat_buf_peak / 1024)
                       .arg(t_cap.at(50), 0, 'f', 1).arg(t_cap.at(95), 0, 'f', 1)
                       .arg(t_cvt.at(50), 0, 'f', 1).arg(t_cvt.at(95), 0, 'f', 1)
                       .arg(t_enc.at(50), 0, 'f', 1).arg(t_enc.at(95), 0, 'f', 1)
                       .arg(stat_captured / std::max(win_s, 0.001), 0, 'f', 1)
                       .arg(stat_last_fid)
                       .arg(t_pipe.at(50), 0, 'f', 1).arg(t_pipe.at(95), 0, 'f', 1)
                       .arg(t_age.at(50), 0, 'f', 1).arg(t_age.at(95), 0, 'f', 1)
                       .arg(err.isEmpty() ? QStringLiteral("-") : err);
            stat_captured = stat_encoded = stat_sent = 0;
            stat_idle = stat_timeouts = stat_send_fail = 0;
            stat_pkts = stat_bytes = stat_key = 0;
            stat_buf_peak = 0;
            stat_window_at = now;
        }

        // 异步切屏请求：新采集器建立同样在本线程完成
        const int want_screen = pending_screen_.exchange(-1);
        if (want_screen >= 0 && want_screen != current_screen_) {
            auto next = pxc::create_screen_capturer();
            if (next && next->start(want_screen)) {
                std::lock_guard<std::mutex> lock(settings_mutex_);
                capturer_       = std::move(next);  // 旧对象由 shared 拷贝保活
                current_screen_ = want_screen;
                force_key_      = true;
                config_dirty_   = true;
                actual_width_   = 0;   // 重新按新分辨率配置编码器
                actual_height_  = 0;
                qInfo().noquote() << "[video-sender] 切屏到" << want_screen;
            } else {
                set_error(next ? (next->last_error().empty()
                                       ? "屏幕采集启动失败"
                                       : next->last_error())
                                : "无法创建屏幕采集器");
            }
            // 分辨率可能变化，旧空闲缓存作废
            idle_bgra.reset();
            idle_w = idle_h = 0;
            stall_since = std::chrono::steady_clock::time_point{};
            stall_backoff_ms = 0;
        }

        pxc::VideoEncoderConfig config;
        bool force_key = false;
        std::shared_ptr<pxc::ScreenCapturer> capturer;
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            config     = settings_;
            force_key  = force_key_;
            force_key_ = false;
            capturer   = capturer_;  // shared 拷贝：切换屏幕不影响本 tick
        }
        PendingKeyframe pending_keyframe{*this, force_key};
        if (!capturer) break;

        // 编码器按「画质上限」先备好；实际尺寸在拿到帧后可能再收窄
        bool encoder_dirty = false;
        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            encoder_dirty = config_dirty_ || !encoder_;
        }
        if (encoder_dirty) {
            if (!encoder_) {
                std::string enc_error;
                encoder_ = pxc::create_video_encoder(&enc_error);
                if (!encoder_) {
                    set_error(enc_error.empty() ? "无法创建编码器" : enc_error);
                    if (!sleep_interruptible(200)) break;
                    continue;
                }
                enc_scales = encoder_->handles_scaling();
            }
            if (!encoder_->configure(config)) {
                set_error(encoder_->last_error());
                if (!sleep_interruptible(200)) break;
                continue;
            }
            std::lock_guard<std::mutex> lock(settings_mutex_);
            config_dirty_ = false;
            enc_cfg_w = config.width;
            enc_cfg_h = config.height;
        }

        pxc::RawFrame raw;
        // 半个周期的采集超时；无新帧是常态（画面静止时 DXGI 不产帧）
        const auto capture_timeout =
            std::chrono::milliseconds(std::max<uint32_t>(1000 / fps / 2, 5));
        const auto cap_t0 = std::chrono::steady_clock::now();
        const bool cap_ok = capturer->capture(raw, capture_timeout);
        if (cap_ok) t_cap.push(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - cap_t0)
                                   .count());
        if (channel_) {
            stat_buf_peak = std::max(stat_buf_peak, channel_->bufferedAmount());
        }
        if (!cap_ok) {
            const std::string error = capturer->last_error();
            if (!error.empty()) {
                set_error(error);
                // 采集器内部已尝试重建；持续失败时降低重试频率
                if (!sleep_interruptible(200)) break;
                continue;
            }
            ++stat_timeouts;

            // 空闲保帧：无新帧不代表可以停发。接收端解码器需要持续
            // 输入才能出画（MF 同步 MFT 有预热缓冲），重发缓存的最后
            // 一帧；空闲超过 2s 时强制关键帧（中途重建的解码器可立即
            // 出画，无需等下一个 GOP）。
            const auto idle_now = std::chrono::steady_clock::now();

            // 采集停滞：按 kStall* 阈值重建采集会话。有远端输入却
            // 无新帧是最强信号——注入必然损伤画面，只有会话停摆才
            // 会既收输入又不出帧。
            if (stall_since == std::chrono::steady_clock::time_point{}) {
                stall_since = idle_now;
            } else {
                const auto stalled_ms = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        idle_now - stall_since).count());
                const bool never_captured =
                    last_capture_at == std::chrono::steady_clock::time_point{};
                const int64_t input_ms   = pxc::gui::last_remote_input_ms();
                const int64_t now_ms     = static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        idle_now.time_since_epoch()).count());
                const int64_t capture_ms = never_captured ? 0 : static_cast<int64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        last_capture_at.time_since_epoch()).count());
                // 只信任「持续输入」：最后一次输入打点必须新近
                // （kInputRecentMs 内），否则视为画面静止走空闲阈值
                const bool input_active =
                    input_ms > 0 && now_ms - input_ms < kInputRecentMs;
                const bool input_since_frame =
                    input_active && input_ms > capture_ms;
                const int64_t threshold_ms = never_captured   ? kStallNeverMs
                                             : input_since_frame ? kStallInputMs
                                                                 : kStallIdleMs;
                // 有输入时退避封顶（kStallNeverMs）：退避是为防重建风暴，
                // 但用户正在主动使用的桌面冻结 40s 不可接受，宁可定期重建
                const int64_t backoff_ms = input_since_frame
                    ? std::min<int64_t>(stall_backoff_ms, kStallNeverMs)
                    : stall_backoff_ms;
                if (stalled_ms >= threshold_ms + backoff_ms) {
                    qInfo().noquote() << QString(
                        "[video-sender] 采集停滞 %1ms 无新帧(有输入=%2 "
                        "退避=%3ms)，重建采集会话")
                        .arg(stalled_ms)
                        .arg(input_since_frame ? 1 : 0)
                        .arg(backoff_ms);
                    set_error("采集持续无帧，重建采集会话");
                    {
                        // 销毁旧采集器内含 PipeWire/D-Bus 清理（可能
                        // 秒级）：锁外销毁，避免阻塞 set_config 等
                        // 控制路径线程
                        std::shared_ptr<pxc::ScreenCapturer> dead;
                        {
                            std::lock_guard<std::mutex> lock(settings_mutex_);
                            dead = std::move(capturer_);
                        }
                        if (dead) {
                            const std::string dump = dead->debug_dump();
                            if (!dump.empty()) {
                                qInfo().noquote() << QString(
                                    "[video-sender] 停滞采集器调试历史:\n%1")
                                    .arg(QString::fromStdString(dump));
                            }
                        }
                        capturer.reset();  // 释放本轮的 shared_ptr，确保旧会话先关闭
                        dead.reset();
                    }
                    if (!ensure_capturer(current_screen_)) break;
                    // 无论是否首轮，重建后都进入退避：首轮时
                    // last_rebuild_at 还是 epoch，原「上次重建后
                    // 仍无新帧」比较恒假，退避不生效，第二次重建
                    // 可能只隔 5s——重建风暴既冲击 mutter 会话，
                    // 也放大 PipeWire 拆除/重建竞态窗口
                    stall_backoff_ms = std::min(
                        stall_backoff_ms ? stall_backoff_ms * 2
                                         : kStallNeverMs,
                        kStallBackoffMaxMs);
                    stall_since     = std::chrono::steady_clock::now();
                }
            }

            uint32_t enc_w = 0, enc_h = 0;
            {
                std::lock_guard<std::mutex> lock(settings_mutex_);
                enc_w = actual_width_;
                enc_h = actual_height_;
            }
            // 缓存有效性：sws 编码器接受任意输入尺寸，缓存始终可用；
            // 非缩放编码器必须与当前流尺寸一致
            const bool cache_usable = idle_bgra && !idle_bgra->empty() &&
                (enc_scales || (idle_w == enc_w && idle_h == enc_h));
            if (cache_usable && (force_key || idle_now - last_encode_at >=
                    std::chrono::milliseconds(kIdleResendMs))) {
                // 画质切换会把编码器按新上限重配，而空闲缓存还是旧档位
                // 尺寸：先按当前流尺寸对齐（相同参数时 configure 为
                // no-op）。否则非缩放编码器会按新配置尺寸去读旧档位
                // 缓冲——越界读崩溃（静止画面下频繁切画质必现）。
                pxc::VideoEncoderConfig stream_cfg = config;
                stream_cfg.width  = enc_w;
                stream_cfg.height = enc_h;
                if (!encoder_->configure(stream_cfg)) {
                    set_error(encoder_->last_error());
                } else {
                    enc_cfg_w = enc_w;
                    enc_cfg_h = enc_h;
                    // 空洞修复（force_key：丢帧/解码端请求）优先于空闲
                    // 关键帧节拍
                    const bool idle_key = force_key ||
                        idle_now - last_key_at >=
                            std::chrono::milliseconds(kIdleKeyframeMs);
                    if (idle_key && channel_ &&
                        channel_->bufferedAmount() > kCongestBytes) {
                        // 拥塞未消退：延后关键帧，写回已消费的 force_key
                        std::lock_guard<std::mutex> lock(settings_mutex_);
                        force_key_ = true;
                    } else {
                        std::vector<pxc::VideoFrame> encoded;
                        if (encoder_->encode(idle_bgra->data(), idle_stride,
                                             idle_w, idle_h, idle_key, encoded)) {
                            for (const auto& frame : encoded) {
                                if (frame.keyframe) ++stat_key;
                                if (send_frame(frame, &stat_pkts, &stat_bytes)) {
                                    ++stat_sent;
                                    ++stat_idle;
                                    last_encode_at = idle_now;
                                    if (frame.keyframe) { last_key_at = idle_now; force_key = false; }
                                    stat_last_fid = frame.frame_id;
                                } else {
                                    ++stat_send_fail;
                                }
                            }
                        }
                    }
                }
            }
            continue;
        }
        ++stat_captured;
        stall_since      = std::chrono::steady_clock::time_point{};
        last_capture_at  = std::chrono::steady_clock::now();
        stall_backoff_ms = 0;
        const uint64_t raw_ts_us = raw.timestamp_us;
        const auto     feed_t0   = std::chrono::steady_clock::now();

        // 画质上限只缩不放：4K 屏开 1080P 档时真正编码 1080p
        uint32_t target_w = 0, target_h = 0;
        pxc::fit_within(raw.width, raw.height, config.width, config.height,
                        target_w, target_h);

        const uint8_t* encode_src    = raw.bgra->data();
        size_t         encode_stride = raw.stride;
        uint32_t       feed_w = raw.width, feed_h = raw.height;
        if (!enc_scales && (target_w != raw.width || target_h != raw.height)) {
            // 无内置缩放的编码器（MF）：朴素 box filter 缩放（待优化）
            const auto cvt_t0 = std::chrono::steady_clock::now();
            if (scaled_w != target_w || scaled_h != target_h) {
                scaled.resize(static_cast<size_t>(target_w) * target_h * 4);
                scaled_w = target_w;
                scaled_h = target_h;
            }
            pxc::downscale_bgra(raw.bgra->data(), raw.stride, raw.width, raw.height,
                                scaled.data(), static_cast<size_t>(target_w) * 4,
                                target_w, target_h);
            encode_src    = scaled.data();
            encode_stride = static_cast<size_t>(target_w) * 4;
            feed_w = target_w;
            feed_h = target_h;
            t_cvt.push(std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - cvt_t0)
                           .count());
        }
        // sws 编码器：原始帧直接进 encode，色彩转换+缩放一步完成
        // （耗時計入 t_enc，t_cvt 恒为 0）

        // 实际编码尺寸变化（切屏/改画质/分辨率变化）时重配并强制关键帧
        if (enc_cfg_w != target_w || enc_cfg_h != target_h) {
            pxc::VideoEncoderConfig actual = config;
            actual.width  = target_w;
            actual.height = target_h;
            if (!encoder_->configure(actual)) {
                set_error(encoder_->last_error());
                continue;
            }
            enc_cfg_w = target_w;
            enc_cfg_h = target_h;
            force_key = true;
        }

        // 关键帧补发需要缓冲有余量：拥塞未消退时延后（硬塞 1MB+ 的
        // IDR 只会加剧排队延迟或在硬上限处被截断），写回快照已消费
        // 的 force_key，下一 tick 再试。非关键帧不受影响，照常走
        // send_frame 的拥塞丢弃。
        // Cache every final capture, even if encoding is deferred. Raw frames
        // already own their pixels, so the normal path needs no extra copy.
        idle_bgra = encode_src == raw.bgra->data() ? raw.bgra :
            std::make_shared<std::vector<uint8_t>>(encode_src, encode_src + encode_stride * feed_h);
        idle_w = feed_w; idle_h = feed_h; idle_stride = encode_stride;
        if (force_key && channel_ &&
            channel_->bufferedAmount() > kCongestBytes) {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            force_key_ = true;
            continue;
        }

        std::vector<pxc::VideoFrame> encoded;
        const auto enc_t0 = std::chrono::steady_clock::now();
        const bool enc_ok =
            encoder_->encode(encode_src, encode_stride, feed_w, feed_h,
                             force_key, encoded);
        t_enc.push(std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - enc_t0)
                       .count());
        if (!enc_ok) {
            set_error(encoder_->last_error());
            continue;
        }
        for (const auto& frame : encoded) {
            if (frame.keyframe) ++stat_key;
        }

        {
            std::lock_guard<std::mutex> lock(settings_mutex_);
            actual_width_  = target_w;
            actual_height_ = target_h;
            encoder_name_  = encoder_->name();
        }

        for (const auto& frame : encoded) {
            ++stat_encoded;
            if (send_frame(frame, &stat_pkts, &stat_bytes)) {
                ++stat_sent;
                last_encode_at = now;
                if (frame.keyframe) { last_key_at = now; force_key = false; }
                stat_last_fid = frame.frame_id;
                const auto sent_at = std::chrono::steady_clock::now();
                t_pipe.push(std::chrono::duration<double, std::milli>(
                                sent_at - feed_t0).count());
                if (raw_ts_us) {
                    t_age.push(std::chrono::duration<double, std::milli>(
                                   sent_at - std::chrono::steady_clock::time_point(
                                       std::chrono::microseconds(raw_ts_us)))
                                   .count());
                }
            } else {
                ++stat_send_fail;
            }
        }
    }

    // 采集器与编码器的析构（PipeWire 拆图 + D-Bus Session.Stop）
    // 在本线程完成，绝不落到 UI 线程上。
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        capturer_.reset();
        encoder_.reset();
    }
}

}  // namespace pxc::gui
