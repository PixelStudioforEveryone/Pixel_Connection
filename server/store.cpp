#include "store.h"

#include <algorithm>
#include <cctype>
#include <ctime>

#include <sqlite3.h>

namespace pxc::store {
namespace {

// RAII 语句封装，避免每条路径都要记得 finalize
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            stmt_ = nullptr;
        }
    }
    ~Stmt() {
        if (stmt_) sqlite3_finalize(stmt_);
    }
    Stmt(const Stmt&)            = delete;
    Stmt& operator=(const Stmt&) = delete;

    bool          ok() const { return stmt_ != nullptr; }
    sqlite3_stmt* get() const { return stmt_; }

    bool bind_text(int idx, const std::string& value) {
        return sqlite3_bind_text(stmt_, idx, value.c_str(),
                                 static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
    }
    bool bind_int64(int idx, int64_t value) {
        return sqlite3_bind_int64(stmt_, idx, value) == SQLITE_OK;
    }

    bool step_done() { return sqlite3_step(stmt_) == SQLITE_DONE; }

    std::string column_text(int idx) const {
        const unsigned char* p = sqlite3_column_text(stmt_, idx);
        return p ? reinterpret_cast<const char*>(p) : std::string();
    }
    int64_t column_int64(int idx) const { return sqlite3_column_int64(stmt_, idx); }
    int     column_int(int idx) const { return sqlite3_column_int(stmt_, idx); }

private:
    sqlite3_stmt* stmt_ = nullptr;
};

std::string utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm     tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

// 时间戳格式统一为 ISO-8601 UTC（"2026-10-03T12:00:00Z"），
// 字典序即时间序，可以直接用字符串比较判断过期。
bool is_future(const std::string& iso) {
    return iso > utc_now();
}

}  // namespace

// 用户名/邮箱规范化：去首尾空白 + 转小写。
// 存一份原始值用于展示，存一份 norm 用于唯一性和登录匹配，
// 避免 "Alice" 和 "alice" 被当成两个账号。
std::string normalize_identifier(const std::string& value) {
    size_t begin = 0;
    size_t end   = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;

    std::string out = value.substr(begin, end - begin);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string now_iso8601() {
    return utc_now();
}

std::string iso_after_seconds(int seconds) {
    std::time_t t = std::time(nullptr) + seconds;
    std::tm     tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

Database::~Database() {
    close();
}

void Database::close() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::exec_locked(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        last_error_ = err ? err : "sqlite error";
        if (err) sqlite3_free(err);
        return false;
    }
    return true;
}

bool Database::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        last_error_ = db_ ? sqlite3_errmsg(db_) : "sqlite3_open failed";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }

    // WAL 提升并发读性能；busy_timeout 让并发写自动等待而不是立刻报错；
    // 外键约束默认是关闭的，必须显式打开，否则删除账号不会级联清理设备。
    if (!exec_locked("PRAGMA journal_mode=WAL;") ||
        !exec_locked("PRAGMA synchronous=NORMAL;") ||
        !exec_locked("PRAGMA foreign_keys=ON;") ||
        !exec_locked("PRAGMA busy_timeout=5000;")) {
        return false;
    }

    return migrate_locked();
}

