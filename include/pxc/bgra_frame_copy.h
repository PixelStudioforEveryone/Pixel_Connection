#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace pxc {
// Validate the entire mapped image before replacing the last complete capture.
inline bool copy_bgra_frame(const uint8_t* data, size_t capacity, size_t offset,
                            size_t bytes, int64_t stride, uint32_t width,
                            uint32_t height, std::vector<uint8_t>& output) {
    if (!data || !width || !height || width > std::numeric_limits<size_t>::max() / 4)
        return false;
    const size_t row = static_cast<size_t>(width) * 4;
    if (stride == 0) stride = static_cast<int64_t>(row);
    if (stride < 0 || static_cast<uint64_t>(stride) < row || offset > capacity ||
        bytes > capacity - offset || height > std::numeric_limits<size_t>::max() / row)
        return false;
    const size_t pitch = static_cast<size_t>(stride);
    if (height > 1 && (height - 1) > (std::numeric_limits<size_t>::max() - row) / pitch)
        return false;
    const size_t required = pitch * (height - 1) + row;
    if (required > bytes) return false;
    output.resize(row * height);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(output.data() + y * row, data + offset + y * pitch, row);
    return true;
}
} // namespace pxc
