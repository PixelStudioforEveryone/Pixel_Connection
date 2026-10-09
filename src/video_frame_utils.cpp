#include "pxc/frame_utils.h"

#include <algorithm>
#include <cmath>

namespace pxc {
namespace {

inline uint8_t clamp_u8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

// BT.601 有限范围（视频常用的 16-235），与大多数编码器预期一致。
void bgra_to_nv12(const uint8_t* bgra, size_t bgra_stride,
                  uint32_t w, uint32_t h,
                  uint8_t* nv12, size_t nv12_stride) {
    uint8_t* y_plane  = nv12;
    uint8_t* uv_plane = nv12 + static_cast<size_t>(h) * nv12_stride;

    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t* src = bgra + static_cast<size_t>(row) * bgra_stride;
        uint8_t*       dst = y_plane + static_cast<size_t>(row) * nv12_stride;
        for (uint32_t col = 0; col < w; ++col) {
            const int b = src[col * 4 + 0];
            const int g = src[col * 4 + 1];
            const int r = src[col * 4 + 2];
            dst[col] = clamp_u8((77 * r + 150 * g + 29 * b) >> 8);
        }
    }

    for (uint32_t row = 0; row < h / 2; ++row) {
        const uint8_t* s0 = bgra + static_cast<size_t>(row * 2) * bgra_stride;
        const uint8_t* s1 = s0 + bgra_stride;
        uint8_t*       uv = uv_plane + static_cast<size_t>(row) * nv12_stride;
        for (uint32_t col = 0; col < w / 2; ++col) {
            // 2x2 区域取平均后再做色度转换，比逐点抽样抗锯齿
            const int c = col * 8;
            const int b = (s0[c + 0] + s0[c + 4] + s1[c + 0] + s1[c + 4]) >> 2;
            const int g = (s0[c + 1] + s0[c + 5] + s1[c + 1] + s1[c + 5]) >> 2;
            const int r = (s0[c + 2] + s0[c + 6] + s1[c + 2] + s1[c + 6]) >> 2;
            uv[col * 2 + 0] = clamp_u8((( -43 * r - 85 * g + 128 * b) >> 8) + 128);
            uv[col * 2 + 1] = clamp_u8(((128 * r - 107 * g - 21 * b) >> 8) + 128);
        }
    }
}

void bgra_to_yuv420p(const uint8_t* bgra, size_t bgra_stride,
                     uint32_t w, uint32_t h,
                     uint8_t* y, size_t y_stride,
                     uint8_t* u, size_t u_stride,
                     uint8_t* v, size_t v_stride) {
    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t* src = bgra + row * bgra_stride;
        uint8_t* ydst = y + row * y_stride;
        for (uint32_t col = 0; col < w; ++col) {
            const int b = src[col * 4 + 0];
            const int g = src[col * 4 + 1];
            const int r = src[col * 4 + 2];
            ydst[col] = clamp_u8((77 * r + 150 * g + 29 * b) >> 8);
        }
    }

    for (uint32_t row = 0; row < h / 2; ++row) {
        const uint8_t* s0 = bgra + (row * 2) * bgra_stride;
        const uint8_t* s1 = s0 + bgra_stride;
        uint8_t* udst = u + row * u_stride;
        uint8_t* vdst = v + row * v_stride;
        for (uint32_t col = 0; col < w / 2; ++col) {
            // 2x2 区域取平均后再做色度转换
            const int c = col * 8;
            const int b = (s0[c + 0] + s0[c + 4] + s1[c + 0] + s1[c + 4]) >> 2;
            const int g = (s0[c + 1] + s0[c + 5] + s1[c + 1] + s1[c + 5]) >> 2;
            const int r = (s0[c + 2] + s0[c + 6] + s1[c + 2] + s1[c + 6]) >> 2;
            udst[col] = clamp_u8(((-43 * r - 85 * g + 128 * b) >> 8) + 128);
            vdst[col] = clamp_u8(((128 * r - 107 * g - 21 * b) >> 8) + 128);
        }
    }
}

void nv12_to_bgra(const uint8_t* nv12, size_t nv12_stride,
                  uint32_t w, uint32_t h,
                  uint8_t* bgra, size_t bgra_stride) {
    const uint8_t* y_plane  = nv12;
    const uint8_t* uv_plane = nv12 + static_cast<size_t>(h) * nv12_stride;

    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t* ysrc = y_plane + static_cast<size_t>(row) * nv12_stride;
        const uint8_t* uv   = uv_plane + static_cast<size_t>(row / 2) * nv12_stride;
        uint8_t*       dst  = bgra + static_cast<size_t>(row) * bgra_stride;
        for (uint32_t col = 0; col < w; ++col) {
            const int yv = ysrc[col] - 16;
            const int u  = uv[(col / 2) * 2 + 0] - 128;
            const int v  = uv[(col / 2) * 2 + 1] - 128;
            dst[col * 4 + 0] = clamp_u8((298 * yv + 516 * u + 128) >> 8);
            dst[col * 4 + 1] = clamp_u8((298 * yv - 100 * u - 208 * v + 128) >> 8);
            dst[col * 4 + 2] = clamp_u8((298 * yv + 409 * v + 128) >> 8);
            dst[col * 4 + 3] = 255;
        }
    }
}

