#pragma once

// 会话连接认证：证明主控端知道目标设备的连接密钥。
//
// 对应需求「每次连接都要输入那台机器的私钥」。真正的设备私钥（Ed25519）
// 只用于设备向服务器证明身份，不能外传；主控端每次输入的是设备在
// 「添加本机」时生成的**连接密钥**，两者是独立的两个秘密。
//
// 为什么要在 ch-control 上做，而不是交给服务器校验：
//   - 服务器只保存连接密钥的派生值，拿不到原文，也就不可能代替用户连接
//   - 校验发生在已经建立的 WebRTC 加密通道里，口令原文不出任何一端
//   - 证明绑定本次 P2P 的 DTLS 证书指纹，信令服务器即使被攻破也无法
//     把连接转发给它自己（它的证书指纹不同，证明就对不上）
//
// 流程（ch-control 上的文本消息）：
//   设备 -> 主控   PXC_AUTH_CHALLENGE <nonce_hex>
//   主控 -> 设备   PXC_AUTH_RESPONSE <nonce_hex> <proof_hex>
//   设备 -> 主控   PXC_AUTH_OK | PXC_AUTH_FAIL
//
// 其中 proof = HMAC-SHA256(key = 归一化连接密钥, message = 域前缀 | nonce | 指纹)
// 设备校验通过前，不应接受任何键鼠、画面或剪贴板数据。

#include <string>

namespace pxc {

inline constexpr const char* kSessionAuthChallenge = "PXC_AUTH_CHALLENGE";
inline constexpr const char* kSessionAuthResponse  = "PXC_AUTH_RESPONSE";
inline constexpr const char* kSessionAuthOk        = "PXC_AUTH_OK";
inline constexpr const char* kSessionAuthFail      = "PXC_AUTH_FAIL";

// 域前缀，避免连接密钥的派生结果被用在其他协议语境里
inline constexpr const char* kSessionAuthDomain = "pxc-session-auth-v1";

// 挑战 nonce 的长度（字节）。128 bit 足够抵抗重放窗口内的猜测。
inline constexpr size_t kSessionNonceBytes = 16;

struct SessionAuthResult {
    bool        parsed = false;   // 消息格式是否符合预期
    bool        ok = false;       // 证明是否正确
    std::string nonce_hex;
    std::string proof_hex;
};

// 生成一次性挑战（hex）
std::string make_session_nonce();

// 被签名的消息串。两端必须完全一致。
std::string session_auth_message(const std::string& nonce_hex,
                                 const std::string& dtls_fingerprint);

// 主控端用它把用户输入的连接密钥变成证明。
// 连接密钥会先做归一化（去分隔符、纠正易混字符），所以用户抄写时少些挫败。
std::string compute_session_proof(const std::string& connection_key,
                                  const std::string& nonce_hex,
                                  const std::string& dtls_fingerprint);

// 设备端用它校验主控端的证明。常数时间比较，避免逐字节时序侧信道。
bool verify_session_proof(const std::string& connection_key,
                          const std::string& nonce_hex,
                          const std::string& dtls_fingerprint,
                          const std::string& proof_hex);

// 解析 "PXC_AUTH_RESPONSE <nonce> <proof>" 形式的控制消息
SessionAuthResult parse_session_auth_response(const std::string& message);
bool              parse_session_auth_challenge(const std::string& message,
                                               std::string&       out_nonce_hex);

}  // namespace pxc
