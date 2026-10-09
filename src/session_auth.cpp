#include "pxc/session_auth.h"

#include <sstream>
#include <vector>

#include "pxc/crypto.h"

namespace pxc {
namespace {

// 从控制消息里切出参数
std::vector<std::string> split_tokens(const std::string& message) {
    std::istringstream       iss(message);
    std::vector<std::string> tokens;
    std::string              token;
    while (iss >> token) tokens.push_back(token);
    return tokens;
}

bool is_hex(const std::string& value, size_t expected_len) {
    if (value.size() != expected_len) return false;
    for (char c : value) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        const bool upper = c >= 'A' && c <= 'F';
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

}  // namespace

std::string make_session_nonce() {
    return crypto::to_hex(crypto::random_bytes(kSessionNonceBytes));
}

std::string session_auth_message(const std::string& nonce_hex,
                                 const std::string& dtls_fingerprint) {
    // 绑定 nonce（防重放）和本次 P2P 的 DTLS 指纹（防中间人转发）
    return std::string(kSessionAuthDomain) + "|" + nonce_hex + "|" + dtls_fingerprint;
}

std::string compute_session_proof(const std::string& connection_key,
                                  const std::string& nonce_hex,
                                  const std::string& dtls_fingerprint) {
    // 归一化后再派生：用户手输的连接密钥大小写、分隔符、易混字符都不影响结果
    const std::string normalized = crypto::normalize_connection_key(connection_key);
    if (normalized.empty()) return {};

    const std::string message = session_auth_message(nonce_hex, dtls_fingerprint);
    const auto        proof   = crypto::hmac_sha256(
        std::vector<uint8_t>(normalized.begin(), normalized.end()), message);
    return crypto::to_hex(proof);
}

bool verify_session_proof(const std::string& connection_key,
                          const std::string& nonce_hex,
                          const std::string& dtls_fingerprint,
                          const std::string& proof_hex) {
    const std::string expected =
        compute_session_proof(connection_key, nonce_hex, dtls_fingerprint);
    if (expected.empty() || proof_hex.empty()) return false;

    // 常数时间比较，避免通过响应时间逐字节推断证明
    return crypto::constant_time_equal(expected, proof_hex);
}

bool parse_session_auth_challenge(const std::string& message, std::string& out_nonce_hex) {
    const auto tokens = split_tokens(message);
    if (tokens.size() != 2) return false;
    if (tokens[0] != kSessionAuthChallenge) return false;
    if (!is_hex(tokens[1], kSessionNonceBytes * 2)) return false;

    out_nonce_hex = tokens[1];
    return true;
}

SessionAuthResult parse_session_auth_response(const std::string& message) {
    SessionAuthResult result;

    const auto tokens = split_tokens(message);
    if (tokens.size() != 3 || tokens[0] != kSessionAuthResponse) return result;
    if (!is_hex(tokens[1], kSessionNonceBytes * 2)) return result;
    if (!is_hex(tokens[2], 64)) return result;  // HMAC-SHA256 是 32 字节

    result.parsed   = true;
    result.nonce_hex = tokens[1];
    result.proof_hex = tokens[2];
    return result;
}

}  // namespace pxc
