#pragma once

// 信令客户端：与信令服务器之间的 WebSocket 长连接。
//
// 只负责控制面：账号认证、设备上线、保活、拉列表、转发 SDP/ICE。
// 所有回调都在 IXWebSocket 的 I/O 线程上触发，调用方需要自己保证线程安全。
//
// 保活走 WebSocket 协议层的 ping/pong（每 30s 一个 4 字节帧），
// 不用应用层心跳：半开连接也能被检测到，且重连由 IXWebSocket 自动处理。
//
// 账号模式下的上线流程（服务器强制要求，未认证的连接不能作为设备上线）：
//   1. WebSocket 建立后发送 auth，携带 access_token
//   2. 服务器返回 auth_challenge，内含一次性 nonce
//   3. 客户端用设备私钥签名 domain + nonce + device_id，发送 register
//   4. 服务器校验签名与公钥指纹后，设备上线并收到同账号的设备列表
//
// 设备私钥和连接密钥都不经网络传输：私钥只用于本地签名，服务器只验证签名。

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <ixwebsocket/IXWebSocketMessage.h>

#include "pxc/identity.h"
#include "pxc/protocol.h"

namespace ix {
class WebSocket;
}

namespace pxc {

class SignalingClient {
public:
    struct Callbacks {
        std::function<void()>                   on_open;
        std::function<void(const std::string&)> on_close;
        std::function<void(const std::string&)> on_error;
        std::function<void(const Message&)>     on_message;
        // 服务器确认设备已上线（收到 registered）
        std::function<void(const Message&)>     on_registered;
        // 账号认证通过、等待提交签名（收到 auth_challenge）
        std::function<void(const Message&)>     on_authenticated;
    };

    SignalingClient(std::string url, std::string device_name);
    ~SignalingClient();

    SignalingClient(const SignalingClient&)            = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // 账号模式凭据。必须在 start() 之前设置。
    // identity 必须已经包含设备密钥（即设备已「添加本机」）。
    void set_credentials(std::string access_token, Identity identity, std::string platform);

    // 更新访问令牌（重新登录后）。空 token 会让客户端停止上线。
    void set_access_token(std::string access_token);

    bool has_credentials() const;

    void set_callbacks(Callbacks cb);

    void start();
    void stop();

    bool connected() const { return connected_.load(); }
    // 是否已作为设备上线（可收发信令）
    bool online() const { return online_.load(); }

    void send(const Message& m);
    void send_raw(const std::string& text);

    // 主动发送 auth（连接建立后自动调用；重连后也会重新认证）
    void authenticate();

    const std::string& url() const { return url_; }
    const std::string& device_name() const { return device_name_; }
    // 取当前设备 ID；凭据未设置时为空
    std::string device_id() const;

private:
    void handle_message(const ix::WebSocketMessagePtr& msg);
    // 对服务器下发的挑战签名并提交 register
    void register_with_signature(const std::string& challenge, const Message& challenge_msg);
    void invoke(const std::function<void()>& fn);
    void invoke_message(const Message& msg);

    std::string                    url_;
    std::string                    device_name_;
    std::unique_ptr<ix::WebSocket> ws_;

    mutable std::mutex cred_mutex_;
    std::string        access_token_;
    Identity           identity_;
    std::string        platform_;
    bool               has_credentials_ = false;

    // 服务器下发的一次性挑战（base64），等待用私钥签名
    std::mutex  nonce_mutex_;
    std::string pending_nonce_;

    Callbacks          cb_;
    mutable std::mutex cb_mutex_;

    std::atomic<bool> connected_{false};
    std::atomic<bool> online_{false};
};

}  // namespace pxc
