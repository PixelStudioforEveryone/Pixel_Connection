// Windows H.264 编码器：Media Foundation MFT。
//
// 选择策略：先枚举硬件编码器（NVIDIA/AMD/Intel），失败回退到系统软件编码器。
// 两种 MFT（同步/异步）都支持：硬件 MFT 通常是异步（事件驱动），
// 软件的 Microsoft H264 Encoder MFT 走同步 ProcessInput/ProcessOutput。
//
// 低延迟配置（第一天就做对）：
//   - 关 B 帧（AVEncMPVDefaultBPictureCount = 0）
//   - CBR 码率 + 低延迟模式
//   - GOP = 3 秒（关键帧间隔 2~5 秒）
//   - 输入 BGRA -> NV12 转换后喂给 MFT（编码器对 NV12 支持最广）

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// ICodecAPI 声明在 strmif.h（DirectShow 基接头文件），codecapi.h 只含 GUID/枚举
#include <strmif.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <utility>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "pxc/frame_utils.h"
#include "pxc/video_encoder.h"

namespace pxc {
namespace {

using Microsoft::WRL::ComPtr;

struct MfRuntime {
    bool ok = false;
    MfRuntime()  { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfRuntime() { if (ok) MFShutdown(); }
};

bool copy_activate_name(IMFActivate* activate, std::string& name) {
    WCHAR buffer[256] = {};
    UINT32 len = 0;
    if (FAILED(activate->GetString(MFT_FRIENDLY_NAME_Attribute, buffer, 256, &len))) {
        return false;
    }
    for (UINT32 i = 0; i < len && i < 255; ++i) {
        name.push_back(static_cast<char>(buffer[i]));
    }
    return true;
}

// 枚举全部编码器 MFT（先硬件后软件）。每个候选都要经过完整 setup 尝试，
// 因为部分硬件 MFT 在系统内存输入下 SetInputType 会失败，需回退软件编码器。
void collect_encoder_mfts(std::vector<std::pair<ComPtr<IMFTransform>, std::string>>& out) {
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_H264};

    const UINT32 flag_sets[] = {
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,  // 硬件优先
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
            MFT_ENUM_FLAG_SORTANDFILTER,                        // 软件兜底
    };

    std::set<std::string> seen;
    for (const UINT32 flags : flag_sets) {
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
                             &in_info, &out_info, &activates, &count)) ||
            count == 0) {
            if (activates) CoTaskMemFree(activates);
            continue;
        }

        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IMFTransform> candidate;
            std::string candidate_name;
            if (SUCCEEDED(activates[i]->ActivateObject(IID_IMFTransform, &candidate)) &&
                candidate) {
                copy_activate_name(activates[i], candidate_name);
                if (seen.insert(candidate_name).second) {
                    out.push_back({std::move(candidate), candidate_name});
                }
            }
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
}

ComPtr<IMFMediaType> make_video_type(const GUID& subtype, uint32_t w, uint32_t h,
                                     uint32_t fps, uint32_t avg_bitrate) {
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))) return nullptr;
    if (FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))) return nullptr;
    if (FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype))) return nullptr;
    if (FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h))) return nullptr;
    if (FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps, 1))) return nullptr;
    if (FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) return nullptr;
    if (FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) {
        return nullptr;
    }
    if (FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(w)))) {
        return nullptr;  // 内存 buffer 的 NV12 行距 = 宽度
    }
    if (avg_bitrate &&
        FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, avg_bitrate))) {
        return nullptr;
    }
    return type;
}

class VideoEncoderMf : public VideoEncoder {
public:
    explicit VideoEncoderMf(std::string* error) {
        runtime_ = std::make_unique<MfRuntime>();
        if (!runtime_->ok && error) *error = "MFStartup 失败";
        ready_ = runtime_->ok;
    }
    ~VideoEncoderMf() override { shutdown(); }

