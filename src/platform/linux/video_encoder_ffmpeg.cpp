// Linux H.264 编码器：FFmpeg libavcodec（Ubuntu 自带的 libx264 软编）。
//
// 低延迟配置与 Windows MF 侧对齐：
//   - preset=ultrafast + tune=zerolatency（关 lookahead、关 B 帧）
//   - CBR 目标码率、GOP = 3 秒
//   - 输入 BGRA -> YUV420P 由 libswscale 一步完成（含任意缩放，
//     SIMD 优化）。此前 sender 侧朴素 box filter 缩放 4K->1080p 要
//     33ms/帧、4K->2K 50ms/帧，是 2K/1080p 档跳帧的主因。
//
// 线程约定：configure/encode 只在 VideoSender 的发送线程调用，无需加锁。

#if defined(__linux__)

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#if PXC_HAVE_SWSCALE
// libswscale 运行库随 ffmpeg 必装，但 libswscale-dev（头文件）在
// 部分环境缺失：这里只声明用到的三个函数，直接链 libswscale.so。
extern "C" {
struct SwsContext;
struct SwsFilter;
struct SwsContext* sws_getContext(int srcW, int srcH, enum AVPixelFormat srcFormat,
                                  int dstW, int dstH, enum AVPixelFormat dstFormat,
                                  int flags, struct SwsFilter* srcFilter,
                                  struct SwsFilter* dstFilter, const double* param);
int sws_scale(struct SwsContext* c, const uint8_t* const srcSlice[],
              const int srcStride[], int srcSliceY, int srcSliceH,
              uint8_t* const dst[], const int dstStride[]);
void sws_freeContext(struct SwsContext* swsContext);
}  // extern "C"
// 与 libswscale/swscale.h 一致的算法标志（下采样用 AREA 质量更好）
constexpr int kSwsBilinear = 2;
constexpr int kSwsArea     = 0x20;
#endif  // PXC_HAVE_SWSCALE

#include <cstring>
#include <string>
#include <vector>

#include "pxc/frame_utils.h"
#include "pxc/video_encoder.h"

namespace pxc {
namespace {

class VideoEncoderFfmpeg : public VideoEncoder {
public:
    VideoEncoderFfmpeg()  = default;
    ~VideoEncoderFfmpeg() override { shutdown(); }

    bool configure(const VideoEncoderConfig& config) override {
        if (codec_ctx_ && config.width == config_.width && config.height == config_.height &&
            config.fps == config_.fps && config.bitrate_kbps == config_.bitrate_kbps) {
            return true;
        }
        shutdown();
        config_ = config;
        return build();
    }

    bool encode(const uint8_t* bgra, size_t stride, uint32_t src_w, uint32_t src_h,
                bool force_keyframe, std::vector<VideoFrame>& out) override {
        out.clear();
        if (!codec_ctx_) return fail("编码器未配置");
        if (src_w == 0 || src_h == 0 || src_w % 2 || src_h % 2) {
            return fail("输入帧尺寸无效");
        }

        // 平面布局：Y | U | V（连续单块，linesize 即平面宽度）
        const size_t need = static_cast<size_t>(config_.width) * config_.height;
        if (yuv_.size() != need * 3 / 2) yuv_.resize(need * 3 / 2);
        uint8_t* y_plane = yuv_.data();
        uint8_t* u_plane = y_plane + need;
        uint8_t* v_plane = u_plane + need / 4;
        frame_->data[0]    = y_plane;
        frame_->data[1]    = u_plane;
        frame_->data[2]    = v_plane;
        frame_->linesize[0] = static_cast<int>(config_.width);
        frame_->linesize[1] = static_cast<int>(config_.width / 2);
        frame_->linesize[2] = static_cast<int>(config_.width / 2);

#if PXC_HAVE_SWSCALE
        // sws 上下文按 (源尺寸 -> 配置尺寸) 缓存，任一变化时重建
        if (!sws_ || sws_src_w_ != src_w || sws_src_h_ != src_h ||
            sws_dst_w_ != config_.width || sws_dst_h_ != config_.height) {
            // BILINEAR：实测 4K->1080p 约 4ms（AREA 约 10ms；60fps 预算
            // 只有 16.6ms，实时优先）
            sws_.reset(sws_getContext(
                static_cast<int>(src_w), static_cast<int>(src_h), AV_PIX_FMT_BGRA,
                static_cast<int>(config_.width), static_cast<int>(config_.height),
                AV_PIX_FMT_YUV420P, kSwsBilinear, nullptr, nullptr, nullptr));
            if (!sws_) return fail("创建 swscale 上下文失败");
            sws_src_w_ = src_w;
            sws_src_h_ = src_h;
            sws_dst_w_ = config_.width;
            sws_dst_h_ = config_.height;
        }
        const uint8_t* src_slice[1]  = {bgra};
        const int      src_linesize[1] = {static_cast<int>(stride)};
        sws_scale(sws_.get(), src_slice, src_linesize, 0,
                  static_cast<int>(src_h), frame_->data, frame_->linesize);
#else
        // 无 swscale：只接受与配置一致的输入（sender 侧负责缩放）
        if (src_w != config_.width || src_h != config_.height) {
            return fail("输入帧尺寸与编码器配置不一致");
        }
        pxc::bgra_to_yuv420p(bgra, stride, config_.width, config_.height,
                             y_plane, config_.width,
                             u_plane, config_.width / 2,
                             v_plane, config_.width / 2);
#endif

        frame_->pts = pts_;
        // 强制关键帧：x264 尊重 pict_type 覆盖
        frame_->pict_type = force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        ++pts_;

        const int send = avcodec_send_frame(codec_ctx_.get(), frame_.get());
        if (send == AVERROR_EOF) return true;   // 编码器已冲刷，等下一轮
        if (send < 0) return fail_ffmpeg("avcodec_send_frame 失败", send);

        for (;;) {
            AVPacket* pkt = packet_.get();
            const int recv = avcodec_receive_packet(codec_ctx_.get(), pkt);
            if (recv == AVERROR(EAGAIN) || recv == AVERROR_EOF) break;
            if (recv < 0) return fail_ffmpeg("avcodec_receive_packet 失败", recv);

            VideoFrame frame;
            frame.frame_id     = static_cast<uint32_t>(out_frames_++);
            frame.timestamp_us = static_cast<uint64_t>(pkt->pts) *
                                 1000000 * time_base_num_ / time_base_den_;
            frame.codec        = VideoCodec::H264;
            frame.keyframe     = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            frame.payload.assign(pkt->data, pkt->data + pkt->size);
            out.push_back(std::move(frame));
        }
        return true;
    }

