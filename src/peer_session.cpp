#include "pxc/peer_session.h"

#include <sstream>

namespace pxc {
namespace {

bool add_ice_server(const PeerSession::IceServer& source, rtc::Configuration& target) {
    if (source.url.empty()) return false;
    const std::string prefix = source.url.rfind("stun:", 0) == 0 ? "stun:" :
                               source.url.rfind("turns:", 0) == 0 ? "turns:" :
                               (source.url.rfind("turn:", 0) == 0 ? "turn:" : "");
    if (prefix.empty() || (source.username.empty() && source.url.find('@') != std::string::npos)) {
        target.iceServers.emplace_back(source.url);
        return true;
    }

    std::string endpoint = source.url.substr(prefix.size());
    if (endpoint.rfind("//", 0) == 0) endpoint.erase(0, 2);
    const auto query = endpoint.find('?');
    const std::string parameters = query == std::string::npos ? "" : endpoint.substr(query + 1);
    if (query != std::string::npos) endpoint.resize(query);
    const auto slash = endpoint.find('/');
    if (slash != std::string::npos) endpoint.resize(slash);
    std::string host;
    uint16_t port = (prefix == "turns:") ? 5349 : 3478;
    auto parsePort = [](const std::string& value) {
        size_t consumed = 0;
        const auto number = std::stoul(value, &consumed);
        if (consumed != value.size() || number == 0 || number > 65535)
            throw std::invalid_argument("ICE server port must be 1..65535");
        return static_cast<uint16_t>(number);
    };
    if (!endpoint.empty() && endpoint.front() == '[') {
        const auto close = endpoint.find(']');
        if (close == std::string::npos) return false;
        host = endpoint.substr(1, close - 1);
        if (close + 1 < endpoint.size() && endpoint[close + 1] == ':')
            port = parsePort(endpoint.substr(close + 2));
        else if (close + 1 != endpoint.size()) return false;
    } else {
        const auto colon = endpoint.rfind(':');
        if (colon != std::string::npos) {
            host = endpoint.substr(0, colon);
            if (endpoint.find(':') != colon) return false; // IPv6 must be bracketed
            port = parsePort(endpoint.substr(colon + 1));
        } else {
            host = endpoint;
        }
    }
    if (host.empty()) return false;
    if (prefix == "stun:") {
        target.iceServers.emplace_back(host, port);
        return true;
    }
    const auto relay = prefix == "turns:" ? rtc::IceServer::RelayType::TurnTls :
        parameters.find("transport=tcp") != std::string::npos ? rtc::IceServer::RelayType::TurnTcp :
        rtc::IceServer::RelayType::TurnUdp;
    target.iceServers.emplace_back(host, port, source.username, source.credential, relay);
    return true;
}

}  // namespace

namespace {

std::string state_to_string(rtc::PeerConnection::State s) {
    using S = rtc::PeerConnection::State;
    switch (s) {
        case S::New:          return "new";
        case S::Connecting:   return "connecting";
        case S::Connected:    return "connected";
        case S::Disconnected: return "disconnected";
        case S::Failed:       return "failed";
        case S::Closed:       return "closed";
        default:              return "unknown";
    }
}

}  // namespace

PeerSession::PeerSession(Role role, Config config)
    : role_(role), config_(std::move(config)) {}

PeerSession::~PeerSession() {
    close();
}

std::shared_ptr<PeerSession> PeerSession::create(Role role, Config config) {
    // 构造函数私有，用 shared_ptr 管理生命周期：回调里会持有 weak_ptr
    return std::shared_ptr<PeerSession>(new PeerSession(role, std::move(config)));
}

void PeerSession::set_callbacks(Callbacks cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    cb_ = std::move(cb);
}

void PeerSession::invoke_log(const std::string& msg) const {
    std::function<void(const std::string&)> fn;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        fn = cb_.on_log;
    }
    if (fn) fn(msg);
}

