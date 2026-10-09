#pragma once

// 信令服务器的在线设备表。
//
// 服务器在这里承担两件事：
//   1. 维护「谁在线」——连接建立/断开时增删，并向同账号的其他设备广播状态变化
//   2. 校验身份——设备自报的 device_id 必须与其公钥指纹一致，
//      否则任何人都能冒充别人的 ID 去接别人的连接
//
// 注意与 store::Database 的分工：
//   这里是「易失的在线状态」，进程重启即清空；
//   持久事实（账号、设备归属、最近观测 IP）在 store 里。
//
// 账号隔离：peer_list() 只返回同一 account_id 下的设备，
// 广播也只发给同账号，避免跨账号枚举和探测。
//
// 服务器不持有任何会话密钥，也无法解密 P2P 上的内容。

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "pxc/protocol.h"

namespace ix {
class WebSocket;
}

namespace pxc::server {

struct DeviceEntry {
    std::string        device_id;         // 已归一化（无分隔符、大写）
    int64_t            account_id = 0;    // 当前注册所用账号
    std::set<int64_t>  extra_account_ids; // 历史注册过的其他账号（可见性并集）
    std::string        name;
    std::string        pubkey_b64;
    std::string        platform;
    std::string        remote_ip;   // 服务器观测到的连接源地址
    std::string                           conn_id;     // 当前挂载的连接
    std::chrono::steady_clock::time_point connected_at;
};

class DeviceRegistry {
public:
    enum class RegisterResult {
        Ok,
        Replaced,     // 已认证的同一身份接管旧 socket（重连）
        IdMismatch,   // 自报 ID 与公钥指纹对不上，疑似冒充
        Duplicate,    // 该 ID 已有活跃连接
        NoAccount,    // 未通过账号认证就想作为设备上线
    };

    // 连接建立时登记 socket，断开时移除。
    // 保存 weak_ptr 而非裸指针：socket 对象由服务器持有，
    // 连接断开后我们不会悬空解引用。
    void bind_connection(const std::string& conn_id, std::weak_ptr<ix::WebSocket> ws);
    void unbind_connection(const std::string& conn_id);

    // entry.conn_id 由本函数填充，调用方无需（也不应）自己设置
    RegisterResult register_device(DeviceEntry entry, const std::string& conn_id,
                                   std::shared_ptr<ix::WebSocket>* replaced = nullptr);

    // 断开连接时调用。移除了设备则返回其 device_id，否则返回 nullopt。
    std::optional<std::string> unregister_connection(const std::string& conn_id,
                                                    std::set<int64_t>* accounts = nullptr);

    // 向指定设备投递信令。设备不在线或 socket 已失效返回 false。
    bool send_to(const std::string& device_id, const Message& msg);

    // 同一账号下的在线设备（默认排除自己）。
    // 多账号模型：包含「登记到该账号」的所有在线设备。
    std::vector<PeerInfo> peer_list(int64_t account_id, const std::string& except_device_id = "") const;

    // 某设备当前注册所用的账号；未注册返回 nullopt
    std::optional<int64_t> account_of_device(const std::string& device_id) const;

    // 设备的历史登记账号集合（不含当前注册账号）；未注册返回空集
    std::set<int64_t> extra_accounts_of_device(const std::string& device_id) const;

    // 设备对某账号是否可见（当前注册账号或历史登记过的账号）
    bool device_visible_to(const std::string& device_id, int64_t account_id) const;

    // 补记设备的历史账号（多账号可见性并集）
    void add_device_account(const std::string& device_id, int64_t account_id);

    std::optional<std::string> device_of_connection(const std::string& conn_id) const;

    // 连接是否已作为设备上线
    bool is_registered(const std::string& conn_id) const;

    size_t device_count() const;

private:
    mutable std::mutex                                  mtx_;
    std::map<std::string, std::weak_ptr<ix::WebSocket>> conns_;           // conn_id -> socket
    std::map<std::string, DeviceEntry>                  devices_;         // device_id -> entry
    std::map<std::string, std::string>                  conn_to_device_;  // conn_id -> device_id
};

}  // namespace pxc::server
