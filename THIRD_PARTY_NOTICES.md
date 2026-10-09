# 第三方组件说明

原创代码采用 GPL-3.0-only；依赖保留原作者版权与许可。分发二进制时附上所需声明和对应源码／源码获取方式。

| 组件 | 来源 | 许可 |
|---|---|---|
| libdatachannel | https://github.com/paullouisageneau/libdatachannel | MPL-2.0 |
| libjuice | https://github.com/paullouisageneau/libjuice | BSD-2-Clause |
| usrsctp | https://github.com/sctplab/usrsctp | BSD-3-Clause |
| IXWebSocket | https://github.com/machinezone/IXWebSocket | BSD-3-Clause |
| nlohmann/json | https://github.com/nlohmann/json | MIT |
| OpenSSL 3.x | https://github.com/openssl/openssl | Apache-2.0 |
| SQLite | https://sqlite.org/copyright.html | Public domain |
| Qt 6 | https://www.qt.io/licensing | 模块适用 LGPL／GPL／商业许可，按实际构建遵守 |
| FFmpeg | https://ffmpeg.org/legal.html | LGPL／GPL，按启用组件及链接方式 |
| zlib | https://www.zlib.net/zlib_license.html | zlib |
| Mozilla CA bundle（curl 转换） | https://curl.se/docs/caextract.html | MPL-2.0，见 HOS rawfile NOTICE |

DXGI／Media Foundation、X11／PipeWire／D-Bus、HarmonyOS SDK 平台库按各自 SDK／发行版条款使用。Git 源码不包含 SDK、私有签名材料或本机依赖缓存；Releases 可提供附有许可证的运行库及安装包。

Windows 发行包动态链接 Qt 和 OpenSSL，用户可更换兼容运行库；Linux DEB 使用发行版系统库。NSIS 安装器将协议、依赖声明和许可证放入安装目录。静态依赖版本见 `cmake/deps.cmake`，其源码可从表中上游取得。

Qt 6.7.3 对应源码：https://download.qt.io/archive/qt/6.7/6.7.3/single/qt-everywhere-src-6.7.3.tar.xz 。Windows OpenSSL 3.5.9 对应源码：https://github.com/openssl/openssl/releases/tag/openssl-3.5.9 ，构建包由 conda-forge 提供：https://github.com/conda-forge/openssl-feedstock 。MSVC 运行库遵守 Microsoft Visual C++ 可再分发组件许可；安装器使用 NSIS 的 zlib／相关组件许可，来源：https://nsis.sourceforge.io/License 。