void PeerSession::log(const std::string& msg) const {
    invoke_log(msg);
}

void PeerSession::start() {
    setup_peer_connection();

    if (role_ == Role::Offerer) {
        create_local_channels();
    }
    // Answerer 不建通道：等对端的 offer 到达后，由 libdatachannel 通过
    // onDataChannel 回调把通道交给我们。
}

void PeerSession::setup_peer_connection() {
    rtc::Configuration config;

    for (const auto& server : config_.ice_servers) {
        try {
            add_ice_server(server, config);
        } catch (const std::exception& e) {
            log("忽略无效 ICE server 配置（检查协议、IPv6 方括号与端口）");
        }
    }
    for (const auto& url : config_.stun_servers) {
        try { if (!url.empty()) add_ice_server({url, "", ""}, config); }
        catch (const std::exception&) { log("忽略无效 STUN server 配置"); }
    }
    if (config_.relay_only) {
        config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    }

    if (!config_.bind_address.empty()) {
        config.bindAddress = config_.bind_address;
    }
    config.enableIceUdpMux = config_.enable_ice_udp_mux;
    config.mtu             = config_.mtu;
    config.maxMessageSize  = 512 * 1024; // escaped 64 KiB clipboard text fits one reliable message

    pc_ = std::make_shared<rtc::PeerConnection>(config);

    std::weak_ptr<PeerSession> weak = weak_from_this();

    pc_->onLocalDescription([weak](rtc::Description description) {
        auto self = weak.lock();
        if (!self) return;

        const std::string type = description.typeString();
        self->invoke_log("生成本端 SDP (" + type + ")");

        std::function<void(const std::string&, const std::string&)> fn;
        {
            std::lock_guard<std::mutex> lock(self->cb_mutex_);
            fn = self->cb_.on_local_description;
        }
        if (fn) fn(std::string(description), type);
    });

    pc_->onLocalCandidate([weak](rtc::Candidate candidate) {
        auto self = weak.lock();
        if (!self) return;

        const std::string cand = candidate.candidate();
        const std::string mid  = candidate.mid();
        self->invoke_log("收集到 candidate: " + cand);

        std::function<void(const std::string&, const std::string&)> fn;
        {
            std::lock_guard<std::mutex> lock(self->cb_mutex_);
            fn = self->cb_.on_local_candidate;
        }
        if (fn) fn(cand, mid);
    });

    pc_->onStateChange([weak](rtc::PeerConnection::State state) {
        auto self = weak.lock();
        if (!self) return;
        self->invoke_log("连接状态: " + state_to_string(state));

        std::function<void(rtc::PeerConnection::State)> fn;
        {
            std::lock_guard<std::mutex> lock(self->cb_mutex_);
            fn = self->cb_.on_state;
        }
        if (fn) fn(state);
    });

    pc_->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState state) {
        auto self = weak.lock();
        if (!self) return;
        using G = rtc::PeerConnection::GatheringState;
        const char* name = "unknown";
        switch (state) {
            case G::New:        name = "new"; break;
            case G::InProgress: name = "in-progress"; break;
            case G::Complete:   name = "complete"; break;
        }
        self->invoke_log(std::string("candidate 收集: ") + name);
    });

    // 对端开的通道到达（answerer 侧走这里）
    pc_->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> dc) {
        auto self = weak.lock();
        if (!self) return;
        self->invoke_log("对端打开通道: " + dc->label());

        {
            std::lock_guard<std::mutex> lock(self->ch_mutex_);
            self->channels_[dc->label()] = dc;
        }

        std::function<void(std::shared_ptr<rtc::DataChannel>)> fn;
        {
            std::lock_guard<std::mutex> lock(self->cb_mutex_);
            fn = self->cb_.on_data_channel;
        }
        if (fn) fn(dc);
    });
}