    bool configure(const VideoEncoderConfig& config) override {
        if (!ready_) return fail("Media Foundation 不可用");
        if (configured_ && config.width == config_.width && config.height == config_.height &&
            config.fps == config_.fps && config.bitrate_kbps == config_.bitrate_kbps) {
            return true;
        }
        shutdown();
        config_ = config;
        if (!build()) return false;
        configured_ = true;
        return true;
    }

    bool encode(const uint8_t* bgra, size_t stride, uint32_t src_w, uint32_t src_h,
                bool force_keyframe, std::vector<VideoFrame>& out) override {
        out.clear();
        if (!configured_) return fail("编码器未配置");

        // 分辨率与配置不一致（画质切换/采集端变化）直接报错，由上层
        // 重新 configure；绝不能按配置尺寸去读不匹配的输入缓冲——
        // 静止画面切画质时空闲帧是旧档位尺寸，越界读会直接崩溃。
        if (src_w != config_.width || src_h != config_.height ||
            nv12_.size() != nv12_size()) {
            return fail("输入帧尺寸与编码器配置不一致");
        }
        pxc::bgra_to_nv12(bgra, stride, config_.width, config_.height,
                          nv12_.data(), config_.width);

        if (force_keyframe) request_keyframe();

        if (async_) return encode_async(out);
        return encode_sync(out);
    }

    std::string name() const override { return name_; }
    std::string last_error() const override { return error_; }

private:
    size_t nv12_size() const {
        return static_cast<size_t>(config_.width) * config_.height * 3 / 2;
    }

    bool build() {
        // 枚举全部候选（硬件优先），逐个尝试完整 setup；
        // 部分硬件 MFT 在系统内存输入下 SetInputType 会失败，必须回退软件编码器
        std::vector<std::pair<ComPtr<IMFTransform>, std::string>> candidates;
        collect_encoder_mfts(candidates);
        if (candidates.empty()) return fail("找不到可用的 H.264 编码器");

        std::string last_reason = "无候选";
        for (auto& [candidate, candidate_name] : candidates) {
            shutdown();  // 清理上一个候选的残留状态
            if (setup_mft(candidate, candidate_name, last_reason)) {
                mft_ = std::move(candidate);
                name_ = candidate_name;
                configured_ = true;
                return true;
            }
            last_reason += "（" + candidate_name + "）";
        }
        return fail(last_reason);
    }

