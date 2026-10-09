#include "pxc/video_protocol.h"

#include <algorithm>
#include <limits>

namespace pxc {
namespace {

constexpr size_t kMagicOffset       = 0;
constexpr size_t kVersionOffset     = 2;
constexpr size_t kCodecOffset       = 3;
constexpr size_t kFlagsOffset       = 4;
constexpr size_t kReservedOffset    = 5;
constexpr size_t kFrameIdOffset     = 8;
constexpr size_t kTimestampOffset   = 12;
constexpr size_t kFragmentIndexOff  = 20;
constexpr size_t kFragmentCountOff  = 22;
constexpr size_t kFrameSizeOffset   = 24;
constexpr size_t kPayloadSizeOffset = 28;
constexpr size_t kFragmentOffsetOff = 32;
constexpr size_t kHeaderReservedOff = 36;
constexpr uint8_t kKnownFlags       = kVideoFlagKeyframe;

void put_u16(std::vector<uint8_t>& out, size_t offset, uint16_t value) {
    out[offset] = static_cast<uint8_t>(value >> 8);
    out[offset + 1] = static_cast<uint8_t>(value);
}

void put_u32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
    out[offset] = static_cast<uint8_t>(value >> 24);
    out[offset + 1] = static_cast<uint8_t>(value >> 16);
    out[offset + 2] = static_cast<uint8_t>(value >> 8);
    out[offset + 3] = static_cast<uint8_t>(value);
}

void put_u64(std::vector<uint8_t>& out, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<uint8_t>(value >> (56 - i * 8));
    }
}

uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t get_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

uint64_t get_u64(const uint8_t* p) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | p[i];
    return value;
}

bool known_codec(VideoCodec codec) {
    return codec == VideoCodec::Unknown || codec == VideoCodec::H264 || codec == VideoCodec::VP8;
}

bool valid_packet_fields(const VideoPacket& packet) {
    if (!known_codec(packet.codec) || packet.fragment_count == 0 ||
        packet.fragment_count > kVideoMaxFragments ||
        packet.fragment_index >= packet.fragment_count ||
        packet.frame_size == 0 || packet.frame_size > kVideoMaxFrameBytes ||
        packet.payload.empty() || packet.payload.size() > kVideoPacketMaxPayload ||
        packet.payload.size() > packet.frame_size) {
        return false;
    }
    return true;
}

}  // namespace

std::vector<uint8_t> encode_video_packet(const VideoPacket& packet) {
    if (!known_codec(packet.codec) || packet.fragment_count == 0 ||
        packet.fragment_count > kVideoMaxFragments ||
        packet.fragment_index >= packet.fragment_count ||
        packet.frame_size == 0 || packet.frame_size > kVideoMaxFrameBytes ||
        packet.payload.empty() || packet.payload.size() > kVideoPacketMaxPayload ||
        packet.payload.size() > packet.frame_size) {
        return {};
    }

    const uint64_t end = static_cast<uint64_t>(packet.fragment_offset) + packet.payload.size();
    if (end > packet.frame_size ||
        (packet.fragment_index == 0 && packet.fragment_offset != 0) ||
        (packet.fragment_index + 1u == packet.fragment_count && end != packet.frame_size)) {
        return {};
    }

    std::vector<uint8_t> out(kVideoPacketHeaderSize + packet.payload.size(), 0);
    put_u16(out, kMagicOffset, kVideoPacketMagic);
    out[kVersionOffset] = kVideoPacketVersion;
    out[kCodecOffset] = static_cast<uint8_t>(packet.codec);
    out[kFlagsOffset] = packet.keyframe ? kVideoFlagKeyframe : 0;
    put_u32(out, kFrameIdOffset, packet.frame_id);
    put_u64(out, kTimestampOffset, packet.timestamp_us);
    put_u16(out, kFragmentIndexOff, packet.fragment_index);
    put_u16(out, kFragmentCountOff, packet.fragment_count);
    put_u32(out, kFrameSizeOffset, packet.frame_size);
    put_u32(out, kPayloadSizeOffset, static_cast<uint32_t>(packet.payload.size()));
    put_u32(out, kFragmentOffsetOff, packet.fragment_offset);
    std::copy(packet.payload.begin(), packet.payload.end(), out.begin() + kVideoPacketHeaderSize);
    return out;
}

