// 信令服务器 + 账号 API。
//
// 它仍然是整个系统里唯一的中心节点，但只做控制面：
//   - 账号注册/登录（HTTP API，独立端口）
//   - 设备列表、改名、删除、撤销（HTTP API）
//   - 设备上线认证（WebSocket，token + 私钥签名挑战）
//   - 转发会话协商消息（SDP / ICE candidate）
//
// 画面、键鼠、剪贴板全部走 P2P，不经过这里。
// 单次会话的信令量是 KB 级，所以对带宽几乎没有要求。
//
// 账号隔离：设备只能看到同账号下的其他设备，也只能向同账号设备发起连接。
// 未通过 token 认证的连接不能作为设备上线，也无法发起或被发起连接。
//
// 默认同时监听 IPv4 和 IPv6。在对称 NAT 环境下 IPv6 是主要的连通路径，
// 两个监听共用同一张在线设备表，因此跨协议族的设备之间也能协商。

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>  // AF_INET / AF_INET6，必须位于 windows.h 之前
#include <windows.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <ixwebsocket/IXConnectionState.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "auth_service.h"
#include "http_api.h"
#include "pxc/identity.h"
#include <set>
#include "pxc/protocol.h"
#include "registry.h"
#include "proxy_client_ip.h"
#include "store.h"
#include "server_runtime.h"

namespace {

std::atomic<bool> g_running{true};

void on_signal(int) {
    g_running.store(false);
}

std::string now_string() {
    char        buf[32];
    std::time_t t = std::time(nullptr);
    std::tm     tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
    return buf;
}

void log_line(const std::string& who, const std::string& text) {
    std::cout << "[" << now_string() << "] " << who << " " << text << std::endl;
}

// IPv4-mapped IPv6 地址显示成纯 IPv4，日志更易读
std::string short_addr(const std::string& ip) {
    if (ip.rfind("::ffff:", 0) == 0) return ip.substr(7);
    return ip;
}

struct ServerOptions {
    int         port      = 9910;
    int         api_port  = 29910;
    std::string v4_host   = "0.0.0.0";
    std::string v6_host   = "::";
    bool        enable_v4 = true;
    bool        enable_v6 = true;
    std::string db_path   = "pxc-accounts.db";
    // 本地开发默认允许明文；公网/生产部署应绑定回环并由反代提供 TLS。
    bool        allow_plain_http = true;
    bool        control_stdin = false;
    bool        trust_loopback_proxy = false;
    std::string advertised_api_url;
    std::string advertised_signaling_url;
};

class SignalingServer {
public:
    SignalingServer(const ServerOptions& opt,
                    pxc::server::AuthService& auth,
                    pxc::server::DeviceRegistry& registry)
        : opt_(opt), auth_(auth), registry_(registry) {}

    bool start() {
        bool any = false;
        if (opt_.enable_v4) any |= start_listener(opt_.v4_host, AF_INET, "IPv4");
        if (opt_.enable_v6) any |= start_listener(opt_.v6_host, AF_INET6, "IPv6");
        return any;
    }

    void stop() {
        for (auto& s : servers_) {
            if (s) s->stop();
        }
    }

private:
    bool start_listener(const std::string& host, int family, const std::string& label) {
        auto server = std::make_unique<ix::WebSocketServer>(
            opt_.port, host, ix::SocketServer::kDefaultTcpBacklog,
            ix::SocketServer::kDefaultMaxConnections,
            ix::WebSocketServer::kDefaultHandShakeTimeoutSecs, family,
            30 /* ping 间隔，秒 */);

        // IXWebSocket 要求在每个连接对象上注册消息回调；只设置 server-level
        // callback 会被库拒绝（并返回 502 给客户端）。
        server->setOnConnectionCallback(
            [this, label](std::weak_ptr<ix::WebSocket> weak_ws,
                          std::shared_ptr<ix::ConnectionState> state) {
                auto ws = weak_ws.lock();
                if (!ws) return;

                const std::string conn_id = state->getId();
                registry_.bind_connection(conn_id, ws);
                ws->setOnMessageCallback(
                    [this, label, state, ws](const ix::WebSocketMessagePtr& msg) {
                        handle_message(label, state, *ws, msg);
                    });
            });

        if (!server->listenAndStart()) {
            std::cerr << "[!] " << label << " 监听 " << host << ":" << opt_.port
                      << " 失败（端口被占用，或该协议栈不可用）\n";
            return false;
        }

        std::cout << "[*] 信令监听 " << label << " " << host << ":" << opt_.port << std::endl;
        servers_.push_back(std::move(server));
        return true;
    }

