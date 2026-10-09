// M1 最小 P2P 验证程序。
//
// 用法：
//   pxc-demo --server ws://[服务器IPv6]:9910 --name 主控
//   pxc-demo --server ws://[服务器IPv6]:9910 --name 被控
//
// 两个终端启动后，输入 list 查看在线设备，再输入 connect <设备ID>。
// 对端输入 accept 或 reject；accept 后通过 WebRTC DataChannel 互发消息。
//
// 这个程序只验证信令、IPv6/IPv4 ICE 和 DataChannel，不包含屏幕采集。

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "pxc/account_api.h"
#include "pxc/crypto.h"
#include "pxc/identity.h"
#include "pxc/peer_session.h"
#include "pxc/protocol.h"
#include "pxc/session_auth.h"
#include "pxc/signaling_client.h"
#include "pxc/video_transport.h"

// IXNetSystem.h 在 Windows 上会引入 windows.h，先锁住 min/max 宏
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <ixwebsocket/IXNetSystem.h>

namespace {

struct DemoApp {
    std::string server_url = "ws://127.0.0.1:9910";
    std::string api_url    = "http://127.0.0.1:29910";
    std::string name       = "pxc-demo";
    std::string identity_path;
    bool        auto_accept = false;  // 收到连接请求时自动接受（模拟无人值守被控端）

    std::unique_ptr<pxc::SignalingClient>  signaling;
    std::unique_ptr<pxc::AccountApiClient> api;
    std::shared_ptr<pxc::PeerSession>      session;

    // 账号状态：token 只在内存里，不落盘
    std::string access_token;
    std::string username;

    std::mutex              io_mutex;
    std::condition_variable connected_cv;
    bool                    p2p_connected = false;
    std::atomic<bool>       stopping{false};
    std::shared_ptr<rtc::DataChannel> video_channel;
    std::unique_ptr<pxc::VideoReassembler> video_reassembler;
    uint32_t next_test_frame_id = 1;

    // 会话认证状态。
    // session_key：主控端本次连接输入的目标设备连接密钥。
    // device_key：本机作为被控端时自己的连接密钥，用于校验主控端的证明。
    std::string session_key;
    std::string device_key;
    std::string session_nonce;      // 被控端本次下发的挑战
    bool        session_authenticated = false;  // 证明通过前不接受任何控制数据

    void print(const std::string& text) {
        std::lock_guard<std::mutex> lock(io_mutex);
        std::cout << text << std::endl;
    }

    void setup() {
        if (identity_path.empty()) identity_path = "pxc-identity.json";

        api = std::make_unique<pxc::AccountApiClient>(api_url);
        signaling = std::make_unique<pxc::SignalingClient>(server_url, name);

        pxc::SignalingClient::Callbacks cb;
        cb.on_open = [this] {
            print("[信令] 已连接");
        };
        cb.on_close = [this](const std::string& reason) {
            print("[信令] 已断开: " + reason);
        };
        cb.on_error = [this](const std::string& reason) {
            print("[信令] 错误: " + reason);
        };
        cb.on_authenticated = [this](const pxc::Message& msg) {
            print("[账号] 认证通过（" + msg.account + "），正在用设备私钥签名上线...");
        };
        cb.on_registered = [this](const pxc::Message& msg) {
            print("[信令] 设备已上线: " + msg.device_id + "。本账号其他设备:");
            print_peers(msg.peers);
        };
        cb.on_message = [this](const pxc::Message& msg) {
            on_signaling_message(msg);
        };
        signaling->set_callbacks(std::move(cb));

        print("信令地址: " + server_url);
        print("账号 API: " + api_url);
        print("命令: login <用户名> <密码> 后，用 adddevice 把本机加入账号");
    }

