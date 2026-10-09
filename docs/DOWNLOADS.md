# 安装包 / Installers

下载页面：[GitHub Releases](https://github.com/PixelStudioforEveryone/Pixel_Connection/releases)。

| 文件 / Asset | 适用系统 / Target |
|---|---|
| `PixelConnection-0.1.0-windows-x64-setup.exe` | Windows 10 / 11 x64；包含 Qt、OpenSSL 和 MSVC 运行库 / Includes required runtime libraries |
| `pixelconnection_0.1.0_ubuntu22.04_amd64.deb` | Ubuntu 22.04 x86_64；依赖由 apt 安装 / Uses Ubuntu system dependencies |
| `SHA256SUMS.txt` | 安装包完整性校验 / Package checksums |

Windows 安装到当前用户目录，创建桌面／开始菜单入口，并提供卸载器。无需第二个服务器 exe，本地服务已集成。安装包当前未进行 Authenticode 签名。

Ubuntu 下载后，在文件所在目录执行：

```bash
sudo apt install ./pixelconnection_0.1.0_ubuntu22.04_amd64.deb
pixelconnection
```

该包不是通用 Linux 二进制；其他发行版或 Ubuntu 版本建议按[源码构建文档](CLIENT_BUILD.md)构建，避免 FFmpeg / Qt ABI 不匹配。包不安装后台常驻服务；需要本地账号／信令时，在客户端中启动。

HarmonyOS 应用显示名为 **Pixel远程**，当前提供[发布准备源码](../Pixel_Connection_HOS/RELEASE.md)，HAP 由开发者使用自己的证书编译与签名。

安装包不包含账号数据库、设备身份、记住的密码、部署地址或签名凭据。首次使用需要配置自己的服务器。源码版本与依赖声明随包附带。

Windows installs for the current user and includes a launcher, an uninstaller and runtime DLLs. The installer currently has no Authenticode signature. The DEB targets Ubuntu 22.04 amd64; other distributions should build from source. Neither package includes personal accounts, identities or deployment credentials. HarmonyOS (**Pixel远程**) is provided as source for the developer to build and sign separately.

## 打包 / Packaging

复用原构建目录编译 Release 客户端。Windows 使用 `tools/package_windows.py`，参数指定已编译 exe、Qt bin、OpenSSL 3.x bin、zlib DLL、MSVC CRT 目录和 NSIS 编译器。它从空的 `artifacts/windows-installer-stage` 组装文件，不复制日常运行目录。

Linux 在目标 Ubuntu 系统运行 `bash tools/package_linux.sh`，从原 `build/apps/client/pxc-client` 生成 DEB，并用 `dpkg-shlibdeps` 检测依赖。暂存目录须为空；最终产物统一放入 `dist/releases`。