    void handle_message(const std::string&                    proto_label,
                        std::shared_ptr<ix::ConnectionState>  state,
                        ix::WebSocket&                        ws,
                        const ix::WebSocketMessagePtr&        msg) {
        const std::string conn_id = state->getId();

        switch (msg->type) {
            case ix::WebSocketMessageType::Open: {
                const auto ip = pxc::server::proxy_client_ip(state->getRemoteIp(),
                    msg->openInfo.headers, opt_.trust_loopback_proxy);
                { std::lock_guard<std::mutex> lock(pending_mtx_); connection_ips_[conn_id] = ip; }
                log_line(proto_label, "连接建立 " + short_addr(ip) +
                                          " id=" + conn_id.substr(0, 8));
                break;
            }

            case ix::WebSocketMessageType::Error:
                log_line(proto_label, "连接错误 " + msg->errorInfo.reason);
                [[fallthrough]];
            case ix::WebSocketMessageType::Close: {
                std::set<int64_t> accounts;
                auto removed = registry_.unregister_connection(conn_id, &accounts);
                registry_.unbind_connection(conn_id);
                // 清理未完成认证的待定状态，避免 map 随连接数无限增长
                {
                    std::lock_guard<std::mutex> lock(pending_mtx_);
                    pending_account_.erase(conn_id);
                    pending_username_.erase(conn_id);
                    connection_ips_.erase(conn_id);
                }
                if (removed) {
                    log_line(proto_label, "设备下线 " + *removed);
                    // 持久层同步离线状态，但保留上次观测到的 IP
                    auth_.mark_offline(*removed);
                    broadcast_peer_event(proto_label, *removed, pxc::kPeerOffline, accounts);
                }
                break;
            }

            case ix::WebSocketMessageType::Message:
                if (msg->binary) break;  // 信令只走文本 JSON
                {
                    std::string ip = state->getRemoteIp();
                    { std::lock_guard<std::mutex> lock(pending_mtx_);
                      const auto it = connection_ips_.find(conn_id);
                      if (it != connection_ips_.end()) ip = it->second; }
                    dispatch(proto_label, conn_id, ip, ws, msg->str);
                }
                break;

            default:
                break;
        }
    }

    void dispatch(const std::string& proto_label,
                  const std::string& conn_id,
                  const std::string& remote_ip,
                  ix::WebSocket&     ws,
                  const std::string& text) {
        pxc::Message in;
        if (!pxc::decode(text, in)) {
            send_error(ws, pxc::kErrBadMessage, "无法解析的信令消息");
            return;
        }

        // 设备上线分两步：auth 换挑战，register 交签名。
        // 认证本身通过 HTTP API 登录完成，这里只消费 access token。
        if (in.type == pxc::kAuth) {
            handle_auth(proto_label, conn_id, ws, in);
            return;
        }
        if (in.type == pxc::kRegister) {
            handle_register(proto_label, conn_id, remote_ip, ws, in);
            return;
        }

        // 其余消息都必须来自已上线的设备
        auto self_id = registry_.device_of_connection(conn_id);
        if (!self_id) {
            send_error(ws, pxc::kErrNotRegistered, "尚未作为设备上线");
            return;
        }
        auto account_id = registry_.account_of_device(*self_id);
        if (!account_id) {
            send_error(ws, pxc::kErrNotRegistered, "设备账号信息缺失");
            return;
        }

        if (in.type == pxc::kListPeers) {
            pxc::Message out;
            out.type  = pxc::kPeerList;
            out.peers = registry_.peer_list(*account_id, *self_id);
            ws.send(pxc::encode(out));
            return;
        }

        if (in.target.empty()) {
            send_error(ws, pxc::kErrBadMessage, "缺少 target 字段");
            return;
        }

        forward_to_target(proto_label, *self_id, *account_id, ws, in);
    }