    bool setup_mft(ComPtr<IMFTransform>& candidate, const std::string& candidate_name,
                   std::string& reason) {
        mft_ = candidate;
        name_ = candidate_name;
        async_ = false;
        events_.Reset();
        nv12_.clear();

        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
            UINT32 async = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async) {
                if (FAILED(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, 1))) {
                    reason = "异步编码器解锁失败";
                    return false;
                }
                async_ = true;
                mft_->QueryInterface(IID_PPV_ARGS(&events_));
            }
        }

        // 顺序兼容：部分编码 MFT（含 MS 软件 H264）要求先设输出类型
        auto output = make_video_type(MFVideoFormat_H264, config_.width, config_.height,
                                      config_.fps, config_.bitrate_kbps * 1000);
        if (!output) {
            reason = "创建输出类型失败";
            return false;
        }
        const HRESULT out_hr = mft_->SetOutputType(0, output.Get(), 0);
        if (FAILED(out_hr)) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08lX",
                          static_cast<unsigned long>(out_hr));
            reason = std::string("设置输出类型失败 hr=") + hex;
            return false;
        }

        auto input = make_video_type(MFVideoFormat_NV12, config_.width, config_.height,
                                     config_.fps, 0);
        if (!input) {
            reason = "创建输入类型失败";
            return false;
        }
        const HRESULT in_hr = mft_->SetInputType(0, input.Get(), 0);
        if (FAILED(in_hr)) {
            char hex[16];
            std::snprintf(hex, sizeof(hex), "0x%08lX",
                          static_cast<unsigned long>(in_hr));
            reason = std::string("设置输入类型失败 hr=") + hex;
            return false;
        }

        apply_low_latency();

        nv12_.resize(nv12_size());

        // 通知 MFT 流开始；异步 MFT 之后才会发 NEED_INPUT/HAVE_OUTPUT 事件
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    void apply_low_latency() {
        ComPtr<ICodecAPI> codec;
        if (FAILED(mft_->QueryInterface(IID_PPV_ARGS(&codec))) || !codec) return;

        auto set_u32 = [&](const GUID& api, UINT32 value) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = value;
            codec->SetValue(&api, &v);  // 不支持的项静默忽略
            VariantClear(&v);
        };

        set_u32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        set_u32(CODECAPI_AVEncCommonMeanBitRate, config_.bitrate_kbps * 1000);
        set_u32(CODECAPI_AVEncCommonLowLatency, VARIANT_TRUE);
        set_u32(CODECAPI_AVLowLatencyMode, VARIANT_TRUE);
        set_u32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);  // 关 B 帧
        set_u32(CODECAPI_AVEncMPVGOPSize, config_.fps * 3);  // 关键帧间隔 3 秒
    }

    void request_keyframe() {
        ComPtr<ICodecAPI> codec;
        if (FAILED(mft_->QueryInterface(IID_PPV_ARGS(&codec))) || !codec) return;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 1;
        codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        VariantClear(&v);
    }

    bool make_input_sample(ComPtr<IMFSample>& sample) {
        sample.Reset();
        if (FAILED(MFCreateSample(&sample))) return false;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(nv12_.size()), &buffer))) {
            return false;
        }
        BYTE* dst = nullptr;
        if (FAILED(buffer->Lock(&dst, nullptr, nullptr))) return false;
        std::memcpy(dst, nv12_.data(), nv12_.size());
        buffer->Unlock();
        if (FAILED(buffer->SetCurrentLength(static_cast<DWORD>(nv12_.size())))) return false;
        if (FAILED(sample->AddBuffer(buffer.Get()))) return false;

        const LONGLONG hundred_ns = static_cast<LONGLONG>(pts_us_ * 10);
        sample->SetSampleTime(hundred_ns);
        sample->SetSampleDuration(static_cast<LONGLONG>(10000000LL / config_.fps));
        pts_us_ += 1000000ULL / config_.fps;
        return true;
    }

    // 异步 MFT：事件驱动。编码器攒着输入攒够才吐输出，这里以「事件 + 有界等待」
    // 的方式每帧驱动一次，输出可能滞后 1~2 帧（正常）。
    bool encode_async(std::vector<VideoFrame>& out) {
        if (!events_) return fail("异步编码器缺少事件接口");

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
        while (!need_input_ && !have_output_) {
            if (!pump(std::chrono::milliseconds(20))) break;
            if (std::chrono::steady_clock::now() > deadline) break;
        }
        drain_outputs(out);

        if (!need_input_) return true;  // 本帧被丢弃，等下一个发送时钟
        need_input_ = 0;

        ComPtr<IMFSample> sample;
        if (!make_input_sample(sample)) return fail("创建输入样本失败");
        const HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) return true;  // 输入队列满，丢本帧
        if (FAILED(hr)) return fail("ProcessInput 失败 (hr=" + std::to_string(hr) + ")");

        drain_outputs(out);
        return true;
    }

    bool encode_sync(std::vector<VideoFrame>& out) {
        ComPtr<IMFSample> sample;
        if (!make_input_sample(sample)) return fail("创建输入样本失败");
        const HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr) && hr != MF_E_NOTACCEPTING) {
            return fail("ProcessInput 失败 (hr=" + std::to_string(hr) + ")");
        }
        drain_outputs(out);
        return true;
    }

    void drain_outputs(std::vector<VideoFrame>& out) {
        for (;;) {
            if (async_ && have_output_ == 0) {
                if (!pump(std::chrono::milliseconds(5))) break;
                if (have_output_ == 0) break;
            }

            MFT_OUTPUT_STREAM_INFO stream_info{};
            mft_->GetOutputStreamInfo(0, &stream_info);
            const bool provides = (stream_info.dwFlags &
                                   (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

            ComPtr<IMFSample> allocated;
            MFT_OUTPUT_DATA_BUFFER buffer{};
            buffer.dwStreamID = 0;
            if (!provides) {
                if (FAILED(MFCreateSample(&allocated))) return;
                ComPtr<IMFMediaBuffer> buf;
                if (stream_info.cbSize &&
                    FAILED(MFCreateMemoryBuffer(stream_info.cbSize, &buf))) return;
                if (buf) allocated->AddBuffer(buf.Get());
                buffer.pSample = allocated.Get();
            }

            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &buffer, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) { need_input_ = 1; break; }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                renegotiate_output();
                continue;
            }
            if (FAILED(hr)) { fail("ProcessOutput 失败 (hr=" + std::to_string(hr) + ")"); return; }
            if (async_ && have_output_ > 0) --have_output_;

            // MFT 返回的样本指针已带一个引用，用 Attach 接管避免双重计数
            ComPtr<IMFSample> sample;
            if (buffer.pSample && (!allocated || buffer.pSample != allocated.Get())) {
                sample.Attach(buffer.pSample);
            } else {
                sample = allocated;
            }
            if (sample) collect_frame(sample.Get(), out);
        }
    }

    void renegotiate_output() {
        ComPtr<IMFMediaType> type;
        if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &type)) && type) {
            mft_->SetOutputType(0, type.Get(), 0);
        }
    }

    void collect_frame(IMFSample* sample, std::vector<VideoFrame>& out) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &len))) return;

        VideoFrame frame;
        frame.frame_id     = static_cast<uint32_t>(out_frames_++);
        frame.timestamp_us = pts_us_;
        frame.codec        = VideoCodec::H264;
        UINT32 clean = 0;
        frame.keyframe = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) &&
                         clean != 0;
        frame.payload.assign(reinterpret_cast<const uint8_t*>(data),
                             reinterpret_cast<const uint8_t*>(data) + len);
        buffer->Unlock();
        if (!frame.payload.empty()) out.push_back(std::move(frame));
    }

    // 事件泵：非阻塞轮询，最多等 timeout。返回 true 表示拿到了事件。
    bool pump(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            IMFMediaEvent* raw_event = nullptr;
            const HRESULT hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &raw_event);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                ::Sleep(1);
                continue;
            }
            if (FAILED(hr) || !raw_event) return false;

            MediaEventType type = MEUnknown;
            raw_event->GetType(&type);
            raw_event->Release();

            switch (type) {
                case METransformNeedInput:  ++need_input_;  break;
                case METransformHaveOutput: ++have_output_; break;
                default: break;  // DrainComplete 等在单帧驱动模式下不关心
            }
            return true;
        }
    }

    bool fail(const std::string& reason) {
        error_ = reason;
        return false;
    }

    void shutdown() {
        if (mft_) {
            mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        mft_.Reset();
        events_.Reset();
        async_ = false;
        configured_ = false;
        need_input_ = 0;
        have_output_ = 0;
        nv12_.clear();
    }

    std::unique_ptr<MfRuntime> runtime_;
    ComPtr<IMFTransform>        mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    std::string  name_ = "h264-mf";
    std::string  error_;
    VideoEncoderConfig config_;
    std::vector<uint8_t> nv12_;
    bool  ready_      = false;
    bool  configured_ = false;
    bool  async_      = false;
    int   need_input_  = 0;
    int   have_output_ = 0;
    uint64_t pts_us_    = 0;
    uint64_t out_frames_ = 0;
};

}  // namespace

std::unique_ptr<VideoEncoder> create_video_encoder(std::string* error) {
    auto encoder = std::make_unique<VideoEncoderMf>(error);
    if (!encoder->last_error().empty() && error && error->empty()) {
        *error = encoder->last_error();
    }
    // 构造失败也要返回对象：configure() 会给出明确错误，调用方据此降级
    return encoder;
}

}  // namespace pxc

#endif  // _WIN32
