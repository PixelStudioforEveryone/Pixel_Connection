#include "hos_controller.h"

#include <chrono>
#include <optional>
#include <utility>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>

#include "pxc/crypto.h"
#include "pxc/protocol.h"
#include "pxc/session_auth.h"
#include "session_wire.h"

namespace pxc::hos {
namespace {

// 进入 authenticating 后仍未认证成功的明确失败时限（ICE 被网络策略拦截时
// 会无限挂起），与桌面端一致。
constexpr auto kAuthTimeout      = std::chrono::seconds(20);
constexpr auto kKeyframeCooldown = std::chrono::milliseconds(400);

std::string trim_copy(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

}  // namespace

HosController::HosController(EventSink sink) : sink_(std::move(sink)) {}

HosController::~HosController() {
    destroyed_.store(true);
    finishFile("连接已断开");
    cancelAuthTimeout();
    {
        std::thread timer;
        {
            std::lock_guard<std::mutex> lk(auth_timer_mtx_);
            timer = std::move(auth_timer_thread_);
        }
        if (timer.joinable()) timer.join();
    }
    if (signaling_) signaling_->stop();
    if (session_) session_->close();
    joinWorkers();
}

void HosController::emit(std::string event, nlohmann::json payload) {
    if (!sink_) return;
    sink_(std::move(event), payload.dump());
}

void HosController::runAsync(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(workers_mtx_);
    if (destroyed_.load()) return;
    workers_.emplace_back([this, task = std::move(fn)]() mutable {
        try {
            task();
        } catch (const std::exception& e) {
            emit("log", {{"message", std::string("后台任务异常: ") + e.what()}});
        }
    });
}

void HosController::joinWorkers() {
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lk(workers_mtx_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        if (worker.joinable()) worker.join();
    }
}

// ------------------------------------------------------------------ 初始化

void HosController::configure(std::string api_url, std::string ws_url,
                              std::string files_dir, std::string device_name) {
    cancelLoginAttempt();
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        api_url_     = std::move(api_url);
        ws_url_      = std::move(ws_url);
        files_dir_   = std::move(files_dir);
        device_name_ = std::move(device_name);
    }

    std::string base_api, base_ws, name;
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        base_api = api_url_;
        base_ws  = ws_url_;
        name     = device_name_;
    }

    {
        std::lock_guard<std::mutex> lk(core_mtx_);
        api_        = std::make_shared<pxc::AccountApiClient>(base_api);
        signaling_  = std::make_unique<pxc::SignalingClient>(base_ws, name);
    }

    pxc::SignalingClient::Callbacks cb;
    cb.on_open = [this] { emit("log", {{"message", "信令已连接"}}); };
    cb.on_close = [this](const std::string& reason) {
        emit("log", {{"message", "信令断开: " + reason}});
        std::string target;
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            target = target_device_id_;
        }
        runAsync([this] { finishSession(); });
        emit("signalingOnline", {{"online", false}});
    };
    cb.on_error = [this](const std::string& reason) {
        emit("log", {{"message", "信令错误: " + reason}});
    };
    cb.on_authenticated = [this](const pxc::Message& msg) {
        emit("log", {{"message", "设备身份认证通过（" + msg.account + "），正在签名上线"}});
    };
    cb.on_registered = [this](const pxc::Message& msg) {
        emit("log", {{"message", "本机已上线: " + msg.device_id}});
        emit("signalingOnline", {{"online", true}, {"deviceId", msg.device_id}});
        refreshDevices();
    };
    cb.on_message = [this](const pxc::Message& msg) { handleSignalingMessage(msg); };
    signaling_->set_callbacks(std::move(cb));

    // 已加入账号的设备：先读出 ID / 密钥供界面显示
    pxc::Identity identity;
    if (pxc::Identity::load(identityPath(), identity)) {
        std::lock_guard<std::mutex> lk(id_mtx_);
        local_device_id_ = identity.device_id();
        if (!identity.connection_key().empty()) {
            local_connection_key_ = identity.connection_key();
        }
    }
}

std::string HosController::identityPath() const {
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    return files_dir_.empty() ? std::string() : files_dir_ + "/identity.json";
}

// ------------------------------------------------------------------ 本机身份

bool HosController::isEnrolled() const {
    std::lock_guard<std::mutex> lk(id_mtx_);
    return !local_connection_key_.empty();
}

std::string HosController::localDeviceId() const {
    std::lock_guard<std::mutex> lk(id_mtx_);
    return local_device_id_;
}

std::string HosController::localConnectionKey() const {
    std::lock_guard<std::mutex> lk(id_mtx_);
    return local_connection_key_;
}

