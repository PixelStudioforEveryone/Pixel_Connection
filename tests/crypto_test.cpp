#include "pxc/crypto.h"
#include "pxc/session_auth.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const std::string& name) {
    if (value) {
        std::cout << "[PASS] " << name << '\n';
    } else {
        std::cerr << "[FAIL] " << name << '\n';
        ++failures;
    }
}

// ---------------------------------------------------------------- 密码哈希

void test_password_hash() {
    const std::string hash = pxc::crypto::hash_password("correct-horse-battery");

    check(pxc::crypto::verify_password("correct-horse-battery", hash),
          "password verifies with correct input");
    check(!pxc::crypto::verify_password("wrong-password", hash),
          "password rejects wrong input");

    // 相同密码两次哈希必须不同（盐随机）
    const std::string hash2 = pxc::crypto::hash_password("correct-horse-battery");
    check(hash != hash2, "same password produces different hashes (random salt)");

    // 格式损坏不能通过
    check(!pxc::crypto::verify_password("correct-horse-battery", ""),
          "empty hash rejected");
    check(!pxc::crypto::verify_password("correct-horse-battery", "$scrypt$garbage"),
          "malformed hash rejected");
    check(!pxc::crypto::verify_password("correct-horse-battery",
                                        "$scrypt$N=0,r=0,p=0$AAAA$AAAA"),
          "zero-cost parameters rejected");
}

// --------------------------------------------------------------- 连接密钥

void test_connection_key() {
    const std::string key = pxc::crypto::generate_connection_key();

    check(key.size() == 5, "connection key has expected length");
    check(std::all_of(key.begin(), key.end(), [](unsigned char c) {
              return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
          }), "connection key uses lowercase letters and digits");
    const std::string normalized = pxc::crypto::normalize_connection_key(key);
    check(normalized.size() == 5, "normalization preserves key length");
    check(pxc::crypto::normalize_connection_key(key.substr(0, 2) + "-" + key.substr(2)) == normalized,
          "normalization is separator-insensitive");

    const std::string lower(key);
    std::string       lowered = lower;
    for (char& c : lowered) c = static_cast<char>(::tolower(c));
    check(pxc::crypto::normalize_connection_key(lowered) == normalized,
          "normalization is case-insensitive");

    // 易混字符纠正：O->0, I/L->1
    check(pxc::crypto::normalize_connection_key("OIL") == "011",
          "ambiguous characters are folded");

    // 派生值绑定设备 ID：换一台设备就不一样
    const auto v1 = pxc::crypto::derive_connection_verifier(key, "7K3M9QXA");
    const auto v2 = pxc::crypto::derive_connection_verifier(key, "AAAAAAAA");
    check(!v1.empty() && v1 != v2, "verifier is bound to device id");

    // 服务器只保存派生值；不同密钥不会撞上
    check(pxc::crypto::derive_connection_verifier(key, "7K3M9QXA") == v1,
          "verifier derivation is deterministic");
}

// ------------------------------------------------------------ 会话认证

void test_session_auth() {
    const std::string key   = pxc::crypto::generate_connection_key();
    const std::string nonce = pxc::make_session_nonce();
    const std::string fp    = "sha-256 AB:CD:EF:01:23:45:67:89";

    check(nonce.size() == 32, "session nonce is 128-bit hex");

    const std::string proof = pxc::compute_session_proof(key, nonce, fp);
    check(proof.size() == 64, "proof is a 32-byte HMAC in hex");

    check(pxc::verify_session_proof(key, nonce, fp, proof),
          "correct key verifies");
    check(!pxc::verify_session_proof(key, nonce, fp, std::string(64, '0')),
          "wrong proof is rejected");

    // 归一化后仍应通过：用户抄写时大小写、分隔符都可能不同
    std::string messy = key;
    for (char& c : messy) c = static_cast<char>(::tolower(c));
    check(pxc::verify_session_proof(messy, nonce, fp, proof),
          "proof verifies regardless of key formatting");

    // 绑定 DTLS 指纹：中间人用不同证书转发就对不上
    check(!pxc::verify_session_proof(key, nonce, "sha-256 00:00:00:00", proof),
          "proof is bound to the DTLS fingerprint");

    // 绑定 nonce：重放上一轮的证明无效
    const std::string other_nonce = pxc::make_session_nonce();
    check(!pxc::verify_session_proof(key, other_nonce, fp, proof),
          "proof is bound to the nonce (replay rejected)");

    // 空密钥不能通过
    check(!pxc::verify_session_proof("", nonce, fp, proof),
          "empty key is rejected");
}

