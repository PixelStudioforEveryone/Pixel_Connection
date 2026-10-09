// Windows H.264 解码器：Media Foundation MFT（同步软件解码）。
//
// 只枚举同步 MFT（MFT_ENUM_FLAG_SYNCMFT = 系统自带的 Microsoft DTV-DVD
// 视频解码器），处理路径简单可靠，CPU 解码 NV12 足够 4K@60。
// 硬件解码（D3D11VA/异步 MFT）以后需要 D3D 渲染路径时再接。
//
// 输入：video_protocol 重组后的 H.264 Annex B 帧；
// 输出：BGRA（与 QImage::Format_ARGB32 内存布局一致）。

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>

#include <icodecapi.h>

#ifndef CODECAPI_AVLowLatencyMode
// 旧 SDK 未定义：CODECAPI_AVLowLatencyMode {9C27891A-ED7A-40e1-88E8-B89527CC097C}
static const GUID kAvLowLatencyModeGuid =
    {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb8, 0x95, 0x27, 0xcc, 0x09, 0x7c}};
#define CODECAPI_AVLowLatencyMode kAvLowLatencyModeGuid
#endif

#include <cstring>
#include <string>

#include "pxc/frame_utils.h"
#include "pxc/video_decoder.h"

namespace pxc {
namespace {

using Microsoft::WRL::ComPtr;

struct MfRuntime {
    bool ok = false;
    MfRuntime()  { ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)); }
    ~MfRuntime() { if (ok) MFShutdown(); }
};

class VideoDecoderMf : public VideoDecoder {
public:
    explicit VideoDecoderMf(VideoAccelerationMode mode) : mode_(mode) {
        runtime_ = std::make_unique<MfRuntime>();
        ready_   = runtime_->ok;
        if (!ready_) error_ = "MFStartup 失败";
    }
    ~VideoDecoderMf() override { shutdown(); }

    bool decode(const uint8_t* data, size_t size, DecodedFrame& out) override {
        if (decode_once(data, size, out)) return true;
        if (mode_ == VideoAccelerationMode::Smart &&
            selected_acceleration_ == "智能选择（硬件解码）" && !error_.empty()) {
            const std::string hardware_error = error_;
            shutdown();
            error_.clear();
            if (build_with_flags(MFT_ENUM_FLAG_SYNCMFT)) {
                selected_acceleration_ = "智能选择（软件回退）";
                error_.clear();
                if (decode_once(data, size, out)) return true;
                if (error_.empty()) return false;
                return fail("硬件解码运行失败（" + hardware_error + "）；软件回退失败（" + error_ + "）");
            }
            return fail("硬件解码运行失败（" + hardware_error + "）；软件回退失败（" + error_ + "）");
        }
        return false;
    }

    bool decode_once(const uint8_t* data, size_t size, DecodedFrame& out) {
        out = {};
        error_.clear();
        if (!ready_) return false;
        if (!mft_ && !build()) return false;
        if (size == 0) return false;

        // 输入样本
        ComPtr<IMFSample> sample;
        if (FAILED(MFCreateSample(&sample))) return fail("MFCreateSample 失败");
        ComPtr<IMFMediaBuffer> in_buffer;
        if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), &in_buffer))) {
            return fail("MFCreateMemoryBuffer 失败");
        }
        BYTE* dst = nullptr;
        if (FAILED(in_buffer->Lock(&dst, nullptr, nullptr))) return fail("Lock 失败");
        std::memcpy(dst, data, size);
        in_buffer->Unlock();
        in_buffer->SetCurrentLength(static_cast<DWORD>(size));
        sample->AddBuffer(in_buffer.Get());
        sample->SetSampleTime(static_cast<LONGLONG>(pts_us_ * 10));
        pts_us_ += 33000;  // 名义 30fps，仅用于 MFT 内部时间戳排序

        bool have_output = false;
        HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // 输入队列满：先排空再重试一次（可能已得到一帧）
            have_output = drain(out);
            hr = mft_->ProcessInput(0, sample.Get(), 0);
        }
        if (FAILED(hr)) {
            if (have_output) return true;  // 已有画面，先交上去
            return fail("ProcessInput 失败 (hr=" + std::to_string(hr) + ")");
        }

        // 排空所有输出，保留最新一帧；无新输出时保留先前结果
        if (drain(out)) have_output = true;
        return have_output;
    }

    std::string last_error() const override { return error_; }
    std::string acceleration_name() const override { return selected_acceleration_; }

    // 输出样本对应的输入序号+1（0 = 尚无输出）
    uint64_t last_out_index() const override { return last_out_index_; }

