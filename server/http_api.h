#pragma once

// 账号 HTTP API。
//
// 与 WebSocket 信令同进程、不同端口：信令走长连接推送在线状态，
// 账号接口是短请求（注册/登录/设备列表/改名/删除），用 HTTP 更自然，
// 也让 Qt 客户端可以直接用 QNetworkAccessManager。
//
// 这一层只做：路径解析、鉴权头解析、JSON 编解码、状态码映射。
// 所有规则都在 AuthService 里。

#include <memory>
#include <atomic>
#include <string>
#include <vector>

#include <ixwebsocket/IXHttp.h>

#include "auth_service.h"

namespace ix {
class HttpServer;
class ConnectionState;
}  // namespace ix

namespace pxc::server {

struct ApiOptions {
    int         port      = 29910;
    std::string host_v4   = "0.0.0.0";
    std::string host_v6   = "::";
    bool        enable_v4 = true;
    bool        enable_v6 = true;
    // 生产必须走 HTTPS。默认允许明文只是为了本机开发，启动时会显著提示。
    bool        allow_plain_http = true;
    bool        trust_loopback_proxy = false;
    std::string server_id;
    std::string server_name;
    std::string server_platform;
    int signaling_port = 9910;
    std::string advertised_api_url;
    std::string advertised_signaling_url;
};

class AccountApi {
public:
    AccountApi(AuthService& auth, ApiOptions options);
    ~AccountApi();

    AccountApi(const AccountApi&)            = delete;
    AccountApi& operator=(const AccountApi&) = delete;

    bool start();
    void stop();
    void set_ready(bool ready) { ready_.store(ready); }

    // 已成功启动的监听描述，用于日志
    const std::vector<std::string>& listeners() const { return listeners_; }

private:
    ix::HttpResponsePtr route(const ix::HttpRequestPtr&            request,
                              const std::shared_ptr<ix::ConnectionState>& state);

    AuthService&                                 auth_;
    ApiOptions                                   options_;
    std::atomic<bool> ready_{false};
    std::vector<std::shared_ptr<ix::HttpServer>> servers_;
    std::vector<std::string>                     listeners_;
};

}  // namespace pxc::server
