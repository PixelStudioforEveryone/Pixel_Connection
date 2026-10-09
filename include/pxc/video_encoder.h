#pragma once

// 视频编码器：跨平台纯虚接口。
//
// 平台实现：
//   Windows  src/platform/windows/video_encoder_mf.cpp  (Media Foundation H.264)
//   Linux    暂未实现（create_video_encoder 返回 nullptr）
//
// 低延迟约定（第一版就做对，后补很痛苦）：
//   - 关 B 帧、关 lookahead；
//   - CBR 码率，GOP 2~5 秒；
//   - 输入统一 BGRA，内部转 NV12 后交给 MFT。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pxc/video_protocol.h"

namespace pxc {

struct VideoEncoderConfig {
    uint32_t width        = 1920;  // 必须为 2 的倍数
    uint32_t height       = 1080;
    uint32_t fps          = 30;
    uint32_t bitrate_kbps = 8000;
};

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    // (重新)配置编码器。宽高/帧率/码率变化时允许内部重建。
    // 返回 false 时 last_error() 说明原因。
    virtual bool configure(const VideoEncoderConfig& config) = 0;

    // 输入一帧 BGRA（尺寸 src_w×src_h，行距 stride），输出 0..n 个
    // H.264 帧（Annex B）。force_keyframe 为 true 时下一输出必须是
    // 关键帧。没有输出是正常情况（编码器热身/缓冲），调用方按发送
    // 时钟持续喂帧即可。
    //
    // 输入尺寸约定：handles_scaling() 为 true 的实现接受任意输入尺寸
    // （内部一步完成色彩转换+缩放到配置尺寸）；否则输入尺寸必须与
    // 配置尺寸完全一致，实现遇到不匹配必须返回 false 而不是越界读。
    virtual bool encode(const uint8_t* bgra, size_t stride,
                        uint32_t src_w, uint32_t src_h,
                        bool force_keyframe,
                        std::vector<VideoFrame>& out) = 0;

    // 编码器是否内置高效缩放（如 swscale）。为 true 时调用方可跳过
    // 自己的 CPU 缩放，把原始分辨率的帧直接喂给 encode()。
    virtual bool handles_scaling() const { return false; }

    virtual std::string name() const = 0;
    virtual std::string last_error() const = 0;
};

// 平台工厂；不支持时返回 nullptr 并写入原因。
std::unique_ptr<VideoEncoder> create_video_encoder(std::string* error = nullptr);

}  // namespace pxc
