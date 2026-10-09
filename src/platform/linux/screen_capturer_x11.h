#pragma once

// Linux 平台内部头：X11 采集实现的工厂入口。
// 公共工厂在 screen_capturer_linux.cpp 统一分发（PipeWire 优先）。

#include <string>
#include <vector>

#include "pxc/screen_capturer.h"

namespace pxc {

std::vector<ScreenInfo> enumerate_screens_x11(std::string* error = nullptr);
std::unique_ptr<ScreenCapturer> create_screen_capturer_x11(std::string* error = nullptr);

#if defined(PXC_HAVE_PIPEWIRE)
std::unique_ptr<ScreenCapturer> create_screen_capturer_pipewire(std::string* error = nullptr);
bool pipewire_environment_available();
#endif

}  // namespace pxc
