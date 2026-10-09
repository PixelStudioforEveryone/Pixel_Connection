# 服务器构建与部署

PixelConnection 免费开源，可在自己的电脑或云服务器运行。服务器资源、域名和公网中继流量由部署者承担。Windows／Linux 均可运行账号与信令；HarmonyOS 当前只作为客户端。

## 1. 选择部署模式

| 模式 | 账号／信令在哪里运行 | 客户端填写 | TURN |
|---|---|---|---|
| 同一内网 | 一台 Windows 或 Linux | 该主机的 LAN IP 与端口 | 通常无需 |
| 云端完整服务器 | 公网云服务器 | 自己的 HTTPS／WSS 域名 | 跨 NAT 建议部署 |
| 校园网／无入站地址 | 内网后端＋云端网关，SSH 反向隧道 | 云端 HTTPS／WSS 域名 | 跨 NAT 建议部署 |

账号数据库属于运行后端的机器。新建服务器使用独立账号库，账号不会自动跨服务器同步。保留原账号需要迁移原数据库，或保留原后端并使用网关。

## 2. Linux 无界面服务器构建

Ubuntu／Debian：

```bash
sudo apt update
sudo apt install -y git cmake build-essential pkg-config libssl-dev zlib1g-dev
git clone https://github.com/sxd15963949546/Pixel_Connection.git
cd Pixel_Connection
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPXC_BUILD_QT_CLIENT=OFF -DPXC_BUILD_STANDALONE_SERVER=ON
cmake --build build --target pxc-server -j"$(nproc)"
```

Alibaba Cloud Linux／RHEL 系发行版所需包通常为 `git cmake gcc gcc-c++ make openssl-devel zlib-devel`；使用发行版支持的 C++17 工具链。

产物为 `build/server/pxc-server`。首次配置由 CMake 下载 libdatachannel、IXWebSocket、nlohmann/json 与 SQLite 源码，需要访问 GitHub 和 sqlite.org。服务器不需要 Qt、桌面会话或屏幕采集权限。

## 3. Windows 无界面服务器构建

安装 Visual Studio 2022 的“使用 C++ 的桌面开发”、CMake、Git、匹配 x64 工具链的 OpenSSL 和 zlib 开发文件。在开发者 PowerShell 中运行，路径按自己的安装调整：

```powershell
git clone https://github.com/sxd15963949546/Pixel_Connection.git
cd Pixel_Connection
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DPXC_BUILD_QT_CLIENT=OFF -DPXC_BUILD_STANDALONE_SERVER=ON `
  -DOPENSSL_ROOT_DIR="C:/OpenSSL-Win64" `
  -DCMAKE_PREFIX_PATH="C:/Libraries/zlib"
cmake --build build --config Release --target pxc-server --parallel
```

产物为 `build\server\Release\pxc-server.exe`。使用动态 OpenSSL／zlib 时需一并部署相应运行库。已有 Qt 客户端可以直接使用 `pxc-client.exe --server`；它和独立服务器共用实现，不要同时占用同一端口或数据库。

## 4. 内网服务器

最方便的方式是在 PC 客户端登录页选择“使用本机服务器”，启动并复制分享窗口的地址。也可运行独立服务器：

```bash
./build/server/pxc-server --db ./accounts.db --api-port 29910 --port 9910 \
  --v4 0.0.0.0 --v6 :: \
  --advertise-api http://192.168.1.10:29910 \
  --advertise-ws ws://192.168.1.10:9910
```

替换示例 IP 为真实 LAN IP。`--advertise-api` 和 `--advertise-ws` 必须成对填写，用于设置页复制分享，不改变监听地址。

允许客户端来源访问 TCP 29910／9910。Ubuntu UFW 示例，网段按实际网络调整：

```bash
sudo ufw allow from 192.168.1.0/24 to any port 29910 proto tcp
sudo ufw allow from 192.168.1.0/24 to any port 9910 proto tcp
```

Windows 可在 Defender 防火墙为这两个 TCP 端口建立限制到需要网段的入站规则。也需检查上级网络策略。HTTP／WS 不加密账号链路，仅适用于可信内网；公网使用 HTTPS／WSS。

其他设备选择“连接已有服务器”，填写 `http://192.168.1.10:29910` 和 `ws://192.168.1.10:9910`。`127.0.0.1` 是当前设备，`0.0.0.0` 是监听通配地址，都不能作为其他设备的服务器地址。

## 5. 公网完整部署

下方 `remote.example.com` 表示自己的、已解析到服务器的域名，不能直接照抄。

### 后端与 systemd

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