    // 内置 swscale：接受任意输入尺寸，调用方可跳过自己的 CPU 缩放。
    // 无 swscale 编译时返回 false（sender 走朴素缩放路径）。
    bool handles_scaling() const override { return kHaveSws; }

    std::string name() const override { return name_; }
    std::string last_error() const override { return error_; }

private:
    bool build() {
        const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!codec) return fail("找不到 H.264 编码器（libavcodec 无编码器）");

        codec_ctx_.reset(avcodec_alloc_context3(codec));
        if (!codec_ctx_) return fail("分配编码器上下文失败");

        codec_ctx_->width     = static_cast<int>(config_.width);
        codec_ctx_->height    = static_cast<int>(config_.height);
        codec_ctx_->pix_fmt   = AV_PIX_FMT_YUV420P;
        codec_ctx_->time_base = AVRational{1, static_cast<int>(config_.fps)};
        codec_ctx_->framerate = AVRational{static_cast<int>(config_.fps), 1};
        codec_ctx_->gop_size  = static_cast<int>(config_.fps) * 3;  // 关键帧间隔 3 秒
        codec_ctx_->max_b_frames = 0;                               // 关 B 帧
        codec_ctx_->bit_rate  = static_cast<int64_t>(config_.bitrate_kbps) * 1000;
        codec_ctx_->thread_count = 8;  // slice 并行（zerolatency 下无帧延迟）

        // libx264 低延迟档；找不到 libx264 时这些选项会被忽略
        av_opt_set(codec_ctx_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);
        // Recovery frames must discard all prior references, not just be intra-coded.
        av_opt_set(codec_ctx_->priv_data, "forced-idr", "1", 0);

        if (avcodec_open2(codec_ctx_.get(), codec, nullptr) < 0) {
            return fail("打开编码器失败");
        }

        time_base_num_ = codec_ctx_->time_base.num > 0 ? codec_ctx_->time_base.num : 1;
        time_base_den_ = codec_ctx_->time_base.den > 0 ? codec_ctx_->time_base.den
                                                       : static_cast<int>(config_.fps);

        frame_.reset(av_frame_alloc());
        if (!frame_) return fail("分配 AVFrame 失败");
        frame_->format = AV_PIX_FMT_YUV420P;
        frame_->width  = codec_ctx_->width;
        frame_->height = codec_ctx_->height;
        // data/linesize 每次 encode 时指向 yuv_ 缓冲，不单独分配
        packet_.reset(av_packet_alloc());
        if (!packet_) return fail("分配 AVPacket 失败");

        const char* codec_name = codec->name ? codec->name : "h264";
        name_ = std::string("h264-") + codec_name + (kHaveSws ? "+sws" : "");
        return true;
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
#if PXC_HAVE_SWSCALE
        sws_.reset();
        sws_src_w_ = sws_src_h_ = sws_dst_w_ = sws_dst_h_ = 0;
#endif
        yuv_.clear();
        // pts_/out_frames_ 刻意不归零：切档（分辨率/帧率/码率变化）会
        // 走 shutdown()+build() 重建，若 frame_id 从 0 重来，接收端
        // 重组器会把"比 newest 旧"的帧号全部拒绝——永久冻屏。
        // 重建只发生在同一编码器对象内，跨会话是全新对象+全新接收端。
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
#if PXC_HAVE_SWSCALE
    struct SwsDeleter {
        void operator()(SwsContext* s) const { sws_freeContext(s); }
    };
    static constexpr bool kHaveSws = true;
#else
    static constexpr bool kHaveSws = false;
#endif

    std::unique_ptr<AVCodecContext, AvctxDeleter> codec_ctx_;
    std::unique_ptr<AVFrame, AvFrameDeleter>      frame_;
    std::unique_ptr<AVPacket, AvPacketDeleter>    packet_;
#if PXC_HAVE_SWSCALE
    std::unique_ptr<SwsContext, SwsDeleter>       sws_;
    uint32_t sws_src_w_ = 0, sws_src_h_ = 0;
    uint32_t sws_dst_w_ = 0, sws_dst_h_ = 0;
#endif
    std::vector<uint8_t> yuv_;
    std::string name_  = "h264-ffmpeg";
    std::string error_;
    VideoEncoderConfig config_;
    int64_t pts_ = 0;
    uint64_t out_frames_ = 0;
    int time_base_num_ = 1;
    int time_base_den_ = 30;
};

}  // namespace

std::unique_ptr<VideoEncoder> create_video_encoder(std::string* error) {
    auto encoder = std::make_unique<VideoEncoderFfmpeg>();
    return encoder;
}

}  // namespace pxc

#endif  // __linux__