bool Database::migrate_locked() {
    const char* schema = R"SQL(
CREATE TABLE IF NOT EXISTS accounts (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT NOT NULL,
    username_norm TEXT NOT NULL UNIQUE,
    email         TEXT NOT NULL,
    email_norm    TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    created_at    TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS sessions (
    token_hash TEXT PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    expires_at TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_account ON sessions(account_id);

CREATE TABLE IF NOT EXISTS devices (
    device_id           TEXT NOT NULL,
    account_id          INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    name                TEXT NOT NULL,
    public_key_b64      TEXT NOT NULL,
    connection_verifier TEXT NOT NULL,
    last_public_ip      TEXT NOT NULL DEFAULT '',
    last_seen           TEXT NOT NULL DEFAULT '',
    platform            TEXT NOT NULL DEFAULT '',
    online              INTEGER NOT NULL DEFAULT 0,
    revoked             INTEGER NOT NULL DEFAULT 0,
    created_at          TEXT NOT NULL,
    PRIMARY KEY (device_id, account_id)
);
CREATE INDEX IF NOT EXISTS idx_devices_account ON devices(account_id);

CREATE TABLE IF NOT EXISTS claims (
    code_hash  TEXT PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    expires_at TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_claims_account ON claims(account_id);
CREATE TABLE IF NOT EXISTS server_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
INSERT OR IGNORE INTO server_metadata(key,value) VALUES ('server_id',lower(hex(randomblob(16))));
)SQL";

    if (!exec_locked(schema)) return false;

    // 迁移：旧结构 devices 以 device_id 单列为主键（单账号模型），
    // 新结构为 (device_id, account_id) 复合主键（多账号模型）。
    // 检测「主键列总数 == 1」区分旧结构（新结构主键列数为 2）。
    {
        Stmt check(db_, "SELECT COUNT(*) FROM pragma_table_info('devices') "
                        "WHERE pk > 0");
        if (!check.ok()) return false;
        const int rc = sqlite3_step(check.get());
        const int pk_cols = (rc == SQLITE_ROW) ? check.column_int(0) : 0;
        if (pk_cols == 1) {
            const char* migration =
                "ALTER TABLE devices RENAME TO devices_legacy;"
                "CREATE TABLE devices (\n"
                "    device_id           TEXT NOT NULL,\n"
                "    account_id          INTEGER NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,\n"
                "    name                TEXT NOT NULL,\n"
                "    public_key_b64      TEXT NOT NULL,\n"
                "    connection_verifier TEXT NOT NULL,\n"
                "    last_public_ip      TEXT NOT NULL DEFAULT '',\n"
                "    last_seen           TEXT NOT NULL DEFAULT '',\n"
                "    platform            TEXT NOT NULL DEFAULT '',\n"
                "    online              INTEGER NOT NULL DEFAULT 0,\n"
                "    revoked             INTEGER NOT NULL DEFAULT 0,\n"
                "    created_at          TEXT NOT NULL,\n"
                "    PRIMARY KEY (device_id, account_id)\n"
                ");"
                "CREATE INDEX IF NOT EXISTS idx_devices_account ON devices(account_id);"
                "INSERT OR REPLACE INTO devices "
                "    (device_id, account_id, name, public_key_b64, connection_verifier,"
                "     last_public_ip, last_seen, platform, online, revoked, created_at) "
                "SELECT device_id, account_id, name, public_key_b64, connection_verifier,"
                "     last_public_ip, last_seen, platform, online, revoked, created_at "
                "FROM devices_legacy;"
                "DROP TABLE devices_legacy;";
            if (!exec_locked(migration)) return false;
        }
    }

    return exec_locked(schema);
}

std::string Database::server_id() {
    std::lock_guard<std::mutex> lock(mtx_);
    Stmt query(db_, "SELECT value FROM server_metadata WHERE key='server_id'");
    return query.ok() && sqlite3_step(query.get()) == SQLITE_ROW ? query.column_text(0) : std::string();
}

// ---------------------------------------------------------------------- 账号

Result Database::create_account(const std::string& username,
                                          const std::string& email,
                                          const std::string& password_hash,
                                          int64_t&           out_account_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    const std::string username_norm = normalize_identifier(username);
    const std::string email_norm    = normalize_identifier(email);
    if (username_norm.empty() || email_norm.empty()) return Result::InvalidArgument;

    Stmt stmt(db_,
              "INSERT INTO accounts (username, username_norm, email, email_norm, password_hash, created_at) "
              "VALUES (?, ?, ?, ?, ?, ?);");
    if (!stmt.ok()) return Result::Internal;

    stmt.bind_text(1, username);
    stmt.bind_text(2, username_norm);
    stmt.bind_text(3, email);
    stmt.bind_text(4, email_norm);
    stmt.bind_text(5, password_hash);
    stmt.bind_text(6, utc_now());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_CONSTRAINT) return Result::Duplicate;
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return Result::Internal;
    }

    out_account_id = sqlite3_last_insert_rowid(db_);
    return Result::Ok;
}

