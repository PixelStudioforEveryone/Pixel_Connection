#pragma once

// HOS 主控端会话控制器：apps/client/client_controller.cpp 的 Qt-free 移植。
//
// 差异（相对桌面蓝本）：
//   - 仅主控端：入站 connect_request 一律拒绝（accept=false）
//   - 线程模型：所有回调（IXWebSocket I/O 线程、rtc 回调线程、worker 线程）
//     直接触发，状态经互斥锁保护；UI 事件经 EventSink 投递到 JS 线程
//   - 视频解码不在这里：ch-video 重组出完整帧后经 FrameSink 交给
//     Phase 5 的 OH_AVCodec surface 解码器（native 内部，不过 JS）
//   - 「记住连接密钥」「自动重登凭据」由 ArkTS 侧 preferences 承接：
//     认证成功时发 keyVerified 事件供持久化，token 过期时 native 自动重登
//
// 事件（EventSink 第一参）与 JSON 载荷（第二参）：
//   log               {"message": string}
//   loginResult       {"ok": bool, "username"?: string, "error"?: string}
//   registerResult    {"ok": bool, "message": string}
//   devicesUpdated    {"devices": [{deviceId,name,publicIp,lastSeen,platform,online}]}
//   deviceListFailed  {"error": string}
//   deviceEnrolled    {"deviceId": string, "connectionKey": string}
//   connectionKeyChanged {"deviceId": string, "connectionKey": string}
//   deviceEnrollFailed {"error": string}
//   signalingOnline   {"online": bool, "deviceId"?: string}
//   sessionState      {"deviceId": string, "state": string}
//   keyVerified       {"deviceId": string, "connectionKey": string}
//   screensUpdated    {"screens": [...]}
//   videoState        {"width","height","fps","screen","encoder","error"}
//   videoNotice       {"message": string, "isError": bool}
//   fileEvent         {"op": "ready"} 或远端 JSON 原文
//
// sessionState 取值（与桌面端词汇一致）：
//   connecting / authenticating / authenticated / connected / rejected /
//   closed / p2p_timeout

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "pxc/account_api.h"
#include "pxc/identity.h"
#include "pxc/peer_session.h"
#include "pxc/signaling_client.h"
#include "pxc/video_transport.h"
#include "pxc/video_reference_guard.h"

namespace pxc::hos {

class HosController {
public:
    // event + json 载荷；实现方负责跨线程投递（NAPI 层用 threadsafe function）
    using EventSink  = std::function<void(const std::string& event, const std::string& json)>;
    // 完整视频帧回调：(数据, 长度, frame_id, 是否关键帧, 时间戳 us)。Phase 5 解码器注入。
    using FrameSink  = std::function<void(const uint8_t* data, size_t size, uint32_t frameId,
                                          bool keyframe, uint64_t timestampUs)>;

    explicit HosController(EventSink sink);
    ~HosController();

    HosController(const HosController&)            = delete;
    HosController& operator=(const HosController&) = delete;

    // ------------------------------------------------------------- 初始化
    // files_dir：应用沙箱文件目录（identity.json 存放处），ArkTS 传 context.filesDir
    // device_name：信令注册时上报的设备名
    void configure(std::string api_url, std::string ws_url, std::string files_dir,
                   std::string device_name);

    // ------------------------------------------------------------- 本机身份
    bool        isEnrolled() const;
    std::string localDeviceId() const;
    std::string localConnectionKey() const;
    bool setConnectionKey(const std::string& key);

    // 「添加本机」：生成 Ed25519 身份 + 连接密钥并落盘，随后自动尝试信令上线
    void addThisDevice();

    // ------------------------------------------------------------- 账号
    void login(const std::string& identifier, const std::string& password);
    void restoreLogin(const std::string& token, const std::string& username);
    nlohmann::json getLoginSession();
    void cancelLoginAttempt();
    void registerAccount(const std::string& username, const std::string& email,
                         const std::string& password);
    void logout();
    void refreshDevices();

    // ------------------------------------------------------------- 网络
    void setIceServers(const std::string& stun_url, const std::string& turn_url,
                       const std::string& turn_user, const std::string& turn_pass);

    // ------------------------------------------------------------- 会话（主控端）
    void connectToDevice(const std::string& device_id, const std::string& connection_key);
    void disconnect();
    bool sessionAuthenticated() const;