bool HosController::setConnectionKey(const std::string& value) {
    const std::string key = trim_copy(value);
    size_t characters = 0;
    for (const unsigned char c : key) if ((c & 0xc0) != 0x80) ++characters;
    if (characters < 5 || characters > 128 || key.find('\0') != std::string::npos) return false;
    const std::string path = identityPath();
    const std::string pending = path + ".key-update";
    pxc::Identity identity;
    if (path.empty() || !pxc::Identity::load(path, identity) || identity.connection_key().empty()) return false;
    identity.set_connection_key(key);
    // Save to a 0600 temporary file and replace atomically. A failed write must
    // preserve the original identity and Ed25519 key pair.
    std::error_code error;
    try {
        if (!identity.save(pending)) {
            std::filesystem::remove(pending, error);
            return false;
        }
        std::filesystem::rename(pending, path, error);
        if (error) {
            std::filesystem::remove(pending, error);
            return false;
        }
    } catch (...) {
        std::filesystem::remove(pending, error);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(id_mtx_);
        local_device_id_ = identity.device_id();
        local_connection_key_ = key;
    }
    // Updating credentials re-authenticates an existing signaling connection;
    // the server receives a verifier derived from the new key, never the key.
    startSignalingIfPossible();
    emit("connectionKeyChanged", {{"deviceId", identity.device_id()}, {"connectionKey", key}});
    return true;
}

void HosController::addThisDevice() {
    std::string token;
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        token = access_token_;
    }
    if (token.empty()) {
        emit("deviceEnrollFailed", {{"error", "请先登录账号"}});
        return;
    }

    pxc::Identity existing;
    if (pxc::Identity::load(identityPath(), existing) &&
        !existing.connection_key().empty()) {
        std::string device_id, key;
        {
            std::lock_guard<std::mutex> lk(id_mtx_);
            local_device_id_      = existing.device_id();
            local_connection_key_ = existing.connection_key();
            device_id             = local_device_id_;
            key                   = local_connection_key_;
        }
        startSignalingIfPossible();
        emit("deviceEnrolled", {{"deviceId", device_id}, {"connectionKey", key}});
        emit("log", {{"message", "本机已是设备，无需重复添加"}});
        return;
    }

    std::string device_id, key;
    try {
        pxc::Identity identity = pxc::Identity::generate();
        identity.set_connection_key(pxc::crypto::generate_connection_key());
        if (!identity.save(identityPath())) {
            emit("deviceEnrollFailed", {{"error", "无法保存设备身份文件"}});
            return;
        }
        {
            std::lock_guard<std::mutex> lk(id_mtx_);
            local_device_id_      = identity.device_id();
            local_connection_key_ = identity.connection_key();
            device_id             = local_device_id_;
            key                   = local_connection_key_;
        }
    } catch (const std::exception& e) {
        emit("deviceEnrollFailed",
             {{"error", std::string("生成设备身份失败: ") + e.what()}});
        return;
    }

    startSignalingIfPossible();
    emit("deviceEnrolled", {{"deviceId", device_id}, {"connectionKey", key}});
}

void HosController::startSignalingIfPossible() {
    std::string token;
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        token = access_token_;
    }
    if (!signaling_ || token.empty()) return;

    pxc::Identity identity;
    if (!pxc::Identity::load(identityPath(), identity)) return;
    if (identity.connection_key().empty()) return;  // 还没加入账号

    signaling_->set_credentials(token, identity, "harmonyos");
    if (!signaling_->connected()) signaling_->start();
    // 已连接时 set_credentials 会主动补发一次认证
}

// ------------------------------------------------------------------ 账号

void HosController::login(const std::string& identifier, const std::string& password) {
    std::shared_ptr<pxc::AccountApiClient> api;
    { std::lock_guard<std::mutex> lk(core_mtx_); api = api_; }
    if (!api) {
        emit("loginResult", {{"ok", false}, {"error", "尚未配置服务器地址"}});
        return;
    }
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        generation = ++account_generation_;
        auto_identifier_ = identifier;
        auto_password_   = password;
    }
    runAsync([this, api, generation, identifier, password] {
        std::string token, name, error;
        const bool  ok = api->login(identifier, password, token, name, error);
        if (destroyed_.load() || generation != account_generation_) return;

        if (!ok) {
            emit("loginResult", {{"ok", false}, {"error", error}, {"generation", generation}});
            return;
        }
        {
            std::lock_guard<std::mutex> lk(acct_mtx_);
            if (generation != account_generation_) return;
            access_token_ = token;
            username_     = name.empty() ? identifier : name;
        }
        std::string user;
        {
            std::lock_guard<std::mutex> lk(acct_mtx_);
            user = username_;
        }
        emit("loginResult", {{"ok", true}, {"username", user}, {"generation", generation}});
        startSignalingIfPossible();
        refreshDevices();
    });
}

void HosController::cancelLoginAttempt() {
    std::lock_guard<std::mutex> lk(acct_mtx_);
    ++account_generation_;
}

nlohmann::json HosController::getLoginSession() {
    std::lock_guard<std::mutex> lk(acct_mtx_);
    return {{"token", access_token_}, {"username", username_}, {"generation", account_generation_.load()}};
}

