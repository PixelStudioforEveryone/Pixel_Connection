#pragma once

// 账号 HTTP 客户端。
//
// 客户端和 Qt 界面都用它访问账号 API：
//   - 注册 / 登录 / 注销
//   - 设备列表 / 改名 / 删除
//
// 只依赖 IXWebSocket 自带的 HTTP 客户端，不引入额外依赖。
// 所有方法都是阻塞调用；Qt 侧应放在工作线程或改用 QNetworkAccessManager。
//
// 安全约定：access token 只在内存中传递，不写日志、不落盘、不进 SDP。

#include <string>
#include <vector>

#include "pxc/protocol.h"

namespace pxc {

struct HttpResult {
    bool        ok = false;       // 传输成功且 2xx
    int         status = 0;       // HTTP 状态码，0 表示未拿到响应
    std::string body;             // 原始响应体
    std::string error;            // 传输层错误或服务器返回的错误码
};

class AccountApiClient {
public:
    // base_url 形如 "http://127.0.0.1:29910" 或 "https://api.example.com"
    explicit AccountApiClient(std::string base_url, int timeout_seconds = 15);

    void set_base_url(const std::string& base_url) { base_url_ = base_url; }
    const std::string& base_url() const { return base_url_; }

    // 生产环境必须校验证书。自签证书测试时才允许关闭。
    void set_verify_tls(bool verify) { verify_tls_ = verify; }

    // ------------------------------------------------------------------ 账号
    bool register_account(const std::string& username,
                          const std::string& email,
                          const std::string& password,
                          std::string&       error);

    bool login(const std::string& identifier,
               const std::string& password,
               std::string&       out_token,
               std::string&       out_username,
               std::string&       error);

    bool logout(const std::string& token, std::string& error);

    // ------------------------------------------------------------------ 设备
    bool list_devices(const std::string&    token,
                      std::vector<PeerInfo>& out_devices,
                      std::string&          error);

    bool rename_device(const std::string& token,
                       const std::string& device_id,
                       const std::string& new_name,
                       std::string&       error);

    bool remove_device(const std::string& token,
                       const std::string& device_id,
                       std::string&       error);

    bool revoke_device(const std::string& token,
                       const std::string& device_id,
                       std::string&       error);

private:
    HttpResult request(const std::string& method,
                       const std::string& path,
                       const std::string& body,
                       const std::string& token);

    std::string base_url_;
    int         timeout_seconds_;
    bool        verify_tls_ = true;
};

}  // namespace pxc
