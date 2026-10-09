// Linux 编码/解码占位实现。
//
// 按项目规划（docs/WINDOWS_TASKS.md），视频编码解码先在 Windows 侧落地；
// Linux 侧后续接入 FFmpeg/libavcodec 时替换这里的工厂函数即可，
// 接口（include/pxc/video_encoder.h、video_decoder.h）不变。
//
// 在此之前，Linux 作为被控端/主控端的界面、控制中心（屏幕/画质/帧率）
// 与信令全部可用，只有视频流会明确报「暂不支持」。

#if defined(__linux__)

#include <string>

#include "pxc/video_decoder.h"
#include "pxc/video_encoder.h"

namespace pxc {

std::unique_ptr<VideoEncoder> create_video_encoder(std::string* error) {
    if (error) *error = "Linux 编码器尚未实现（计划接入 FFmpeg/libavcodec）";
    return nullptr;
}

std::unique_ptr<VideoDecoder> create_video_decoder(std::string* error) {
    if (error) *error = "Linux 解码器尚未实现（计划接入 FFmpeg/libavcodec）";
    return nullptr;
}

}  // namespace pxc

#endif  // __linux__
