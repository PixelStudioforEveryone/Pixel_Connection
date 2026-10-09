// Linux H.264 解码器：FFmpeg libavcodec（软件解码）。
//
// 输入：video_protocol 重组后的 H.264 Annex B 帧；
// 输出：BGRA（与 QImage::Format_ARGB32 内存布局一致）。
// 解码输出可能是 YUV420P / NV12（取决于流 profile），两种都处理。
//
// 线程约定：decode 只在 VideoReceiver 的回调线程经互斥锁调用。

#if defined(__linux__)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <cstring>
#include <string>

#include "pxc/frame_utils.h"
#include "pxc/video_decoder.h"

namespace pxc {
namespace {

class VideoDecoderFfmpeg : public VideoDecoder {
public:
    VideoDecoderFfmpeg()  = default;
    ~VideoDecoderFfmpeg() override { shutdown(); }

    bool decode(const uint8_t* data, size_t size, DecodedFrame& out) override {
        out = {};
        error_.clear();
        if (!codec_ctx_ && !build()) return false;
        if (size == 0) return false;

        AVPacket* pkt = packet_.get();
        if (av_new_packet(pkt, static_cast<int>(size)) < 0) return fail("分配 packet 失败");
        std::memcpy(pkt->data, data, size);
        pkt->pts = pts_++;
        pkt->dts = pkt->pts;

        int send = avcodec_send_packet(codec_ctx_.get(), pkt);
        av_packet_unref(pkt);
        if (send < 0 && send != AVERROR(EAGAIN)) {
            return fail_ffmpeg("avcodec_send_packet 失败", send);
        }

        bool got_frame = false;
        for (;;) {
            const int recv = avcodec_receive_frame(codec_ctx_.get(), frame_.get());
            if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
            if (recv < 0) return fail_ffmpeg("avcodec_receive_frame 失败", recv);
            if (emit_bgra(frame_.get(), out)) got_frame = true;
            av_frame_unref(frame_.get());
            break;  // 每次调用取一帧足够，剩余帧由后续输入驱动
        }
        return got_frame;
    }

    std::string last_error() const override { return error_; }

private:
    bool build() {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) return fail("找不到 H.264 解码器");

        codec_ctx_.reset(avcodec_alloc_context3(codec));
        if (!codec_ctx_) return fail("分配解码器上下文失败");
        codec_ctx_->thread_count = 4;
        codec_ctx_->thread_type = FF_THREAD_SLICE;
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (avcodec_open2(codec_ctx_.get(), codec, nullptr) < 0) {
            return fail("打开解码器失败");
        }
        frame_.reset(av_frame_alloc());
        packet_.reset(av_packet_alloc());
        if (!frame_ || !packet_) return fail("分配 AVFrame/AVPacket 失败");
        return true;
    }

    bool emit_bgra(AVFrame* frame, DecodedFrame& out) {
        const int w = frame->width;
        const int h = frame->height;
        if (w <= 0 || h <= 0) return false;

        out.width  = static_cast<uint32_t>(w);
        out.height = static_cast<uint32_t>(h);
        out.stride = static_cast<uint32_t>(w) * 4;
        out.bgra.resize(static_cast<size_t>(out.stride) * h);

        if (frame->format == AV_PIX_FMT_YUV420P) {
            pxc::yuv420p_to_bgra(frame->data[0], frame->linesize[0],
                                 frame->data[1], frame->linesize[1],
                                 frame->data[2], frame->linesize[2],
                                 out.width, out.height,
                                 out.bgra.data(), out.stride);
            return true;
        }
        if (frame->format == AV_PIX_FMT_NV12) {
            // NV12 半平面：Y 平面 + 交错 UV 平面
            pxc::nv12_to_bgra(frame->data[0], frame->linesize[0],
                              out.width, out.height,
                              out.bgra.data(), out.stride);
            return true;
        }
        return fail("解码输出了不支持的像素格式 " + std::to_string(frame->format));
    }

    bool fail(const std::string& reason) {
        error_ = reason;
        return false;
    }

    bool fail_ffmpeg(const std::string& reason, int err) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(err, buf, sizeof(buf));
        return fail(reason + ": " + buf);
    }

    void shutdown() {
        codec_ctx_.reset();
        frame_.reset();
        packet_.reset();
    }

    struct AvctxDeleter {
        void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
    };
    struct AvFrameDeleter {
        void operator()(AVFrame* f) const { av_frame_free(&f); }
    };
    struct AvPacketDeleter {
        void operator()(AVPacket* p) const { av_packet_free(&p); }
    };

    std::unique_ptr<AVCodecContext, AvctxDeleter> codec_ctx_;
    std::unique_ptr<AVFrame, AvFrameDeleter>      frame_;
    std::unique_ptr<AVPacket, AvPacketDeleter>    packet_;
    std::string error_;
    int64_t pts_ = 0;
};

}  // namespace

std::unique_ptr<VideoDecoder> create_video_decoder(std::string* error,
                                                   VideoAccelerationMode mode) {
    (void)mode;  // FFmpeg 后端由编译时可用的 codec 决定；当前构建使用 CPU 解码。
    (void)error;
    auto decoder = std::make_unique<VideoDecoderFfmpeg>();
    return decoder;
}

}  // namespace pxc

#endif  // __linux__