void yuv420p_to_bgra(const uint8_t* y, size_t y_stride,
                     const uint8_t* u, size_t u_stride,
                     const uint8_t* v, size_t v_stride,
                     uint32_t w, uint32_t h,
                     uint8_t* bgra, size_t bgra_stride) {
    for (uint32_t row = 0; row < h; ++row) {
        const uint8_t* ysrc = y + row * y_stride;
        const uint8_t* usrc = u + (row / 2) * u_stride;
        const uint8_t* vsrc = v + (row / 2) * v_stride;
        uint8_t* dst = bgra + row * bgra_stride;
        for (uint32_t col = 0; col < w; ++col) {
            const int yv = ysrc[col] - 16;
            const int uv = usrc[col / 2] - 128;
            const int vv = vsrc[col / 2] - 128;
            dst[col * 4 + 0] = clamp_u8((298 * yv + 516 * uv + 128) >> 8);
            dst[col * 4 + 1] = clamp_u8((298 * yv - 100 * uv - 208 * vv + 128) >> 8);
            dst[col * 4 + 2] = clamp_u8((298 * yv + 409 * vv + 128) >> 8);
            dst[col * 4 + 3] = 255;
        }
    }
}

void downscale_bgra(const uint8_t* src, size_t src_stride, uint32_t sw, uint32_t sh,
                    uint8_t* dst, size_t dst_stride, uint32_t dw, uint32_t dh) {
    // box/area 平均：每个目标像素覆盖的源区域求均值。
    // 用 64 位累加避免 4K->1080p 时溢出（单通道最大 255 * 4096^2 仍 < 2^38）。
    for (uint32_t dy = 0; dy < dh; ++dy) {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(dy) * sh / dh);
        const uint32_t y1 = std::max<uint32_t>(
            y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(dy + 1) * sh / dh));
        uint8_t* dstrow = dst + static_cast<size_t>(dy) * dst_stride;
        for (uint32_t dx = 0; dx < dw; ++dx) {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(dx) * sw / dw);
            const uint32_t x1 = std::max<uint32_t>(
                x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(dx + 1) * sw / dw));
            uint64_t acc[4] = {0, 0, 0, 0};
            uint64_t count  = 0;
            for (uint32_t sy = y0; sy < y1 && sy < sh; ++sy) {
                const uint8_t* srow = src + static_cast<size_t>(sy) * src_stride;
                for (uint32_t sx = x0; sx < x1 && sx < sw; ++sx) {
                    acc[0] += srow[sx * 4 + 0];
                    acc[1] += srow[sx * 4 + 1];
                    acc[2] += srow[sx * 4 + 2];
                    acc[3] += srow[sx * 4 + 3];
                    ++count;
                }
            }
            if (count == 0) count = 1;
            dstrow[dx * 4 + 0] = static_cast<uint8_t>(acc[0] / count);
            dstrow[dx * 4 + 1] = static_cast<uint8_t>(acc[1] / count);
            dstrow[dx * 4 + 2] = static_cast<uint8_t>(acc[2] / count);
            dstrow[dx * 4 + 3] = static_cast<uint8_t>(acc[3] / count);
        }
    }
}

void fit_within(uint32_t sw, uint32_t sh,
                uint32_t max_w, uint32_t max_h,
                uint32_t& out_w, uint32_t& out_h) {
    out_w = sw;
    out_h = sh;
    if (sw == 0 || sh == 0 || max_w == 0 || max_h == 0) return;
    if (sw <= max_w && sh <= max_h) return;  // 只缩不放

    // 选缩得更多的那一边，保证同时装进两个限制
    const double rw = static_cast<double>(max_w) / sw;
    const double rh = static_cast<double>(max_h) / sh;
    const double r  = std::min(rw, rh);

    // 结果对齐到 2 的倍数（NV12/编码器要求）
    uint32_t w = static_cast<uint32_t>(std::lround(sw * r));
    uint32_t h = static_cast<uint32_t>(std::lround(sh * r));
    w = std::max<uint32_t>(w & ~1u, 2);
    h = std::max<uint32_t>(h & ~1u, 2);
    out_w = std::min(w, max_w & ~1u);
    out_h = std::min(h, max_h & ~1u);
}

}  // namespace pxc
