# 通用部署模板

完整构建和部署见 [服务器构建文档](../docs/SERVER_BUILD.md)。

- Caddyfile.example：HTTPS 账号 API 与 WSS /signal 网关，设置 PXC_DOMAIN 为自己的域名。
- turnserver.conf.example：认证 TURN UDP，替换域名、用户名、随机密码和地址映射。
- pxc-server.service.example：无界面后端，回环监听并保留 SQLite 数据库。

模板仅用占位符，不包含作者服务器地址、账号、密钥、密码或签名材料。未替换的模板不能直接使用。
