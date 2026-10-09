#pragma once

// 账号体系与连接授权共用的密码学工具。全部基于 OpenSSL，不引入额外依赖。
//
//   - 账号密码：scrypt 慢哈希（PHC 风格字符串，自带参数和盐，便于以后调参）
//   - 会话 token：32 字节随机数；服务器只存 SHA-256，泄库也拿不到可用 token
//   - 设备连接密钥：被控端认领时生成、主控端每次连接时输入的高熵口令
//   - 连接授权：在已建立的 P2P 加密通道上做 HMAC 挑战应答，口令原文从不出本机

#include <cstdint>
#include <string>
#include <vector>

namespace pxc::crypto {

// 密码学安全随机数
std::vector<uint8_t> random_bytes(size_t n);

// 小写十六进制
std::string to_hex(const std::vector<uint8_t>& data);
bool        from_hex(const std::string& hex, std::vector<uint8_t>& out);

std::vector<uint8_t> sha256(const std::string& data);
std::string          sha256_hex(const std::string& data);

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key, const std::string& data);

// 常数时间比较：避免逐字节提前返回造成的时序侧信道
bool constant_time_equal(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);
bool constant_time_equal(const std::string& a, const std::string& b);

// ----------------------------------------------------------------- 账号密码
// 返回形如 "$scrypt$N=32768,r=8,p=1$<salt_b64>$<hash_b64>" 的字符串。
// 参数名是 N（代价，2 的幂），不是 PHC 习惯的 ln。
// 校验时会强制：N 是 [2^10, 2^20] 内的 2 的幂、r 与 p 在 [1,32]、
// 盐长 16 字节、哈希长 32 字节，任何不符一律判为失败。
std::string hash_password(const std::string& password);
bool        verify_password(const std::string& password, const std::string& encoded);

// ---------------------------------------------------------------- 会话 token
// 64 位十六进制随机串（256 bit），交给客户端
std::string generate_token();

// ------------------------------------------------------------ 设备连接密钥
// 形如 "7KQ2M9-XA3PTF-W8RD6H-NC4B2V"：24 个 Crockford base32 字符 = 120 bit 熵，
// 分 4 组、每组 6 字符，便于人工抄录。
std::string generate_connection_key();

// 去掉分隔符、统一大写、把易混字符（O->0, I/L->1）纠正后再比较，
// 用户手输时少一点挫败感。
std::string normalize_connection_key(const std::string& key);

// 由连接密钥派生 HMAC 校验用的密钥材料。被控端只保存这个派生值，
// 不保存口令原文；主控端用用户输入的口令现算。
std::vector<uint8_t> derive_connection_verifier(const std::string& connection_key,
                                                const std::string& device_id);

}  // namespace pxc::crypto
