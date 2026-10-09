#include "hos_video_receiver.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <hilog/log.h>

#include <nlohmann/json.hpp>

namespace pxc::hos {

namespace {

constexpr int      kMaxFails        = 10;   // 连续重建上限（对照桌面端 fail_count）
constexpr uint64_t kStatsPeriodMs   = 1000;
constexpr uint64_t kRestartDelayMs  = 200;

#define PXC_LOGE(...) OH_LOG_Print(LOG_APP, LOG_ERROR, 0xC050, "PxcVideo", __VA_ARGS__)
#define PXC_LOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO,  0xC050, "PxcVideo", __VA_ARGS__)

uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

HosVideoReceiver::HosVideoReceiver(EventSink sink, KeyframeRequester requester)
    : sink_(std::move(sink)), requester_(std::move(requester)) {}

HosVideoReceiver::~HosVideoReceiver() {
    stop();
}

// ------------------------------------------------------------------ 生命周期

bool HosVideoReceiver::start(uint64_t surface_id, int width, int height) {
    std::lock_guard<std::mutex> lk(mtx_);
    surface_id_ = surface_id;
    width_      = std::max(width, 16);
    height_     = std::max(height, 16);
    if (!setupLocked()) {
        teardownLocked();
        return false;
    }
    running_ = true;
    // 新解码器从干净状态开始，必须从关键帧起步
    if (requester_) requester_();
    return true;
}

void HosVideoReceiver::stop() {
    stopping_.store(true);
    std::thread rt;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        rt = std::move(restart_thread_);
    }
    if (rt.joinable()) rt.join();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        teardownLocked();
        running_ = false;
    }
}

bool HosVideoReceiver::setupLocked() {
    reference_.reset();
    last_key_request_ms_ = 0;
    PXC_LOGI("setup begin surface=%{public}llu size=%{public}dx%{public}d",
             static_cast<unsigned long long>(surface_id_), width_, height_);

    codec_ = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (codec_ == nullptr) {
        PXC_LOGE("CreateByMime failed");
        emitNotice("创建 H264 解码器失败（设备可能不支持 video/avc）", true);
        return false;
    }
    PXC_LOGI("CreateByMime ok");

    OH_AVCodecCallback cb {};
    cb.onError             = &HosVideoReceiver::onCodecError;
    cb.onStreamChanged     = &HosVideoReceiver::onStreamChanged;
    cb.onNeedInputBuffer   = &HosVideoReceiver::onNeedInputBuffer;
    cb.onNewOutputBuffer   = &HosVideoReceiver::onNewOutputBuffer;
    const int32_t reg_ret = OH_VideoDecoder_RegisterCallback(codec_, cb, this);
    if (reg_ret != AV_ERR_OK) {
        PXC_LOGE("RegisterCallback failed ret=%{public}d", reg_ret);
        emitNotice("注册解码器回调失败 code=" + std::to_string(reg_ret), true);
        return false;
    }

    // 官方 Surface 模式顺序：Register → Configure → SetSurface → Prepare → Start。
    // 真机 h.vdec 在 Initialized 状态会拒绝 SetOutputSurface（INVALID_STATE）。
    OH_AVFormat* fmt = OH_AVFormat_Create();
    if (fmt == nullptr) {
        emitNotice("创建 AVFormat 失败", true);
        return false;
    }
    OH_AVFormat_SetStringValue(fmt, OH_MD_KEY_CODEC_MIME, OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_WIDTH, width_);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_HEIGHT, height_);
    const int32_t cfg_ret = OH_VideoDecoder_Configure(codec_, fmt);
    OH_AVFormat_Destroy(fmt);
    PXC_LOGI("Configure ret=%{public}d", cfg_ret);
    if (cfg_ret != AV_ERR_OK) {
        emitNotice("解码器配置失败（分辨率 " + std::to_string(width_) + "x" +
                   std::to_string(height_) + " code=" + std::to_string(cfg_ret) + "）", true);
        return false;
    }

    if (window_ == nullptr) {
        const int32_t win_ret =
            OH_NativeWindow_CreateNativeWindowFromSurfaceId(surface_id_, &window_);
        PXC_LOGI("CreateNativeWindow ret=%{public}d window=%{public}p",
                 win_ret, static_cast<void*>(window_));
        if (win_ret != 0 || window_ == nullptr) {
            emitNotice("由 surfaceId 创建 NativeWindow 失败 code=" +
                       std::to_string(win_ret), true);
            return false;
        }
    }
    // 服务端 Init/Configure 为异步，SetSurface 可能因竞态报 INVALID_STATE，短重试
    int32_t surf_ret = AV_ERR_UNKNOWN;
    for (int attempt = 0; attempt < 6; ++attempt) {
        surf_ret = OH_VideoDecoder_SetSurface(codec_, window_);
        PXC_LOGI("SetSurface attempt=%{public}d ret=%{public}d", attempt, surf_ret);
        if (surf_ret == AV_ERR_OK) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (surf_ret != AV_ERR_OK) {
        emitNotice("解码器绑定输出 Surface 失败 code=" + std::to_string(surf_ret), true);
        return false;
    }

    const int32_t prep_ret = OH_VideoDecoder_Prepare(codec_);
    const int32_t start_ret = prep_ret == AV_ERR_OK ? OH_VideoDecoder_Start(codec_) : prep_ret;
    PXC_LOGI("Prepare/Start ret=%{public}d/%{public}d", prep_ret, start_ret);
    if (prep_ret != AV_ERR_OK || start_ret != AV_ERR_OK) {
        emitNotice("解码器 Prepare/Start 失败 code=" +
                   std::to_string(prep_ret != AV_ERR_OK ? prep_ret : start_ret), true);
        return false;
    }
    inputs_.clear();
    fail_count_       = 0;
    overflow_noticed_ = false;
    PXC_LOGI("setup ok");
    return true;
}

