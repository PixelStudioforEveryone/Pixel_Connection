// 采集/转换/编码/解码链路基准与诊断工具（S1）。
//
// 模式:
//   synth   合成帧源：每帧内容都变化（色块/滚动条/帧号），按单调时钟调度
//   capture 真实屏幕采集（画面静止时无新帧属预期，cap_to 计数）
//
// 用法:
//   pxc-capture-test [--screen N] [--mode synth|capture] [--w N] [--h N]
//                    [--fps N] [--bitrate KBPS] [--duration SEC] [--loop]
//
// 输出: 每秒一行统计（预热 5 秒单独标记）:
//   [t=12] src=30 cap_to=0 t_cap=0.4/1.1 t_cvt=1.9/2.4 t_enc=5.2/6.8
//           enc=30 key=0 out=1.2MB dec=30 t_dec=3.1/4.0 fps=30.0
//           (P50/P95，单位 ms)
// --loop 时编码输出再本地解码，验证码流正确性并测解码耗时。

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pxc/frame_utils.h"
#include "pxc/screen_capturer.h"
#include "pxc/video_decoder.h"
#include "pxc/video_encoder.h"

namespace {

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------
class Percentiles {
public:
    void push(double ms) {
        samples_.push_back(ms);
        if (samples_.size() > kCap) samples_.erase(samples_.begin());
    }
    // p 为 0..100；无样本返回 0
    double at(double p) const {
        if (samples_.empty()) return 0.0;
        std::vector<double> sorted(samples_);
        std::sort(sorted.begin(), sorted.end());
        const size_t idx = std::min(sorted.size() - 1,
            static_cast<size_t>(p / 100.0 * sorted.size()));
        return sorted[idx];
    }
    bool empty() const { return samples_.empty(); }
private:
    static constexpr size_t kCap = 4096;
    std::vector<double> samples_;
};

double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// ---------------------------------------------------------------
// 合成帧：内容逐帧变化，保证「独立新帧」；含滚动条、色块、帧号。
void render_synth(std::vector<uint8_t>& bgra, uint32_t w, uint32_t h,
                  uint64_t frame_no) {
    const size_t stride = static_cast<size_t>(w) * 4;
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = bgra.data() + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* px = row + x * 4;
            // 移动渐变背景：保证每帧全屏变化
            const uint32_t dx = (x + static_cast<uint32_t>(frame_no * 7)) & 255;
            const uint32_t dy = (y + static_cast<uint32_t>(frame_no * 3)) & 255;
            px[0] = static_cast<uint8_t>(dx);
            px[1] = static_cast<uint8_t>(dy);
            px[2] = static_cast<uint8_t>((dx + dy) & 255);
            px[3] = 255;
        }
        // 滚动白条（1/16 高度周期），制造高频水平边缘
        const uint32_t band = (static_cast<uint32_t>(frame_no) * 9) % h;
        if (y >= band && y < band + h / 16) {
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* px = row + x * 4;
                px[0] = px[1] = px[2] = 250;
            }
        }
    }
    // 左上角 4 位帧号（手工 3x5 点阵，够辨认即可）
    char digits[8];
    std::snprintf(digits, sizeof(digits), "%04llu",
                  static_cast<unsigned long long>(frame_no % 10000));
    const uint32_t cell = 10, ox = 20, oy = 20;
    static const uint8_t font[10][5] = {
        {0xE,0x11,0x11,0x11,0xE}, {0x4,0xC,0x4,0x4,0xE}, {0xE,0x1,0xE,0x8,0xF},
        {0xE,0x1,0xE,0x1,0xE},    {0x2,0x6,0xA,0xF,0x2}, {0xF,0x8,0xE,0x1,0xE},
        {0xE,0x8,0xE,0x11,0xE},   {0xF,0x1,0x2,0x4,0x4}, {0xE,0x11,0xE,0x11,0xE},
        {0xE,0x11,0xE,0x1,0xE},
    };
    for (int d = 0; d < 4; ++d) {
        for (int fy = 0; fy < 5; ++fy) {
            const uint8_t rowbits = font[digits[d] - '0'][fy];
            for (int fx = 0; fx < 4; ++fx) {
                if (!((rowbits >> (3 - fx)) & 1)) continue;
                for (uint32_t py = 0; py < cell; ++py) {
                    if (oy + (fy + d * 6) * cell + py >= h) continue;
                    uint8_t* r = bgra.data() +
                        static_cast<size_t>(oy + (fy + d * 6) * cell + py) * stride;
                    for (uint32_t px2 = 0; px2 < cell; ++px2) {
                        const uint32_t x = ox + (d * 5 + fx) * cell + px2;
                        if (x >= w) continue;
                        r[x * 4] = r[x * 4 + 1] = r[x * 4 + 2] = 255;
                    }
                }
            }
        }
    }
}