private:
    bool build() {
        constexpr UINT32 hardware_flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT;
        constexpr UINT32 software_flags = MFT_ENUM_FLAG_SYNCMFT;
        if (mode_ == VideoAccelerationMode::Software) {
            if (!build_with_flags(software_flags)) return false;
            selected_acceleration_ = "软件解码";
            return true;
        }
        if (mode_ == VideoAccelerationMode::Hardware) {
            if (!build_with_flags(hardware_flags)) return false;
            selected_acceleration_ = "硬件解码";
            return true;
        }
        if (build_with_flags(hardware_flags)) {
            selected_acceleration_ = "智能选择（硬件解码）";
            return true;
        }
        const std::string hardware_error = error_;
        shutdown();
        error_.clear();
        if (build_with_flags(software_flags)) {
            selected_acceleration_ = "智能选择（软件回退）";
            return true;
        }
        return fail("硬件解码不可用（" + hardware_error + "）；软件回退失败（" + error_ + "）");
    }

    bool build_with_flags(UINT32 enum_flags) {
        MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_H264};
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        // 只枚举同步软件解码器，处理路径简单且行为确定
        if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, enum_flags,
                             &in_info, nullptr, &activates, &count)) ||
            count == 0) {
            if (activates) CoTaskMemFree(activates);
            return fail("找不到 H.264 解码器");
        }

        HRESULT hr = activates[0]->ActivateObject(IID_IMFTransform, &mft_);
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr)) return fail("激活解码器失败");

        ComPtr<IMFMediaType> input;
        if (FAILED(MFCreateMediaType(&input))) return fail("创建输入类型失败");
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (FAILED(mft_->SetInputType(0, input.Get(), 0))) return fail("设置输入类型失败");

        // 在可用输出类型里挑 NV12
        bool found = false;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> avail;
            if (FAILED(mft_->GetOutputAvailableType(0, i, &avail))) break;
            GUID subtype{};
            if (SUCCEEDED(avail->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12) {
                if (SUCCEEDED(mft_->SetOutputType(0, avail.Get(), 0))) found = true;
                break;
            }
        }
        if (!found) return fail("解码器不支持 NV12 输出");

        // 低延迟模式：MS H.264 解码器默认按自己的重排/缓冲启发式攒帧，
        // 实测内部能积压几十帧（30fps 下即数秒延迟）。MF_LOW_LATENCY +
        // AVLowLatencyMode 让它解码一帧立即吐一帧。
        {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(mft_->GetAttributes(&attrs)) && attrs) {
                attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            }
            ComPtr<ICodecAPI> codec;
            if (SUCCEEDED(mft_.As(&codec))) {
                VARIANT v;
                v.vt     = VT_BOOL;
                v.boolVal = VARIANT_TRUE;
                codec->SetValue(&CODECAPI_AVLowLatencyMode, &v);
            }
        }

        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool drain(DecodedFrame& out) {
        bool got = false;
        for (;;) {
            MFT_OUTPUT_STREAM_INFO info{};
            mft_->GetOutputStreamInfo(0, &info);
            const bool provides = (info.dwFlags &
                                   (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                    MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

            ComPtr<IMFSample> allocated;
            MFT_OUTPUT_DATA_BUFFER buffer{};
            buffer.dwStreamID = 0;
            if (!provides) {
                if (FAILED(MFCreateSample(&allocated))) return fail("MFCreateSample 失败");
                ComPtr<IMFMediaBuffer> buf;
                if (info.cbSize && FAILED(MFCreateMemoryBuffer(info.cbSize, &buf))) {
                    return fail("输出缓冲分配失败");
                }
                if (buf) allocated->AddBuffer(buf.Get());
                buffer.pSample = allocated.Get();
            }

            DWORD status = 0;
            const HRESULT hr = mft_->ProcessOutput(0, 1, &buffer, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return got;  // 正常：等下一帧
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!renegotiate()) return got;
                continue;
            }
            if (FAILED(hr)) {
                // 已拿到过输出时不再报错，先把画面交上去
                if (got) return true;
                return fail("ProcessOutput 失败 (hr=" + std::to_string(hr) + ")");
            }

            ComPtr<IMFSample> sample;
            if (buffer.pSample && (!allocated || buffer.pSample != allocated.Get())) {
                sample.Attach(buffer.pSample);
            } else {
                sample = allocated;
            }
            if (!sample) continue;
            // 取空所有积压输出、只保留最新一帧：MFT 在流切换/预热后
            // 可能一次吐多帧，若每次只取一帧，旧帧会滞留在解码器内
            // 队列里造成持续延迟
            DecodedFrame frame;
            if (emit_nv12(sample.Get(), frame)) {
                out = std::move(frame);
                got = true;
            } else if (!got) {
                return false;
            }
        }
    }

    bool renegotiate() {
        // 流格式变化（关键帧带了新分辨率）：读回当前输出类型并重设
        ComPtr<IMFMediaType> type;
        if (FAILED(mft_->GetOutputAvailableType(0, 0, &type)) || !type) {
            return fail("读取新的输出类型失败");
        }
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) {
            // 类型变了还得挑 NV12
            bool found = false;
            for (DWORD i = 0; ; ++i) {
                ComPtr<IMFMediaType> avail;
                if (FAILED(mft_->GetOutputAvailableType(0, i, &avail))) break;
                GUID st{};
                if (SUCCEEDED(avail->GetGUID(MF_MT_SUBTYPE, &st)) && st == MFVideoFormat_NV12) {
                    type = avail;
                    found = true;
                    break;
                }
            }
            if (!found) return fail("解码器切换输出类型失败");
        }
        if (FAILED(mft_->SetOutputType(0, type.Get(), 0))) return fail("重设输出类型失败");
        width_ = height_ = 0;  // 触发 emit 时重新读取尺寸
        return true;
    }

    bool emit_nv12(IMFSample* sample, DecodedFrame& out) {
        if (width_ == 0 || height_ == 0) {
            ComPtr<IMFMediaType> type;
            if (FAILED(mft_->GetOutputCurrentType(0, &type)) || !type) {
                return fail("读取输出类型失败");
            }
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
            if (w == 0 || h == 0) return fail("输出分辨率未知");
            width_ = w;
            height_ = h;
        }

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return fail("取输出缓冲失败");
        BYTE* src = nullptr;
        DWORD len = 0;
        if (FAILED(buffer->Lock(&src, nullptr, &len))) return fail("输出缓冲 Lock 失败");

        out.width  = width_;
        out.height = height_;
        out.stride = width_ * 4;
        out.bgra.resize(static_cast<size_t>(out.stride) * height_);
        const size_t need = static_cast<size_t>(width_) * height_ * 3 / 2;
        if (len < need) {
            error_ = "NV12 输出比预期小";
            buffer->Unlock();
            return false;
        }
        pxc::nv12_to_bgra(src, width_, width_, height_, out.bgra.data(), out.stride);
        buffer->Unlock();
        // 记录输出样本序号（SetSampleTime 的 pts 换算），供上层测解码器
        // 内部缓冲深度
        LONGLONG t = 0;
        if (SUCCEEDED(sample->GetSampleTime(&t)) && t >= 0) {
            last_out_index_ = static_cast<uint64_t>(t / 10 / 33000) + 1;
        }
        return true;
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
        width_ = height_ = 0;
    }

    std::unique_ptr<MfRuntime> runtime_;
    VideoAccelerationMode mode_ = VideoAccelerationMode::Smart;
    std::string selected_acceleration_;
    ComPtr<IMFTransform> mft_;
    std::string error_;
    UINT32   width_  = 0;
    UINT32   height_ = 0;
    uint64_t pts_us_ = 0;
    uint64_t last_out_index_ = 0;
    bool     ready_  = false;
};

}  // namespace

std::unique_ptr<VideoDecoder> create_video_decoder(std::string* error,
                                                   VideoAccelerationMode mode) {
    auto decoder = std::make_unique<VideoDecoderMf>(mode);
    if (!decoder->last_error().empty() && error) *error = decoder->last_error();
    return decoder;
}

}  // namespace pxc

#endif  // _WIN32
