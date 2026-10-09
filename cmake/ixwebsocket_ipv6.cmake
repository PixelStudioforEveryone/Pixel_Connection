# IXWebSocket's IPv6 listener otherwise also claims IPv4 on Linux, colliding
# with the separate IPv4 API/signaling sockets. Keep family bindings independent.
set(_pxc_ix_socket "${ixwebsocket_SOURCE_DIR}/ixwebsocket/IXSocketServer.cpp")
file(READ "${_pxc_ix_socket}" _pxc_ix_source)
if(NOT _pxc_ix_source MATCHES "IPV6_V6ONLY")
    set(_pxc_ix_anchor "            struct sockaddr_in6 server;")
    string(FIND "${_pxc_ix_source}" "${_pxc_ix_anchor}" _pxc_ix_pos)
    if(_pxc_ix_pos EQUAL -1)
        message(FATAL_ERROR "IXWebSocket IPv6 listener changed: review the V6ONLY patch")
    endif()
    set(_pxc_ix_patch [=[            int ipv6Only = 1;
            if (setsockopt(_serverFd, IPPROTO_IPV6, IPV6_V6ONLY,
                           reinterpret_cast<const char*>(&ipv6Only), sizeof(ipv6Only)) < 0)
            {
                Socket::closeSocket(_serverFd);
                _serverFd = -1;
                return std::make_pair(false, "Cannot configure IPv6-only listener");
            }
]=])
    string(REPLACE "${_pxc_ix_anchor}" "${_pxc_ix_patch}${_pxc_ix_anchor}" _pxc_ix_source "${_pxc_ix_source}")
    file(WRITE "${_pxc_ix_socket}" "${_pxc_ix_source}")
endif()
