// 无桌面平台 / 缺少 X11 时的兜底实现。
//
// 仅当 CMake 判定「本平台没有任何桌面采集/注入实现」时编译该文件，
// 保证 pxc_core 在纯服务器环境（如容器内的 Linux CI）也能链接。
// Windows 与有 X11 的 Linux 各自的平台文件提供了真正的实现。

#include <string>

#include "pxc/input_injector.h"
#include "pxc/screen_capturer.h"
#include "pxc/video_decoder.h"
#include "pxc/video_encoder.h"

#if !defined(_WIN32)

namespace pxc {

std::vector<ScreenInfo> enumerate_screens(std::string* error) {
    if (error) *error = "当前平台不支持屏幕采集";
    return {};
}

std::unique_ptr<ScreenCapturer> create_screen_capturer(std::string* error) {
    if (error) *error = "当前平台不支持屏幕采集";
    return nullptr;
}

std::unique_ptr<InputInjector> create_input_injector(std::string* error) {
    if (error) *error = "当前平台不支持键鼠注入";
    return nullptr;
}

}  // namespace pxc

#endif  // !_WIN32