    void on_signaling_message(const pxc::Message& msg) {
        if (msg.type == pxc::kRegistered) {
            print("[信令] 注册成功。在线设备:");
            print_peers(msg.peers);
            return;
        }
        if (msg.type == pxc::kPeerList) {
            print_peers(msg.peers);
            return;
        }
        if (msg.type == pxc::kPeerOnline) {
            print("[信令] 设备上线: " + msg.device_id);
            return;
        }
        if (msg.type == pxc::kPeerOffline) {
            print("[信令] 设备下线: " + msg.device_id);
            return;
        }
        if (msg.type == pxc::kError) {
            print("[信令] 错误 " + msg.code + ": " + msg.detail);
            return;
        }
        if (msg.type == pxc::kIncoming) {
            print("[会话] 收到来自 " + msg.from + " 的连接请求。");
            incoming_peer = msg.from;
            role = pxc::PeerSession::Role::Answerer;
            if (auto_accept) {
                // 模拟无人值守被控端：自动接受（接受后仍需主控端通过连接密钥认证）
                print("[会话] --auto-accept：自动接受，等待对端 SDP");
                answer(true);
            } else {
                print("[会话] 输入 accept 或 reject");
            }
            return;
        }
        if (msg.type == pxc::kAnswered) {
            print(std::string("[会话] 对方已答复: ") + (msg.accept ? "接受" : "拒绝"));
            if (!msg.accept) {
                close_session();
            } else {
                ensure_session(pxc::PeerSession::Role::Offerer);
            }
            return;
        }
        if (msg.type == pxc::kSdp) {
            ensure_session(role);
            session->set_remote_description(msg.sdp, msg.sdp_type);
            return;
        }
        if (msg.type == pxc::kCandidate) {
            ensure_session(role);
            session->add_remote_candidate(msg.candidate, msg.mid);
            return;
        }
    }

    void print_peers(const std::vector<pxc::PeerInfo>& peers) {
        if (peers.empty()) {
            print("  （没有其他在线设备）");
            return;
        }
        for (const auto& peer : peers) {
            print("  " + peer.device_id + "  " + peer.name);
        }
    }

    void ensure_session(pxc::PeerSession::Role wanted_role) {
        if (session) return;

        role = wanted_role;
        pxc::PeerSession::Config config;
        // IPv6 优先：ICE 同时收集 IPv6 和 IPv4 candidate。
        // 服务器部署在公网 IPv6 主机时，IPv6 candidate 会绕过对称 NAT。
        config.stun_servers = {"stun.l.google.com:19302", "stun.cloudflare.com:3478"};
        config.enable_ice_udp_mux = true;

        session = pxc::PeerSession::create(wanted_role, std::move(config));
        std::weak_ptr<pxc::PeerSession> weak = session;

        pxc::PeerSession::Callbacks cb;
        cb.on_log = [this](const std::string& text) {
            print("[P2P] " + text);
        };
        cb.on_state = [this](rtc::PeerConnection::State state) {
            if (state == rtc::PeerConnection::State::Connected) {
                {
                    std::lock_guard<std::mutex> lock(io_mutex);
                    p2p_connected = true;
                }
                connected_cv.notify_all();
                print("[P2P] ✅ WebRTC 已连接，已建立直连 DataChannel");
                if (session) print("[P2P] 候选地址: " + session->selected_pair_string());
            } else if (state == rtc::PeerConnection::State::Failed) {
                print("[P2P] ❌ 连接失败；如果两端都是 IPv4 对称 NAT，需要 TURN 中继或公网 IPv6");
            }
        };
        cb.on_local_description = [this](const std::string& sdp, const std::string& type) {
            pxc::Message msg;
            msg.type     = pxc::kSdp;
            msg.target   = remote_peer;
            msg.sdp      = sdp;
            msg.sdp_type = type;
            signaling->send(msg);
        };
        cb.on_local_candidate = [this](const std::string& candidate, const std::string& mid) {
            pxc::Message msg;
            msg.type      = pxc::kCandidate;
            msg.target    = remote_peer;
            msg.candidate = candidate;
            msg.mid       = mid;
            signaling->send(msg);
        };
        cb.on_data_channel = [this](std::shared_ptr<rtc::DataChannel> dc) {
            wire_channel(dc);
        };
        session->set_callbacks(std::move(cb));
        session->start();

        // offerer 创建的三个通道由 PeerSession 内部保存；这里挂接消息回调。
        if (wanted_role == pxc::PeerSession::Role::Offerer) {
            wire_channel(session->channel(pxc::kChControl));
            wire_channel(session->channel(pxc::kChVideo));
            wire_channel(session->channel(pxc::kChClip));
        }
    }