void HosController::restoreLogin(const std::string& token, const std::string& username) {
    std::shared_ptr<pxc::AccountApiClient> api;
    { std::lock_guard<std::mutex> lk(core_mtx_); api = api_; }
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        generation = ++account_generation_;
        auto_identifier_.clear();
        auto_password_.clear();
    }
    runAsync([this, api, generation, token, username] {
        std::vector<pxc::PeerInfo> devices;
        std::string error;
        bool ok = false;
        try {
            if (api && !token.empty() && !username.empty()) ok = api->list_devices(token, devices, error);
            else error = "登录状态无效，请重新登录";
        } catch (const std::exception&) { error = "无法恢复登录，请重试"; }
        if (destroyed_.load() || generation != account_generation_) return;
        if (!ok) {
            const bool invalid = token.empty() || username.empty() || error.find("unauthorized") != std::string::npos ||
                error.find("HTTP 401") != std::string::npos || error.find("token_expired") != std::string::npos;
            emit("loginResult", {{"ok", false}, {"restored", true}, {"invalidSession", invalid},
                                {"error", error}, {"generation", generation}});
            return;
        }
        {
            std::lock_guard<std::mutex> lk(acct_mtx_);
            if (generation != account_generation_) return;
            access_token_ = token;
            username_ = username;
        }
        emit("loginResult", {{"ok", true}, {"username", username}, {"restored", true}, {"generation", generation}});
        startSignalingIfPossible();
        refreshDevices();
    });
}

void HosController::registerAccount(const std::string& username,
                                    const std::string& email,
                                    const std::string& password) {
    std::shared_ptr<pxc::AccountApiClient> api;
    { std::lock_guard<std::mutex> lk(core_mtx_); api = api_; }
    if (!api) {
        emit("registerResult", {{"ok", false}, {"message", "尚未配置服务器地址"}});
        return;
    }
    runAsync([this, api, username, email, password] {
        std::string error;
        const bool  ok = api->register_account(username, email, password, error);
        if (destroyed_.load()) return;
        emit("registerResult",
             {{"ok", ok},
              {"message", ok ? std::string("注册成功，请登录") : error}});
    });
}

void HosController::logout() {
    std::shared_ptr<pxc::AccountApiClient> api;
    { std::lock_guard<std::mutex> lk(core_mtx_); api = api_; }
    std::string token;
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        ++account_generation_;
        token = access_token_;
        access_token_.clear();
        username_.clear();
        auto_identifier_.clear();
        auto_password_.clear();
    }
    if (!token.empty() && api) {
        runAsync([api, token] {
            std::string error;
            api->logout(token, error);  // Local state stays cleared even when the server is unavailable.
        });
    }
    if (signaling_) signaling_->stop();
    finishSession();
    emit("sessionState", {{"deviceId", ""}, {"state", "closed"}});
}

void HosController::refreshDevices() {
    std::shared_ptr<pxc::AccountApiClient> api;
    { std::lock_guard<std::mutex> lk(core_mtx_); api = api_; }
    const auto generation = account_generation_.load();
    std::string token;
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        token = access_token_;
    }
    if (!api || token.empty()) return;
    runAsync([this, api, generation, token] {
        std::vector<pxc::PeerInfo> devices;
        std::string                error;
        const bool                 ok = api->list_devices(token, devices, error);
        if (destroyed_.load() || generation != account_generation_) return;

        if (!ok) {
            emit("deviceListFailed", {{"error", error}});
            return;
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& d : devices) {
            arr.push_back({{"deviceId", d.device_id},
                           {"name", d.name},
                           {"publicIp", d.public_ip},
                           {"lastSeen", d.last_seen},
                           {"platform", d.platform},
                           {"online", d.online}});
        }
        emit("devicesUpdated", {{"devices", arr}});
    });
}

// ------------------------------------------------------------------ 网络

void HosController::setIceServers(const std::string& stun_url, const std::string& turn_url,
                                  const std::string& turn_user, const std::string& turn_pass) {
    std::lock_guard<std::mutex> lk(cfg_mtx_);
    stun_url_   = stun_url;
    turn_url_   = turn_url;
    turn_user_  = turn_user;
    turn_pass_  = turn_pass;
}

// ------------------------------------------------------------------ 会话

void HosController::connectToDevice(const std::string& device_id,
                                    const std::string& connection_key) {
    std::string token;
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        token = access_token_;
    }
    if (token.empty()) {
        emit("sessionState", {{"deviceId", device_id}, {"state", "rejected"}});
        emit("log", {{"message", "请先登录账号"}});
        return;
    }

    const std::string key = trim_copy(connection_key);
    if (key.empty()) {
        emit("sessionState", {{"deviceId", device_id}, {"state", "rejected"}});
        emit("log", {{"message", "必须输入目标设备的连接密钥"}});
        return;
    }
    if (!signaling_ || !signaling_->online()) {
        emit("sessionState", {{"deviceId", device_id}, {"state", "rejected"}});
        emit("log", {{"message", "本机尚未上线，无法发起连接"}});
        return;
    }

    finishSession(nullptr, false);
    {
        std::lock_guard<std::mutex> lk(sess_mtx_);
        target_device_id_      = device_id;
        target_connection_key_ = key;
        session_authenticated_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(video_mtx_);
        reassembler_.reset();
        video_reference_.reset();
        last_drop_count_ = 0;
    }

    pxc::Message msg;
    msg.type   = pxc::kConnectRequest;
    msg.target = device_id;
    signaling_->send(msg);

    emit("sessionState", {{"deviceId", device_id}, {"state", "connecting"}});
    emit("log", {{"message", "已请求连接 " + device_id + "，等待对方确认"}});
}

