#include "auth_service.h"

#include <algorithm>

#include "pxc/crypto.h"
#include "pxc/identity.h"
#include "pxc/protocol.h"  // kRegisterSignDomain：上线签名所用的域分隔前缀

namespace pxc::server {
namespace {

// 一次性挑战的存活时间。够一次网络往返即可，不宜长。
constexpr int kNonceTtlSeconds = 120;

constexpr size_t kMaxUsernameLen = 64;
constexpr size_t kMaxEmailLen    = 254;
constexpr size_t kMinPasswordLen = 8;
constexpr size_t kMaxPasswordLen = 1024;

bool looks_like_email(const std::string& value) {
    const size_t at = value.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= value.size()) return false;
    if (value.find('@', at + 1) != std::string::npos) return false;
    const size_t dot = value.find('.', at + 1);
    return dot != std::string::npos && dot + 1 < value.size();
}

bool plausible_username(const std::string& value) {
    const std::string norm = store::normalize_identifier(value);
    if (norm.size() < 3 || norm.size() > kMaxUsernameLen) return false;
    // 只允许常见字符，避免同形字和空白导致的账号混淆
    return std::all_of(norm.begin(), norm.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '@';
    });
}

}  // namespace

AuthService::AuthService(store::Database& db, AuthConfig config)
    : db_(db), config_(config) {}

// ---------------------------------------------------------------------- 账号

AuthStatus AuthService::register_account(const std::string& username,
                                                     const std::string& email,
                                                     const std::string& password,
                                                     store::Account&    out_account) {
    if (!config_.allow_registration) return AuthStatus::RegistrationClosed;

    if (!plausible_username(username)) return AuthStatus::InvalidArgument;
    if (!looks_like_email(email) || email.size() > kMaxEmailLen) return AuthStatus::InvalidArgument;
    if (password.size() < kMinPasswordLen || password.size() > kMaxPasswordLen) {
        return AuthStatus::InvalidArgument;
    }

    int64_t account_id = 0;
    const auto rc = db_.create_account(username, email, crypto::hash_password(password), account_id);
    switch (rc) {
        case store::Result::Ok:
            break;
        case store::Result::Duplicate:
            return AuthStatus::DuplicateAccount;
        case store::Result::InvalidArgument:
            return AuthStatus::InvalidArgument;
        default:
            return AuthStatus::Internal;
    }

    if (db_.account_by_id(account_id, out_account) != store::Result::Ok) {
        return AuthStatus::Internal;
    }
    return AuthStatus::Ok;
}