void HosVideoReceiver::teardownLocked() {
    running_ = false;
    inputs_.clear();
    if (codec_ != nullptr) {
        OH_VideoDecoder_Stop(codec_);
        OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
    }
    if (window_ != nullptr) {
        OH_NativeWindow_DestroyNativeWindow(window_);
        window_ = nullptr;
    }
}

// ------------------------------------------------------------------ 帧入口

void HosVideoReceiver::onFrame(const uint8_t* data, size_t size, uint32_t frame_id,
                               bool keyframe, uint64_t timestamp_us) {
    InputItem item;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!running_ || codec_ == nullptr || stopping_.load()) {
            return;
        }
        pxc::VideoFrame frame;
        frame.frame_id = frame_id;
        frame.keyframe = keyframe;
        const bool accepted = reference_.accept(frame);
        if (!accepted || inputs_.empty()) {
            // A local decoder queue drop also breaks the reference chain;
            // the transport reassembler cannot observe this loss.
            if (accepted) reference_.invalidate();
            const auto now = nowMs();
            if (reference_.needs_keyframe() && now - last_key_request_ms_ >= 400) {
                last_key_request_ms_ = now;
                if (requester_) requester_();
            }
            ++frames_dropped_;
            maybeEmitStats();
            return;
        }
        item = inputs_.front();
        inputs_.pop_front();
    }

    ++frames_in_;
    uint8_t* addr = OH_AVBuffer_GetAddr(item.buffer);
    const int32_t cap = OH_AVBuffer_GetCapacity(item.buffer);
    if (addr == nullptr || cap <= 0 || size > static_cast<size_t>(cap)) {
        // 该 input 槽未提交前归还队列，避免泄漏
        {
            std::lock_guard<std::mutex> lk(mtx_);
            inputs_.push_front(item);
            reference_.invalidate();
        }
        ++frames_dropped_;
        if (!overflow_noticed_) {
            overflow_noticed_ = true;
            emitNotice("帧大小超出解码器输入缓冲（" + std::to_string(size) + " > " +
                       std::to_string(cap) + "）", true);
        }
        return;
    }

    std::memcpy(addr, data, size);
    OH_AVCodecBufferAttr attr {};
    attr.pts    = static_cast<int64_t>(timestamp_us);
    attr.size   = static_cast<int32_t>(size);
    attr.offset = 0;
    attr.flags  = keyframe ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVBuffer_SetBufferAttr(item.buffer, &attr);

    if (OH_VideoDecoder_PushInputBuffer(codec_, item.index) != AV_ERR_OK) {
        std::lock_guard<std::mutex> lk(mtx_);
        reference_.invalidate();
        ++frames_dropped_;
        return;
    }
    ++frames_pushed_;
    ++stats_window_count_;
    maybeEmitStats();
}