    // ------------------------------------------------------------ 设备上线认证

    void handle_auth(const std::string&  proto_label,
                     const std::string&  conn_id,
                     ix::WebSocket&      ws,
                     const pxc::Message& in) {
        pxc::store::Account account;
        const auto status = auth_.validate_token(in.access_token, account);

        if (status != pxc::server::AuthStatus::Ok) {
            log_line(proto_label, "认证失败：token 无效或已过期 id=" + conn_id.substr(0, 8));
            send_error(ws, pxc::kErrUnauthorized, "访问令牌无效或已过期");
            return;
        }

        // 签发一次性挑战。设备必须用它的私钥签名，才允许作为设备上线。
        const std::string nonce = auth_.issue_nonce();
        {
            std::lock_guard<std::mutex> lock(pending_mtx_);
            pending_account_[conn_id]  = account.id;
            pending_username_[conn_id] = account.username;
        }

        pxc::Message out;
        out.type    = pxc::kAuthChallenge;
        out.nonce   = pxc::base64_encode(
            std::vector<uint8_t>(nonce.begin(), nonce.end()));
        out.account = account.username;
        ws.send(pxc::encode(out));

        log_line(proto_label, "账号认证通过 " + account.username +
                                  "，已下发上线挑战");
    }

    void handle_register(const std::string&  proto_label,
                         const std::string&  conn_id,
                         const std::string&  remote_ip,
                         ix::WebSocket&      ws,
                         const pxc::Message& in) {
        int64_t account_id = 0;
        {
            std::lock_guard<std::mutex> lock(pending_mtx_);
            auto pending = pending_account_.find(conn_id);
            if (pending == pending_account_.end()) {
                // 没经过 auth 就想上线：这正是「未登录也能被连接」的入口，必须拒绝
                send_error(ws, pxc::kErrUnauthorized, "请先发送 auth 完成账号认证");
                return;
            }
            account_id = pending->second;
        }

        // 挑战必须一次性且签名正确，否则任何人都能拿别人的公钥去上线
        if (!auth_.consume_nonce_and_verify(in.device_id, in.nonce, in.signature, in.pubkey)) {
            log_line(proto_label, "拒绝注册：挑战/签名校验失败 id=" + in.device_id);
            send_error(ws, pxc::kErrForbidden, "设备签名校验失败");
            return;
        }

        // 设备入网：把设备登记到账号名下（幂等，同账号重装客户端也会走这里）。
        // connection_verifier 是设备端用连接密钥派生的，服务器只存派生值，
        // 拿不到连接密钥原文，也就不可能代替用户去连接这台设备。
        pxc::store::Device device;
        const auto enroll = auth_.enroll_device(in.access_token, in.device_id, in.pubkey,
                                                in.connection_verifier, in.name,
                                                in.platform, device);
        if (enroll != pxc::server::AuthStatus::Ok) {
            log_line(proto_label, "拒绝注册：设备入网失败 id=" + in.device_id);
            send_error(ws, pxc::kErrForbidden, "设备无法登记到该账号");
            return;
        }

        pxc::server::DeviceEntry entry;
        entry.device_id    = in.device_id;
        entry.account_id   = account_id;
        entry.name         = in.name.empty() ? "未命名设备" : in.name;
        entry.pubkey_b64   = in.pubkey;
        entry.platform     = in.platform;
        entry.remote_ip    = remote_ip;  // 只信服务器观测到的地址
        entry.connected_at = std::chrono::steady_clock::now();

        std::shared_ptr<ix::WebSocket> replaced;
        switch (registry_.register_device(entry, conn_id, &replaced)) {
            case pxc::server::DeviceRegistry::RegisterResult::IdMismatch:
                log_line(proto_label, "拒绝注册：设备 ID 与公钥指纹不符 id=" + in.device_id);
                send_error(ws, pxc::kErrForbidden, "设备 ID 与公钥不匹配");
                return;

            case pxc::server::DeviceRegistry::RegisterResult::Duplicate:
                log_line(proto_label, "拒绝注册：ID 已被占用 " + in.device_id);
                send_error(ws, pxc::kErrDuplicateId, "该设备 ID 已在线");
                return;

            case pxc::server::DeviceRegistry::RegisterResult::NoAccount:
                send_error(ws, pxc::kErrUnauthorized, "缺少账号归属");
                return;

            case pxc::server::DeviceRegistry::RegisterResult::Ok:
                break;
            case pxc::server::DeviceRegistry::RegisterResult::Replaced:
                // 先让对端释放属于旧 socket 的 P2P 会话，再宣布新连接上线。
                broadcast_peer_event(proto_label, entry.device_id, pxc::kPeerOffline);
                if (replaced) replaced->close(1000, "device reconnected");
                break;
        }

        {
            std::lock_guard<std::mutex> lock(pending_mtx_);
            pending_account_.erase(conn_id);
            pending_username_.erase(conn_id);
        }

        const std::string device_id = pxc::Identity::normalize_device_id(in.device_id);
        log_line(proto_label, "设备上线 " + device_id + " (" + entry.name + ") 来自 " +
                                  short_addr(remote_ip));

        // 持久层记录观测到的公网 IP 与最近在线时间
        auth_.mark_online(device_id, remote_ip, entry.platform);

        pxc::Message ack;
        ack.type      = pxc::kRegistered;
        ack.device_id = device_id;
        ack.peers     = registry_.peer_list(account_id, device_id);
        ws.send(pxc::encode(ack));

        broadcast_peer_event(proto_label, device_id, pxc::kPeerOnline);
    }

