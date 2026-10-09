#pragma once

// 信令协议：主控端 / 被控端 <-> 信令服务器之间交换的 JSON 消息。
//
// 设计约束：信令服务器带宽极低，所以这里只走控制面（谁在线、谁来连谁、
// SDP/ICE 交换）。真正的画面和键鼠数据永远不经过服务器。
//
// 账号相关的注册/登录/设备认领走独立的 HTTP API（见 account_api.h），
// 账号密码永远不进入本协议。本协议只携带短期 access token。

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace pxc {

// ------------------------------------------------------------------ 消息类型
// 客户端 -> 服务器
inline constexpr const char* kRegister        = "register";         // 注册上线
inline constexpr const char* kHeartbeat       = "heartbeat";        // 保活
inline constexpr const char* kListPeers       = "list_peers";       // 拉取设备列表
inline constexpr const char* kConnectRequest  = "connect_request";  // 请求连接某设备
inline constexpr const char* kConnectResponse = "connect_response"; // 被控端答复
inline constexpr const char* kSdp             = "sdp";              // SDP offer/answer
inline constexpr const char* kCandidate       = "candidate";        // ICE candidate
inline constexpr const char* kBye             = "bye";              // 结束会话

// 账号模式下设备上线是两步：auth 拿到一次性挑战，再用设备私钥签名提交 register。
inline constexpr const char* kAuth            = "auth";             // 携带 access token
inline constexpr const char* kAuthChallenge   = "auth_challenge";   // 服务器 -> 客户端：签名挑战

// 服务器 -> 客户端
inline constexpr const char* kRegistered  = "registered";    // 注册成功，带设备列表
inline constexpr const char* kAuthOk      = "auth_ok";       // 账号认证通过（未作为设备上线）
inline constexpr const char* kPeerOnline  = "peer_online";
inline constexpr const char* kPeerOffline = "peer_offline";
inline constexpr const char* kPeerList    = "peer_list";
inline constexpr const char* kIncoming    = "incoming";      // 有人请求连你
inline constexpr const char* kAnswered    = "answered";      // 对方答复了你的请求
inline constexpr const char* kError       = "error";

// 信令错误码
inline constexpr const char* kErrBadMessage    = "bad_message";
inline constexpr const char* kErrNotRegistered = "not_registered";
inline constexpr const char* kErrNoSuchPeer    = "no_such_peer";
inline constexpr const char* kErrPeerOffline   = "peer_offline";
inline constexpr const char* kErrBusy          = "peer_busy";
inline constexpr const char* kErrDuplicateId   = "duplicate_id";
inline constexpr const char* kErrUnauthorized  = "unauthorized";   // token 无效/过期
inline constexpr const char* kErrForbidden     = "forbidden";      // 设备不属于该账号 / 签名不对
inline constexpr const char* kErrRateLimited   = "rate_limited";

// 设备上线签名所用的域分隔前缀。签名内容 = 前缀 + nonce + device_id，
// 防止同一私钥签过的其他内容被拿来冒充上线证明。
inline constexpr const char* kRegisterSignDomain = "pxc-device-register-v1";

// ------------------------------------------------------------------ 设备信息
struct PeerInfo {
    std::string device_id;
    std::string name;
    bool        online = true;
    // 以下字段只在账号模式下由服务器填写
    std::string public_ip;  // 服务器观测到的该设备信令连接源地址（不保证可直连）
    std::string last_seen;  // ISO-8601 UTC
    std::string platform;   // "windows" / "linux" 等，展示用
};

inline void to_json(nlohmann::json& j, const PeerInfo& p) {
    j = nlohmann::json{{"device_id", p.device_id}, {"name", p.name}, {"online", p.online}};
    if (!p.public_ip.empty()) j["public_ip"] = p.public_ip;
    if (!p.last_seen.empty()) j["last_seen"] = p.last_seen;
    if (!p.platform.empty()) j["platform"] = p.platform;
}

inline void from_json(const nlohmann::json& j, PeerInfo& p) {
    j.at("device_id").get_to(p.device_id);
    j.at("name").get_to(p.name);
    p.online    = j.value("online", true);
    p.public_ip = j.value("public_ip", "");
    p.last_seen = j.value("last_seen", "");
    p.platform  = j.value("platform", "");
}

// ------------------------------------------------------------------ 消息体
// 一个扁平结构覆盖所有消息类型。字段按 type 取用，未用到的保持空。
// 信令消息量极小，不需要为每种类型单独建模。
struct Message {
    std::string type;

    // 身份
    std::string device_id;   // 发送方自己的 ID（register 时使用）
    std::string name;        // 人类可读的设备名
    std::string pubkey;      // base64 编码的 Ed25519 公钥

    // 账号模式
    std::string access_token;  // auth 消息携带；服务器从不回显
    std::string nonce;         // auth_challenge 下发的一次性挑战（base64）
    std::string signature;     // register 时对挑战的 Ed25519 签名（base64）
    std::string account;       // auth_ok 回显的用户名，展示用
    std::string platform;      // "windows" / "linux" / "macos"，展示用
    // 连接密钥的 HMAC 派生值（hex）。服务器只存这个，永远拿不到连接密钥原文。
    std::string connection_verifier;

    // 路由
    std::string from;        // 服务器填充：消息来源
    std::string target;      // 消息目标设备

    // SDP / ICE
    std::string sdp;         // SDP 内容
    std::string sdp_type;    // "offer" | "answer"
    std::string candidate;   // ICE candidate 字符串
    std::string mid;         // candidate 所属的 media id

    // 会话控制
    bool accept = false;     // connect_response 用

    // 设备列表
    std::vector<PeerInfo> peers;

    // 错误
    std::string code;
    std::string detail;
};

inline void to_json(nlohmann::json& j, const Message& m) {
    j = nlohmann::json{{"type", m.type}};
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) j[k] = v;
    };
    put("device_id", m.device_id);
    put("name", m.name);
    put("pubkey", m.pubkey);
    put("access_token", m.access_token);
    put("nonce", m.nonce);
    put("signature", m.signature);
    put("account", m.account);
    put("platform", m.platform);
    put("connection_verifier", m.connection_verifier);
    put("from", m.from);
    put("target", m.target);
    put("sdp", m.sdp);
    put("sdp_type", m.sdp_type);
    put("candidate", m.candidate);
    put("mid", m.mid);
    put("code", m.code);
    put("detail", m.detail);
    if (m.accept) j["accept"] = true;
    if (!m.peers.empty()) j["peers"] = m.peers;
}

inline void from_json(const nlohmann::json& j, Message& m) {
    m.type         = j.value("type", "");
    m.device_id    = j.value("device_id", "");
    m.name         = j.value("name", "");
    m.pubkey       = j.value("pubkey", "");
    m.access_token = j.value("access_token", "");
    m.nonce        = j.value("nonce", "");
    m.signature    = j.value("signature", "");
    m.account      = j.value("account", "");
    m.platform     = j.value("platform", "");
    m.connection_verifier = j.value("connection_verifier", "");
    m.from         = j.value("from", "");
    m.target       = j.value("target", "");
    m.sdp          = j.value("sdp", "");
    m.sdp_type     = j.value("sdp_type", "");
    m.candidate    = j.value("candidate", "");
    m.mid          = j.value("mid", "");
    m.code         = j.value("code", "");
    m.detail       = j.value("detail", "");
    m.accept       = j.value("accept", false);
    if (j.contains("peers")) m.peers = j.at("peers").get<std::vector<PeerInfo>>();
}

// 序列化 / 反序列化。解析失败返回 false，不抛异常给调用方。
std::string encode(const Message& m);
bool        decode(const std::string& text, Message& out);

}  // namespace pxc