void HosController::disconnect() { finishSession(); }

bool HosController::sessionAuthenticated() const {
    std::lock_guard<std::mutex> lk(sess_mtx_);
    return session_authenticated_;
}

void HosController::finishSession(std::shared_ptr<pxc::PeerSession> expected, bool notify) {
    std::shared_ptr<pxc::PeerSession> session;
    std::string target;
    {
        std::scoped_lock lk(core_mtx_, sess_mtx_);
        if (expected && session_ != expected) return;
        session = std::move(session_);
        target = std::move(target_device_id_);
        target_connection_key_.clear();
        session_authenticated_ = false;
        wallpaper_saved_ = false;
        clipboard_has_text_ = false; clipboard_text_.clear();
    }
    cancelAuthTimeout();
    if (session) session->close();
    finishFile("连接已断开");
    {
        std::lock_guard<std::mutex> lk(video_mtx_);
        reassembler_.reset();
        video_reference_.reset();
        last_drop_count_ = 0;
    }
    if (notify && !target.empty()) emit("sessionState", {{"deviceId", target}, {"state", "closed"}});
}

// ------------------------------------------------------------------ 信令入站

void HosController::handleSignalingMessage(const pxc::Message& msg) {
    if (msg.type == pxc::kIncoming) {
        // HOS 端只做主控端：入站请求一律拒绝
        pxc::Message deny;
        deny.type   = pxc::kConnectResponse;
        deny.target = msg.from;
        deny.accept = false;
        if (signaling_) signaling_->send(deny);
        emit("log", {{"message", "已拒绝来自 " + msg.from + " 的连接请求（本机仅作主控端）"}});
        return;
    }

    if (msg.type == pxc::kAnswered) {
        std::string target;
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            target = target_device_id_;
            if (target.empty() || pxc::Identity::normalize_device_id(msg.from) !=
                                  pxc::Identity::normalize_device_id(target)) return;
            if (!msg.accept) session_authenticated_ = false;
        }
        if (!msg.accept) {
            finishSession();
            emit("sessionState", {{"deviceId", target}, {"state", "rejected"}});
            emit("log", {{"message", "对方拒绝了连接"}});
            return;
        }
        ensureSession(pxc::PeerSession::Role::Offerer);
        armAuthTimeout();
        emit("sessionState", {{"deviceId", target}, {"state", "authenticating"}});
        return;
    }

    if (msg.type == pxc::kSdp) {
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            if (target_device_id_.empty() || pxc::Identity::normalize_device_id(msg.from) !=
                pxc::Identity::normalize_device_id(target_device_id_)) return;
        }
        std::shared_ptr<pxc::PeerSession> session;
        {
            std::lock_guard<std::mutex> lk(core_mtx_);
            session = session_;
        }
        if (session) session->set_remote_description(msg.sdp, msg.sdp_type);
        return;
    }

    if (msg.type == pxc::kCandidate) {
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            if (target_device_id_.empty() || pxc::Identity::normalize_device_id(msg.from) !=
                pxc::Identity::normalize_device_id(target_device_id_)) return;
        }
        std::shared_ptr<pxc::PeerSession> session;
        {
            std::lock_guard<std::mutex> lk(core_mtx_);
            session = session_;
        }
        if (session) session->add_remote_candidate(msg.candidate, msg.mid);
        return;
    }

    if (msg.type == pxc::kError) {
        emit("log", {{"message", "服务器错误: " + msg.code + " " + msg.detail}});
        if (msg.code == pxc::kErrNoSuchPeer || msg.code == pxc::kErrPeerOffline || msg.code == pxc::kErrBusy) {
            std::string target;
            { std::lock_guard lk(sess_mtx_); if (!session_authenticated_) target=target_device_id_; }
            if (!target.empty()) {
                finishSession(nullptr,false);
                emit("sessionState",{{"deviceId",target},{"state","rejected"},{"reason",msg.detail}});
            }
        }
        // token 过期自动重登（与桌面端一致）
        if (msg.code == pxc::kErrUnauthorized) {
            std::string identifier, pass, user;
            {
                std::lock_guard<std::mutex> lk(acct_mtx_);
                identifier     = auto_identifier_;
                pass           = auto_password_;
                user           = username_;
            }
            if (!identifier.empty() && !user.empty()) {
                emit("log", {{"message", "登录凭证已过期，正在自动重新登录..."}});
                login(identifier, pass);
            }
        }
        return;
    }

    if (msg.type == pxc::kPeerOnline || msg.type == pxc::kPeerOffline) {
        bool ended = false;
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            ended = msg.type == pxc::kPeerOffline && !target_device_id_.empty() &&
                pxc::Identity::normalize_device_id(msg.device_id) ==
                pxc::Identity::normalize_device_id(target_device_id_);
        }
        if (ended) finishSession();
        refreshDevices();
        return;
    }
}

