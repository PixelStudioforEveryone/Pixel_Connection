#pragma once
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocketHttpHeaders.h>
#include <string>

namespace pxc::server {
inline bool loopback_address(const std::string& ip) {
    return ip == "127.0.0.1" || ip == "::1" || ip == "::ffff:127.0.0.1";
}
inline std::string proxy_client_ip(const std::string& peer,
                                  const ix::WebSocketHttpHeaders& headers, bool trust) {
    if (!trust || !loopback_address(peer)) return peer;
    const auto it = headers.find("X-PXC-Client-IP");
    if (it == headers.end() || it->second.size() > 64) return peer;
    unsigned char address[16];
    if (ix::inet_pton(AF_INET, it->second.c_str(), address) == 1 ||
        ix::inet_pton(AF_INET6, it->second.c_str(), address) == 1) return it->second;
    return peer;
}
}
