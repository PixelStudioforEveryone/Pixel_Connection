#pragma once

// 视频解码器：跨平台纯虚接口。
//
// 平台实现：
//   Windows  src/platform/windows/video_decoder_mf.cpp  (Media Foundation H.264)
//   Linux    暂未实现（create_video_decoder 返回 nullptr）
//
// 输入是 video_protocol 分片重组后的 H.264 Annex B 帧，
// 输出统一 BGRA（与 QImage::Format_ARGB32 内存布局一致，渲染端零转换）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pxc {

enum class VideoAccelerationMode { Smart = 0, Hardware = 1, Software = 2 };

struct DecodedFrame {
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t stride = 0;  // 字节步长
    std::vector<uint8_t> bgra;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    // 输入一个完整 H.264 帧（Annex B）。返回 true 表示 out 有效。
    // 返回 false 可能只是「还差后续帧才能出画面」（H.264 特性），
    // 持续失败时用 last_error() 区分。
    virtual bool decode(const uint8_t* data, size_t size, DecodedFrame& out) = 0;

    // 最近一次 decode 产出的输出样本序号（按输入序号计），无输出时
    // 返回 0 且不递增。用于测量解码器内部缓冲深度（延迟定位）。
    virtual uint64_t last_out_index() const { return 0; }

    // 实际采用的解码路径，供控制面板状态说明使用。
    virtual std::string acceleration_name() const { return {}; }

    virtual std::string last_error() const = 0;
};

// 平台工厂；不支持时返回 nullptr 并写入原因。
std::unique_ptr<VideoDecoder> create_video_decoder(
    std::string* error = nullptr,
    VideoAccelerationMode mode = VideoAccelerationMode::Smart);

}  // namespace pxc
