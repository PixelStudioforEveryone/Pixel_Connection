#include "pxc/crypto.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "pxc/identity.h"  // 复用 base64_encode / base64_decode

namespace pxc::crypto {
namespace {

// scrypt 参数：N=2^15, r=8, p=1 —— 单次约 32 MB 内存、几十毫秒，
// 用于登录足够慢，对交互无明显影响。参数随哈希一起存储，后续可上调。
constexpr uint64_t kScryptN      = 1u << 15;
constexpr uint64_t kScryptR      = 8;
constexpr uint64_t kScryptP      = 1;
constexpr size_t   kScryptKeyLen = 32;
constexpr size_t   kScryptSaltLen = 16;
constexpr uint64_t kScryptMaxMem = 128ull * 1024 * 1024;

std::vector<uint8_t> scrypt(const std::string& password,
                            const std::vector<uint8_t>& salt,
                            uint64_t n, uint64_t r, uint64_t p, size_t keylen) {
    std::vector<uint8_t> out(keylen);
    if (EVP_PBE_scrypt(password.data(), password.size(),
                       salt.data(), salt.size(),
                       n, r, p, kScryptMaxMem,
                       out.data(), out.size()) != 1) {
        throw std::runtime_error("scrypt failed");
    }
    return out;
}

// Crockford base32：去掉 I/L/O/U，避免人工抄录时的歧义
const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

}  // namespace

std::vector<uint8_t> random_bytes(size_t n) {
    std::vector<uint8_t> out(n);
    if (n > 0 && RAND_bytes(out.data(), static_cast<int>(n)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
    return out;
}

std::string to_hex(const std::vector<uint8_t>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (uint8_t b : data) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
    }
    return out;
}

bool from_hex(const std::string& hex, std::vector<uint8_t>& out) {
    out.clear();
    if (hex.size() % 2 != 0) return false;
    out.reserve(hex.size() / 2);

    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::vector<uint8_t> sha256(const std::string& data) {
    std::vector<uint8_t> digest(EVP_MAX_MD_SIZE);
    unsigned int len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    const bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1 &&
                    EVP_DigestUpdate(ctx, data.data(), data.size()) == 1 &&
                    EVP_DigestFinal_ex(ctx, digest.data(), &len) == 1;
    EVP_MD_CTX_free(ctx);
    if (!ok) throw std::runtime_error("sha256 failed");

    digest.resize(len);
    return digest;
}

std::string sha256_hex(const std::string& data) {
    return to_hex(sha256(data));
}

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key, const std::string& data) {
    if (key.empty()) throw std::runtime_error("hmac: empty key");

    std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
    unsigned int len = 0;

    const unsigned char* result = HMAC(EVP_sha256(),
                                       key.data(), static_cast<int>(key.size()),
                                       reinterpret_cast<const unsigned char*>(data.data()),
                                       data.size(),
                                       out.data(), &len);
    if (!result) throw std::runtime_error("hmac failed");

    out.resize(len);
    return out;
}

bool constant_time_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    // 先比长度再比内容；长度不同不算敏感信息
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

bool constant_time_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    if (a.empty()) return true;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// ----------------------------------------------------------------- 账号密码

std::string hash_password(const std::string& password) {
    const auto salt = random_bytes(kScryptSaltLen);
    const auto hash = scrypt(password, salt, kScryptN, kScryptR, kScryptP, kScryptKeyLen);

    return "$scrypt$N=" + std::to_string(kScryptN) +
           ",r=" + std::to_string(kScryptR) +
           ",p=" + std::to_string(kScryptP) + "$" +
           base64_encode(salt) + "$" + base64_encode(hash);
}

bool verify_password(const std::string& password, const std::string& encoded) {
    if (encoded.rfind("$scrypt$", 0) != 0) return false;

    // 格式：$scrypt$N=<n>,r=<r>,p=<p>$<salt_b64>$<hash_b64>
    const size_t params_end = encoded.find('$', 8);
    if (params_end == std::string::npos) return false;
    const size_t salt_end = encoded.find('$', params_end + 1);
    if (salt_end == std::string::npos) return false;

    const std::string params   = encoded.substr(8, params_end - 8);
    const std::string salt_b64 = encoded.substr(params_end + 1, salt_end - params_end - 1);
    const std::string hash_b64 = encoded.substr(salt_end + 1);

    // 严格解析参数。不能用 find + strtoull 的松散写法：
    //   - find("N=") 是子串匹配，"N=32768junk" 会被接受，跨字段也可能误匹配
    //   - strtoull 不检查 endptr/errno，超范围会静默变成 ULLONG_MAX
    // 这里要求三段恰为 N、r、p，且每段都被完整消费。
    uint64_t n = 0, r = 0, p = 0;
    {
        std::vector<std::string> fields;
        size_t                   begin = 0;
        while (true) {
            const size_t comma = params.find(',', begin);
            if (comma == std::string::npos) {
                fields.push_back(params.substr(begin));
                break;
            }
            fields.push_back(params.substr(begin, comma - begin));
            begin = comma + 1;
        }
        if (fields.size() != 3) return false;

        auto parse_field = [](const std::string& field, const char* key, uint64_t& dst) -> bool {
            const std::string prefix = std::string(key) + "=";
            if (field.rfind(prefix, 0) != 0) return false;

            const std::string digits = field.substr(prefix.size());
            if (digits.empty()) return false;
            for (char c : digits) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            }

            errno = 0;
            char*      end   = nullptr;
            const auto value = std::strtoull(digits.c_str(), &end, 10);
            if (errno == ERANGE) return false;
            if (end == nullptr || *end != '\0') return false;
            if (value == 0) return false;

            dst = value;
            return true;
        };

        if (!parse_field(fields[0], "N", n) ||
            !parse_field(fields[1], "r", r) ||
            !parse_field(fields[2], "p", p)) {
            return false;
        }
    }

    // 参数上下界：过小的 N 让哈希形同虚设，过大的 N 是可以触发的 DoS
    if (n < (1u << 10) || n > (1u << 20)) return false;
    if ((n & (n - 1)) != 0) return false;  // 必须是 2 的幂
    if (r < 1 || r > 32) return false;
    if (p < 1 || p > 32) return false;

    // 盐和哈希长度必须与生成时一致。
    // 不校验的话，"$scrypt$...$$<b64>" 这种空盐记录会被接受，
    // 于是所有账号共用同一个盐，相同密码产生相同凭证；
    // 而超短哈希记录会把口令空间压缩到可暴力穷举的程度。
    std::vector<uint8_t> salt, expected;
    if (!base64_decode(salt_b64, salt) || !base64_decode(hash_b64, expected)) return false;
    if (salt.size() != kScryptSaltLen) return false;
    if (expected.size() != kScryptKeyLen) return false;

    // 内存上限：128 * N * r 必须落在 kScryptMaxMem 之内，否则 OpenSSL 会直接失败
    if (128ull * n * r > kScryptMaxMem) return false;

    std::vector<uint8_t> actual;
    try {
        actual = scrypt(password, salt, n, r, p, expected.size());
    } catch (const std::exception&) {
        return false;
    }
    return constant_time_equal(actual, expected);
}

// ---------------------------------------------------------------- 会话 token

std::string generate_token() {
    return to_hex(random_bytes(32));
}

// ------------------------------------------------------------ 设备连接密钥

std::string generate_connection_key() {
    // 5 位小写字母数字（用户指定）：日常局域网/受控网络场景下便于口述抄写。
    // 熵约 25.8 bit，比旧版 24 位 base32 短——安全要求更高时可在
    // 「设置-安全-自定义验证码」改成更长的自定义密码，校验逻辑完全兼容。
    static const char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    constexpr int kKeyLength = 5;
    constexpr int kAlphabetSize = 36;

    // 拒绝采样避免取模偏差：256 % 36 = 4，丢弃 >= 252 的字节
    std::string out;
    out.reserve(kKeyLength);
    while (out.size() < kKeyLength) {
        const auto raw = random_bytes(1);
        const uint8_t byte = raw[0];
        if (byte >= 252) continue;
        out.push_back(kAlphabet[byte % kAlphabetSize]);
    }
    return out;
}

std::string normalize_connection_key(const std::string& key) {
    std::string out;
    out.reserve(key.size());
    for (char c : key) {
        const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (up == '-' || up == ' ' || up == '_' || up == '\t') continue;
        // Crockford 把易混字符映射到数字，用户写错也能对上
        if (up == 'O') { out.push_back('0'); continue; }
        if (up == 'I' || up == 'L') { out.push_back('1'); continue; }
        out.push_back(up);
    }
    return out;
}

std::vector<uint8_t> derive_connection_verifier(const std::string& connection_key,
                                               const std::string& device_id) {
    // 绑定 device_id，防止一台设备的连接密钥被拿到另一台上复用
    const std::string material = std::string("pxc-conn-v1|") + device_id;
    const std::string normalized = normalize_connection_key(connection_key);
    return hmac_sha256(std::vector<uint8_t>(normalized.begin(), normalized.end()), material);
}

}  // namespace pxc::crypto