    void wire_channel(const std::shared_ptr<rtc::DataChannel>& dc) {
        if (!dc) return;
        if (dc->label() == pxc::kChVideo) {
            wire_video_channel(dc);
            return;
        }
        dc->onOpen([this, dc] {
            print("[通道] " + dc->label() + " 已打开");
            if (dc->label() == pxc::kChControl) {
                on_control_open(dc);
            }
        });
        dc->onClosed([this, dc] {
            print("[通道] " + dc->label() + " 已关闭");
            if (dc->label() == pxc::kChControl) session_authenticated = false;
        });
        dc->onError([this, dc](std::string error) {
            print("[通道] " + dc->label() + " 错误: " + error);
        });
        dc->onMessage([this, dc](rtc::message_variant data) {
            if (std::holds_alternative<std::string>(data)) {
                const std::string text = std::get<std::string>(std::move(data));
                if (dc->label() == pxc::kChControl) {
                    on_control_message(dc, text);
                    return;
                }
                print("[通道] " + dc->label() + " 收到: " + text);
            } else {
                print("[通道] " + dc->label() + " 收到二进制 " +
                      std::to_string(std::get<rtc::binary>(data).size()) + " 字节");
            }
        });
    }

    void wire_video_channel(const std::shared_ptr<rtc::DataChannel>& dc) {
        if (!dc || video_channel == dc) return;
        video_channel = dc;
        if (!video_reassembler) video_reassembler = std::make_unique<pxc::VideoReassembler>();

        dc->onOpen([this, dc] {
            print("[视频] ch-video 已打开；可使用 video-test <bytes> 发送测试帧");
        });
        dc->onClosed([this, dc] {
            print("[视频] ch-video 已关闭");
            if (video_channel == dc) video_channel.reset();
        });
        dc->onError([this](std::string error) {
            print("[视频] 通道错误: " + error);
        });
        dc->onMessage([this](rtc::message_variant data) {
            if (!std::holds_alternative<rtc::binary>(data)) {
                print("[视频] 忽略非二进制消息");
                return;
            }
            if (!video_reassembler) return;

            const auto& bytes = std::get<rtc::binary>(data);
            auto frame = video_reassembler->push(
                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
            if (frame) {
                print("[视频] 收到完整测试帧 id=" + std::to_string(frame->frame_id) +
                      " bytes=" + std::to_string(frame->payload.size()) +
                      " keyframe=" + (frame->keyframe ? "true" : "false"));
            }
        });
    }

    void send_test_frame(size_t bytes) {
        auto dc = video_channel;
        if (!dc || !dc->isOpen()) {
            print("视频通道尚未打开");
            return;
        }
        if (bytes == 0 || bytes > pxc::kVideoMaxFrameBytes) {
            print("测试帧长度必须在 1 到 " + std::to_string(pxc::kVideoMaxFrameBytes) + " 字节之间");
            return;
        }

        pxc::VideoFrame frame;
        frame.frame_id = next_test_frame_id++;
        frame.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        frame.codec = pxc::VideoCodec::Unknown;
        frame.keyframe = true;
        frame.payload.resize(bytes);
        for (size_t i = 0; i < bytes; ++i) {
            frame.payload[i] = static_cast<uint8_t>((i * 31 + frame.frame_id) & 0xff);
        }

        const size_t max_message = dc->maxMessageSize();
        if (max_message <= pxc::kVideoPacketHeaderSize) {
            print("协商的 DataChannel 消息上限过小");
            return;
        }
        const size_t payload_limit = std::min(
            pxc::kVideoPacketDefaultPayload, max_message - pxc::kVideoPacketHeaderSize);
        auto packets = pxc::packetize_video_frame(frame, payload_limit);
        if (packets.empty()) {
            print("测试帧封包失败");
            return;
        }
        if (dc->bufferedAmount() > 4 * 1024 * 1024) {
            print("视频发送队列已拥塞，丢弃测试帧");
            return;
        }

        size_t queued = 0;
        bool complete_enqueue = true;
        for (const auto& packet : packets) {
            if (!dc->isOpen() || dc->bufferedAmount() > 4 * 1024 * 1024) {
                complete_enqueue = false;
                break;
            }
            if (packet.size() > dc->maxMessageSize()) {
                print("分片超过协商的 DataChannel 消息上限");
                return;
            }
            rtc::binary binary(packet.size());
            std::transform(packet.begin(), packet.end(), binary.begin(),
                           [](uint8_t value) { return static_cast<std::byte>(value); });
            // libdatachannel 的 false 表示发送缓冲已满；该分片未能被正常排队。
            if (!dc->send(std::move(binary))) {
                complete_enqueue = false;
                break;
            }
            ++queued;
        }
        print("[视频] 测试帧 id=" + std::to_string(frame.frame_id) +
              " bytes=" + std::to_string(frame.payload.size()) +
              " fragments=" + std::to_string(packets.size()) +
              " queued=" + std::to_string(queued) +
              (complete_enqueue ? " (已全部排队)" : " (部分分片未排队，接收端将丢弃不完整帧)"));
    }

    // -------------------------------------------------------------- 会话认证

    // ch-control 打开。被控端主动下发挑战；主控端等挑战到达后再出示证明。
    void on_control_open(const std::shared_ptr<rtc::DataChannel>& dc) {
        session_authenticated = false;

        const bool is_answerer = (role == pxc::PeerSession::Role::Answerer);
        if (!is_answerer) {
            print("[会话] 等待被控端下发连接认证挑战...");
            return;
        }

        if (device_key.empty()) {
            print("[会话] ❌ 本机连接密钥不可用，拒绝接受控制");
            dc->send(pxc::kSessionAuthFail);
            return;
        }

        session_nonce = pxc::make_session_nonce();
        dc->send(std::string(pxc::kSessionAuthChallenge) + " " + session_nonce);
        print("[会话] 已下发连接认证挑战，等待主控端出示连接密钥");
    }

    void on_control_message(const std::shared_ptr<rtc::DataChannel>& dc,
                            const std::string&                      text) {
        const bool is_answerer = (role == pxc::PeerSession::Role::Answerer);

        // ---------------- 被控端：校验主控端出示的证明
        if (is_answerer) {
            const auto result = pxc::parse_session_auth_response(text);
            if (!result.parsed) {
                print("[会话] 控制消息: " + text);
                return;
            }

            // 挑战必须与本次下发的一致，防止重放上一轮的证明
            if (result.nonce_hex != session_nonce) {
                dc->send(pxc::kSessionAuthFail);
                print("[会话] ❌ 认证失败：挑战不匹配");
                return;
            }

            const std::string fingerprint = session ? session->local_dtls_fingerprint() : "";
            if (fingerprint.empty()) {
                dc->send(pxc::kSessionAuthFail);
                print("[会话] ❌ 认证失败：本端 DTLS 指纹不可用");
                return;
            }

            if (!pxc::verify_session_proof(device_key, result.nonce_hex, fingerprint,
                                           result.proof_hex)) {
                dc->send(pxc::kSessionAuthFail);
                print("[会话] ❌ 认证失败：连接密钥不正确");
                return;
            }

            session_authenticated = true;
            dc->send(pxc::kSessionAuthOk);
            print("[会话] ✅ 主控端连接密钥验证通过，本会话允许控制");
            return;
        }

        // ---------------- 主控端：收到挑战后出示证明
        std::string nonce;
        if (pxc::parse_session_auth_challenge(text, nonce)) {
            if (session_key.empty()) {
                print("[会话] ❌ 未提供连接密钥，无法完成认证");
                dc->send(pxc::kSessionAuthFail);
                return;
            }

            const std::string fingerprint = session ? session->remote_dtls_fingerprint() : "";
            if (fingerprint.empty()) {
                print("[会话] ❌ 对端 DTLS 指纹不可用，拒绝发送证明");
                return;
            }

            const std::string proof =
                pxc::compute_session_proof(session_key, nonce, fingerprint);
            if (proof.empty()) {
                print("[会话] ❌ 连接密钥格式无效");
                dc->send(pxc::kSessionAuthFail);
                return;
            }

            dc->send(std::string(pxc::kSessionAuthResponse) + " " + nonce + " " + proof);
            print("[会话] 已出示连接密钥证明，等待被控端确认...");
            return;
        }

        if (text == pxc::kSessionAuthOk) {
            session_authenticated = true;
            print("[会话] ✅ 连接密钥验证通过，现在可以控制该设备");
            return;
        }
        if (text == pxc::kSessionAuthFail) {
            session_authenticated = false;
            print("[会话] ❌ 认证失败：连接密钥错误，本会话不可用");
            return;
        }

        // 认证完成前，任何控制数据都不接受
        if (!session_authenticated) {
            print("[会话] 认证未完成，忽略控制消息");
            return;
        }
        print("[控制] " + text);
    }

    // 主控端发起连接：必须带上目标设备的连接密钥
    void request_connection(const std::string& peer, const std::string& key) {
        remote_peer = pxc::Identity::normalize_device_id(peer);
        session_key = key;
        session_authenticated = false;

        pxc::Message msg;
        msg.type   = pxc::kConnectRequest;
        msg.target = remote_peer;
        signaling->send(msg);
        print("[会话] 已请求连接 " + remote_peer + "，等待对方确认");
        print("[会话] 将使用你输入的本机连接密钥完成认证");
    }

    void answer(bool accept) {
        if (incoming_peer.empty()) {
            print("当前没有待处理的连接请求");
            return;
        }
        remote_peer = incoming_peer;
        pxc::Message msg;
        msg.type   = pxc::kConnectResponse;
        msg.target = incoming_peer;
        msg.accept = accept;
        signaling->send(msg);

        if (accept) {
            // 等 offer 到达后 on_signaling_message 会创建 Answerer session
            print("[会话] 已接受，等待对端 SDP...");
        } else {
            print("[会话] 已拒绝");
        }
        incoming_peer.clear();
    }

    void on_connect_request_from_server(const pxc::Message& msg) {
        (void)msg;
    }

    // ------------------------------------------------------------------ 账号

    void do_login(const std::string& user, const std::string& password) {
        std::string token, name, error;
        if (!api->login(user, password, token, name, error)) {
            print("[账号] 登录失败: " + error);
            return;
        }

        access_token = token;
        username     = name.empty() ? user : name;
        print("[账号] 登录成功: " + username + "（令牌仅保存在内存）");
        print("提示: 如果是本机第一次使用，请执行 adddevice 把本机加入账号");

        // 已加入过账号的设备直接带凭据上线
        pxc::Identity identity;
        if (pxc::Identity::load(identity_path, identity) && !identity.connection_key().empty()) {
            // 本机作为被控端时，用它校验主控端出示的证明
            device_key = identity.connection_key();
            signaling->set_credentials(access_token, identity, "");
            print("[信令] 使用已有设备身份上线: " + identity.device_id());
            if (!signaling->connected()) signaling->start();
        }
    }

    void do_add_device() {
        if (access_token.empty()) {
            print("[账号] 请先 login <用户名> <密码>");
            return;
        }

        // 已经是本账号的设备就不重复生成密钥
        pxc::Identity existing;
        if (pxc::Identity::load(identity_path, existing) && !existing.connection_key().empty()) {
            print("[账号] 本机已是设备 " + existing.device_id() + "，无需重复添加");
            print("连接密钥: " + existing.connection_key());
            device_key = existing.connection_key();
            signaling->set_credentials(access_token, existing, "");
            if (!signaling->connected()) signaling->start();
            return;
        }

        // 用户明确点击「添加本机」后才生成设备私钥和设备 ID。
        // 私钥只写入本机文件（0600），永不上传。
        pxc::Identity identity = pxc::Identity::generate();
        identity.set_connection_key(pxc::crypto::generate_connection_key());
        if (!identity.save(identity_path)) {
            print("[账号] 保存设备身份失败: " + identity_path);
            return;
        }

        print("=== 本机已加入账号 ===");
        print("设备 ID:   " + identity.device_id());
        print("设备名称:   " + name);
        print("连接密钥:   " + identity.connection_key());
        print("请抄下连接密钥：主控端每次连接本机都要输入它。");
        print("设备私钥保存在 " + identity_path + "（权限 0600），不会上传。");

        // 本机作为被控端时，用它校验主控端出示的证明
        device_key = identity.connection_key();

        signaling->set_credentials(access_token, identity, "");
        if (!signaling->connected()) signaling->start();
    }

    void show_connection_key() {
        pxc::Identity identity;
        if (!pxc::Identity::load(identity_path, identity) || identity.connection_key().empty()) {
            print("[账号] 本机尚未加入账号，请先 login 再 adddevice");
            return;
        }
        print("设备 ID: " + identity.device_id());
        print("连接密钥: " + identity.connection_key());
    }

    void do_list_devices() {
        if (access_token.empty()) {
            print("[账号] 请先 login <用户名> <密码>");
            return;
        }

        std::vector<pxc::PeerInfo> devices;
        std::string                error;
        if (!api->list_devices(access_token, devices, error)) {
            print("[账号] 获取设备列表失败: " + error);
            return;
        }

        print("=== 本账号下的设备 ===");
        if (devices.empty()) {
            print("  （还没有设备，在本机执行 adddevice 可加入）");
            return;
        }
        for (const auto& d : devices) {
            print("  " + d.device_id + "  " + d.name + "  " +
                  (d.public_ip.empty() ? "IP未知" : d.public_ip) + "  " +
                  (d.online ? "在线" : "离线"));
        }
        print("说明: IP 是服务器观测到的信令连接地址，不保证可直连。");
    }

    void close_session() {
        if (session) session->close();
        session.reset();
        p2p_connected = false;
    }

    void command_loop() {
        print("命令: list | connect <设备ID> | accept | reject | send <文本> | video-test <字节数> | quit");
        std::string line;
        while (!stopping.load() && std::getline(std::cin, line)) {
            if (line == "list") {
                pxc::Message msg;
                msg.type = pxc::kListPeers;
                signaling->send(msg);
            } else if (line.rfind("connect ", 0) == 0) {
                std::istringstream iss(line.substr(8));
                std::string peer, key;
                iss >> peer >> key;
                if (peer.empty()) {
                    print("用法: connect <设备ID> <连接密钥>");
                } else if (key.empty()) {
                    print("连接必须提供目标设备的连接密钥。");
                    print("用法: connect <设备ID> <连接密钥>");
                    print("密钥在被控端执行 adddevice 时显示（也可用 key 命令再查看）。");
                } else {
                    request_connection(peer, key);
                }
            } else if (line == "accept") {
                answer(true);
            } else if (line == "reject") {
                answer(false);
            } else if (line.rfind("send ", 0) == 0) {
                auto dc = session ? session->channel(pxc::kChControl) : nullptr;
                if (!dc || !dc->isOpen()) print("控制通道尚未打开");
                else dc->send(line.substr(5));
            } else if (line.rfind("video-test ", 0) == 0) {
                try {
                    const auto bytes = std::stoull(line.substr(11));
                    send_test_frame(static_cast<size_t>(bytes));
                } catch (const std::exception&) {
                    print("video-test 参数必须是字节数");
                }
            } else if (line.rfind("login ", 0) == 0) {
                std::istringstream iss(line.substr(6));
                std::string user, password;
                iss >> user >> password;
                if (user.empty() || password.empty()) {
                    print("用法: login <用户名或邮箱> <密码>");
                } else {
                    do_login(user, password);
                }
            } else if (line == "adddevice") {
                do_add_device();
            } else if (line == "devices") {
                do_list_devices();
            } else if (line == "key") {
                show_connection_key();
            } else if (line == "quit" || line == "exit") {
                stopping.store(true);
                break;
            } else if (!line.empty()) {
                print("未知命令: " + line);
            }
        }
    }

    void run() {
        setup();
        signaling->start();
        command_loop();
        close_session();
        signaling->stop();
    }

private:
    std::string incoming_peer;
    std::string remote_peer;
    pxc::PeerSession::Role role = pxc::PeerSession::Role::Offerer;
};

void usage(const char* argv0) {
    std::cout << "用法: " << argv0
              << " [--server WS_URL] [--api-url HTTP_URL] [--name NAME] [--identity FILE]\n"
              << " [--auto-accept]\n"
              << "  --server    信令服务器地址 (默认 ws://127.0.0.1:9910)\n"
              << "  --api-url   账号 API 地址   (默认 http://127.0.0.1:29910)\n"
              << "  --name      设备显示名称\n"
              << "  --identity  设备身份文件 (默认 pxc-identity.json)\n"
              << "\n运行时命令: login <用户> <密码> | adddevice | devices | key\n"
              << "            list | connect <设备ID> | accept | reject | send <文本>\n"
              << "            video-test <字节数> | quit\n";
}

}  // namespace

int main(int argc, char** argv) {
    DemoApp app;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) app.server_url = argv[++i];
        else if (arg == "--api-url" && i + 1 < argc) app.api_url = argv[++i];
        else if (arg == "--name" && i + 1 < argc) app.name = argv[++i];
        else if (arg == "--identity" && i + 1 < argc) app.identity_path = argv[++i];
        else if (arg == "--auto-accept") app.auto_accept = true;
        else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "未知参数: " << arg << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    try {
        // Windows 上 ixwebsocket 需要应用显式初始化 WinSock；Linux 为空操作
        const bool net_inited = ix::initNetSystem();
        app.run();
        if (net_inited) ix::uninitNetSystem();
    } catch (const std::exception& e) {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