// ------------------------------------------------------------------ P2P 会话

void HosController::ensureSession(pxc::PeerSession::Role role) {
    {
        std::lock_guard<std::mutex> lk(core_mtx_);
        if (session_) return;
    }

    pxc::PeerSession::Config config;
    // STUN 默认关闭（不可达的 STUN 会卡死候选收集）；跨 NAT 在设置页配置
    config.enable_ice_udp_mux = true;
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        if (!stun_url_.empty()) config.stun_servers.push_back(stun_url_);
        if (!turn_url_.empty()) {
            pxc::PeerSession::IceServer turn;
            turn.url        = turn_url_;
            turn.username   = turn_user_;
            turn.credential = turn_pass_;
            config.ice_servers.push_back(turn);
        }
    }

    auto session = pxc::PeerSession::create(role, std::move(config));

    std::string target;
    {
        std::lock_guard<std::mutex> lk(sess_mtx_);
        target = target_device_id_;
    }

    pxc::PeerSession::Callbacks cb;
    cb.on_log = [this](const std::string& text) {
        emit("log", {{"message", "[P2P] " + text}});
    };
    const std::weak_ptr<pxc::PeerSession> weak_session = session;
    cb.on_state = [this, target, weak_session](rtc::PeerConnection::State state) {
        const auto owner = weak_session.lock();
        if (!owner) return;
        {
            std::lock_guard<std::mutex> lk(core_mtx_);
            if (session_ != owner) return;
        }
        using S = rtc::PeerConnection::State;
        const char* name = "connecting";
        switch (state) {
            case S::Connected:    name = "connected"; break;
            case S::Connecting:   name = "connecting"; break;
            case S::Failed:       name = "rejected"; break;
            case S::Disconnected: name = "closed"; break;
            case S::Closed:       name = "closed"; break;
            default: break;
        }
        const bool ended = state == S::Failed || state == S::Disconnected ||
                           state == S::Closed;
        if (ended) {
            // 不在 rtc 回调线程里 close/join：交给 worker 完整清理
            runAsync([this, weak_session] {
                {
                    std::lock_guard<std::mutex> lk(core_mtx_);
                    if (session_ != weak_session.lock()) return;
                }
                if (auto expected = weak_session.lock()) finishSession(expected);
            });
        }
        emit("sessionState", {{"deviceId", target}, {"state", name}});
    };
    cb.on_local_description = [this, target, weak_session](const std::string& sdp,
                                             const std::string& type) {
        { std::lock_guard<std::mutex> lk(core_mtx_); if (session_ != weak_session.lock()) return; }
        if (!signaling_) return;
        pxc::Message out;
        out.type     = pxc::kSdp;
        out.target   = target;
        out.sdp      = sdp;
        out.sdp_type = type;
        signaling_->send(out);
    };
    cb.on_local_candidate = [this, target, weak_session](const std::string& candidate,
                                           const std::string& mid) {
        { std::lock_guard<std::mutex> lk(core_mtx_); if (session_ != weak_session.lock()) return; }
        if (!signaling_) return;
        pxc::Message out;
        out.type      = pxc::kCandidate;
        out.target    = target;
        out.candidate = candidate;
        out.mid       = mid;
        signaling_->send(out);
    };
    cb.on_data_channel = [this, weak_session](std::shared_ptr<rtc::DataChannel> channel) {
        { std::lock_guard<std::mutex> lk(core_mtx_); if (session_ != weak_session.lock()) return; }
        wireChannel(channel);
    };
    session->set_callbacks(std::move(cb));
    {
        std::lock_guard<std::mutex> lk(core_mtx_);
        session_ = session;
    }
    session->start();

    // Offerer 本地创建通道；Answerer 通过 on_data_channel 收到它们
    if (role == pxc::PeerSession::Role::Offerer) {
        wireChannel(session->channel(pxc::kChControl));
        wireChannel(session->channel(pxc::kChVideo));
        wireChannel(session->channel(pxc::kChClip));
        wireChannel(session->channel(pxc::kChFile));
    }
}