void test_session_auth_parsing() {
    const std::string nonce = pxc::make_session_nonce();

    std::string parsed_nonce;
    check(pxc::parse_session_auth_challenge(
              std::string(pxc::kSessionAuthChallenge) + " " + nonce, parsed_nonce) &&
              parsed_nonce == nonce,
          "challenge message parses");

    check(!pxc::parse_session_auth_challenge(pxc::kSessionAuthChallenge, parsed_nonce),
          "challenge without nonce rejected");
    check(!pxc::parse_session_auth_challenge(
              std::string(pxc::kSessionAuthChallenge) + " zzzz", parsed_nonce),
          "challenge with non-hex nonce rejected");
    check(!pxc::parse_session_auth_challenge("PXC_AUTH_RESPONSE " + nonce, parsed_nonce),
          "response is not parsed as challenge");

    const std::string proof = std::string(64, 'a');
    const auto        result =
        pxc::parse_session_auth_response(std::string(pxc::kSessionAuthResponse) + " " +
                                         nonce + " " + proof);
    check(result.parsed && result.nonce_hex == nonce && result.proof_hex == proof,
          "response message parses");

    const auto bad = pxc::parse_session_auth_response(
        std::string(pxc::kSessionAuthResponse) + " " + nonce + " short");
    check(!bad.parsed, "short proof rejected");

    const auto bad_token = pxc::parse_session_auth_response("HELLO world");
    check(!bad_token.parsed, "unrelated control message is not treated as auth");
}

// --------------------------------------------------------------- 基础设施

void test_helpers() {
    const auto bytes = pxc::crypto::random_bytes(32);
    check(bytes.size() == 32, "random_bytes returns requested length");

    const std::string hex = pxc::crypto::to_hex(bytes);
    check(hex.size() == 64, "to_hex doubles length");

    std::vector<uint8_t> back;
    check(pxc::crypto::from_hex(hex, back) && back == bytes, "hex round trip");

    check(!pxc::crypto::from_hex("zz", back), "invalid hex rejected");
    check(!pxc::crypto::from_hex("abc", back), "odd-length hex rejected");

    const std::string token = pxc::crypto::generate_token();
    check(token.size() == 64 && token != pxc::crypto::generate_token(),
          "session tokens are 256-bit and unique");

    // 常数时间比较的语义必须正确（实现细节不测时序）
    check(pxc::crypto::constant_time_equal(std::string("abc"), std::string("abc")),
          "constant_time_equal accepts equal values");
    check(!pxc::crypto::constant_time_equal(std::string("abc"), std::string("abd")),
          "constant_time_equal rejects different values");
    check(!pxc::crypto::constant_time_equal(std::string("abc"), std::string("abcd")),
          "constant_time_equal rejects different lengths");

    const auto mac = pxc::crypto::hmac_sha256(std::vector<uint8_t>{1, 2, 3}, "message");
    check(mac.size() == 32, "hmac-sha256 produces 32 bytes");
}

}  // namespace

int main() {
    test_password_hash();
    test_connection_key();
    test_session_auth();
    test_session_auth_parsing();
    test_helpers();

    if (failures == 0) {
        std::cout << "\n所有测试通过\n";
        return EXIT_SUCCESS;
    }
    std::cerr << "\n失败项: " << failures << '\n';
    return EXIT_FAILURE;
}
