#include "pxc/video_transport.h"

#include <algorithm>
#include <limits>

namespace pxc {
namespace {

// frame_id 使用 uint32 序号；半范围比较可正确处理自然回绕。
bool sequence_newer(uint32_t a, uint32_t b) {
    return a != b && static_cast<uint32_t>(a - b) < 0x80000000u;
}

}  // namespace

VideoReassembler::VideoReassembler(VideoReassemblerConfig config) : config_(config) {
    if (config_.fragment_timeout <= std::chrono::milliseconds::zero()) {
        config_.fragment_timeout = std::chrono::milliseconds(1);
    }
    if (config_.max_inflight_frames == 0) config_.max_inflight_frames = 1;
    if (config_.max_buffered_bytes == 0) config_.max_buffered_bytes = 1;
}

void VideoReassembler::drop_frame(uint32_t frame_id) {
    auto it = inflight_.find(frame_id);
    if (it == inflight_.end()) return;
    buffered_bytes_ -= it->second.buffered_bytes;
    stats_.bytes_buffered = buffered_bytes_;
    stats_.frames_dropped++;
    inflight_.erase(it);
}

void VideoReassembler::expire(std::chrono::steady_clock::time_point now) {
    std::vector<uint32_t> expired;
    for (const auto& entry : inflight_) {
        if (now - entry.second.first_seen >= config_.fragment_timeout) {
            expired.push_back(entry.first);
        }
    }
    for (uint32_t id : expired) {
        drop_frame(id);
        expired_frames_[id] = now;
    }

    for (auto it = expired_frames_.begin(); it != expired_frames_.end();) {
        if (now - it->second >= config_.fragment_timeout) {
            it = expired_frames_.erase(it);
        } else {
            ++it;
        }
    }
}

bool VideoReassembler::admit_frame(const VideoPacket& packet,
                                   std::chrono::steady_clock::time_point now) {
    expire(now);
    if (expired_frames_.find(packet.frame_id) != expired_frames_.end()) return false;

    // 新帧到来后，旧的未完成帧没有继续等待价值；保留它只会浪费有限缓存。
    if (newest_frame_id_ && sequence_newer(packet.frame_id, *newest_frame_id_)) {
        newest_frame_id_ = packet.frame_id;
        std::vector<uint32_t> old;
        for (const auto& entry : inflight_) {
            if (entry.first != packet.frame_id &&
                !sequence_newer(entry.first, packet.frame_id)) {
                old.push_back(entry.first);
            }
        }
        for (uint32_t id : old) drop_frame(id);
    } else if (newest_frame_id_ && packet.frame_id != *newest_frame_id_ &&
               !sequence_newer(packet.frame_id, *newest_frame_id_)) {
        return false;
    } else if (!newest_frame_id_) {
        newest_frame_id_ = packet.frame_id;
    }

    if (inflight_.find(packet.frame_id) != inflight_.end()) return true;

    while (inflight_.size() >= config_.max_inflight_frames ||
           packet.frame_size > config_.max_buffered_bytes -
                                   std::min(buffered_bytes_, config_.max_buffered_bytes)) {
        if (inflight_.empty()) return false;
        drop_frame(inflight_.begin()->first);
    }

    InflightFrame frame;
    frame.first_packet = packet;
    frame.fragments.resize(packet.fragment_count);
    frame.received.assign(packet.fragment_count, false);
    frame.offsets.assign(packet.fragment_count, 0);
    frame.first_seen = now;
    inflight_.emplace(packet.frame_id, std::move(frame));
    return true;
}

void VideoReassembler::enforce_limits(uint32_t protected_frame_id) {
    while (inflight_.size() > config_.max_inflight_frames ||
           buffered_bytes_ > config_.max_buffered_bytes) {
        auto it = inflight_.begin();
        if (it == inflight_.end()) break;
        if (it->first == protected_frame_id) {
            ++it;
            if (it == inflight_.end()) break;
        }
        drop_frame(it->first);
    }
}

std::optional<VideoFrame> VideoReassembler::push(
    const uint8_t* data, size_t size, std::chrono::steady_clock::time_point now) {
    auto packet = decode_video_packet(data, size);
    if (!packet) {
        stats_.packets_rejected++;
        expire(now);
        return std::nullopt;
    }
    stats_.packets_received++;

    if (!admit_frame(*packet, now)) {
        stats_.packets_rejected++;
        return std::nullopt;
    }

    auto it = inflight_.find(packet->frame_id);
    if (it == inflight_.end()) {
        stats_.packets_rejected++;
        return std::nullopt;
    }
    auto& frame = it->second;

    // 每个分片都必须携带完全一致的元数据，避免拼接出混合帧。
    if (packet->fragment_count != frame.first_packet.fragment_count ||
        packet->frame_size != frame.first_packet.frame_size ||
        packet->timestamp_us != frame.first_packet.timestamp_us ||
        packet->codec != frame.first_packet.codec ||
        packet->keyframe != frame.first_packet.keyframe) {
        stats_.packets_rejected++;
        return std::nullopt;
    }

    if (frame.received[packet->fragment_index]) {
        stats_.duplicate_packets++;
        return std::nullopt;
    }

    const uint64_t new_begin = packet->fragment_offset;
    const uint64_t new_end = new_begin + packet->payload.size();
    for (size_t i = 0; i < frame.received.size(); ++i) {
        if (!frame.received[i]) continue;
        const uint64_t old_begin = frame.offsets[i];
        const uint64_t old_end = old_begin + frame.fragments[i].size();
        if (new_begin < old_end && old_begin < new_end) {
            stats_.packets_rejected++;
            return std::nullopt;
        }
    }

    frame.fragments[packet->fragment_index] = std::move(packet->payload);
    frame.offsets[packet->fragment_index] = packet->fragment_offset;
    frame.received[packet->fragment_index] = true;
    frame.received_count++;
    frame.buffered_bytes += frame.fragments[packet->fragment_index].size();
    buffered_bytes_ += frame.fragments[packet->fragment_index].size();
    stats_.bytes_buffered = buffered_bytes_;
    enforce_limits(packet->frame_id);

    if (inflight_.find(packet->frame_id) == inflight_.end()) return std::nullopt;
    if (frame.received_count != frame.fragments.size()) return std::nullopt;
    if (frame.buffered_bytes != frame.first_packet.frame_size) {
        drop_frame(packet->frame_id);
        return std::nullopt;
    }

    std::vector<size_t> order(frame.fragments.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&frame](size_t a, size_t b) {
        return frame.offsets[a] < frame.offsets[b];
    });

