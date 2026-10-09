# Building and hosting a PixelConnection server

[简体中文](SERVER_BUILD.md) · **English** · [Project overview](../README_EN.md)

PixelConnection is free and open source. You can run the account and signaling services on your own Windows / Linux PC or cloud server. You pay for any hosting, domain and relay traffic. HarmonyOS currently runs as a client.

## Choose a deployment

| Deployment | Where accounts / signaling run | Client addresses | TURN |
|---|---|---|---|
| Same LAN | A Windows or Linux PC | That PC's LAN IP and ports | Usually unnecessary |
| Full cloud deployment | Your public server | Your HTTPS / WSS domain | Recommended for peers behind NAT |
| Campus network / no inbound access | LAN backend with a cloud gateway and SSH reverse tunnel | The cloud HTTPS / WSS domain | Recommended for peers behind NAT |

The account database belongs to the backend, not to a client URL. A new backend has a separate account database. To preserve existing accounts, migrate the database or keep that backend behind a gateway. Keep a recoverable backup before moving it.

## Linux headless build

Ubuntu / Debian:

```bash
sudo apt update
sudo apt install -y git cmake build-essential pkg-config libssl-dev zlib1g-dev
git clone https://github.com/sxd15963949546/Pixel_Connection.git
cd Pixel_Connection
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPXC_BUILD_QT_CLIENT=OFF -DPXC_BUILD_STANDALONE_SERVER=ON
cmake --build build --target pxc-server -j"$(nproc)"
```

For Alibaba Cloud Linux / RHEL-based distributions, the corresponding packages are generally `git cmake gcc gcc-c++ make openssl-devel zlib-devel`. Use a supported C++17 toolchain.

The executable is `build/server/pxc-server`. Initial CMake configuration fetches libdatachannel, IXWebSocket, nlohmann/json and SQLite sources and requires access to GitHub and sqlite.org. The headless server needs no Qt, desktop session or capture permission.

## Windows headless build

Install Visual Studio 2022 with Desktop development with C++, CMake, Git, and x64 OpenSSL / zlib development files compatible with the toolchain. Run these commands in a developer PowerShell and adjust the library paths:

```powershell
git clone https://github.com/sxd15963949546/Pixel_Connection.git
cd Pixel_Connection
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DPXC_BUILD_QT_CLIENT=OFF -DPXC_BUILD_STANDALONE_SERVER=ON `
  -DOPENSSL_ROOT_DIR="C:/OpenSSL-Win64" `
  -DCMAKE_PREFIX_PATH="C:/Libraries/zlib"
cmake --build build --config Release --target pxc-server --parallel
```

The executable is `build\server\Release\pxc-server.exe`. Deploy any required OpenSSL / zlib runtime DLLs with it. An existing Qt client can run the same backend as `pxc-client.exe --server`. Do not start two backend instances on the same ports or database.

## Local network

The simplest option is to start the local server from the PC login screen and copy the shared addresses. Alternatively, start the standalone executable:

```bash
./build/server/pxc-server --db ./accounts.db --api-port 29910 --port 9910 \
  --v4 0.0.0.0 --v6 :: \
  --advertise-api http://192.168.1.10:29910 \
  --advertise-ws ws://192.168.1.10:9910
```

Replace `192.168.1.10` with the host's actual LAN IP. The two `--advertise-*` options must be supplied together. They describe the shareable addresses and do not change the listeners.

Allow TCP **29910** and **9910** from the intended network. For Ubuntu UFW, replace the example subnet with your own:

```bash
sudo ufw allow from 192.168.1.0/24 to any port 29910 proto tcp
sudo ufw allow from 192.168.1.0/24 to any port 9910 proto tcp
```

On Windows, create equivalent scoped inbound rules in Defender Firewall. Also check upstream network policies. On the other clients, choose an existing server and enter the host's API / signaling URLs, then register and sign in to the same account.

`127.0.0.1` points to the current device. `0.0.0.0` is a listener wildcard. Neither is a shareable server address. Keep the server host running. Use HTTP / WS only on a trusted LAN; use HTTPS / WSS over the internet.

## Full public deployment

In the examples below, **`remote.example.com` is a placeholder** for your own domain, already pointing to your server.

### Backend service

After building on Linux:

```bash
sudo useradd --system --user-group --home-dir /var/lib/pixelconnection \
  --create-home --shell /usr/sbin/nologin pixelconnection
sudo install -d -m 700 -o pixelconnection -g pixelconnection /var/lib/pixelconnection
sudo install -d -m 755 /opt/pixelconnection/server
sudo install -m 755 build/server/pxc-server /opt/pixelconnection/server/pxc-server
sudo cp deploy/pxc-server.service.example /etc/systemd/system/pxc-server.service
sudoedit /etc/systemd/system/pxc-server.service
sudo systemctl daemon-reload
sudo systemctl enable --now pxc-server
curl --fail http://127.0.0.1:29910/api/v1/health
```