    // ------------------------------------------------------------------ 转发

    void forward_to_target(const std::string&  proto_label,
                           const std::string&  self_id,
                           int64_t             account_id,
                           ix::WebSocket&      ws,
                           const pxc::Message& in) {
        const std::string target = pxc::Identity::normalize_device_id(in.target);

        // 账号隔离（多账号模型）：目标设备必须登记过发起方账号
        // （当前注册账号或历史登记账号）。对外一律回答「不存在」，
        // 避免探测其他账号的设备是否存在。
        if (!registry_.device_visible_to(target, account_id)) {
            log_line(proto_label, "拒绝跨账号转发 " + self_id + " -> " + target);
            send_error(ws, pxc::kErrNoSuchPeer, "目标设备不存在");
            return;
        }

        pxc::Message out = in;
        out.from = self_id;
        out.target.clear();  // 收方不需要再看到 target

        // 对端使用更明确的服务器事件类型
        if (in.type == pxc::kConnectRequest) {
            out.type = pxc::kIncoming;
        } else if (in.type == pxc::kConnectResponse) {
            out.type = pxc::kAnswered;
        }

        // 被控端需要知道是谁在请求
        if (in.type == pxc::kConnectRequest) {
            for (const auto& p : registry_.peer_list(account_id, self_id)) {
                if (p.device_id == self_id) {
                    out.name = p.name;
                    break;
                }
            }
        }

        if (!registry_.send_to(target, out)) {
            log_line(proto_label, "转发失败 " + self_id + " -> " + target + " (" + in.type +
                                      ", 目标离线)");

            pxc::Message err;
            err.type   = pxc::kError;
            err.code   = in.type == pxc::kConnectRequest ? pxc::kErrPeerOffline
                                                         : pxc::kErrNoSuchPeer;
            err.detail = "目标设备不在线";
            err.target = target;
            ws.send(pxc::encode(err));
            return;
        }

        log_line(proto_label, "转发 " + in.type + "  " + self_id + " -> " + target);
    }

    // 广播给「对该设备可见」的所有在线设备：
    // 该设备当前注册账号 ∪ 历史登记账号名下的全部在线端
    void broadcast_peer_event(const std::string& proto_label,
                              const std::string& device_id,
                              const char*        type,
                              std::set<int64_t> accounts = {}) {
        if (accounts.empty()) {
            auto account_id = registry_.account_of_device(device_id);
            if (!account_id) return;
            accounts = registry_.extra_accounts_of_device(device_id);
            accounts.insert(*account_id);
        }

        pxc::Message ev;
        ev.type      = type;
        ev.device_id = device_id;

        std::set<std::string> notified;
        for (const auto& acc : accounts) {
            for (const auto& p : registry_.peer_list(acc, device_id)) {
                if (notified.insert(p.device_id).second) {
                    registry_.send_to(p.device_id, ev);
                }
            }
        }
        (void)proto_label;
    }