void HosController::wireChannel(const std::shared_ptr<rtc::DataChannel>& channel) {
    if (!channel) return;
    const std::string label = channel->label();

    if (label == pxc::kChVideo) {
        const std::weak_ptr<rtc::DataChannel> weak = channel;
        // ch-video：二进制分片。主控端收画面，重组后交给 FrameSink（Phase 5 解码）。
        channel->onOpen([this] { emit("log", {{"message", "视频通道已打开"}}); });
        channel->onMessage([this, weak](rtc::message_variant data) {
            { std::scoped_lock lk(core_mtx_, sess_mtx_);
              if (!session_authenticated_ || !session_ || session_->channel(pxc::kChVideo) != weak.lock()) return; }
            if (!std::holds_alternative<rtc::binary>(data)) return;
            const auto& bytes = std::get<rtc::binary>(data);
            pushVideoFragment(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        });
        channel->onClosed([this] { emit("log", {{"message", "视频通道已关闭"}}); });
        return;
    }

    if (label == pxc::kChFile) {
        const std::weak_ptr<rtc::DataChannel> weak = channel;
        channel->onOpen([this] {
            emit("log", {{"message", "文件传输通道已打开"}});
            emit("fileEvent", {{"op", "ready"}});
        });
        channel->onMessage([this,weak](rtc::message_variant data) {
            if (weak.lock()!=fileChannel()) return;
            if (std::holds_alternative<std::string>(data)) {
                const auto j=nlohmann::json::parse(std::get<std::string>(data),nullptr,false);
                if (j.is_object()) {
                    try { handleFileMessage(j); }
                    catch (const std::exception&) { emit("log", {{"message", "忽略格式错误的文件消息"}}); }
                }
            } else if (std::holds_alternative<rtc::binary>(data)) handleFileChunk(std::get<rtc::binary>(data));
        });
        return;
    }

    if (label == pxc::kChClip) {
        const std::weak_ptr<rtc::DataChannel> weak = channel;
        channel->onOpen([this] {
            if (sessionAuthenticated()) emit("clipboardReady", {});
        });
        channel->onMessage([this, weak](rtc::message_variant data) {
            if (!sessionAuthenticated() || !std::holds_alternative<std::string>(data)) return;
            {
                std::lock_guard<std::mutex> lk(core_mtx_);
                if (!session_ || session_->channel(pxc::kChClip) != weak.lock()) return;
            }
            const auto& raw = std::get<std::string>(data);
            if (raw.size() > 400 * 1024) return;
            const auto j = nlohmann::json::parse(raw, nullptr, false);
            if (!j.is_object() || !j.contains("type") || j["type"] != "text" ||
                !j.contains("text") || !j["text"].is_string()) return;
            const auto text = j["text"].get<std::string>();
            if (text.size() <= 64 * 1024) {
                { std::scoped_lock lk(core_mtx_, sess_mtx_);
                  if (!session_authenticated_ || !session_ || session_->channel(pxc::kChClip) != weak.lock()) return;
                  clipboard_text_ = text; clipboard_has_text_ = true; }
                emit("clipboardText", {{"text", text}});
            }
        });
        return;
    }
    if (label != pxc::kChControl) return;

    // ch-control：可靠有序。认证挑战/应答 + JSON 控制面都走这里。
    channel->onOpen([this] {
        emit("log", {{"message", "控制通道已打开，等待认证挑战"}});
    });
    const std::weak_ptr<rtc::DataChannel> weak_channel = channel;
    std::weak_ptr<pxc::PeerSession> owner;
    {
        std::lock_guard<std::mutex> lk(core_mtx_);
        owner = session_;
    }
    channel->onClosed([this, owner] {
        runAsync([this, owner] {
            if (auto expected = owner.lock()) finishSession(expected);
        });
    });
    channel->onMessage([this, weak_channel](rtc::message_variant data) {
        {
            std::lock_guard<std::mutex> lk(core_mtx_);
            if (!session_ || session_->channel(pxc::kChControl) != weak_channel.lock()) return;
        }
        if (!std::holds_alternative<std::string>(data)) return;
        onControlMessage(std::get<std::string>(std::move(data)));
    });
}

void HosController::onControlMessage(const std::string& message) {
    // ---- JSON 控制面 ----
    if (!message.empty() && message.front() == '{') {
        nlohmann::json j;
        if (pxc::gui::parse_session_message(message, j)) handleSessionJson(j);
        return;
    }

    // ---- 主控端：收到挑战后出示证明 ----
    std::string nonce;
    if (pxc::parse_session_auth_challenge(message, nonce)) {
        std::shared_ptr<pxc::PeerSession> session;
        {
            std::lock_guard<std::mutex> lk(core_mtx_);
            session = session_;
        }
        const std::string fingerprint =
            session ? session->remote_dtls_fingerprint() : std::string();
        if (fingerprint.empty()) {
            emit("log", {{"message", "对端 DTLS 指纹不可用，拒绝发送证明"}});
            return;
        }
        std::string key;
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            key = target_connection_key_;
        }
        const std::string proof = pxc::compute_session_proof(key, nonce, fingerprint);
        auto channel = session ? session->channel(pxc::kChControl) : nullptr;
        if (proof.empty()) {
            emit("log", {{"message", "连接密钥格式无效"}});
            if (channel) channel->send(pxc::kSessionAuthFail);
            return;
        }
        if (channel) {
            channel->send(std::string(pxc::kSessionAuthResponse) + " " + nonce + " " +
                          proof);
        }
        emit("log", {{"message", "已出示连接密钥证明，等待被控端确认"}});
        return;
    }

    if (message == pxc::kSessionAuthOk) {
        std::string target, key;
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            session_authenticated_ = true;
            target                 = target_device_id_;
            key                    = target_connection_key_;
        }
        cancelAuthTimeout();
        emit("sessionState", {{"deviceId", target}, {"state", "authenticated"}});
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            wallpaper_saved_ = false;
        }
        requestRemoteWallpaper();
        // 认证通过：让 ArkTS 把密钥存入 preferences（记住密钥，下次免输入）
        if (!target.empty() && !key.empty()) {
            emit("keyVerified", {{"deviceId", target}, {"connectionKey", key}});
        }
        emit("log", {{"message", "✅ 连接密钥验证通过，可以控制该设备"}});
        return;
    }

    if (message == pxc::kSessionAuthFail) {
        std::string target;
        {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            session_authenticated_ = false;
            target                 = target_device_id_;
        }
        finishSession();
        emit("sessionState", {{"deviceId", target}, {"state", "rejected"}});
        emit("log", {{"message", "❌ 认证失败：连接密钥错误"}});
        return;
    }

    bool authed;
    {
        std::lock_guard<std::mutex> lk(sess_mtx_);
        authed = session_authenticated_;
    }
    if (!authed) {
        emit("log", {{"message", "认证未完成，忽略控制消息"}});
        return;
    }
    emit("log", {{"message", "[控制] " + message}});
}

