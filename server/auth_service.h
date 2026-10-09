#pragma once

// 账号、会话、设备入网的业务逻辑。
//
// 这一层不关心 HTTP：HTTP 处理器只做参数解析和状态码，规则都在这里，
// 单元测试可以直接调用，不需要起网络。
//
// 关键约定：
//   - 账号密码只以 scrypt 哈希存库
//   - access token 明文只返回给客户端一次，服务端只存 SHA-256
//   - 设备私钥永不接触服务器；服务器只保存公钥，用来校验设备签名
//   - 设备连接密钥的原文也永不接触服务器；只保存 HMAC 派生值
//   - 公网 IP 只由调用方传入服务器观测到的连接地址，不信任客户端上报
//
// 设备入网（即用户说的「认领」）流程：
//   1. 用户在设备上登录账号，拿到 access_token
//   2. 用户点击「添加本机」，设备这时才生成 Ed25519 密钥对和设备 ID
//   3. 设备把 access_token 交给信令服务器，服务器下发一次性 nonce
//   4. 设备用新私钥对 nonce 签名，连同公钥、设备 ID 一起提交
//   5. 服务器校验签名与公钥指纹，把设备登记到 token 对应的账号名下

#include <cstdint>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "store.h"

namespace pxc::server {

struct AuthConfig {
    int  token_ttl_seconds    = 30 * 60;  // access token 有效期
    int  login_max_attempts    = 8;       // 限速窗口内允许的失败次数
    int  login_window_seconds  = 300;     // 限速窗口长度
    int  register_max_attempts = 12;      // 同一来源 IP 的注册尝试次数
    int  register_window_seconds = 3600;  // 注册限速窗口（1 小时）
    bool allow_registration   = true;     // 关掉后新账号无法注册
};

struct SessionInfo {
    std::string token;       // 只在登录响应里出现一次
    std::string expires_at;
    int64_t     account_id = 0;
    std::string username;
};

// 用独立枚举而不是异常，方便 HTTP 层映射状态码
enum class AuthStatus {
    Ok,
    InvalidArgument,
    DuplicateAccount,
    BadCredentials,
    Unauthorized,   // token 无效/过期
    Forbidden,      // 资源不属于该账号，或签名不对
    NotFound,
    RateLimited,
    RegistrationClosed,
    Internal,
};

class AuthService {
public:
    AuthService(store::Database& db, AuthConfig config = {});

    // ------------------------------------------------------------------ 账号
    AuthStatus register_account(const std::string& username,
                                const std::string& email,
                                const std::string& password,
                                store::Account&    out_account);

    AuthStatus login(const std::string& identifier,
                     const std::string& password,
                     const std::string& client_ip,
                     SessionInfo&       out_session);

    AuthStatus logout(const std::string& token);

    // HTTP 中间件和信令认证共用
    AuthStatus validate_token(const std::string& token, store::Account& out_account);

    // ------------------------------------------------------------ 设备入网
    // 信令服务器调用：签发一次性挑战
    std::string issue_nonce();

    // 校验设备提交的 nonce 签名。nonce 只能成功使用一次。
    bool consume_nonce_and_verify(const std::string& device_id,
                                  const std::string& nonce_b64,
                                  const std::string& signature_b64,
                                  const std::string& public_key_b64);

    // 设备入网：把设备登记到账号名下。设备已属于该账号时视为重复入网。
    AuthStatus enroll_device(const std::string& token,
                             const std::string& device_id,
                             const std::string& public_key_b64,
                             const std::string& connection_verifier_hex,
                             const std::string& name,
                             const std::string& platform,
                             store::Device&     out_device);

    // 信令服务器调用：取设备记录并确认它属于该账号
    AuthStatus device_for_account(int64_t            account_id,
                                  const std::string& device_id,
                                  store::Device&     out_device);

    // ---------------------------------------------------------------- 设备
    AuthStatus list_devices(int64_t account_id, std::vector<store::Device>& out_devices);

    AuthStatus rename_device(int64_t            account_id,
                             const std::string& device_id,
                             const std::string& new_name);

    AuthStatus remove_device(int64_t account_id, const std::string& device_id);
    AuthStatus revoke_device(int64_t account_id, const std::string& device_id);

    // 信令服务器调用：更新在线状态与观测到的连接地址
    void mark_online(const std::string& device_id,
                     const std::string& public_ip,
                     const std::string& platform);
    void mark_offline(const std::string& device_id);

    // 注册接口不需要认证，必须按来源 IP 限速，避免批量账号枚举/滥用。
    bool register_allowed(const std::string& client_ip);
    void note_register_attempt(const std::string& client_ip);

    const AuthConfig& config() const { return config_; }

private:
    bool rate_limit_ok(const std::string& key);
    void rate_limit_note_failure(const std::string& key);
    void rate_limit_reset(const std::string& key);

    store::Database& db_;
    AuthConfig       config_;

    struct Window {
        int         failures     = 0;
        std::time_t window_start = 0;
    };
    std::mutex                    rate_mtx_;
    std::map<std::string, Window> rate_;
    std::map<std::string, Window> register_rate_;

    // 一次性 nonce：存入即带过期时间，校验后立即删除
    struct NonceEntry {
        std::string device_id;
        std::time_t expires_at = 0;
    };
    std::mutex                   nonce_mtx_;
    std::map<std::string, NonceEntry> nonces_;
};

}  // namespace pxc::server
