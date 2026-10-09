#pragma once

// 帧处理工具：BGRA <-> NV12 转换与降采样。
//
// 为什么需要这些：
//   - MF 编码器的输入统一使用 NV12（硬件/软件编码器都支持得最好）；
//   - 画质档位（1080P/2K/4K）需要对采集帧做降采样后再编码；
//   - MF 解码器输出 NV12，渲染前要转回 BGRA（QImage::Format_ARGB32）。
//
// 全部是纯 CPU 实现，跨平台、无第三方依赖。性能关键路径以后可以再 SIMD 化，
// 接口不变。

#include <cstddef>
#include <cstdint>

namespace pxc {

// BGRA(bgra_stride) -> NV12(nv12_stride)。w、h 必须是偶数（NV12 需要 2 对齐）。
void bgra_to_nv12(const uint8_t* bgra, size_t bgra_stride,
                  uint32_t w, uint32_t h,
                  uint8_t* nv12, size_t nv12_stride);

// BGRA -> YUV420P 三个平面（FFmpeg H.264 编码器的输入格式）。
// u/v 平面尺寸为 w/2 x h/2，各自有独立行距。
void bgra_to_yuv420p(const uint8_t* bgra, size_t bgra_stride,
                     uint32_t w, uint32_t h,
                     uint8_t* y, size_t y_stride,
                     uint8_t* u, size_t u_stride,
                     uint8_t* v, size_t v_stride);

// NV12(nv12_stride) -> BGRA(bgra_stride)。
void nv12_to_bgra(const uint8_t* nv12, size_t nv12_stride,
                  uint32_t w, uint32_t h,
                  uint8_t* bgra, size_t bgra_stride);

// YUV420P 三个平面 -> BGRA。libavcodec 的 H.264 解码器输出此格式。
// y/u/v 各平面可有独立行距（对齐后的 linesize）。
void yuv420p_to_bgra(const uint8_t* y, size_t y_stride,
                     const uint8_t* u, size_t u_stride,
                     const uint8_t* v, size_t v_stride,
                     uint32_t w, uint32_t h,
                     uint8_t* bgra, size_t bgra_stride);

// 把 BGRA 源降采样到恰好 dst 尺寸（box/area 平均，缩小比例大时质量稳定）。
// 只支持缩小；dw/sw、dh/sh 需为正数。宽高都为 2 的倍数由调用方保证。
void downscale_bgra(const uint8_t* src, size_t src_stride, uint32_t sw, uint32_t sh,
                    uint8_t* dst, size_t dst_stride, uint32_t dw, uint32_t dh);

// 计算把 sw x sh「装进」max_w x max_h 的目标尺寸：
// 只缩不放（源更小时保持原样），保持宽高比，结果为 2 的倍数。
void fit_within(uint32_t sw, uint32_t sh,
                uint32_t max_w, uint32_t max_h,
                uint32_t& out_w, uint32_t& out_h);

}  // namespace pxc
