# 三端客户端构建

## Windows

安装 Visual Studio 2022 C++ 工具链、Git、CMake、Qt 6.2+（Core／Widgets／Network，匹配 MSVC 版本）、x64 OpenSSL 及 zlib 开发文件。路径按实际安装调整：

```powershell
cmake -S . -B build-qt -G "Visual Studio 17 2022" -A x64 `
  -DPXC_BUILD_QT_CLIENT=ON -DPXC_BUILD_STANDALONE_SERVER=OFF `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.7.3/msvc2019_64;C:/Libraries/zlib" `
  -DOPENSSL_ROOT_DIR="C:/OpenSSL-Win64"
cmake --build build-qt --config Release --target pxc-client --parallel
```

产物 `build-qt\apps\client\Release\pxc-client.exe`。使用 Qt 的 `windeployqt` 收集运行依赖，复制需要的 OpenSSL／zlib 运行库，发布整个目录。单独复制 exe 到另一台电脑通常无法运行。应用图标在 `src/icon.png`／`src/icon.ico`，Qt UI 图标在 `apps/client/icons/`。

## Ubuntu／Linux

```bash
sudo apt update
sudo apt install -y git cmake build-essential pkg-config libssl-dev zlib1g-dev \
  qt6-base-dev libx11-dev libxinerama-dev libxtst-dev \
  libavcodec-dev libavutil-dev libswscale-dev libpipewire-0.3-dev libdbus-1-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DPXC_BUILD_QT_CLIENT=ON -DPXC_BUILD_STANDALONE_SERVER=OFF
cmake --build build --target pxc-client -j"$(nproc)"
./build/apps/client/pxc-client
```

Windows 使用 DXGI／Media Foundation／SendInput；Linux 使用 X11 或 GNOME Mutter RemoteDesktop＋ScreenCast／PipeWire，FFmpeg 编解码。GNOME Wayland 被控要求支持这些 D-Bus 接口的桌面会话。缺失平台库时可能选择桩实现，仅适用于服务器／排查构建，不等于可用桌面客户端。

PC 已集成本地服务，使用 `pxc-client --server`，客户端安装包无需第二个服务器 exe。

## HarmonyOS 手机与大屏

先将 `Pixel_Connection_HOS/build-profile.example.json5` 复制为同目录的 `build-profile.json5`，再用 DevEco Studio 打开 `Pixel_Connection_HOS/`，匹配项目 modelVersion／SDK。真实 build profile 由 Git 忽略，公开模板不含签名材料。当前目标 SDK 26.0.0，兼容 6.1.1(24)，按实际安装情况调整并验证。原生 ABI 为 `arm64-v8a` 和 `x86_64`。

### OpenSSL 静态依赖

公开仓库不分发作者的预编译静态库。在 Linux／WSL 准备构建工具，设置 SDK 的实际 sysroot，再交叉编译：

```bash
sudo apt install -y clang-14 llvm-14 perl make curl
export PXC_SYSROOT="/path/to/harmony-sdk/openharmony/native/sysroot"
bash Pixel_Connection_HOS/third_party/openssl/build_openssl_linux.sh
```

产物应在 `Pixel_Connection_HOS/third_party/openssl/<abi>/include` 和 `lib/libssl.a`／`libcrypto.a`。脚本保留已验证基线版本，可用 `PXC_OPENSSL_VERSION` 指定经过验证的新版本，维护发行版时同时检查安全更新。

其他依赖有本地源码缓存时复用，否则 CMake 下载固定版本和所需子模块。鸿蒙引用仓库根共享 C++ 源码，不可只复制 HOS 子目录而丢掉根目录 `src`、`include` 或 `apps/client/session_wire.cpp`。

### 构建与签名

1. 在 DevEco 同步 SDK 和 ohpm 依赖。
2. 使用自己的开发者账户／设备生成调试签名，或自己的发布证书和 profile。
3. 公开 build profile 模板不含签名材料，可生成未签名 HAP；安装真机需自行签名。

Windows 辅助脚本：

```powershell
.\tools\build_harmony.ps1 -DevEcoHome "C:/Program Files/DevEco Studio"
```

已有完整私有 profile 时使用 `-LocalSigningProfile "私有文件的实际路径"`。脚本仅构建期间使用它，退出后还原公开 profile；私有文件保存在仓库外或已忽略目录，不提交、不打印内容。

发布构建添加 `-BuildMode release`，应用市场上传包再加 `-PackageType app`。APP 输出于 `Pixel_Connection_HOS/build/outputs/default/`。签名 profile 的产品配置必须引用自己的 `signingConfig`。应用声明剪贴板及 Documents／Download／Desktop 目录权限；目录权限用于支持的 2in1 设备。受限的 `READ_PASTEBOARD`、`READ_WRITE_DESKTOP_DIRECTORY` 还需在华为申请并重新生成包含授权的发布 profile；源码声明不等于签名授权。文件选择器的现有文件访问流程保留。

HAP 在 `Pixel_Connection_HOS/pixel_connection/build/default/outputs/default/`。通过 SDK HDC 安装，自行验证 ABI 和签名权限。HarmonyOS 当前只做控制端，不提供被控桌面／本地服务器。

## 测试

在现有构建目录开启测试：

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target pxc-crypto-tests pxc-video-tests -j
ctest --test-dir build --output-on-failure
```

Windows 多配置构建添加 `--config Release`，ctest 添加 `-C Release`。先将 OpenSSL 的 bin 及其他动态运行库目录加入 PATH，避免缺 DLL：`$env:PATH = "C:/OpenSSL-Win64/bin;" + $env:PATH`。测试使用合成数据及运行时生成的密钥，不提供真实可用账号或设备凭据。
