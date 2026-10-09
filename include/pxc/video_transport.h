#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "pxc/video_protocol.h"

namespace pxc {

struct VideoReassemblerConfig {
    std::chrono::milliseconds fragment_timeout{500};
    size_t max_inflight_frames = 8;
    size_t max_buffered_bytes = 32 * 1024 * 1024;
};

struct VideoReassemblerStats {
    uint64_t packets_received = 0;
    uint64_t packets_rejected = 0;
    uint64_t duplicate_packets = 0;
    uint64_t frames_completed = 0;
    uint64_t frames_dropped = 0;
    uint64_t bytes_buffered = 0;
};

// 处理 ch-video 的乱序/丢片 binary message。
// push() 只在完整帧到达时返回 frame；不为缺片阻塞，也不重传。
class VideoReassembler {
public:
    explicit VideoReassembler(VideoReassemblerConfig config = {});

    std::optional<VideoFrame> push(const uint8_t* data, size_t size,
                                   std::chrono::steady_clock::time_point now =
                                       std::chrono::steady_clock::now());
    std::optional<VideoFrame> push(const std::vector<uint8_t>& data,
                                   std::chrono::steady_clock::time_point now =
                                       std::chrono::steady_clock::now()) {
        return push(data.data(), data.size(), now);
    }

    // 清除超过 fragment_timeout 的不完整帧。
    void expire(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void reset();

    const VideoReassemblerStats& stats() const { return stats_; }
    size_t buffered_bytes() const { return buffered_bytes_; }

private:
    struct InflightFrame {
        VideoPacket first_packet;
        std::vector<std::vector<uint8_t>> fragments;
        std::vector<bool> received;
        std::vector<uint32_t> offsets;
        size_t received_count = 0;
        size_t buffered_bytes = 0;
        std::chrono::steady_clock::time_point first_seen;
    };

    bool admit_frame(const VideoPacket& packet,
                     std::chrono::steady_clock::time_point now);
    void drop_frame(uint32_t frame_id);
    void enforce_limits(uint32_t protected_frame_id);

    VideoReassemblerConfig config_;
    std::map<uint32_t, InflightFrame> inflight_;
    std::map<uint32_t, std::chrono::steady_clock::time_point> expired_frames_;
    size_t buffered_bytes_ = 0;
    std::optional<uint32_t> newest_frame_id_;
    VideoReassemblerStats stats_;
};

}  // namespace pxc