// ------------------------------------------------------------------ 解码器回调

void HosVideoReceiver::onCodecError(OH_AVCodec* /*codec*/, int32_t code, void* user) {
    auto* self = static_cast<HosVideoReceiver*>(user);
    if (self == nullptr) return;
    PXC_LOGE("onCodecError code=%{public}d", code);
    self->emitNotice("解码器错误 code=" + std::to_string(code) + "，尝试重建", true);
    self->scheduleRestart();
}

void HosVideoReceiver::onStreamChanged(OH_AVCodec* /*codec*/, OH_AVFormat* format, void* user) {
    auto* self = static_cast<HosVideoReceiver*>(user);
    if (self == nullptr || self->sink_ == nullptr) return;
    int32_t w = 0;
    int32_t h = 0;
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w);
    OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h);
    self->sink_("videoState",
                nlohmann::json({{"width", w}, {"height", h}, {"fps", 0}, {"screen", 0},
                                {"encoder", ""}, {"error", ""}})
                    .dump());
}

void HosVideoReceiver::onNeedInputBuffer(OH_AVCodec* /*codec*/, uint32_t index,
                                         OH_AVBuffer* buffer, void* user) {
    auto* self = static_cast<HosVideoReceiver*>(user);
    if (self == nullptr) return;
    std::lock_guard<std::mutex> lk(self->mtx_);
    if (!self->running_) return;
    self->inputs_.push_back(InputItem{index, buffer});
}

void HosVideoReceiver::onNewOutputBuffer(OH_AVCodec* codec, uint32_t index,
                                         OH_AVBuffer* /*buffer*/, void* user) {
    auto* self = static_cast<HosVideoReceiver*>(user);
    if (self == nullptr) return;
    // surface 模式：直接渲染到 XComponent 的 surface
    OH_VideoDecoder_RenderOutputBuffer(codec, index);
}

// ------------------------------------------------------------------ 自愈与统计

void HosVideoReceiver::scheduleRestart() {
    // 先回收上一轮重启线程（可能已结束或仍在跑）；join 必须在锁外，
    // 否则旧线程在 lambda 内等 mtx_ 会死锁
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopping_.load()) {
            return;
        }
    }
    std::thread old;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        old = std::move(restart_thread_);
    }
    if (old.joinable()) {
        old.join();
    }

    std::lock_guard<std::mutex> lk(mtx_);
    if (stopping_.load()) {
        return;
    }
    ++fail_count_;
    if (fail_count_ > kMaxFails) {
        emitNotice("解码器连续重建超过上限，放弃（" + std::to_string(fail_count_ - 1) +
                   " 次）", true);
        return;
    }
    restart_thread_ = std::thread([this] {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            teardownLocked();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kRestartDelayMs));
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!stopping_.load()) {
                ok = setupLocked();
                if (ok) {
                    running_ = true;
                }
            }
        }
        if (ok) {
            if (requester_) requester_();
        } else {
            emitNotice("解码器重建失败，视频已停止", true);
        }
    });
}

void HosVideoReceiver::emitNotice(const std::string& message, bool is_error) {
    if (sink_ == nullptr) return;
    sink_("videoNotice", nlohmann::json({{"message", message},
                                         {"isError", is_error}}).dump());
}

void HosVideoReceiver::maybeEmitStats() {
    const uint64_t now = nowMs();
    if (last_stats_ms_ == 0) {
        last_stats_ms_       = now;
        stats_window_start_  = now;
        stats_window_count_  = 0;
        return;
    }
    if (now - last_stats_ms_ < kStatsPeriodMs) return;
    const uint64_t fps = stats_window_count_ * 1000 / (now - stats_window_start_ + 1);
    last_stats_ms_      = now;
    stats_window_start_ = now;
    stats_window_count_ = 0;
    if (sink_ == nullptr) return;
    sink_("videoStats", nlohmann::json({{"fps", fps},
                                        {"pushed", frames_pushed_},
                                        {"dropped", frames_dropped_}}).dump());
}

}  // namespace pxc::hos
