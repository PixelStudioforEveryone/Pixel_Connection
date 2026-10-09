#pragma once

// 屏幕采集：跨平台纯虚接口。
//
// 平台实现：
//   Windows  src/platform/windows/screen_capturer_win.cpp  (DXGI Desktop Duplication)
//   Linux    src/platform/linux/screen_capturer_x11.cpp    (X11/Xinerama)
//
// 约定（两端必须遵守，否则编码/传输会错位）：
//   - RawFrame 的像素格式一律为 BGRA（8bit x4，与 QImage::Format_ARGB32 同布局）；
//   - stride 是字节步长，不保证等于 width*4；
//   - capture() 返回 false 且 last_error() 为空表示「暂时没有新帧」（正常），
//     只有内部状态损坏（如 ACCESS_LOST 且重建失败）才写入错误信息。

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pxc {

// 一块屏幕（显示器）的描述。index 是本列表内的下标，
// gdi_device 是 Windows 上的 "\\.\DISPLAYx" 设备名，用于键鼠注入时
// 把屏幕 index 映射回虚拟桌面坐标；其他平台为空。
struct ScreenInfo {
    int         index      = 0;
    std::string name;        // 人类可读的名称，如 "DISPLAY1"
    std::string gdi_device;  // Windows 专用；用于注入端坐标映射
    uint32_t    x      = 0;  // 逻辑桌面坐标系中的原点（多屏时的位置）
    uint32_t    y      = 0;
    uint32_t    width  = 0;
    uint32_t    height = 0;
    bool        primary = false;
};

struct RawFrame {
    uint64_t frame_id     = 0;
    uint64_t timestamp_us = 0;
    uint32_t width        = 0;
    uint32_t height       = 0;
    uint32_t stride       = 0;  // 字节步长
    // BGRA 像素数据（CPU 内存）。GPU 纹理路径以后用不透明句柄扩展，
    // 避免在头文件里暴露 D3D 类型。
    std::shared_ptr<std::vector<uint8_t>> bgra;
};

class ScreenCapturer {
public:
    virtual ~ScreenCapturer() = default;

    // 在指定屏幕上开始采集。重复调用等价于先 stop() 再 start()。
    virtual bool start(int output_index) = 0;
    // 抓一帧。返回 true 表示 out 有效；false 表示暂时无新帧或出错。
    virtual bool capture(RawFrame& out, std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;
    // 最近一次致命错误；空串表示一切正常（含「无新帧」）。
    virtual std::string last_error() const = 0;
    // 调试历史（PipeWire 状态变迁等）；无调试信息的实现返回空串。
    virtual std::string debug_dump() const { return {}; }
};

// 枚举所有屏幕。失败返回空列表并把原因写入 error（可为 nullptr）。
std::vector<ScreenInfo> enumerate_screens(std::string* error = nullptr);

// 创建平台采集器；平台不支持或初始化失败时返回 nullptr（原因在 error）。
std::unique_ptr<ScreenCapturer> create_screen_capturer(std::string* error = nullptr);

}  // namespace pxc
