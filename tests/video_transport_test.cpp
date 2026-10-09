#include "pxc/video_transport.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const std::string& name) {
    if (value) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        std::cerr << "[FAIL] " << name << '\n';
        ++failures;
    }
}

pxc::VideoFrame make_frame(uint32_t id, size_t size, uint8_t seed = 0) {
    pxc::VideoFrame frame;
    frame.frame_id = id;
    frame.timestamp_us = 987654321;
    frame.codec = pxc::VideoCodec::H264;
    frame.keyframe = true;
    frame.payload.resize(size);
    for (size_t i = 0; i < size; ++i) frame.payload[i] = static_cast<uint8_t>(seed + i * 17);
    return frame;
}

bool frame_matches(const pxc::VideoFrame& a, const pxc::VideoFrame& b) {
    return a.frame_id == b.frame_id && a.timestamp_us == b.timestamp_us &&
           a.codec == b.codec && a.keyframe == b.keyframe && a.payload == b.payload;
}

void test_round_trip_single_packet() {
    auto input = make_frame(1, 13);
    auto packets = pxc::packetize_video_frame(input, 100);
    bool ok = packets.size() == 1;
    pxc::VideoReassembler reassembler;
    std::optional<pxc::VideoFrame> out;
    if (ok) out = reassembler.push(packets[0]);
    check(ok && out && frame_matches(input, *out), "single-packet round trip");
}

void test_multi_fragment_out_of_order() {
    auto input = make_frame(2, 37, 5);
    auto packets = pxc::packetize_video_frame(input, 8);
    std::reverse(packets.begin(), packets.end());
    pxc::VideoReassembler reassembler;
    std::optional<pxc::VideoFrame> out;
    for (const auto& packet : packets) {
        auto frame = reassembler.push(packet);
        if (frame) out = std::move(frame);
    }
    check(packets.size() == 5 && out && frame_matches(input, *out),
          "multi-fragment out-of-order reassembly");
}

void test_duplicate_and_missing_expiry() {
    auto input = make_frame(3, 30);
    auto packets = pxc::packetize_video_frame(input, 10);
    pxc::VideoReassembler reassembler;
    const auto now = std::chrono::steady_clock::now();
    auto first = reassembler.push(packets[0], now);
    auto duplicate = reassembler.push(packets[0], now);
    auto late = reassembler.push(packets[1], now + std::chrono::milliseconds(501));
    (void)first;
    (void)duplicate;
    check(!late && reassembler.stats().duplicate_packets == 1 &&
              reassembler.stats().frames_dropped == 1 &&
              reassembler.stats().packets_rejected == 1 &&
              reassembler.buffered_bytes() == 0,
          "duplicate fragment and late-frame expiry");
}

void test_old_frame_drop() {
    auto old_frame = make_frame(10, 20);
    auto new_frame = make_frame(11, 10);
    auto old_packets = pxc::packetize_video_frame(old_frame, 10);
    auto new_packets = pxc::packetize_video_frame(new_frame, 10);
    pxc::VideoReassembler reassembler;
    const auto now = std::chrono::steady_clock::now();
    reassembler.push(old_packets[0], now);
    auto new_complete = reassembler.push(new_packets[0], now + std::chrono::milliseconds(1));
    auto old_late = reassembler.push(old_packets[1], now + std::chrono::milliseconds(2));
    check(new_complete && new_complete->frame_id == 11 && !old_late &&
              reassembler.stats().frames_dropped >= 1,
          "new frame evicts incomplete older frame");
}

void test_invalid_header_rejected() {
    auto input = make_frame(20, 5);
    auto packet = pxc::packetize_video_frame(input, 10).front();
    auto bad_magic = packet;
    bad_magic[0] ^= 0xff;
    auto bad_version = packet;
    bad_version[2] = 99;
    auto bad_payload_length = packet;
    bad_payload_length[31] ^= 1;

    check(!pxc::decode_video_packet(bad_magic) && !pxc::decode_video_packet(bad_version) &&
              !pxc::decode_video_packet(bad_payload_length),
          "invalid magic, version, and payload length rejected");
}

void test_overlapping_fragment_rejected_and_dropped() {
    auto input = make_frame(30, 20);
    auto packets = pxc::packetize_video_frame(input, 10);
    // Change the second fragment offset to overlap the first. Offset lives at byte 32.
    packets[1][35] = 5;
    pxc::VideoReassembler reassembler;
    const auto now = std::chrono::steady_clock::now();
    reassembler.push(packets[0], now);
    auto out = reassembler.push(packets[1], now + std::chrono::milliseconds(1));
    check(!out && reassembler.stats().packets_rejected == 1,
          "overlapping fragment rejected");
}

void test_packetizer_bounds() {
    auto empty = make_frame(40, 0);
    auto too_large_fragment = make_frame(41, 10);
    auto huge_count = make_frame(42, 100);
    check(pxc::packetize_video_frame(empty).empty() &&
              pxc::packetize_video_frame(too_large_fragment, pxc::kVideoPacketMaxPayload + 1).empty() &&
              pxc::packetize_video_frame(huge_count, 0).empty(),
          "packetizer rejects empty and out-of-bound inputs");
}

void test_resource_limit() {
    pxc::VideoReassemblerConfig config;
    config.max_buffered_bytes = 15;
    config.max_inflight_frames = 2;
    pxc::VideoReassembler reassembler(config);
    auto frame = make_frame(50, 20);
    auto packets = pxc::packetize_video_frame(frame, 10);
    auto accepted = reassembler.push(packets[0]);
    check(!accepted && reassembler.stats().packets_rejected == 1 &&
              reassembler.buffered_bytes() == 0,
          "declared frame larger than reassembly budget is rejected");
}

}  // namespace

int main() {
    test_round_trip_single_packet();
    test_multi_fragment_out_of_order();
    test_duplicate_and_missing_expiry();
    test_old_frame_drop();
    test_invalid_header_rejected();
    test_overlapping_fragment_rejected_and_dropped();
    test_packetizer_bounds();
    test_resource_limit();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