Result Database::find_account_by_identifier(const std::string& identifier,
                                                      Account&           out_account,
                                                      std::string&       out_password_hash) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_,
              "SELECT id, username, email, password_hash, created_at FROM accounts "
              "WHERE username_norm = ? OR email_norm = ? LIMIT 1;");
    if (!stmt.ok()) return Result::Internal;

    const std::string norm = normalize_identifier(identifier);
    stmt.bind_text(1, norm);
    stmt.bind_text(2, norm);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Result::Unauthorized;
    if (rc != SQLITE_ROW) {
        last_error_ = sqlite3_errmsg(db_);
        return Result::Internal;
    }

    out_account.id         = stmt.column_int64(0);
    out_account.username   = stmt.column_text(1);
    out_account.email      = stmt.column_text(2);
    out_password_hash      = stmt.column_text(3);
    out_account.created_at = stmt.column_text(4);
    return Result::Ok;
}

Result Database::account_by_id(int64_t account_id, Account& out_account) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_, "SELECT id, username, email, created_at FROM accounts WHERE id = ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_int64(1, account_id);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Result::NotFound;
    if (rc != SQLITE_ROW) return Result::Internal;

    out_account.id         = stmt.column_int64(0);
    out_account.username   = stmt.column_text(1);
    out_account.email      = stmt.column_text(2);
    out_account.created_at = stmt.column_text(3);
    return Result::Ok;
}

// ------------------------------------------------------------------ 会话

Result Database::create_session(int64_t            account_id,
                                          const std::string& token_hash,
                                          const std::string& expires_at) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_,
              "INSERT INTO sessions (token_hash, account_id, expires_at, created_at) VALUES (?, ?, ?, ?);");
    if (!stmt.ok()) return Result::Internal;

    stmt.bind_text(1, token_hash);
    stmt.bind_int64(2, account_id);
    stmt.bind_text(3, expires_at);
    stmt.bind_text(4, utc_now());

    return stmt.step_done() ? Result::Ok : Result::Internal;
}

Result Database::validate_session(const std::string& token_hash,
                                            Account&           out_account,
                                            std::string&       out_expires_at) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_,
              "SELECT a.id, a.username, a.email, a.created_at, s.expires_at "
              "FROM sessions s JOIN accounts a ON a.id = s.account_id "
              "WHERE s.token_hash = ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, token_hash);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Result::Unauthorized;
    if (rc != SQLITE_ROW) return Result::Internal;

    out_account.id         = stmt.column_int64(0);
    out_account.username   = stmt.column_text(1);
    out_account.email      = stmt.column_text(2);
    out_account.created_at = stmt.column_text(3);
    out_expires_at         = stmt.column_text(4);

    if (!is_future(out_expires_at)) return Result::Unauthorized;
    return Result::Ok;
}

Result Database::delete_session(const std::string& token_hash) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_, "DELETE FROM sessions WHERE token_hash = ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, token_hash);
    return stmt.step_done() ? Result::Ok : Result::Internal;
}

Result Database::delete_expired_sessions(const std::string& now_iso) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_, "DELETE FROM sessions WHERE expires_at <= ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, now_iso);
    return stmt.step_done() ? Result::Ok : Result::Internal;
}

// ------------------------------------------------------------------ 设备

Result Database::add_device(const Device& device) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;
    if (device.device_id.empty() || device.public_key_b64.empty()) return Result::InvalidArgument;

    // upsert：同设备同账号重复登记则更新（重装客户端），新账号则新增登记行
    Stmt stmt(db_,
              "INSERT OR REPLACE INTO devices (device_id, account_id, name, public_key_b64, connection_verifier, "
              "last_public_ip, last_seen, platform, online, revoked, created_at) "
              "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
    if (!stmt.ok()) return Result::Internal;

    stmt.bind_text(1, device.device_id);
    stmt.bind_int64(2, device.account_id);
    stmt.bind_text(3, device.name);
    stmt.bind_text(4, device.public_key_b64);
    stmt.bind_text(5, device.connection_verifier_hex);
    stmt.bind_text(6, device.last_public_ip);
    stmt.bind_text(7, device.last_seen);
    stmt.bind_text(8, device.platform);
    sqlite3_bind_int(stmt.get(), 9, device.online ? 1 : 0);
    sqlite3_bind_int(stmt.get(), 10, device.revoked ? 1 : 0);
    stmt.bind_text(11, utc_now());

    const int rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE) {
        last_error_ = sqlite3_errmsg(db_);
        return Result::Internal;
    }
    return Result::Ok;
}

