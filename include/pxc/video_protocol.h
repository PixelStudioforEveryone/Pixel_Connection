#pragma once

// ch-video 上的应用层二进制协议。
// DataChannel 本身保证消息边界，但不保证消息可靠、有序；因此每条
// DataChannel binary message 都携带完整分片头，接收端可以独立校验和丢弃。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace pxc {

inline constexpr uint16_t kVideoPacketMagic   = 0x5058;  // "PX", network byte order
inline constexpr uint8_t  kVideoPacketVersion = 1;
inline constexpr size_t   kVideoPacketHeaderSize = 40;
// 不把 ICE MTU 当作 DataChannel 消息上限；这是应用层主动采用的保守片大小。
inline constexpr size_t kVideoPacketDefaultPayload = 1000;
inline constexpr size_t kVideoPacketMaxPayload     = 16 * 1024;
inline constexpr size_t kVideoMaxFrameBytes        = 16 * 1024 * 1024;
inline constexpr uint16_t kVideoMaxFragments       = 16384;

// 编码类型先预留；M2 测试帧使用 Unknown，后续 Windows 编码器填 H264/VP8。
enum class VideoCodec : uint8_t {
    Unknown = 0,
    H264    = 1,
    VP8     = 2,
};

enum VideoPacketFlags : uint8_t {
    kVideoFlagKeyframe = 1u << 0,
};

struct VideoFrame {
    uint32_t frame_id = 0;
    uint64_t timestamp_us = 0;
    VideoCodec codec = VideoCodec::Unknown;
    bool keyframe = false;
    // 重组侧填充：首片到达时刻（steady_clock 微秒，仅接收进程内有效；
    // 发送侧构造的 VideoFrame 恒为 0）。延迟定位用，不上协议。
    uint64_t reasm_first_us = 0;
    std::vector<uint8_t> payload;
};

struct VideoPacket {
    uint32_t frame_id = 0;
    uint64_t timestamp_us = 0;
    VideoCodec codec = VideoCodec::Unknown;
    bool keyframe = false;
    uint16_t fragment_index = 0;
    uint16_t fragment_count = 0;
    uint32_t frame_size = 0;
    uint32_t fragment_offset = 0;
    std::vector<uint8_t> payload;
};

// 固定 40 字节、网络字节序的分片头后跟 payload。
std::vector<uint8_t> encode_video_packet(const VideoPacket& packet);
std::optional<VideoPacket> decode_video_packet(const uint8_t* data, size_t size);

inline std::optional<VideoPacket> decode_video_packet(const std::vector<uint8_t>& data) {
    return decode_video_packet(data.data(), data.size());
}

// 将一帧切成可独立发送的 binary messages。
std::vector<std::vector<uint8_t>> packetize_video_frame(
    const VideoFrame& frame,
    size_t payload_limit = kVideoPacketDefaultPayload);

}  // namespace pxc
