#pragma once

// P2P 会话：对 libdatachannel 的封装。
//
// 一条会话固定开四条 DataChannel：
//   ch-control 可靠有序   —— 键鼠事件、会话控制、心跳
//   ch-video   不可靠无序 —— 视频帧，允许丢帧，丢帧不阻塞后续
//   ch-clip    可靠有序   —— 剪贴板，大数据自行分块
//   ch-file    可靠有序   —— 文件传输控制与分块数据
//
// 本文件只处理连接建立与通道管理，不涉及采集/编码/注入。

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

namespace pxc {

// 通道名常量。两端必须一致，否则 onDataChannel 无法按 label 匹配。
inline constexpr const char* kChControl = "ch-control";
inline constexpr const char* kChVideo   = "ch-video";
inline constexpr const char* kChClip    = "ch-clip";
inline constexpr const char* kChFile    = "ch-file";

class PeerSession : public std::enable_shared_from_this<PeerSession> {
public:
    enum class Role {
        Offerer,   // 主动发起方（主控端）
        Answerer,  // 被动接受方（被控端）
    };

    struct IceServer {
        std::string url;       // stun: / turn: / turns:
        std::string username;  // TURN 可选
        std::string credential; // TURN 可选；禁止写日志
    };

    struct Config {
        // 新代码使用结构化列表；stun_servers 保留兼容旧调用方。
        std::vector<IceServer> ice_servers;
        std::vector<std::string> stun_servers;
        bool relay_only = false;
        // ICE 使用的绑定地址。留空表示由底层决定（通常是双栈 any）。
        // 指定 "::" 可强制优先 IPv6 —— 在对称 NAT 环境下这是主要出路。
        std::string bind_address;
        bool        enable_ice_udp_mux = true;
        size_t      mtu                = 1200;
    };

    struct Callbacks {
        // 本端生成的 SDP，需通过信令发给对端
        std::function<void(const std::string& sdp, const std::string& type)> on_local_description;
        // 本端收集到的 ICE candidate，需通过信令发给对端
        std::function<void(const std::string& candidate, const std::string& mid)> on_local_candidate;
        // 连接状态变化（connecting / connected / disconnected / failed / closed）
        std::function<void(rtc::PeerConnection::State)> on_state;
        // 对端开的通道到达
        std::function<void(std::shared_ptr<rtc::DataChannel>)> on_data_channel;
        // 人类可读的进度日志
        std::function<void(const std::string&)> on_log;
    };

    static std::shared_ptr<PeerSession> create(Role role, Config config);

    ~PeerSession();

    PeerSession(const PeerSession&)            = delete;
    PeerSession& operator=(const PeerSession&) = delete;

    void set_callbacks(Callbacks cb);

    // 建立 PeerConnection。Offerer 会额外创建四条通道并发出 offer。
    void start();

    void set_remote_description(const std::string& sdp, const std::string& type);
    void add_remote_candidate(const std::string& candidate, const std::string& mid);

    // 取通道；未建立时返回 nullptr
    std::shared_ptr<rtc::DataChannel> channel(const std::string& label);

    // 是否至少有一条通道打开（即 P2P 真正可用）
    bool is_connected() const;

    void close();

    std::string state_string() const;

    // 当前往返时延，用于后续自适应码率。链路未建立时返回 nullopt。
    std::optional<std::chrono::milliseconds> rtt() const;

    // 底层选中的候选地址对，形如 "1.2.3.4:5000 <-> 5.6.7.8:6000"；未选中时返回空串
    std::string selected_pair_string() const;

    // 本次 P2P 的 DTLS 证书指纹，形如 "sha-256 AB:CD:..."。
    // 用于把应用层认证绑定到这条具体通道：中间人转发的连接指纹不同，认证必然失败。
    // 描述尚未交换完成时返回空串。
    std::string local_dtls_fingerprint() const;
    std::string remote_dtls_fingerprint() const;

private:
    PeerSession(Role role, Config config);
    void setup_peer_connection();
    void create_local_channels();
    void log(const std::string& msg) const;
    void invoke_log(const std::string& msg) const;

    Role                                role_;
    Config                              config_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    Callbacks                           cb_;
    mutable std::mutex                  cb_mutex_;

    mutable std::mutex                                   ch_mutex_;
    std::map<std::string, std::shared_ptr<rtc::DataChannel>> channels_;
};

}  // namespace pxc
