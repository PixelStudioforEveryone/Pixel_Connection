#pragma once

// 账号与设备目录的持久化层（SQLite）。
//
// 与在线状态分离：这里存的是长期事实（账号、设备归属、公钥、最近观测 IP、
// 最近上线时间），进程内 DeviceRegistry 只管当前连接。
//
// 线程安全：内部用一把互斥锁串行化。SQLite 编译为串行模式，但 IXWebSocket
// 的 HTTP 回调来自多个连接线程，这里再自己加一层锁更省心，量级也完全够用。

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace pxc::store {

struct Account {
    int64_t     id = 0;
    std::string username;
    std::string email;
    std::string created_at;
};

struct Device {
    std::string device_id;
    int64_t     account_id = 0;
    std::string name;
    std::string public_key_b64;   // Ed25519 公钥，用于校验设备上线签名
    std::string connection_verifier_hex;  // 连接密钥的 HMAC 派生值，不存原文
    std::string last_public_ip;   // 服务器观测到的信令连接源地址
    std::string last_seen;        // ISO-8601 UTC
    std::string platform;
    bool        online = false;
    bool        revoked = false;
    std::string created_at;
};

enum class Result {
    Ok,
    Duplicate,       // 用户名/邮箱/设备 ID 已存在
    NotFound,
    InvalidArgument, // 输入不合法（空用户名、格式错误等）
    Unauthorized,    // 密码错误或 token 无效
    Internal,
};

class Database {
public:
    Database() = default;
    ~Database();

    Database(const Database&)            = delete;
    Database& operator=(const Database&) = delete;

    // 打开并建表。失败返回 false，错误信息写入 error()。
    bool open(const std::string& path);
    // Stable public identifier for this account database, shared by all proxy addresses.
    std::string server_id();
    void close();

    const std::string& last_error() const { return last_error_; }

    // ---------------------------------------------------------------- 账号
    Result create_account(const std::string& username,
                          const std::string& email,
                          const std::string& password_hash,
                          int64_t& out_account_id);

    // 用户名或邮箱查账号，同时取出密码哈希交给调用方校验。
    // 查不到时返回 Unauthorized，调用方必须对「查不到」和「密码错」返回同样的响应，
    // 避免账号枚举。
    Result find_account_by_identifier(const std::string& identifier,
                                      Account& out_account,
                                      std::string& out_password_hash);

    Result account_by_id(int64_t account_id, Account& out_account);

    // ------------------------------------------------------------ 会话 token
    // 只存 token 的 SHA-256
    Result create_session(int64_t account_id,
                          const std::string& token_hash,
                          const std::string& expires_at);

    // 校验 token 是否有效且未过期；有效时返回账号信息
    Result validate_session(const std::string& token_hash,
                            Account& out_account,
                            std::string& out_expires_at);

    Result delete_session(const std::string& token_hash);
    Result delete_expired_sessions(const std::string& now_iso);

    // ---------------------------------------------------------------- 设备
    // 多账号模型：同一台设备可登记到多个账号名下，
    // 每个登记是一行 (device_id, account_id)。
    Result add_device(const Device& device);
    Result device_by_id(const std::string& device_id, Device& out_device);
    // 查询某设备在某账号名下的登记行；不存在返回 NotFound
    Result device_of_account(const std::string& device_id, int64_t account_id,
                             Device& out_device);

    // 账号名下的全部设备（含离线）
    Result devices_of_account(int64_t account_id, std::vector<Device>& out_devices);

    Result set_device_online(const std::string& device_id,
                             bool online,
                             const std::string& public_ip,
                             const std::string& last_seen,
                             const std::string& platform);

    Result rename_device(int64_t account_id,
                         const std::string& device_id,
                         const std::string& new_name);

    Result remove_device(int64_t account_id, const std::string& device_id);
    Result revoke_device(int64_t account_id, const std::string& device_id);

    // --------------------------------------------------- 认领码（一次性，短时）
    Result create_claim(int64_t account_id,
                        const std::string& code_hash,
                        const std::string& expires_at);

    // 消费认领码：成功则删除该条并返回是否曾被消费
    Result consume_claim(const std::string& code_hash, int64_t& out_account_id);

    Result delete_expired_claims(const std::string& now_iso);

private:
    // 以下两个假设调用方已持有 mtx_（open/migrate 内部使用）
    bool exec_locked(const std::string& sql);
    bool migrate_locked();

    sqlite3*    db_ = nullptr;
    std::string last_error_;
    mutable std::mutex mtx_;
};

}  // namespace pxc::store

namespace pxc::store {

// 用户名/邮箱规范化：去首尾空白 + 转小写。
// 数据库存原始值用于展示、存 norm 用于唯一性与登录匹配，
// 避免 "Alice" 和 "alice" 变成两个账号。
std::string normalize_identifier(const std::string& value);

// 统一的 ISO-8601 UTC 时间戳，字典序即时间序
std::string now_iso8601();

// 当前时间 + seconds 秒，用于令牌和认领码的过期时间
std::string iso_after_seconds(int seconds);

}  // namespace pxc::store