Result Database::device_of_account(const std::string& device_id,
                                   int64_t            account_id,
                                   Device&            out_device) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_,
              "SELECT name, public_key_b64, connection_verifier, last_public_ip, "
              "last_seen, platform, online, revoked, created_at "
              "FROM devices WHERE device_id = ? AND account_id = ?;");
    if (!stmt.ok()) return Result::Internal;

    stmt.bind_text(1, device_id);
    stmt.bind_int64(2, account_id);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Result::NotFound;
    if (rc != SQLITE_ROW) return Result::Internal;

    out_device.device_id              = device_id;
    out_device.account_id             = stmt.column_int64(1);
    out_device.name                   = stmt.column_text(0);
    out_device.public_key_b64         = stmt.column_text(1);
    out_device.connection_verifier_hex = stmt.column_text(2);
    out_device.last_public_ip         = stmt.column_text(3);
    out_device.last_seen              = stmt.column_text(4);
    out_device.platform               = stmt.column_text(5);
    out_device.online                 = stmt.column_int(6) != 0;
    out_device.revoked                = stmt.column_int(7) != 0;
    out_device.created_at             = stmt.column_text(8);
    return Result::Ok;
}

Result Database::device_by_id(const std::string& device_id, Device& out_device) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_,
              "SELECT device_id, account_id, name, public_key_b64, connection_verifier, "
              "last_public_ip, last_seen, platform, online, revoked, created_at "
              "FROM devices WHERE device_id = ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, device_id);

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return Result::NotFound;
    if (rc != SQLITE_ROW) return Result::Internal;

    out_device.device_id              = stmt.column_text(0);
    out_device.account_id             = stmt.column_int64(1);
    out_device.name                   = stmt.column_text(2);
    out_device.public_key_b64         = stmt.column_text(3);
    out_device.connection_verifier_hex = stmt.column_text(4);
    out_device.last_public_ip         = stmt.column_text(5);
    out_device.last_seen              = stmt.column_text(6);
    out_device.platform               = stmt.column_text(7);
    out_device.online                 = stmt.column_int(8) != 0;
    out_device.revoked                = stmt.column_int(9) != 0;
    out_device.created_at             = stmt.column_text(10);
    return Result::Ok;
}

Result Database::devices_of_account(int64_t account_id, std::vector<Device>& out_devices) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_,
              "SELECT device_id, account_id, name, public_key_b64, connection_verifier, "
              "last_public_ip, last_seen, platform, online, revoked, created_at "
              "FROM devices WHERE account_id = ? ORDER BY name;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_int64(1, account_id);

    out_devices.clear();
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return Result::Internal;

        Device d;
        d.device_id               = stmt.column_text(0);
        d.account_id              = stmt.column_int64(1);
        d.name                    = stmt.column_text(2);
        d.public_key_b64          = stmt.column_text(3);
        d.connection_verifier_hex = stmt.column_text(4);
        d.last_public_ip          = stmt.column_text(5);
        d.last_seen               = stmt.column_text(6);
        d.platform                = stmt.column_text(7);
        d.online                  = stmt.column_int(8) != 0;
        d.revoked                 = stmt.column_int(9) != 0;
        d.created_at              = stmt.column_text(10);
        out_devices.push_back(std::move(d));
    }
    return Result::Ok;
}