struct Args {
    int         screen        = 0;
    std::string mode          = "capture";
    uint32_t    target_w      = 1920;
    uint32_t    target_h      = 1080;
    uint32_t    fps           = 30;
    uint32_t    bitrate_kbps  = 8000;
    uint32_t    duration_s    = 60;
    bool        loop_decode   = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "缺少 %s 的值\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--screen")        a.screen       = std::atoi(next("--screen"));
        else if (arg == "--mode")     a.mode         = next("--mode");
        else if (arg == "--w")        a.target_w     = static_cast<uint32_t>(std::atoi(next("--w")));
        else if (arg == "--h")        a.target_h     = static_cast<uint32_t>(std::atoi(next("--h")));
        else if (arg == "--fps")      a.fps          = static_cast<uint32_t>(std::atoi(next("--fps")));
        else if (arg == "--bitrate")  a.bitrate_kbps = static_cast<uint32_t>(std::atoi(next("--bitrate")));
        else if (arg == "--duration") a.duration_s   = static_cast<uint32_t>(std::atoi(next("--duration")));
        else if (arg == "--loop")     a.loop_decode  = true;
        else {
            std::fprintf(stderr, "未知参数: %s\n", arg.c_str());
            std::exit(2);
        }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parse_args(argc, argv);
    const bool synth = (args.mode == "synth");

    std::string error;

    uint32_t src_w = args.target_w, src_h = args.target_h, src_stride = 0;
    std::vector<uint8_t> synth_buf;
    std::shared_ptr<pxc::ScreenCapturer> capturer;

    if (synth) {
        src_w = args.target_w;
        src_h = args.target_h;
        src_stride = src_w * 4;
        synth_buf.assign(static_cast<size_t>(src_w) * src_h * 4, 0);
        std::printf("模式: 合成帧 %ux%u@%u\n", src_w, src_h, args.fps);
    } else {
        auto screens = pxc::enumerate_screens(&error);
        std::printf("enumerate_screens: %zu 个", screens.size());
        if (!error.empty()) std::printf(" (%s)", error.c_str());
        std::printf("\n");
        for (const auto& s : screens) {
            std::printf("  [%d] %s %ux%u primary=%d\n", s.index, s.name.c_str(),
                        s.width, s.height, s.primary ? 1 : 0);
        }
        if (screens.empty()) return 1;

        capturer = pxc::create_screen_capturer(&error);
        if (!capturer) {
            std::printf("create_screen_capturer 失败: %s\n", error.c_str());
            return 2;
        }
        if (!capturer->start(args.screen)) {
            std::printf("capturer.start(%d) 失败: %s\n", args.screen,
                        capturer->last_error().c_str());
            return 3;
        }
        std::printf("模式: 真实采集 屏幕=%d\n", args.screen);
        // 先抓一帧拿源尺寸
        pxc::RawFrame probe;
        for (int i = 0; i < 30; ++i) {
            if (capturer->capture(probe, std::chrono::milliseconds(150))) break;
            if (!capturer->last_error().empty()) {
                std::printf("capture 报错: %s\n", capturer->last_error().c_str());
                return 4;
            }
        }
        if (!probe.bgra) {
            std::printf("30 次尝试内没有帧\n");
            return 5;
        }
        src_w = probe.width;
        src_h = probe.height;
        src_stride = probe.stride;
        std::printf("源尺寸: %ux%u stride=%u\n", src_w, src_h, src_stride);
    }

    // 编码目标尺寸: 画质上限只缩不放（与 video_sender 相同语义）
    uint32_t enc_w = 0, enc_h = 0;
    pxc::fit_within(src_w, src_h, args.target_w, args.target_h, enc_w, enc_h);
    if (enc_w % 2) ++enc_w;
    if (enc_h % 2) ++enc_h;
    std::printf("编码尺寸: %ux%u@%u 码率=%ukbps 时长=%us 回环解码=%s\n",
                enc_w, enc_h, args.fps, args.bitrate_kbps, args.duration_s,
                args.loop_decode ? "开" : "关");

    auto encoder = pxc::create_video_encoder(&error);
    if (!encoder) {
        std::printf("create_video_encoder 失败: %s\n", error.c_str());
        return 6;
    }
    pxc::VideoEncoderConfig cfg;
    cfg.width = enc_w;
    cfg.height = enc_h;
    cfg.fps = args.fps;
    cfg.bitrate_kbps = args.bitrate_kbps;
    if (!encoder->configure(cfg)) {
        std::printf("encoder.configure 失败: %s\n", encoder->last_error().c_str());
        return 7;
    }
    std::printf("编码器: %s\n", encoder->name().c_str());

    std::unique_ptr<pxc::VideoDecoder> decoder;
    if (args.loop_decode) {
        decoder = pxc::create_video_decoder(&error);
        if (!decoder) {
            std::printf("create_video_decoder 失败: %s\n", error.c_str());
            return 8;
        }
        std::printf("解码器: 已创建\n");
    }

    std::vector<uint8_t> scaled;
    Percentiles p_cap, p_cvt, p_enc, p_dec;

    // 每秒窗口统计
    uint64_t win_src = 0, win_cap_to = 0, win_enc = 0, win_key = 0;
    uint64_t win_dec = 0, win_bytes = 0, win_dec_bytes = 0;
    auto window_start = Clock::now();
    const auto t_start = window_start;
    uint64_t frame_no = 0;
    bool warmup_done = false;
    const uint32_t warmup_s = 5;

    auto next_tick = Clock::now();
    bool decode_ok_seen = false;
    bool decode_fail_seen = false;

    while (true) {
        const auto now = Clock::now();
        if (now - t_start >= std::chrono::seconds(args.duration_s)) break;

        next_tick += std::chrono::milliseconds(1000 / std::max<uint32_t>(args.fps, 1));
        if (next_tick < now) next_tick = now;
        std::this_thread::sleep_until(next_tick);

        // ---- 取源帧 ----
        const uint8_t* src = nullptr;
        size_t src_stride_now = 0;
        uint32_t src_w_now = src_w, src_h_now = src_h;

        if (synth) {
            render_synth(synth_buf, src_w, src_h, frame_no);
            src = synth_buf.data();
            src_stride_now = static_cast<size_t>(src_w) * 4;
        } else {
            const auto cap_t0 = Clock::now();
            pxc::RawFrame raw;
            const auto timeout = std::chrono::milliseconds(
                std::max<uint32_t>(1000 / std::max<uint32_t>(args.fps, 1) / 2, 5));
            if (!capturer->capture(raw, timeout)) {
                const std::string cap_err = capturer->last_error();
                if (!cap_err.empty()) {
                    std::printf("[t=%llu] capture 错误: %s\n",
                                static_cast<unsigned long long>(
                                    std::chrono::duration_cast<std::chrono::seconds>(
                                        Clock::now() - t_start).count()),
                                cap_err.c_str());
                    return 9;
                }
                ++win_cap_to;
                continue;
            }
            p_cap.push(ms_since(cap_t0));
            src = raw.bgra->data();
            src_stride_now = raw.stride;
            src_w_now = raw.width;
            src_h_now = raw.height;
        }

        // ---- 缩放（仅尺寸不符且编码器无内置缩放时） ----
        const auto cvt_t0 = Clock::now();
        uint32_t to_w = 0, to_h = 0;
        pxc::fit_within(src_w_now, src_h_now, args.target_w, args.target_h, to_w, to_h);
        if (to_w % 2) ++to_w;
        if (to_h % 2) ++to_h;
        const uint8_t* enc_src = src;
        size_t enc_stride = src_stride_now;
        uint32_t enc_in_w = src_w_now, enc_in_h = src_h_now;
        if (!encoder->handles_scaling() && (to_w != src_w_now || to_h != src_h_now)) {
            scaled.assign(static_cast<size_t>(to_w) * to_h * 4, 0);
            pxc::downscale_bgra(src, src_stride_now, src_w_now, src_h_now,
                                scaled.data(), static_cast<size_t>(to_w) * 4,
                                to_w, to_h);
            enc_src = scaled.data();
            enc_stride = static_cast<size_t>(to_w) * 4;
            enc_in_w = to_w;
            enc_in_h = to_h;
        }
        p_cvt.push(ms_since(cvt_t0));

        // ---- 编码 ----
        const auto enc_t0 = Clock::now();
        std::vector<pxc::VideoFrame> out;
        if (!encoder->encode(enc_src, enc_stride, enc_in_w, enc_in_h,
                             frame_no == 0, out)) {
            std::printf("[t=%llu] encode 失败: %s\n",
                        static_cast<unsigned long long>(
                            std::chrono::duration_cast<std::chrono::seconds>(
                                Clock::now() - t_start).count()),
                        encoder->last_error().c_str());
            return 10;
        }
        p_enc.push(ms_since(enc_t0));
        ++win_src;
        for (const auto& f : out) {
            ++win_enc;
            if (f.keyframe) ++win_key;
            win_bytes += f.payload.size();
        }

        // ---- 回环解码 ----
        if (decoder && !out.empty()) {
            const auto dec_t0 = Clock::now();
            for (const auto& f : out) {
                pxc::DecodedFrame d;
                if (decoder->decode(f.payload.data(), f.payload.size(), d)) {
                    if (!d.bgra.empty()) {
                        ++win_dec;
                        win_dec_bytes += d.bgra.size();
                        decode_ok_seen = true;
                    }
                } else if (!decoder->last_error().empty()) {
                    decode_fail_seen = true;
                }
            }
            p_dec.push(ms_since(dec_t0));
        }

        ++frame_no;

        // ---- 每秒统计 ----
        const auto win_now = Clock::now();
        if (win_now - window_start >= std::chrono::seconds(1)) {
            const auto t_s = std::chrono::duration_cast<std::chrono::seconds>(
                                 win_now - t_start).count();
            const double win_secs =
                std::chrono::duration<double>(win_now - window_start).count();
            const uint32_t t = static_cast<uint32_t>(t_s);
            if (!warmup_done && t >= warmup_s) {
                warmup_done = true;
                std::printf("[t=%u] --- 预热结束 ---\n", t);
            }
            const char* tag = (t < warmup_s) ? "WARMUP" : "";
            std::printf("[t=%u] %s src=%llu cap_to=%llu t_cap=%.1f/%.1f "
                        "t_cvt=%.1f/%.1f t_enc=%.1f/%.1f enc=%llu key=%llu "
                        "out=%.2fMB dec=%llu t_dec=%.1f/%.1f fps=%.1f\n",
                        t, tag,
                        static_cast<unsigned long long>(win_src),
                        static_cast<unsigned long long>(win_cap_to),
                        p_cap.at(50), p_cap.at(95),
                        p_cvt.at(50), p_cvt.at(95),
                        p_enc.at(50), p_enc.at(95),
                        static_cast<unsigned long long>(win_enc),
                        static_cast<unsigned long long>(win_key),
                        win_bytes / 1048576.0,
                        static_cast<unsigned long long>(win_dec),
                        p_dec.at(50), p_dec.at(95),
                        win_src / std::max(win_secs, 0.001));
            win_src = win_cap_to = win_enc = win_key = win_dec = 0;
            win_bytes = win_dec_bytes = 0;
            window_start = win_now;
        }
    }

    // ---- 汇总 ----
    const auto total = std::chrono::duration_cast<std::chrono::seconds>(
                           Clock::now() - t_start).count();
    std::printf("--- 完成: %lld 秒 ---\n", static_cast<long long>(total));
    if (args.loop_decode) {
        std::printf("回环解码: %s\n", decode_fail_seen
            ? (decode_ok_seen ? "有成功有失败" : "全部失败")
            : (decode_ok_seen ? "正常" : "无输出（预热不足?）"));
    }
    if (!p_enc.empty()) {
        std::printf("编码耗时全程 P50=%.1fms P95=%.1fms P99=%.1fms\n",
                    p_enc.at(50), p_enc.at(95), p_enc.at(99));
    }
    return 0;
}