void PeerSession::create_local_channels() {
    auto make = [this](const std::string& label, bool reliable) {
        rtc::DataChannelInit init;

        if (reliable) {
            // 可靠有序：控制指令与剪贴板不能丢、不能乱序
            init.reliability.unordered = false;
        } else {
            // 不可靠无序：视频帧。丢帧无所谓，但绝不能因为等重传而阻塞后面的帧
            init.reliability.unordered    = true;
            init.reliability.maxRetransmits = 0;
        }

        auto dc = pc_->createDataChannel(label, init);
        {
            std::lock_guard<std::mutex> lock(ch_mutex_);
            channels_[label] = dc;
        }
        log("创建通道: " + label + (reliable ? " (可靠)" : " (不可靠)"));
        return dc;
    };

    make(kChControl, true);
    make(kChVideo, false);
    make(kChClip, true);
    make(kChFile, true);

    // createDataChannel 会触发自动协商，offer 通过 onLocalDescription 回调送出，
    // 这里不需要再显式调用 setLocalDescription。
}

void PeerSession::set_remote_description(const std::string& sdp, const std::string& type) {
    if (!pc_) return;
    log("收到对端 SDP (" + type + ")");
    pc_->setRemoteDescription(rtc::Description(sdp, type));
}

void PeerSession::add_remote_candidate(const std::string& candidate, const std::string& mid) {
    if (!pc_) return;
    try {
        pc_->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception& e) {
        // 单个 candidate 解析失败不该中断整个会话，其余 candidate 仍可能联通
        log(std::string("candidate 添加失败(已忽略): ") + e.what());
    }
}

std::shared_ptr<rtc::DataChannel> PeerSession::channel(const std::string& label) {
    std::lock_guard<std::mutex> lock(ch_mutex_);
    auto it = channels_.find(label);
    return it == channels_.end() ? nullptr : it->second;
}

bool PeerSession::is_connected() const {
    std::lock_guard<std::mutex> lock(ch_mutex_);
    for (const auto& kv : channels_) {
        if (kv.second && kv.second->isOpen()) return true;
    }
    return false;
}

void PeerSession::close() {
    {
        std::lock_guard<std::mutex> lock(ch_mutex_);
        for (auto& kv : channels_) {
            if (kv.second) {
                try { kv.second->close(); } catch (...) {}
            }
        }
        channels_.clear();
    }

    if (pc_) {
        try { pc_->close(); } catch (...) {}
        pc_.reset();
    }
}

std::string PeerSession::state_string() const {
    if (!pc_) return "closed";
    return state_to_string(pc_->state());
}

std::optional<std::chrono::milliseconds> PeerSession::rtt() const {
    if (!pc_) return std::nullopt;
    return pc_->rtt();
}

std::string PeerSession::local_dtls_fingerprint() const {
    if (!pc_) return {};
    auto desc = pc_->localDescription();
    if (!desc) return {};
    auto fp = desc->fingerprint();
    if (!fp) return {};
    return rtc::CertificateFingerprint::AlgorithmIdentifier(fp->algorithm) + " " + fp->value;
}

std::string PeerSession::remote_dtls_fingerprint() const {
    if (!pc_) return {};
    auto desc = pc_->remoteDescription();
    if (!desc) return {};
    auto fp = desc->fingerprint();
    if (!fp) return {};
    return rtc::CertificateFingerprint::AlgorithmIdentifier(fp->algorithm) + " " + fp->value;
}

std::string PeerSession::selected_pair_string() const {
    if (!pc_) return {};

    rtc::Candidate local, remote;
    if (!pc_->getSelectedCandidatePair(&local, &remote)) return {};

    auto fmt = [](const rtc::Candidate& c) {
        std::string addr = c.address().value_or("?");
        std::string port = c.port() ? std::to_string(*c.port()) : "?";
        // IPv6 地址本身含冒号，加方括号才能和端口分隔符区分开
        if (addr.find(':') != std::string::npos) addr = "[" + addr + "]";
        return addr + ":" + port;
    };

    return fmt(local) + " <-> " + fmt(remote);
}

}  // namespace pxc