Result Database::set_device_online(const std::string& device_id,
                                             bool               online,
                                             const std::string& public_ip,
                                             const std::string& last_seen,
                                             const std::string& platform) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    // public_ip 只由服务器用连接对端地址填入，不接受客户端上报
    Stmt stmt(db_,
              "UPDATE devices SET online = ?, last_public_ip = ?, last_seen = ?, platform = ? "
              "WHERE device_id = ?;");
    if (!stmt.ok()) return Result::Internal;

    sqlite3_bind_int(stmt.get(), 1, online ? 1 : 0);
    stmt.bind_text(2, public_ip);
    stmt.bind_text(3, last_seen);
    stmt.bind_text(4, platform);
    stmt.bind_text(5, device_id);

    if (!stmt.step_done()) return Result::Internal;
    return sqlite3_changes(db_) > 0 ? Result::Ok : Result::NotFound;
}

Result Database::rename_device(int64_t            account_id,
                                         const std::string& device_id,
                                         const std::string& new_name) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;
    if (normalize_identifier(new_name).empty()) return Result::InvalidArgument;

    // 带上 account_id 做条件，确保只能改自己账号下的设备
    Stmt stmt(db_, "UPDATE devices SET name = ? WHERE device_id = ? AND account_id = ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, new_name);
    stmt.bind_text(2, device_id);
    stmt.bind_int64(3, account_id);

    if (!stmt.step_done()) return Result::Internal;
    return sqlite3_changes(db_) > 0 ? Result::Ok : Result::NotFound;
}

Result Database::remove_device(int64_t account_id, const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_, "DELETE FROM devices WHERE device_id = ? AND account_id = ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, device_id);
    stmt.bind_int64(2, account_id);

    if (!stmt.step_done()) return Result::Internal;
    return sqlite3_changes(db_) > 0 ? Result::Ok : Result::NotFound;
}

Result Database::revoke_device(int64_t account_id, const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    // 撤销不删记录：公钥仍然占位，旧签名立即失效，但设备无法被他人重新认领
    Stmt stmt(db_,
              "UPDATE devices SET revoked = 1, online = 0 WHERE device_id = ? AND account_id = ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, device_id);
    stmt.bind_int64(2, account_id);

    if (!stmt.step_done()) return Result::Internal;
    return sqlite3_changes(db_) > 0 ? Result::Ok : Result::NotFound;
}

// ------------------------------------------------------------------ 认领码

Result Database::create_claim(int64_t            account_id,
                                        const std::string& code_hash,
                                        const std::string& expires_at) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_,
              "INSERT INTO claims (code_hash, account_id, expires_at, created_at) VALUES (?, ?, ?, ?);");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, code_hash);
    stmt.bind_int64(2, account_id);
    stmt.bind_text(3, expires_at);
    stmt.bind_text(4, utc_now());

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_CONSTRAINT) return Result::Duplicate;
    return rc == SQLITE_DONE ? Result::Ok : Result::Internal;
}

Result Database::consume_claim(const std::string& code_hash, int64_t& out_account_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    // 先查后删，中间不放开锁；一次性消费由主键删除保证原子性
    {
        Stmt stmt(db_, "SELECT account_id, expires_at FROM claims WHERE code_hash = ?;");
        if (!stmt.ok()) return Result::Internal;
        stmt.bind_text(1, code_hash);

        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) return Result::NotFound;
        if (rc != SQLITE_ROW) return Result::Internal;

        out_account_id       = stmt.column_int64(0);
        const std::string exp = stmt.column_text(1);
        if (!is_future(exp)) {
            // 过期即删除，避免残留
            Stmt del(db_, "DELETE FROM claims WHERE code_hash = ?;");
            if (del.ok()) {
                del.bind_text(1, code_hash);
                del.step_done();
            }
            return Result::NotFound;
        }
    }

    Stmt del(db_, "DELETE FROM claims WHERE code_hash = ?;");
    if (!del.ok()) return Result::Internal;
    del.bind_text(1, code_hash);
    if (!del.step_done()) return Result::Internal;
    if (sqlite3_changes(db_) == 0) return Result::NotFound;  // 已被并发消费

    return Result::Ok;
}

Result Database::delete_expired_claims(const std::string& now_iso) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!db_) return Result::Internal;

    Stmt stmt(db_, "DELETE FROM claims WHERE expires_at <= ?;");
    if (!stmt.ok()) return Result::Internal;
    stmt.bind_text(1, now_iso);
    return stmt.step_done() ? Result::Ok : Result::Internal;
}

}  // namespace pxc::store