std::optional<VideoPacket> decode_video_packet(const uint8_t* data, size_t size) {
    if (!data || size < kVideoPacketHeaderSize || get_u16(data + kMagicOffset) != kVideoPacketMagic ||
        data[kVersionOffset] != kVideoPacketVersion || data[kReservedOffset] != 0 ||
        (data[kFlagsOffset] & ~kKnownFlags) != 0) {
        return std::nullopt;
    }
    for (size_t i = kHeaderReservedOff; i < kVideoPacketHeaderSize; ++i) {
        if (data[i] != 0) return std::nullopt;
    }

    const auto codec = static_cast<VideoCodec>(data[kCodecOffset]);
    if (!known_codec(codec)) return std::nullopt;

    const uint32_t payload_size = get_u32(data + kPayloadSizeOffset);
    if (payload_size != size - kVideoPacketHeaderSize) return std::nullopt;

    VideoPacket packet;
    packet.frame_id = get_u32(data + kFrameIdOffset);
    packet.timestamp_us = get_u64(data + kTimestampOffset);
    packet.codec = codec;
    packet.keyframe = (data[kFlagsOffset] & kVideoFlagKeyframe) != 0;
    packet.fragment_index = get_u16(data + kFragmentIndexOff);
    packet.fragment_count = get_u16(data + kFragmentCountOff);
    packet.frame_size = get_u32(data + kFrameSizeOffset);
    packet.fragment_offset = get_u32(data + kFragmentOffsetOff);
    packet.payload.assign(data + kVideoPacketHeaderSize, data + size);

    if (!valid_packet_fields(packet)) return std::nullopt;
    const uint64_t end = static_cast<uint64_t>(packet.fragment_offset) + packet.payload.size();
    if (end > packet.frame_size ||
        (packet.fragment_index == 0 && packet.fragment_offset != 0) ||
        (packet.fragment_index + 1u == packet.fragment_count && end != packet.frame_size)) {
        return std::nullopt;
    }
    return packet;
}

std::vector<std::vector<uint8_t>> packetize_video_frame(const VideoFrame& frame,
                                                         size_t payload_limit) {
    if (frame.payload.empty() || frame.payload.size() > kVideoMaxFrameBytes ||
        !known_codec(frame.codec) || payload_limit == 0 ||
        payload_limit > kVideoPacketMaxPayload) {
        return {};
    }

    const size_t count = (frame.payload.size() + payload_limit - 1) / payload_limit;
    if (count == 0 || count > kVideoMaxFragments ||
        frame.payload.size() > std::numeric_limits<uint32_t>::max()) {
        return {};
    }

    std::vector<std::vector<uint8_t>> packets;
    packets.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        const size_t offset = index * payload_limit;
        const size_t length = std::min(payload_limit, frame.payload.size() - offset);

        VideoPacket packet;
        packet.frame_id = frame.frame_id;
        packet.timestamp_us = frame.timestamp_us;
        packet.codec = frame.codec;
        packet.keyframe = frame.keyframe;
        packet.fragment_index = static_cast<uint16_t>(index);
        packet.fragment_count = static_cast<uint16_t>(count);
        packet.frame_size = static_cast<uint32_t>(frame.payload.size());
        packet.fragment_offset = static_cast<uint32_t>(offset);
        packet.payload.assign(frame.payload.begin() + offset, frame.payload.begin() + offset + length);

        auto encoded = encode_video_packet(packet);
        if (encoded.empty()) return {};
        packets.push_back(std::move(encoded));
    }
    return packets;
}

}  // namespace pxc
