// Windows 屏幕采集：DXGI Desktop Duplication。
//
// 设计要点（与 docs/WINDOWS_TASKS.md 一致）：
//   - AcquireNextFrame 超时是正常情况（画面没变化），返回 false 不算错误；
//   - DXGI_ERROR_ACCESS_LOST 必须重建 duplication（锁屏/UAC/切屏都会触发），
//     重建有节流，避免在安全桌面上疯狂重试；
//   - 分辨率变化时 staging 纹理跟随重建；
//   - 采集线程只做 CopyResource + 一次 Map 拷出，编码/缩放留给发送线程。

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>
#include <utility>

#include "pxc/screen_capturer.h"

namespace pxc {
namespace {

// COM 初始化守卫：同线程同模式的重复初始化返回 S_FALSE（也算成功），
// 析构时配对 CoUninitialize，保持计数平衡。
struct ComGuard {
    ComGuard() { owns = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
    ~ComGuard() {
        if (owns) ::CoUninitialize();
    }
    bool owns = false;
};

class ScreenCapturerWin : public ScreenCapturer {
public:
    ~ScreenCapturerWin() override { stop(); }

    bool start(int output_index) override {
        stop();
        ComGuard com;
        if (!open_output(output_index) || !create_staging()) {
            release_all();
            return false;
        }
        index_   = output_index;
        started_ = true;
        return true;
    }

    void stop() override {
        release_duplication();
        staging_.Reset();
        context_.Reset();
        device_.Reset();
        adapter_.Reset();
        started_ = false;
    }

    bool capture(RawFrame& out, std::chrono::milliseconds timeout) override {
        if (!started_) return fail("采集器未启动");

        // duplication 丢失（锁屏/UAC/切屏）后重建；节流 500ms，
        // 避免在安全桌面等不可采集状态下空转。
        if (!duplication_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_rebuild_ < std::chrono::milliseconds(500)) return false;
            last_rebuild_ = now;
            ComGuard com;
            if (!open_output(index_) || !create_staging()) {
                release_duplication();
                staging_.Reset();
                return false;  // error_ 已写明原因
            }
        }

        ComGuard com;
        IDXGIResource* resource = nullptr;
        const HRESULT hr = duplication_->AcquireNextFrame(
            static_cast<UINT>(timeout.count()), &frame_info_, &resource);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;  // 无新帧，正常

        if (hr == DXGI_ERROR_ACCESS_LOST) {
            release_duplication();  // 下次 capture 重建
            return false;
        }
        if (FAILED(hr)) {
            release_duplication();
            return fail("AcquireNextFrame 失败 (hr=" + std::to_string(hr) + ")");
        }

        ID3D11Texture2D* frame_tex = nullptr;
        if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&frame_tex)))) {
            resource->Release();
            duplication_->ReleaseFrame();
            return fail("桌面帧不是 D3D11 纹理");
        }
        resource->Release();

        const bool ok = copy_frame(frame_tex);
        frame_tex->Release();
        duplication_->ReleaseFrame();
        if (!ok) return false;

        out = std::move(pending_);
        ++frame_id_;
        return true;
    }

    std::string last_error() const override {
        std::lock_guard<std::mutex> lock(err_mutex_);
        return error_;
    }