void HosController::handleSessionJson(const nlohmann::json& j) {
    bool authed;
    {
        std::lock_guard<std::mutex> lk(sess_mtx_);
        authed = session_authenticated_;
    }
    if (!authed) {
        emit("log", {{"message", "认证未完成，忽略控制消息"}});
        return;
    }

    const std::string kind = j.value("kind", "");
    if (kind == "screens") {
        emit("screensUpdated",
             {{"screens", j.value("screens", nlohmann::json::array())}});
        return;
    }
    if (kind == "video_state") {
        nlohmann::json payload;
        payload["width"]   = j.value("width", 0);
        payload["height"]  = j.value("height", 0);
        payload["fps"]     = j.value("fps", 0);
        payload["screen"]  = j.value("screen", 0);
        payload["encoder"] = j.value("encoder", "");
        payload["error"]   = j.value("error", "");
        emit("videoState", payload);
        return;
    }
    if (kind == "wallpaper" && j.contains("image") && j["image"].is_string()) {
        const auto encoded = j["image"].get<std::string>();
        if (encoded.empty() || encoded.size() > 64 * 1024 || encoded.size() % 4) return;
        std::vector<unsigned char> bytes(encoded.size() / 4 * 3);
        int length = EVP_DecodeBlock(bytes.data(),
            reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
        if (length < 0) return;
        if (encoded.back() == '=') --length;
        if (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') --length;
        if (length < 4 || length > 40 * 1024 || bytes[0] != 0xff || bytes[1] != 0xd8) return;
        // Hold the session lock through persistence: a late reply cannot be attributed to a new peer.
        std::unique_lock<std::mutex> lk(sess_mtx_);
        if (!session_authenticated_ || wallpaper_saved_ || target_device_id_.empty()) return;
        const std::string target = target_device_id_;
        const std::string path = cachedWallpaper(target);
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        if (error) return;
        {
            std::ofstream out(path + ".tmp", std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), length);
            if (!out.good()) return;
        }
        if (std::rename((path + ".tmp").c_str(), path.c_str()) != 0) return;
        wallpaper_saved_ = true;
        lk.unlock();
        emit("wallpaperReady", {{"deviceId", target}, {"path", path}});
    }
}

void HosController::sendSessionJson(const nlohmann::json& j) {
    if (!sessionAuthenticated()) return;
    std::shared_ptr<pxc::PeerSession> session;
    {
        std::lock_guard<std::mutex> lk(core_mtx_);
        session = session_;
    }
    auto channel = session ? session->channel(pxc::kChControl) : nullptr;
    if (!channel || !channel->isOpen()) return;
    channel->send(pxc::gui::encode_session_message(j));
}

// ------------------------------------------------------------------ 控制面

void HosController::requestRemoteScreens() {
    sendSessionJson(pxc::gui::make_screen_list_cmd());
}

std::string HosController::cachedWallpaper(const std::string& device_id) {
    std::string directory, api, account;
    {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        directory = files_dir_;
        api = api_url_;
    }
    {
        std::lock_guard<std::mutex> lk(acct_mtx_);
        account = username_;
    }
    return directory + "/wallpaper-previews/" +
        pxc::crypto::sha256_hex(api + "\n" + account + "\n" + device_id) + ".jpg";
}

void HosController::requestRemoteWallpaper() {
    sendSessionJson({{"kind", "cmd"}, {"cmd", "wallpaper"}});
}

void HosController::switchRemoteScreen(int index) {
    sendSessionJson(pxc::gui::make_screen_switch_cmd(index));
}

void HosController::setRemoteVideoConfig(int width, int height, int fps,
                                         int bitrate_kbps) {
    sendSessionJson(
        pxc::gui::make_video_cfg_cmd(width, height, fps, bitrate_kbps));
}

void HosController::requestRemoteKeyframe() {
    sendSessionJson(pxc::gui::make_keyframe_cmd());
}

bool HosController::sendClipboardText(const std::string& text) {
    if (text.size() > 64 * 1024) return false;
    std::shared_ptr<rtc::DataChannel> channel;
    {
        std::scoped_lock lk(core_mtx_, sess_mtx_);
        if (!session_authenticated_ || !session_) return false;
        channel = session_->channel(pxc::kChClip);
    }
    if (!channel || !channel->isOpen()) return false;
    try {
        const bool sent = channel->send(nlohmann::json{{"type", "text"}, {"text", text}}.dump());
        if (sent) {
            std::scoped_lock lk(core_mtx_, sess_mtx_);
            if (session_ && session_->channel(pxc::kChClip) == channel) {
                clipboard_text_ = text; clipboard_has_text_ = true;
            }
        }
        return sent;
    }
    catch (const std::exception&) { return false; }
}

void HosController::sendInputEvent(const std::string& payload_json) {
    try {
        auto payload = nlohmann::json::parse(payload_json, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) return;
        if (payload.value("type", "") == "key" && payload.value("key", 0) == 86 &&
            payload.value("pressed", false) && (payload.value("mods", 0) & 0x04000000)) {
            std::lock_guard<std::mutex> lk(sess_mtx_);
            if (clipboard_has_text_) payload["clipboard"] = clipboard_text_;
        }
        sendSessionJson(pxc::gui::make_input_event(payload));
    } catch (const std::exception&) {
        // 事件序列化失败直接丢弃，键鼠事件不值得打断会话
    }
}

// ------------------------------------------------------------------ 视频帧出口

void HosController::setFrameSink(FrameSink sink) {
    std::lock_guard<std::mutex> lk(video_mtx_);
    frame_sink_ = std::move(sink);
}

void HosController::resetFrameSink() {
    std::lock_guard<std::mutex> lk(video_mtx_);
    frame_sink_ = nullptr;
}

void HosController::pushVideoFragment(const uint8_t* data, size_t size) {
    FrameSink                       sink;
    std::optional<pxc::VideoFrame>  frame;
    bool                            need_keyframe = false;
    {
        std::lock_guard<std::mutex> lk(video_mtx_);
        frame = reassembler_.push(data, size);
        // 丢帧检测：重组器丢弃不完整帧 = 流上出现空洞，P 帧会在陈旧参考上
        // 预测出脏画面，必须立刻请求关键帧（与桌面端 video_receiver 一致）
        const auto& st = reassembler_.stats();
        if (st.frames_dropped > last_drop_count_) {
            last_drop_count_ = st.frames_dropped;
            video_reference_.invalidate();
        }
        if (frame && !video_reference_.accept(*frame)) frame.reset();
        if (video_reference_.needs_keyframe()) {
            const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
            if (static_cast<uint64_t>(now_ms) - last_hole_keyframe_ms_ >= 400) {
                last_hole_keyframe_ms_ = static_cast<uint64_t>(now_ms);
                need_keyframe = true;
            }
        }
        sink = frame_sink_;
    }
    if (need_keyframe) {
        requestRemoteKeyframe();
    }
    if (frame && sink) {
        sink(frame->payload.data(), frame->payload.size(), frame->frame_id,
             frame->keyframe, frame->timestamp_us);
    }
}

// ------------------------------------------------------------------ 认证超时

void HosController::armAuthTimeout() {
    std::thread old;
    {
        std::lock_guard<std::mutex> lk(auth_timer_mtx_);
        ++auth_timer_gen_;  // 使可能存在的旧定时失效
        old = std::move(auth_timer_thread_);
    }
    if (old.joinable()) old.join();
    {
        std::lock_guard<std::mutex> lk(auth_timer_mtx_);
        const uint64_t gen = auth_timer_gen_;
        auth_timer_thread_ = std::thread([this, gen] {
            std::unique_lock<std::mutex> lk(auth_timer_mtx_);
            const bool cancelled = auth_timer_cv_.wait_for(lk, kAuthTimeout, [&] {
                return destroyed_.load() || auth_timer_gen_ != gen;
            });
            lk.unlock();
            if (cancelled) return;
            onAuthTimeout();
        });
    }
}

void HosController::cancelAuthTimeout() {
    std::lock_guard<std::mutex> lk(auth_timer_mtx_);
    ++auth_timer_gen_;
    auth_timer_cv_.notify_all();
}

void HosController::onAuthTimeout() {
    std::string target;
    bool        authed = false;
    {
        std::lock_guard<std::mutex> lk(sess_mtx_);
        target = target_device_id_;
        authed = session_authenticated_;
    }
    std::shared_ptr<pxc::PeerSession> session;
    {
        std::lock_guard<std::mutex> lk(core_mtx_);
        session = session_;
    }
    if (authed || !session) return;

    // 先给状态再给原因（状态会覆盖状态栏文本，原因要留在最后）
    emit("sessionState", {{"deviceId", target}, {"state", "p2p_timeout"}});
    emit("log", {{"message", "P2P 建连超时（可能被网络策略拦截 UDP），会话已取消；"
                             "可在 设置-网络 配置 TURN 中继后重试"}});
    finishSession();
}

}  // namespace pxc::hos