    // ------------------------------------------------------------- 控制面
    void requestRemoteScreens();
    void requestRemoteWallpaper();
    std::string cachedWallpaper(const std::string& device_id);
    void switchRemoteScreen(int index);
    void setRemoteVideoConfig(int width, int height, int fps, int bitrate_kbps);
    void requestRemoteKeyframe();
    // payload 形如 {"type":"mouse_move","x":0.5,"y":0.5,"screen":0}
    void sendInputEvent(const std::string& payload_json);
    bool sendClipboardText(const std::string& text);
    bool fileChannelReady();
    bool fileCommand(const std::string& json);
    std::string uploadFile(int fd, const std::string& name, const std::string& directory);
    std::string downloadFile(int fd, const std::string& remote_path, const std::string& name);
    void cancelFileTransfer(const std::string& id);

    // ------------------------------------------------------------- 视频帧出口
    void setFrameSink(FrameSink sink);
    void resetFrameSink();

private:
    void emit(std::string event, nlohmann::json payload);
    void runAsync(std::function<void()> fn);
    void joinWorkers();
    void startSignalingIfPossible();
    void handleSignalingMessage(const pxc::Message& msg);
    void ensureSession(pxc::PeerSession::Role role);
    void wireChannel(const std::shared_ptr<rtc::DataChannel>& channel);
    void onControlMessage(const std::string& text);
    void handleSessionJson(const nlohmann::json& j);
    void sendSessionJson(const nlohmann::json& j);
    // 完整清理（等价桌面 disconnectSession 的会话部分）；可在任意线程调用
    void finishSession(std::shared_ptr<pxc::PeerSession> expected = nullptr, bool notify = true);
    void pushVideoFragment(const uint8_t* data, size_t size);
    void maybeRequestKeyframeForHole();
    void handleFileMessage(const nlohmann::json& message);
    void handleFileChunk(const rtc::binary& bytes);
    void pumpFile(const std::string& id, std::shared_ptr<rtc::DataChannel> channel);
    void finishFile(const std::string& state, bool completed = false);
    void fileProgress(const std::string& state, bool done = false);
    std::shared_ptr<rtc::DataChannel> fileChannel();

    void armAuthTimeout();
    void cancelAuthTimeout();
    void onAuthTimeout();

    std::string identityPath() const;

    // ------------------------------------------------------------------ 状态
    EventSink              sink_;            // 构造后不变
    std::atomic<bool>      destroyed_{false};

    mutable std::mutex     cfg_mtx_;
    std::string            api_url_;
    std::string            ws_url_;
    std::string            files_dir_;
    std::string            device_name_;
    std::string            stun_url_;
    std::string            turn_url_;
    std::string            turn_user_;
    std::string            turn_pass_;

    mutable std::mutex     id_mtx_;
    std::string            local_device_id_;
    std::string            local_connection_key_;

    std::mutex             acct_mtx_;
    std::string            access_token_;
    std::string            username_;
    std::string            auto_identifier_;
    std::string            auto_password_;

    // api_ 构造后基本不变；signaling_ 同；session_ 生命周期敏感
    std::mutex             core_mtx_;
    std::shared_ptr<pxc::AccountApiClient>   api_;
    std::atomic<uint64_t> account_generation_{0};
    std::unique_ptr<pxc::SignalingClient>    signaling_;
    std::shared_ptr<pxc::PeerSession>        session_;

    mutable std::mutex     sess_mtx_;
    std::string            target_device_id_;
    std::string            target_connection_key_;
    bool                   session_authenticated_ = false;
    std::string clipboard_text_;
    bool clipboard_has_text_ = false;
    bool                   wallpaper_saved_ = false;
    std::recursive_mutex file_mtx_;
    int file_fd_ = -1;
    std::string file_id_, file_name_;
    int64_t file_total_ = 0, file_transferred_ = 0;
    bool file_upload_ = false, file_started_ = false;
    std::chrono::steady_clock::time_point file_activity_;
    void watchFile(const std::string& id);

    std::mutex             video_mtx_;
    pxc::VideoReassembler  reassembler_;
    pxc::VideoReferenceGuard video_reference_;
    FrameSink              frame_sink_;
    uint64_t               last_hole_keyframe_ms_ = 0;
    uint64_t               last_drop_count_      = 0;

    // worker 线程池（HTTP 等阻塞调用）
    std::mutex             workers_mtx_;
    std::vector<std::thread> workers_;

    // P2P 认证超时（20s）：单定时线程 + 代际计数
    std::mutex             auth_timer_mtx_;
    std::condition_variable auth_timer_cv_;
    std::thread            auth_timer_thread_;
    uint64_t               auth_timer_gen_ = 0;
};

}  // namespace pxc::hos
