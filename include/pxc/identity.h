#pragma once

// 设备身份：每个被控端 / 主控端持有一对长期 Ed25519 密钥。
// device_id 是公钥的指纹，不依赖服务器分配，因此无法被服务器伪造或顶替。
// 首次连接时记录对方公钥（TOFU），之后公钥变化即告警。

#include <cstdint>
#include <string>
#include <vector>

namespace pxc {

class Identity {
public:
    // 生成全新密钥对
    static Identity generate();

    // 只读取，不创建。文件缺失或损坏时返回 false 并保持 out 不变。
    // 账号模式下必须用这个：静默重建身份会让服务器把同一台机器当成新设备。
    static bool load(const std::string& path, Identity& out);

    // 从文件读取；不存在则生成并写入（权限 0600）
    static Identity load_or_create(const std::string& path);

    bool valid() const { return !seckey_.empty() && !pubkey_.empty(); }

    // 8 字符短 ID，形如 "7K3M-9QXA"，用于人工口头传达
    const std::string& device_id() const { return device_id_; }

    // base64 编码的公钥，用于在信令中交换
    const std::string& pubkey_b64() const { return pubkey_b64_; }

    const std::vector<uint8_t>& pubkey() const { return pubkey_; }

    // 用私钥签名
    std::vector<uint8_t> sign(const std::vector<uint8_t>& data) const;

    // 校验签名（静态，验签方只有对方公钥）
    static bool verify(const std::vector<uint8_t>& pubkey,
                       const std::vector<uint8_t>& data,
                       const std::vector<uint8_t>& sig);

    // 从 base64 公钥算出 device_id，用于校验对方自报的 ID
    static std::string derive_device_id(const std::vector<uint8_t>& pubkey);

    // 去掉分隔符并统一大写，用于比较两个 device_id 是否相同。
    // 用户口头传达时会带 "-"，程序内部一律用归一化后的形式比较。
    static std::string normalize_device_id(const std::string& id);

    static bool from_base64_pubkey(const std::string& b64, std::vector<uint8_t>& out);

    // 保存到文件
    bool save(const std::string& path) const;

    // ------------------------------------------------------------ 连接密钥
    // 设备连接密钥：认领时生成，展示给用户一次，之后每次连接由主控端输入。
    // 只保存在被控设备本地；服务器只收到它的 HMAC 派生值，拿不到原文。
    const std::string& connection_key() const { return connection_key_; }
    void set_connection_key(const std::string& key) { connection_key_ = key; }

    // 连接密钥的派生值，用于注册时交给服务器（服务器据此审计，但无法反推密钥）
    std::string connection_verifier_hex() const;

    // 本机平台标识（"windows" / "linux" / "macos"），注册时上报
    static std::string current_platform();

private:
    std::vector<uint8_t> pubkey_;
    std::vector<uint8_t> seckey_;
    std::string          device_id_;
    std::string          pubkey_b64_;
    std::string          connection_key_;  // 连接密钥原文，只在被控设备本地
};

// base64 工具（信令里传输公钥和签名）
std::string base64_encode(const std::vector<uint8_t>& data);
bool        base64_decode(const std::string& text, std::vector<uint8_t>& out);

}  // namespace pxc
