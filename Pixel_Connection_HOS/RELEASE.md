# 鸿蒙发布准备 / HarmonyOS release preparation

应用显示名称：**Pixel远程**，版本 **1.0.0**（versionCode 1000000）。保持现有 bundle 标识，便于覆盖升级。

公开源码不绑定作者的公网域名、内网主机或共用服务器。首次运行展示保留示例域名：

```text
API: https://remote.example.com
信令: wss://remote.example.com/signal
TURN 地址、用户名和密码：空
```

示例地址不能直接登录。登录页会提示打开高级设置，填写自己的服务器；可信内网也可以填写类似 `http://192.168.1.10:29910` 和 `ws://192.168.1.10:9910` 的实际地址。账号、设备身份和登录信息在用户设备上生成或保存，不随源码提供。

发布步骤：

1. 使用自己的 DevEco / SDK 环境同步依赖，按[构建文档](../docs/CLIENT_BUILD.md)准备 OpenSSL。
2. 将 `build-profile.example.json5` 复制为 `build-profile.json5`，用自己的开发者账号生成签名。公开模板的 `signingConfigs` 为空；实际 build profile、证书、私钥和 DevEco 签名 material 均由 Git 忽略。
3. DevEco 自动签名会重新写入证书路径及密码，只在本机忽略文件中保留。提交或公开源码前运行 `tools/export_source.py`，仅提交扫描通过的源码快照。
4. 使用自己的发布签名构建 HAP，辅助脚本支持 `-BuildMode release`。在 AGC 申请并在发布 profile 中包含受限权限授权；尤其检查 `READ_PASTEBOARD` 和 `READ_WRITE_DESKTOP_DIRECTORY`，名单为空的旧 profile 不会自动获得授权。
5. 在干净安装或清理测试账号后验证登录、连接、文件传输和剪贴板。升级安装保留用户已配置的服务器，不会替换现有用户偏好。

公网自建说明：[中文](../docs/SERVER_BUILD.md) / [English](../docs/SERVER_BUILD_EN.md)。项目：https://github.com/PixelStudioforEveryone/Pixel_Connection。

The public app uses reserved example URLs and empty TURN credentials. Users must enter their own server addresses before signing in. Version 1.0.0 declares clipboard and Documents / Download / Desktop directory permissions. Copy the public build-profile template to the ignored local profile and supply your own signing certificate. Restricted permissions must also be authorized in a newly generated release profile; declarations alone do not grant them. DevEco signing material is excluded from the repository. Existing app preferences are retained on upgrade. Build and sign the HAP in your own environment.
