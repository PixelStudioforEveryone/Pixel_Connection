#pragma once
#include <cstdint>
#include <optional>
#include "pxc/video_protocol.h"

namespace pxc {
// A complete P frame is still unsafe after a lost reference frame. Only a new
// complete keyframe repairs the chain; the renderer keeps its last good image.
class VideoReferenceGuard {
public:
    void reset() { last_.reset(); waiting_ = true; }
    void invalidate() { waiting_ = true; }
    bool needs_keyframe() const { return waiting_; }
    bool accept(const VideoFrame& frame) {
        if (last_) {
            const uint32_t distance = frame.frame_id - *last_;
            if (distance == 0 || distance >= 0x80000000u) return false;
            if (distance != 1) waiting_ = true;
        }
        if (frame.keyframe) waiting_ = false;
        if (waiting_) return false;
        last_ = frame.frame_id;
        return true;
    }
private:
    std::optional<uint32_t> last_;
    bool waiting_ = true;
};
} // namespace pxc
