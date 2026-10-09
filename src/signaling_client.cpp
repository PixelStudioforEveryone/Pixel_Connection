#include "pxc/signaling_client.h"

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>

#include "pxc/crypto.h"

namespace pxc {

SignalingClient::SignalingClient(std::string url, std::string device_name)
    : url_(std::move(url)),
      device_name_(std::move(device_name)),
      ws_(std::make_unique<ix::WebSocket>()) {}

SignalingClient::~SignalingClient() {
    stop();
}

void SignalingClient::set_credentials(std::string access_token, Identity identity,
                                      std::string platform) {
    {
        std::lock_guard<std::mutex> lock(cred_mutex_);
        access_token_     = std::move(access_token);
        identity_         = std::move(identity);
        platform_         = std::move(platform);
        has_credentials_  = !access_token_.empty() && identity_.valid();
    }

    // 凭据可能在连接建立之后才拿到（登录 → 添加本机）。
    // 这种情况下必须主动补一次认证，否则设备会一直停在「已连接但未上线」。
    if (connected_.load()) authenticate();
}

void SignalingClient::set_access_token(std::string access_token) {
    {
        std::lock_guard<std::mutex> lock(cred_mutex_);
        access_token_ = std::move(access_token);
        has_credentials_ = !access_token_.empty() && identity_.valid();
        if (!has_credentials_) online_.store(false);
    }

    if (connected_.load()) authenticate();
}

bool SignalingClient::has_credentials() const {
    std::lock_guard<std::mutex> lock(cred_mutex_);
    return has_credentials_;
}

std::string SignalingClient::device_id() const {
    std::lock_guard<std::mutex> lock(cred_mutex_);
    return identity_.valid() ? identity_.device_id() : std::string();
}

void SignalingClient::set_callbacks(Callbacks cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    cb_ = std::move(cb);
}

void SignalingClient::invoke(const std::function<void()>& fn) {
    if (fn) fn();
}

void SignalingClient::invoke_message(const Message& msg) {
    std::function<void(const Message&)> fn;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        fn = cb_.on_message;
    }
    if (fn) fn(msg);
}

void SignalingClient::start() {
    ws_->setUrl(url_);

    // 断线自动重连，间隔 1s 起、指数退避到 30s。
    // 重连后由 on_open 重新走一遍账号认证，令牌过期会在那里被服务器拒绝。
    ws_->enableAutomaticReconnection();
    ws_->setMinWaitBetweenReconnectionRetries(1000);
    ws_->setMaxWaitBetweenReconnectionRetries(30000);

    // 协议层 ping/pong 保活：30s 一个 4 字节帧。
    // 半开连接（网线拔掉、NAT 表项过期）也能被检测到。
    ws_->setPingInterval(30);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        handle_message(msg);
    });

    ws_->start();
}

void SignalingClient::stop() {
    if (!ws_) return;
    ws_->stop();
    connected_.store(false);
    online_.store(false);
}

void SignalingClient::handle_message(const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
        case ix::WebSocketMessageType::Open: {
            connected_.store(true);
            online_.store(false);

            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> lock(cb_mutex_);
                fn = cb_.on_open;
            }
            invoke(fn);

            // 连接建立后立刻认证。没有凭据就不发 auth，
            // 服务器不会允许这种连接作为设备上线。
            authenticate();
            break;
        }

        case ix::WebSocketMessageType::Close: {
            connected_.store(false);
            online_.store(false);

            std::function<void(const std::string&)> fn;
            {
                std::lock_guard<std::mutex> lock(cb_mutex_);
                fn = cb_.on_close;
            }
            if (fn) {
                fn("code=" + std::to_string(msg->closeInfo.code) +
                   " reason=" + msg->closeInfo.reason);
            }
            break;
        }

        case ix::WebSocketMessageType::Error: {
            connected_.store(false);
            online_.store(false);

            std::function<void(const std::string&)> fn;
            {
                std::lock_guard<std::mutex> lock(cb_mutex_);
                fn = cb_.on_error;
            }
            if (fn) {
                fn("重试次数=" + std::to_string(msg->errorInfo.retries) +
                   " 原因=" + msg->errorInfo.reason);
            }
            break;
        }

        case ix::WebSocketMessageType::Message: {
            if (msg->binary) break;  // 信令只走文本 JSON

            Message parsed;
            if (!decode(msg->str, parsed)) break;

            // 挑战必须先处理：它决定了接下来要发的 register 内容
            if (parsed.type == kAuthChallenge) {
                {
                    std::lock_guard<std::mutex> lock(nonce_mutex_);
                    pending_nonce_ = parsed.nonce;
                }

                std::function<void(const Message&)> fn;
                {
                    std::lock_guard<std::mutex> lock(cb_mutex_);
                    fn = cb_.on_authenticated;
                }
                if (fn) fn(parsed);

                register_with_signature(parsed.nonce, parsed);
                return;
            }

            if (parsed.type == kRegistered) {
                online_.store(true);

                std::function<void(const Message&)> fn;
                {
                    std::lock_guard<std::mutex> lock(cb_mutex_);
                    fn = cb_.on_registered;
                }
                if (fn) fn(parsed);
            }

            invoke_message(parsed);
            break;
        }

        default:
            // Ping/Pong/Fragment 由库内部处理
            break;
    }
}

void SignalingClient::authenticate() {
    std::string token;
    {
        std::lock_guard<std::mutex> lock(cred_mutex_);
        if (!has_credentials_) return;
        token = access_token_;
    }
    if (token.empty()) return;

    Message msg;
    msg.type         = kAuth;
    msg.access_token = token;
    send(msg);
}

void SignalingClient::register_with_signature(const std::string& challenge,
                                             const Message&     challenge_msg) {
    std::string device_id;
    std::string pubkey_b64;
    std::string token;
    std::string verifier;
    std::string platform;
    std::vector<uint8_t> signature;

    {
        std::lock_guard<std::mutex> lock(cred_mutex_);
        if (!has_credentials_) return;

        device_id  = identity_.device_id();
        pubkey_b64 = identity_.pubkey_b64();
        token      = access_token_;
        platform   = platform_.empty() ? Identity::current_platform() : platform_;
        verifier   = identity_.connection_verifier_hex();

        // 服务器把 nonce 的 ASCII 字节做了 base64。解出原始字符串后再参与签名，
        // 这样两端签的是同一串字节。
        std::vector<uint8_t> nonce_bytes;
        if (!base64_decode(challenge, nonce_bytes)) return;
        const std::string nonce(nonce_bytes.begin(), nonce_bytes.end());

        // 签名内容 = 域前缀 + nonce + 设备 ID。
        // 绑定用途和设备，避免这把私钥在别处签过的内容被拿来冒充上线。
        const std::string message =
            std::string(kRegisterSignDomain) + nonce + device_id;
        const std::vector<uint8_t> data(message.begin(), message.end());

        try {
            signature = identity_.sign(data);
        } catch (const std::exception&) {
            return;  // 私钥不可用时不上线，而不是退化成无签名注册
        }
    }

    Message reg;
    reg.type                = kRegister;
    reg.device_id           = device_id;
    reg.name                = device_name_;
    reg.pubkey              = pubkey_b64;
    reg.access_token        = token;
    reg.nonce               = challenge;
    reg.signature           = base64_encode(signature);
    reg.platform            = platform;
    reg.connection_verifier = verifier;
    send(reg);
}

void SignalingClient::send_raw(const std::string& text) {
    if (!ws_ || !connected_.load()) return;
    ws_->send(text);
}

void SignalingClient::send(const Message& m) {
    send_raw(encode(m));
}

}  // namespace pxc