Edit the service to advertise your own API / signaling domain and keep a fixed database path. The template binds to `127.0.0.1` / `::1`; public requests should go through the reverse proxy. Use `--trust-loopback-proxy` only with the loopback listeners and a gateway that overwrites source headers.

### HTTPS / WSS gateway

Install Caddy using its official instructions. Point DNS A to the public IPv4 address. Add AAAA only if public IPv6 is reachable. Allow TCP **443** and TCP **80** for certificate validation; keep backend ports 29910 / 9910 on loopback.

To validate and run the supplied gateway configuration in the foreground:

```bash
export PXC_DOMAIN=remote.example.com
caddy validate --config deploy/Caddyfile.example --adapter caddyfile
caddy run --config deploy/Caddyfile.example --adapter caddyfile
```

For a persistent service, follow [Caddy's service instructions](https://caddyserver.com/docs/running), copy the configuration and set `PXC_DOMAIN` in the service environment. The template proxies `/api/*` to the account backend and `/signal` to signaling, limits request body size and overwrites source headers.

Check the public endpoint:

```bash
curl --fail https://remote.example.com/api/v1/health
```

Clients use API `https://remote.example.com` and signaling `wss://remote.example.com/signal`. Register on this backend and sign in to the same account on every client. Use a valid certificate chain and hostname; do not disable TLS verification to work around certificate errors.

### TURN UDP relay

On Ubuntu, install coturn with `sudo apt install coturn`. Start from [the supplied template](../deploy/turnserver.conf.example) and generate your own strong random password:

```bash
openssl rand -hex 32
sudo install -m 600 deploy/turnserver.conf.example /etc/turnserver.conf
sudoedit /etc/turnserver.conf
```

Replace the `realm` and `user` placeholders. If the cloud host is behind public-to-private IPv4 NAT, configure `external-ip=PUBLIC_IPV4/PRIVATE_IPV4` and use the actual listening / relay addresses. Allow only root and the actual coturn service account to read the credential file; for example, `root:turnserver` with mode `0640`, adjusting the group for the distribution.

Allow **UDP 3478** and **UDP 49160–49200** in both the cloud security group and the host firewall. Enable the distribution's coturn service and check its status, logs and bound ports. Avoid duplicate instances.

In each client's network settings, enter:

```text
TURN:     turn://remote.example.com:3478
Username: your own TURN username
Password: your generated random password
```

TURN currently supports **UDP only**. TCP / TLS relaying is not implemented; networks that entirely block UDP can still prevent a session. Allocation quotas do not replace a bandwidth / traffic budget.

## Campus backend with a cloud gateway

If the LAN host cannot receive public inbound connections, keep the backend and its account database on the LAN host and create an outbound SSH reverse tunnel to the cloud gateway:

```bash
ssh -NT -o ExitOnForwardFailure=yes -o ServerAliveInterval=20 \
  -R 127.0.0.1:32910:127.0.0.1:29910 \
  -R 127.0.0.1:39910:127.0.0.1:9910 \
  tunnel-user@YOUR_CLOUD_HOST
```

Generate your own SSH keys and restricted tunnel account. The LAN backend listens on loopback. Change the cloud Caddy upstreams to ports **32910** and **39910**; run coturn on the cloud server.

Restrict the tunnel account to these two loopback remote forwards and disable shell, TTY, agent and X11 forwarding. Verify and pin the cloud host key. Keep the private key on the LAN host.

Use systemd to keep the tunnel running. The LAN host must remain powered on in this arrangement; a backend or tunnel failure can produce gateway HTTP 502 errors. Moving the full backend and its database to the cloud removes that dependency.

## IPv6, backups and verification

- Bracket literal IPv6 addresses in URLs, for example `http://[2001:db8::10]:29910`. This is a documentation address. A dual-stack gateway helps cover both IPv4-only and IPv6-capable clients.
- The health endpoint reports a stable server ID, name, operating system and readiness. The same ID means the same account database; loopback, LAN and public URLs can be different routes to it.
- Back up SQLite with its online backup API, or stop the backend first. Do not copy only the `.db` file while ignoring a live WAL. Preserve a recoverable backup before migration and restart with the same account database.
- Verify in order: HTTPS health → client login → WSS online state → direct / TURN connection → video, input, reconnect, clipboard and bidirectional files.
- If login works but no video appears, inspect ICE / TURN and peer availability. API reachability alone does not establish that the remote desktop path works.

See also: [Caddy reverse proxy](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy), [coturn configuration example](https://github.com/coturn/coturn/blob/master/examples/etc/turnserver.conf), [OpenSSH manuals](https://www.openssh.com/manual.html), and [the project's security notes (Chinese)](../SECURITY.md).