private:
    bool open_output(int output_index) {
        IDXGIFactory1* factory = nullptr;
        HRESULT hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return fail("CreateDXGIFactory1 失败 (hr=" + std::to_string(hr) + ")");

        int            cursor         = 0;
        IDXGIAdapter1* chosen_adapter = nullptr;
        IDXGIOutput*   chosen_output  = nullptr;
        IDXGIAdapter1* adapter        = nullptr;
        for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
            IDXGIOutput* output = nullptr;
            for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
                DXGI_OUTPUT_DESC desc{};
                if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                    if (cursor == output_index) {
                        // 引用计数转给 chosen_*，随后直接持有
                        chosen_adapter = adapter;
                        chosen_output  = output;
                        break;
                    }
                    ++cursor;
                }
                output->Release();
            }
            if (chosen_output) break;
            adapter->Release();
        }
        factory->Release();

        if (!chosen_output) {
            return fail("屏幕 " + std::to_string(output_index) + " 不存在");
        }

        DXGI_OUTPUT_DESC desc{};
        chosen_output->GetDesc(&desc);
        width_  = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        height_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        adapter_ = chosen_adapter;  // 旧的 adapter_ 引用由赋值释放
        hr = ::D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 device_.ReleaseAndGetAddressOf(), nullptr,
                                 context_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            chosen_output->Release();
            return fail("D3D11CreateDevice 失败 (hr=" + std::to_string(hr) + ")");
        }

        IDXGIOutput1* output1 = nullptr;
        hr = chosen_output->QueryInterface(IID_PPV_ARGS(&output1));
        if (FAILED(hr)) {
            chosen_output->Release();
            return fail("IDXGIOutput1 不可用 (hr=" + std::to_string(hr) + ")");
        }
        hr = output1->DuplicateOutput(device_.Get(), &duplication_);
        output1->Release();
        chosen_output->Release();
        if (FAILED(hr)) {
            return fail("DuplicateOutput 失败 (hr=" + std::to_string(hr) +
                        ")；可能被其他进程占用或处于安全桌面");
        }
        return true;
    }

    bool create_staging() {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width            = width_;
        desc.Height           = height_;
        desc.MipLevels        = 1;
        desc.ArraySize        = 1;
        desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage            = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

        const HRESULT hr =
            device_->CreateTexture2D(&desc, nullptr, staging_.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return fail("创建 staging 纹理失败 (hr=" + std::to_string(hr) + ")");
        return true;
    }

    bool copy_frame(ID3D11Texture2D* frame_tex) {
        D3D11_TEXTURE2D_DESC tex_desc{};
        frame_tex->GetDesc(&tex_desc);

        // 分辨率变化（改分辨率/显示模式切换）时 staging 需要重建
        if (tex_desc.Width != width_ || tex_desc.Height != height_ ||
            tex_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            width_  = tex_desc.Width;
            height_ = tex_desc.Height;
            staging_.Reset();
            if (!create_staging()) return false;
        }

        context_->CopyResource(staging_.Get(), frame_tex);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return fail("Map staging 失败 (hr=" + std::to_string(hr) + ")");

        auto buffer = std::make_shared<std::vector<uint8_t>>();
        buffer->resize(mapped.RowPitch * static_cast<size_t>(height_));
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        for (UINT row = 0; row < height_; ++row) {
            std::memcpy(buffer->data() + mapped.RowPitch * static_cast<size_t>(row),
                        src + mapped.RowPitch * static_cast<size_t>(row),
                        static_cast<size_t>(width_) * 4);
        }
        context_->Unmap(staging_.Get(), 0);

        RawFrame frame;
        frame.frame_id     = frame_id_ + 1;
        frame.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        frame.width  = width_;
        frame.height = height_;
        frame.stride = static_cast<uint32_t>(mapped.RowPitch);
        frame.bgra   = std::move(buffer);
        pending_ = std::move(frame);
        return true;
    }

    bool fail(const std::string& reason) {
        std::lock_guard<std::mutex> lock(err_mutex_);
        error_ = reason;
        return false;
    }

    void release_duplication() {
        if (duplication_) {
            duplication_->Release();
            duplication_ = nullptr;
        }
    }

    void release_all() {
        release_duplication();
        staging_.Reset();
        context_.Reset();
        device_.Reset();
        adapter_.Reset();
    }

    int      index_    = 0;
    UINT     width_    = 0;
    UINT     height_   = 0;
    bool     started_  = false;
    uint64_t frame_id_ = 0;

    Microsoft::WRL::ComPtr<IDXGIAdapter1>       adapter_;
    Microsoft::WRL::ComPtr<ID3D11Device>        device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>     staging_;
    IDXGIOutputDuplication*                     duplication_ = nullptr;

    DXGI_OUTDUPL_FRAME_INFO frame_info_{};
    RawFrame                pending_;
    std::chrono::steady_clock::time_point last_rebuild_{};

    mutable std::mutex err_mutex_;
    std::string        error_;
};

}  // namespace

std::vector<ScreenInfo> enumerate_screens(std::string* error) {
    std::vector<ScreenInfo> screens;
    ComGuard com;

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        if (error) *error = "CreateDXGIFactory1 失败 (hr=" + std::to_string(hr) + ")";
        return screens;
    }

    IDXGIAdapter1* adapter = nullptr;
    int index = 0;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        IDXGIOutput* output = nullptr;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) {
                output->Release();
                continue;
            }

            ScreenInfo info;
            info.index = index++;
            // DeviceName 形如 "\\.\DISPLAY1"：gdi_device 保留全名
            // （键鼠注入时用它和 GDI 显示器一一对应），name 只留可读部分
            const std::wstring wdevice(desc.DeviceName);
            info.gdi_device.reserve(wdevice.size());
            for (wchar_t c : wdevice) info.gdi_device.push_back(static_cast<char>(c));
            const auto slash = wdevice.find_last_of(L'\\');
            if (slash != std::wstring::npos) {
                for (size_t i = slash + 1; i < wdevice.size(); ++i) {
                    info.name.push_back(static_cast<char>(wdevice[i]));
                }
            } else {
                info.name = info.gdi_device;
            }
            info.x      = static_cast<uint32_t>(desc.DesktopCoordinates.left);
            info.y      = static_cast<uint32_t>(desc.DesktopCoordinates.top);
            info.width  = static_cast<uint32_t>(desc.DesktopCoordinates.right -
                                                desc.DesktopCoordinates.left);
            info.height = static_cast<uint32_t>(desc.DesktopCoordinates.bottom -
                                                desc.DesktopCoordinates.top);

            // 主屏判定走 GDI 的 MONITORINFOF_PRIMARY，比「原点在 0,0」更可靠
            const RECT rect = desc.DesktopCoordinates;
            if (const HMONITOR monitor = ::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL)) {
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                if (::GetMonitorInfoW(monitor, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                    info.primary = true;
                }
            }
            screens.push_back(std::move(info));
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();

    if (screens.empty() && error) *error = "没有可采集的桌面输出";
    return screens;
}

std::unique_ptr<ScreenCapturer> create_screen_capturer(std::string* error) {
    (void)error;  // 构造不会失败；错误延后到 start()/capture() 报告
    return std::make_unique<ScreenCapturerWin>();
}

}  // namespace pxc

#endif  // _WIN32
