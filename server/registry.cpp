#include "registry.h"

#include <ixwebsocket/IXWebSocket.h>

#include "pxc/identity.h"

namespace pxc::server {

void DeviceRegistry::bind_connection(const std::string& conn_id,
                                     std::weak_ptr<ix::WebSocket> ws) {
    std::lock_guard<std::mutex> lock(mtx_);
    conns_[conn_id] = std::move(ws);
}

void DeviceRegistry::unbind_connection(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    conns_.erase(conn_id);
    conn_to_device_.erase(conn_id);
}

DeviceRegistry::RegisterResult DeviceRegistry::register_device(DeviceEntry entry,
                                                               const std::string& conn_id,
                                                               std::shared_ptr<ix::WebSocket>* replaced) {
    std::lock_guard<std::mutex> lock(mtx_);

    entry.conn_id = conn_id;
    // 统一成归一化形式作为主键，避免 "7K3M-9QXA" 和 "7K3M9QXA" 被当成两台设备
    entry.device_id = Identity::normalize_device_id(entry.device_id);

    // 关键校验：自报 ID 必须等于公钥指纹。
    // 没有这一步，任何客户端都能注册成别人的设备 ID 去截获发往对方的连接请求。
    std::vector<uint8_t> pubkey;
    if (!Identity::from_base64_pubkey(entry.pubkey_b64, pubkey)) {
        return RegisterResult::IdMismatch;
    }
    if (entry.device_id != Identity::derive_device_id(pubkey)) {
        return RegisterResult::IdMismatch;
    }

    // 账号模式下，未通过 token 认证的连接不允许作为设备上线。
    // 这挡住了「未登录也能被连接」这条路径。
    if (entry.account_id == 0) return RegisterResult::NoAccount;

    // 调用方已经验证 token 和私钥签名。同一身份重连必须能接管
    // 尚未收到 TCP 关闭通知的旧 socket，且其迟到关闭不能移除新映射。
    bool takeover = false;
    auto it = devices_.find(entry.device_id);
    if (it != devices_.end() && it->second.conn_id != conn_id) {
        if (it->second.pubkey_b64 != entry.pubkey_b64) return RegisterResult::Duplicate;
        takeover = true;
        auto socket = conns_.find(it->second.conn_id);
        if (replaced && socket != conns_.end()) *replaced = socket->second.lock();
        conn_to_device_.erase(it->second.conn_id);
    }

    // 同一连接可能先注册过别的 ID（重连、改名），先清掉旧映射
    auto prev = conn_to_device_.find(conn_id);
    if (prev != conn_to_device_.end() && prev->second != entry.device_id) {
        devices_.erase(prev->second);
    }

    // 多账号可见性：历史登记账号并集随设备延续（重注册/换账号登录不清空）
    auto existing = devices_.find(entry.device_id);
    if (existing != devices_.end()) {
        if (existing->second.account_id != entry.account_id) {
            entry.extra_account_ids.insert(existing->second.account_id);
        }
        entry.extra_account_ids.insert(existing->second.extra_account_ids.begin(),
                                       existing->second.extra_account_ids.end());
    }

    devices_[entry.device_id] = entry;
    conn_to_device_[conn_id]  = entry.device_id;
    return takeover ? RegisterResult::Replaced : RegisterResult::Ok;
}

std::optional<std::string> DeviceRegistry::unregister_connection(const std::string& conn_id,
                                                               std::set<int64_t>* accounts) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = conn_to_device_.find(conn_id);
    if (it == conn_to_device_.end()) return std::nullopt;

    const std::string device_id = it->second;
    conn_to_device_.erase(it);
    auto device = devices_.find(device_id);
    if (device == devices_.end() || device->second.conn_id != conn_id) return std::nullopt;
    if (accounts) {
        *accounts = device->second.extra_account_ids;
        accounts->insert(device->second.account_id);
    }
    devices_.erase(device);
    return device_id;
}

bool DeviceRegistry::send_to(const std::string& device_id, const Message& msg) {
    std::shared_ptr<ix::WebSocket> ws;
    {
        std::lock_guard<std::mutex> lock(mtx_);

        auto dit = devices_.find(Identity::normalize_device_id(device_id));
        if (dit == devices_.end()) return false;

        auto cit = conns_.find(dit->second.conn_id);
        if (cit == conns_.end()) return false;

        ws = cit->second.lock();
    }

    // 在锁外发送：send 可能阻塞，不能占着注册表的锁
    if (!ws || ws->getReadyState() != ix::ReadyState::Open) return false;
    return ws->send(encode(msg)).success;
}

std::vector<PeerInfo> DeviceRegistry::peer_list(int64_t            account_id,
                                                const std::string& except_device_id) const {
    std::lock_guard<std::mutex> lock(mtx_);

    const std::string except = Identity::normalize_device_id(except_device_id);

    std::vector<PeerInfo> out;
    for (const auto& kv : devices_) {
        if (kv.first == except) continue;

        // 多账号模型：设备对「登记过该账号」的所有账号可见。
        // 可见性 = 当前注册账号 ∪ 历史登记账号，未登记过的账号看不到。
        const DeviceEntry& entry = kv.second;
        const bool visible =
            entry.account_id == account_id ||
            entry.extra_account_ids.count(account_id) > 0;
        if (!visible) continue;

        PeerInfo info;
        info.device_id = entry.device_id;
        info.name      = entry.name;
        info.online    = true;
        info.public_ip = entry.remote_ip;  // 服务器观测到的连接源地址
        info.platform  = entry.platform;
        out.push_back(std::move(info));
    }
    return out;
}

bool DeviceRegistry::device_visible_to(const std::string& device_id,
                                       int64_t            account_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = devices_.find(Identity::normalize_device_id(device_id));
    if (it == devices_.end()) return false;
    return it->second.account_id == account_id ||
           it->second.extra_account_ids.count(account_id) > 0;
}

std::set<int64_t> DeviceRegistry::extra_accounts_of_device(
    const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = devices_.find(Identity::normalize_device_id(device_id));
    if (it == devices_.end()) return {};
    return it->second.extra_account_ids;
}

void DeviceRegistry::add_device_account(const std::string& device_id,
                                        int64_t            account_id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = devices_.find(Identity::normalize_device_id(device_id));
    if (it == devices_.end()) return;
    if (it->second.account_id != account_id) {
        it->second.extra_account_ids.insert(account_id);
    }
}

std::optional<int64_t> DeviceRegistry::account_of_device(const std::string& device_id) const {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = devices_.find(Identity::normalize_device_id(device_id));
    if (it == devices_.end()) return std::nullopt;
    return it->second.account_id;
}

std::optional<std::string> DeviceRegistry::device_of_connection(const std::string& conn_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = conn_to_device_.find(conn_id);
    if (it == conn_to_device_.end()) return std::nullopt;
    return it->second;
}

bool DeviceRegistry::is_registered(const std::string& conn_id) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return conn_to_device_.find(conn_id) != conn_to_device_.end();
}

size_t DeviceRegistry::device_count() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return devices_.size();
}

}  // namespace pxc::server