编辑 unit，将两条公开地址改为自己的域名；保留固定数据库路径。后端监听 `127.0.0.1`／`::1`，只经反向代理访问；`--trust-loopback-proxy` 仅用于回环监听，网关必须覆盖来源头。

### HTTPS／WSS 网关

安装官方 Caddy。DNS A 指向公网 IPv4；有可达公网 IPv6 时再设置 AAAA。开放网关 TCP 443 和证书验证所需的 TCP 80，后端 29910／9910 保持回环。

```bash
export PXC_DOMAIN=remote.example.com
caddy validate --config deploy/Caddyfile.example --adapter caddyfile
caddy run --config deploy/Caddyfile.example --adapter caddyfile
```

上述前台命令用于验证；长期运行按 [Caddy 官方 systemd 部署](https://caddyserver.com/docs/running)安装，复制配置并给服务设置 `PXC_DOMAIN`。网关代理 `/api/*` 到账号端口、`/signal` 到信令端口，启用请求体大小限制及来源头覆盖。

```bash
curl --fail https://remote.example.com/api/v1/health
```

客户端填写 API `https://remote.example.com`、信令 `wss://remote.example.com/signal`。在该服务器注册账号，各端登录同一账号。证书链与主机名必须有效，不能通过关闭 TLS 校验解决错误。

### TURN UDP 中继

Ubuntu 可通过 `sudo apt install coturn` 安装，配置基于 `deploy/turnserver.conf.example`。自行生成强随机密码：

```bash
openssl rand -hex 32
sudo install -m 600 deploy/turnserver.conf.example /etc/turnserver.conf
sudoedit /etc/turnserver.conf
```

替换 `realm` 和 `user` 占位符。若云服务器经过公网 NAT 映射私网地址，设置 `external-ip=公网IPv4/内网IPv4`，使用实际监听／中继地址。配置文件只允许 root 和实际 TURN 服务账户读取，例如 `root:turnserver 0640`，组名以发行版为准。

安全组及主机防火墙放行 **UDP 3478、UDP 49160–49200**。通过发行版对应的 coturn service 启动，检查状态、日志和端口；避免重复实例。

三个客户端在“设置 → 网络”填写：

```text
TURN：turn://remote.example.com:3478
用户名：自行设置的 TURN 用户名
密码：自己生成的随机密码
```

当前支持 TURN UDP，TCP／TLS 中继尚未实现；完全禁止 UDP 的网络仍可能无法远控。分配配额限制连接数量，不代替云带宽／流量预算。

## 6. 校园网后端＋云端入口

内网设备无法接受公网入站时，可保留账号库在内网，主动建立 SSH 隧道。这里只说明映射；密钥与受限账户自行生成：

```bash
ssh -NT -o ExitOnForwardFailure=yes -o ServerAliveInterval=20 \
  -R 127.0.0.1:32910:127.0.0.1:29910 \
  -R 127.0.0.1:39910:127.0.0.1:9910 \
  tunnel-user@YOUR_CLOUD_HOST
```

内网后端回环监听，云端 Caddy upstream 改为 32910／39910，coturn 运行在云端。隧道账户仅允许这两个回环端口的 remote forwarding，禁止 shell、TTY、agent 和 X11 转发；固定验证云端主机公钥，私钥只留内网主机。

长期运行用 systemd 保活。此模式内网主机须持续开机；后端或隧道断开时网关可能返回 502。完整后端迁到云端后，账号服务就不再依赖内网主机。

## 7. IPv6、备份和验收

- IPv6 URL 使用方括号，如 `http://[2001:db8::10]:29910`；这是文档示例。按需要提供双栈入口，只有 IPv6 的服务器不能直接覆盖只有 IPv4 的客户端。
- 健康接口返回稳定服务器 ID、名称、系统及就绪状态。相同 ID 表示同一账号库，URL 不同可能只是回环、内网、公网入口不同。
- SQLite 用在线 backup API 备份，或停止后端后备份；运行时不要只复制 `.db` 而遗漏 WAL。迁移前保留可恢复备份，重启沿用同一账号库。
- 验收顺序：HTTPS 健康接口 → 三端登录 → WSS 在线 → 直连／TURN → 画面、键鼠、重连、剪贴板和双向文件。
- 能登录但无画面时检查 ICE／TURN 和设备在线状态；API 可达不代表远控通道可达。

参考：[Caddy 反向代理](https://caddyserver.com/docs/caddyfile/directives/reverse_proxy)、[coturn 模板](https://github.com/coturn/coturn/blob/master/examples/etc/turnserver.conf)、[OpenSSH](https://www.openssh.com/manual.html)。