AuthStatus AuthService::login(const std::string& identifier,
                                           const std::string& password,
                                           const std::string& client_ip,
                                           SessionInfo&       out_session) {
    const std::string norm = store::normalize_identifier(identifier);
    const std::string rate_key = norm + "|" + client_ip;

    if (norm.empty() || password.empty()) return AuthStatus::InvalidArgument;
    if (!rate_limit_ok(rate_key)) return AuthStatus::RateLimited;

    store::Account account;
    std::string    password_hash;
    const auto     found = db_.find_account_by_identifier(identifier, account, password_hash);

    // 账号不存在时也跑一次哈希校验，让耗时与密码错误相近，
    // 降低通过响应时间枚举账号的可能。
    if (found != store::Result::Ok) {
        crypto::verify_password(password,
                                "$scrypt$N=32768,r=8,p=1$AAAAAAAAAAAAAAAAAAAAAA==$"
                                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=");
        rate_limit_note_failure(rate_key);
        return AuthStatus::BadCredentials;
    }

    if (!crypto::verify_password(password, password_hash)) {
        rate_limit_note_failure(rate_key);
        return AuthStatus::BadCredentials;
    }

    rate_limit_reset(rate_key);

    // token 明文只在此处出现一次，库里只留 SHA-256
    out_session.token      = crypto::generate_token();
    out_session.expires_at = store::iso_after_seconds(config_.token_ttl_seconds);
    out_session.account_id = account.id;
    out_session.username   = account.username;

    const auto stored = db_.create_session(account.id, crypto::sha256_hex(out_session.token),
                                           out_session.expires_at);
    return stored == store::Result::Ok ? AuthStatus::Ok : AuthStatus::Internal;
}

AuthStatus AuthService::logout(const std::string& token) {
    if (token.empty()) return AuthStatus::InvalidArgument;
    const auto rc = db_.delete_session(crypto::sha256_hex(token));
    return rc == store::Result::Ok ? AuthStatus::Ok : AuthStatus::Internal;
}

AuthStatus AuthService::validate_token(const std::string& token,
                                                   store::Account&    out_account) {
    if (token.empty()) return AuthStatus::Unauthorized;

    std::string expires_at;
    const auto  rc = db_.validate_session(crypto::sha256_hex(token), out_account, expires_at);
    switch (rc) {
        case store::Result::Ok:           return AuthStatus::Ok;
        case store::Result::Unauthorized: return AuthStatus::Unauthorized;
        default:                          return AuthStatus::Internal;
    }
}

// ---------------------------------------------------------------- 一次性挑战

std::string AuthService::issue_nonce() {
    const std::string nonce = crypto::to_hex(crypto::random_bytes(32));

    std::lock_guard<std::mutex> lock(nonce_mtx_);
    const std::time_t           now = std::time(nullptr);

    // 顺手清理过期与非活跃条目，避免 map 无限增长
    for (auto it = nonces_.begin(); it != nonces_.end();) {
        if (it->second.expires_at <= now) it = nonces_.erase(it);
        else ++it;
    }

    NonceEntry entry;
    entry.expires_at = now + kNonceTtlSeconds;
    nonces_[nonce]   = entry;
    return nonce;
}

bool AuthService::consume_nonce_and_verify(const std::string& device_id,
                                           const std::string& nonce_b64,
                                           const std::string& signature_b64,
                                           const std::string& public_key_b64) {
    // base64 解出的是字节，重新拼成字符串再查表；
    // 不能用 reinterpret_cast 把 string 当 vector 用（那是未定义行为）。
    std::vector<uint8_t> nonce_bytes;
    if (!base64_decode(nonce_b64, nonce_bytes)) return false;
    const std::string nonce(nonce_bytes.begin(), nonce_bytes.end());

    // 先原子地取走并删除，保证 nonce 只能用一次（并发重放会在这里失败）
    {
        std::lock_guard<std::mutex> lock(nonce_mtx_);
        auto it = nonces_.find(nonce);
        if (it == nonces_.end()) return false;
        if (it->second.expires_at <= std::time(nullptr)) {
            nonces_.erase(it);
            return false;
        }
        nonces_.erase(it);
    }

    std::vector<uint8_t> pubkey, signature;
    if (!Identity::from_base64_pubkey(public_key_b64, pubkey)) return false;
    if (!base64_decode(signature_b64, signature)) return false;

    // 签名内容 = 域前缀 + nonce + 设备 ID，绑定用途与设备，防止跨上下文重放
    const std::string message = std::string(pxc::kRegisterSignDomain) + nonce + device_id;
    const std::vector<uint8_t> data(message.begin(), message.end());
    return Identity::verify(pubkey, data, signature);
}

// ---------------------------------------------------------------- 设备入网

AuthStatus AuthService::enroll_device(const std::string& token,
                                                   const std::string& device_id,
                                                   const std::string& public_key_b64,
                                                   const std::string& connection_verifier_hex,
                                                   const std::string& name,
                                                   const std::string& platform,
                                                   store::Device&     out_device) {
    store::Account account;
    if (validate_token(token, account) != AuthStatus::Ok) return AuthStatus::Unauthorized;

    // 设备 ID 必须等于公钥指纹。这一步是设备身份的根，缺了它任何人都能冒充任意设备 ID。
    std::vector<uint8_t> pubkey;
    if (!Identity::from_base64_pubkey(public_key_b64, pubkey)) return AuthStatus::InvalidArgument;

    const std::string normalized_id = Identity::normalize_device_id(device_id);
    if (normalized_id.empty() || normalized_id != Identity::derive_device_id(pubkey)) {
        return AuthStatus::Forbidden;
    }

    const std::string normalized_name =
        name.empty() ? "未命名设备" : name.substr(0, 128);

    store::Device existing;
    const auto      found = db_.device_by_id(normalized_id, existing);
    if (found == store::Result::Ok) {
        // 设备 ID 与公钥绑定：同一物理设备可登记到多个账号名下，
        // 但同一 ID 永远对应同一把公钥（换公钥 = 换设备 = 新 ID）
        if (existing.public_key_b64 != public_key_b64) return AuthStatus::Forbidden;

        // 本账号名下若已吊销该设备，仍拒绝（吊销是账号级别的信任撤销）
        store::Device own_row;
        if (db_.device_of_account(normalized_id, account.id, own_row) ==
                store::Result::Ok &&
            own_row.revoked) {
            return AuthStatus::Forbidden;
        }
    }

    store::Device device;
    device.device_id               = normalized_id;
    device.account_id              = account.id;
    device.name                    = normalized_name;
    device.public_key_b64          = public_key_b64;
    device.connection_verifier_hex = connection_verifier_hex;
    device.platform                = platform;
    device.online                  = false;
    device.revoked                 = false;

    // upsert：新设备插入；同账号重复登记更新；新账号则新增一行归属
    const auto rc = db_.add_device(device);
    switch (rc) {
        case store::Result::Ok:
            break;
        default:
            return AuthStatus::Internal;
    }

    out_device = device;
    return AuthStatus::Ok;
}

AuthStatus AuthService::device_for_account(int64_t            account_id,
                                                       const std::string& device_id,
                                                       store::Device&     out_device) {
    const auto normalized = Identity::normalize_device_id(device_id);

    store::Device device;
    const auto    rc = db_.device_by_id(normalized, device);
    if (rc == store::Result::NotFound) return AuthStatus::NotFound;
    if (rc != store::Result::Ok) return AuthStatus::Internal;

    // 归属检查：不是自己的设备一律当作不存在，避免探测他人设备
    if (device.account_id != account_id) return AuthStatus::NotFound;
    if (device.revoked) return AuthStatus::Forbidden;

    out_device = device;
    return AuthStatus::Ok;
}

// -------------------------------------------------------------------- 设备

AuthStatus AuthService::list_devices(int64_t                     account_id,
                                                  std::vector<store::Device>& out_devices) {
    const auto rc = db_.devices_of_account(account_id, out_devices);
    if (rc != store::Result::Ok) return AuthStatus::Internal;

    // 已撤销的设备不展示给客户端
    out_devices.erase(std::remove_if(out_devices.begin(), out_devices.end(),
                                     [](const store::Device& d) { return d.revoked; }),
                      out_devices.end());
    return AuthStatus::Ok;
}

AuthStatus AuthService::rename_device(int64_t            account_id,
                                                   const std::string& device_id,
                                                   const std::string& new_name) {
    const std::string trimmed = new_name.substr(0, 128);
    if (store::normalize_identifier(trimmed).empty()) return AuthStatus::InvalidArgument;

    const std::string normalized = Identity::normalize_device_id(device_id);
    const auto        rc = db_.rename_device(account_id, normalized, trimmed);
    if (rc == store::Result::NotFound) return AuthStatus::NotFound;
    return rc == store::Result::Ok ? AuthStatus::Ok : AuthStatus::Internal;
}

AuthStatus AuthService::remove_device(int64_t            account_id,
                                                   const std::string& device_id) {
    const std::string normalized = Identity::normalize_device_id(device_id);
    const auto        rc = db_.remove_device(account_id, normalized);
    if (rc == store::Result::NotFound) return AuthStatus::NotFound;
    return rc == store::Result::Ok ? AuthStatus::Ok : AuthStatus::Internal;
}

AuthStatus AuthService::revoke_device(int64_t            account_id,
                                                   const std::string& device_id) {
    const std::string normalized = Identity::normalize_device_id(device_id);
    const auto        rc = db_.revoke_device(account_id, normalized);
    if (rc == store::Result::NotFound) return AuthStatus::NotFound;
    return rc == store::Result::Ok ? AuthStatus::Ok : AuthStatus::Internal;
}

void AuthService::mark_online(const std::string& device_id,
                              const std::string& public_ip,
                              const std::string& platform) {
    const std::string normalized = Identity::normalize_device_id(device_id);
    db_.set_device_online(normalized, true, public_ip, store::now_iso8601(), platform);
}

void AuthService::mark_offline(const std::string& device_id) {
    store::Device device;
    const std::string normalized = Identity::normalize_device_id(device_id);
    if (db_.device_by_id(normalized, device) != store::Result::Ok) return;

    // 下线的保留上一次观测到的公网 IP，只更新在线标记和最近在线时间
    db_.set_device_online(normalized, false, device.last_public_ip, store::now_iso8601(),
                          device.platform);
}

// -------------------------------------------------------------------- 限速

bool AuthService::rate_limit_ok(const std::string& key) {
    std::lock_guard<std::mutex> lock(rate_mtx_);

    auto it = rate_.find(key);
    if (it == rate_.end()) return true;

    const std::time_t now = std::time(nullptr);
    if (now - it->second.window_start >= config_.login_window_seconds) {
        rate_.erase(it);
        return true;
    }
    return it->second.failures < config_.login_max_attempts;
}

void AuthService::rate_limit_note_failure(const std::string& key) {
    std::lock_guard<std::mutex> lock(rate_mtx_);

    const std::time_t now = std::time(nullptr);
    auto&             window = rate_[key];

    if (window.window_start == 0 || now - window.window_start >= config_.login_window_seconds) {
        window.window_start = now;
        window.failures     = 0;
    }
    ++window.failures;
}

void AuthService::rate_limit_reset(const std::string& key) {
    std::lock_guard<std::mutex> lock(rate_mtx_);
    rate_.erase(key);
}

bool AuthService::register_allowed(const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(rate_mtx_);
    auto it = register_rate_.find(client_ip);
    if (it == register_rate_.end()) return true;

    const std::time_t now = std::time(nullptr);
    if (now - it->second.window_start >= config_.register_window_seconds) {
        register_rate_.erase(it);
        return true;
    }
    return it->second.failures < config_.register_max_attempts;
}

void AuthService::note_register_attempt(const std::string& client_ip) {
    std::lock_guard<std::mutex> lock(rate_mtx_);
    const std::time_t now = std::time(nullptr);
    auto& window = register_rate_[client_ip];
    if (window.window_start == 0 ||
        now - window.window_start >= config_.register_window_seconds) {
        window.window_start = now;
        window.failures = 0;
    }
    ++window.failures;
}

}  // namespace pxc::server