    void send_error(ix::WebSocket& ws, const char* code, const std::string& detail) {
        pxc::Message err;
        err.type   = pxc::kError;
        err.code   = code;
        err.detail = detail;
        ws.send(pxc::encode(err));
    }

    ServerOptions                                      opt_;
    pxc::server::AuthService&                          auth_;
    pxc::server::DeviceRegistry&                       registry_;
    std::vector<std::unique_ptr<ix::WebSocketServer>>  servers_;

    // 已通过账号认证、等待提交设备签名的连接。
    // 不同连接由不同 I/O 线程处理，必须加锁。
    std::mutex                         pending_mtx_;
    std::map<std::string, int64_t>     pending_account_;
    std::map<std::string, std::string> pending_username_;
    std::map<std::string, std::string> connection_ips_;
};

void print_usage(const char* argv0) {
    std::cout << "用法: " << argv0 << " [选项]\n"
              << "  --port <n>      信令端口 (默认 9910)\n"
              << "  --api-port <n>  账号 API 端口 (默认 29910)\n"
              << "  --db <file>     SQLite 数据库路径 (默认 pxc-accounts.db)\n"
              << "  --v4 <addr>     IPv4 绑定地址 (默认 0.0.0.0)\n"
              << "  --v6 <addr>     IPv6 绑定地址 (默认 ::)\n"
              << "  --no-v4         不监听 IPv4\n"
              << "  --no-v6         不监听 IPv6\n"
              << "  --no-register       关闭新账号注册\n"
              << "  --trust-loopback-proxy  信任回环代理覆盖的来源地址（须绑定回环）\n"
              << "  --advertise-api <url>   提供给其他设备的账号 API 地址\n"
              << "  --advertise-ws <url>    提供给其他设备的信令地址\n"
              << "  --allow-plain-http  允许明文 HTTP/WS（仅开发环境）\n"
              << "  --no-plain-http     未配置 TLS 时拒绝启动（生产建议）\n";
}

}  // namespace