    uint64_t expected_offset = 0;
    for (size_t index : order) {
        if (frame.offsets[index] != expected_offset) {
            drop_frame(packet->frame_id);
            return std::nullopt;
        }
        expected_offset += frame.fragments[index].size();
    }
    if (expected_offset != frame.first_packet.frame_size) {
        drop_frame(packet->frame_id);
        return std::nullopt;
    }

    VideoFrame result;
    result.frame_id = frame.first_packet.frame_id;
    result.timestamp_us = frame.first_packet.timestamp_us;
    result.codec = frame.first_packet.codec;
    result.keyframe = frame.first_packet.keyframe;
    result.reasm_first_us = std::chrono::duration_cast<std::chrono::microseconds>(
        frame.first_seen.time_since_epoch()).count();
    result.payload.reserve(frame.buffered_bytes);
    for (size_t index : order) {
        const auto& fragment = frame.fragments[index];
        result.payload.insert(result.payload.end(), fragment.begin(), fragment.end());
    }

    buffered_bytes_ -= frame.buffered_bytes;
    stats_.bytes_buffered = buffered_bytes_;
    inflight_.erase(packet->frame_id);
    stats_.frames_completed++;
    return result;
}

void VideoReassembler::reset() {
    inflight_.clear();
    expired_frames_.clear();
    buffered_bytes_ = 0;
    newest_frame_id_.reset();
    stats_ = {};
}

}  // namespace pxc