int pxc::server::run_server(int argc, char** argv) {
    // Windows 上 ixwebsocket 需要应用显式初始化 WinSock；Linux 为空操作
    const bool net_inited = ix::initNetSystem();
    ServerOptions        opt;
    pxc::server::AuthConfig auth_config;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) return false;
            dst = argv[++i];
            return true;
        };

        if (a == "--port") {
            if (i + 1 >= argc) { std::cerr << "--port 缺少参数\n"; return 2; }
            const int port = std::atoi(argv[++i]);
            if (port < 1 || port > 65535) { std::cerr << "端口必须在 1~65535 范围内\n"; return 2; }
            opt.port = port;
        } else if (a == "--api-port") {
            if (i + 1 >= argc) { std::cerr << "--api-port 缺少参数\n"; return 2; }
            const int port = std::atoi(argv[++i]);
            if (port < 1 || port > 65535) { std::cerr << "端口必须在 1~65535 范围内\n"; return 2; }
            opt.api_port = port;
        } else if (a == "--no-register") {
            auth_config.allow_registration = false;
        } else if (a == "--allow-plain-http") {
            opt.allow_plain_http = true;
        } else if (a == "--no-plain-http") {
            opt.allow_plain_http = false;
        } else if (a == "--control-stdin") {
            opt.control_stdin = true;
        } else if (a == "--trust-loopback-proxy") {
            opt.trust_loopback_proxy = true;
        } else if (a == "--advertise-api") {
            if (!next(opt.advertised_api_url)) { std::cerr << "--advertise-api 缺少参数\n"; return 2; }
        } else if (a == "--advertise-ws") {
            if (!next(opt.advertised_signaling_url)) { std::cerr << "--advertise-ws 缺少参数\n"; return 2; }
        } else if (a == "--db") {
            if (!next(opt.db_path)) { std::cerr << "--db 缺少参数\n"; return 2; }
        } else if (a == "--v4") {
            if (!next(opt.v4_host)) { std::cerr << "--v4 缺少参数\n"; return 2; }
        } else if (a == "--v6") {
            if (!next(opt.v6_host)) { std::cerr << "--v6 缺少参数\n"; return 2; }
        } else if (a == "--no-v4") {
            opt.enable_v4 = false;
        } else if (a == "--no-v6") {
            opt.enable_v6 = false;
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "未知参数: " << a << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    if (opt.advertised_api_url.empty() != opt.advertised_signaling_url.empty()) {
        std::cerr << "--advertise-api 与 --advertise-ws 必须一起提供\n"; return 2;
    }
    std::signal(SIGTERM, on_signal);

    std::cout << "=== PixelConnection 信令服务器 + 账号 API ===\n";
    std::cout << "服务器只做握手、身份校验与账号管理，媒体数据走 P2P，不经过本机。\n";
    if (opt.allow_plain_http) {
        std::cerr << "[!] 警告：当前允许明文 HTTP/WS，仅适合本机/受控局域网测试；"
                     "公网部署必须使用 HTTPS/WSS 反向代理。\n";
    } else {
        std::cerr << "[!] --no-plain-http 需要 TLS 证书配置；当前二进制未内置 TLS listener，"
                     "请先使用 Nginx/Caddy 反向代理。\n";
        return 2;
    }

    pxc::store::Database db;
    if (!db.open(opt.db_path)) {
        std::cerr << "[!] 打开数据库失败: " << opt.db_path << " (" << db.last_error() << ")\n";
        return 1;
    }
    std::cout << "[*] 账号数据库 " << opt.db_path << std::endl;

    pxc::server::AuthService    auth(db, auth_config);
    pxc::server::DeviceRegistry registry;

    if (opt.trust_loopback_proxy &&
        ((opt.enable_v4 && !pxc::server::loopback_address(opt.v4_host)) ||
         (opt.enable_v6 && !pxc::server::loopback_address(opt.v6_host)))) {
        std::cerr << "--trust-loopback-proxy requires loopback-only listeners\n";
        return 2;
    }
    pxc::server::ApiOptions api_options;
    api_options.port           = opt.api_port;
    api_options.host_v4        = opt.v4_host;
    api_options.host_v6        = opt.v6_host;
    api_options.enable_v4      = opt.enable_v4;
    api_options.enable_v6      = opt.enable_v6;
    api_options.allow_plain_http = opt.allow_plain_http;
    api_options.trust_loopback_proxy = opt.trust_loopback_proxy;
    api_options.server_id = db.server_id();
    char server_hostname[256] = {};
    if (gethostname(server_hostname, sizeof(server_hostname) - 1) == 0)
        api_options.server_name = server_hostname;
#if defined(_WIN32)
    api_options.server_platform = "Windows";
#elif defined(__linux__)
    api_options.server_platform = "Linux";
#else
    api_options.server_platform = "Other";
#endif
    api_options.signaling_port = opt.port;
    api_options.advertised_api_url = opt.advertised_api_url;
    api_options.advertised_signaling_url = opt.advertised_signaling_url;

    pxc::server::AccountApi api(auth, api_options);
    if (!api.start()) {
        std::cerr << "[!] 账号 API 未能启动，退出。\n";
        return 1;
    }
    for (const auto& listener : api.listeners()) {
        std::cout << "[*] 账号 API 监听 " << listener << std::endl;
    }

    SignalingServer server(opt, auth, registry);
    if (!server.start()) {
        std::cerr << "[!] 信令服务器未能启动，退出。\n";
        api.stop();
        return 1;
    }
    api.set_ready(true);

    std::cout << "\n就绪。Ctrl-C 退出。\n" << std::endl;
    // GUI 在两种监听都成功后才确认启动；使用固定 ASCII 标记避免编码差异。
    std::cout << "PXC_SERVER_READY" << std::endl;
    if (opt.control_stdin) {
        // 管理进程发送 STOP 或退出导致 stdin EOF，服务都自行清理并退出。
        std::thread([] {
            std::string command;
            while (std::getline(std::cin, command)) {
                if (command == "STOP") break;
            }
            g_running.store(false);
        }).detach();
    }

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\n正在停止..." << std::endl;
    server.stop();
    api.stop();
    if (net_inited) ix::uninitNetSystem();
    return 0;
}
