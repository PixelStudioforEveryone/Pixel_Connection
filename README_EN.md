# PixelConnection

<img src="src/icon.png" alt="PixelConnection logo" width="72" />

[简体中文](README.md) · **English**

**Free, open-source remote desktop software you can host yourself.** Windows, Linux and HarmonyOS share the account, device identity and connection protocols. Original code is licensed under **GPL-3.0-only**. Hosting, domain and network traffic costs are paid by the operator.

**Up to 4K / 60 fps settings, with P2P preferred.** These are configurable limits; actual resolution and frame rate depend on the remote screen, hardware and network.

[GitHub](https://github.com/PixelStudioforEveryone/Pixel_Connection) · [Self-hosting guide](docs/SERVER_BUILD_EN.md) · [Client builds (Chinese)](docs/CLIENT_BUILD.md) · [Usage guide (Chinese)](docs/USAGE.md) · [Security (Chinese)](SECURITY.md)

[Download installers](https://github.com/PixelStudioforEveryone/Pixel_Connection/releases) · [Installation notes](docs/DOWNLOADS.md) · [HarmonyOS Pixel远程 release preparation](Pixel_Connection_HOS/RELEASE.md)

## Interface preview

Windows and Linux share a sidebar and device detail layout. Select a device to preview its wallpaper, open its desktop, or start a separate file transfer session.

| Windows | Linux / Ubuntu |
|---|---|
| <img src="docs/images/windows-device.png" alt="Windows device details with a sidebar, wallpaper preview and connection actions" width="440" /> | <img src="docs/images/linux-device.png" alt="Ubuntu device details with a sidebar, wallpaper preview and connection actions" width="440" /> |

The file transfer window has local and remote directory panes and a transfer queue. Send and download actions stay disabled until a file is selected.

<img src="docs/images/file-transfer.png" alt="File transfer window with local and remote folders, disabled send buttons and a transfer queue" width="640" />

These application screenshots use demo devices, a test wallpaper and temporary files. They illustrate the layout, not transfer speeds or performance over the internet. Some visible menu entries are placeholders; see the supported features below.

<details>
<summary>View the Windows, Linux and HarmonyOS promotional overview</summary>

<img src="docs/images/overview.png" alt="PixelConnection promotional overview: free open-source remote desktop for Windows, Linux and HarmonyOS" width="480" />

This promotional illustration was generated from application UI references. Use the screenshots above and the current build to assess the actual interface. HarmonyOS phones and larger devices currently act as controllers.

</details>

## Platform support

| Platform | Control another device | Be remotely controlled | File transfer | Local account / signaling server |
|---|---|---|---|---|
| Windows | Yes | Yes | Yes | Yes |
| Linux | Yes | Yes, including X11 and GNOME Wayland | Yes | Yes |
| HarmonyOS phones / larger devices | Yes | Currently unavailable | Yes, within the app's file access permissions | Currently unavailable |

Implemented features include desktop video, keyboard / mouse / touch input, monitor selection, quality and frame rate settings, hardware / software / automatic acceleration, bidirectional file transfer and plain-text clipboard synchronization. HarmonyOS provides a system keyboard, a paginated PC keyboard, a virtual mouse and layouts for phones and larger screens.

Device cards show the remote PC's wallpaper. Outgoing control actions are disabled while a PC is being controlled. File transfer can run without opening the desktop viewer. Account and signaling services support IPv4 / IPv6. Media and files prefer a direct peer-to-peer connection and can use authenticated TURN over UDP when a direct connection cannot be established.

Currently unavailable: macOS / iOS clients, HarmonyOS desktop hosting, port forwarding, image / rich-text clipboard synchronization and TURN over TCP / TLS. A successful login does not prove that the video, input and file transfer paths work on a particular network; verify them on your target devices.

## Quick start on a local network

1. On one Windows or Linux PC, open the login screen's server settings, select the local server option and start it.
2. Copy its shared API and signaling addresses to the other devices. Example: API `http://192.168.1.10:29910`, signaling `ws://192.168.1.10:9910`. Replace the example IP with the server PC's actual LAN address.
3. Register an account on that server, sign in to the same account on each client and add the devices to the list.
4. Select an online PC and enter its connection verification code to open its desktop or file transfer session.

Keep the server PC running and allow TCP ports **29910** and **9910** from the required network. Other devices cannot use `127.0.0.1` to reach it. A LAN deployment does not need a paid cloud server; video and files normally travel directly between the peers. HTTP / WS should be used only on a trusted LAN.

## Access over the internet

Host your own account / signaling backend, place it behind an HTTPS / WSS gateway and deploy coturn when NAT traversal needs a relay. Configure every client with your own addresses:

```text
Account API: https://remote.example.com
Signaling:   wss://remote.example.com/signal
TURN:        turn://remote.example.com:3478
```

These are placeholders. Generate your own TURN username and random password, then enter them in the network settings. The backend handles account and connection negotiation; video and files prefer P2P and fall back to TURN when needed. See the [English self-hosting guide](docs/SERVER_BUILD_EN.md) for builds, gateway configuration, firewall ports and an optional SSH reverse tunnel for a backend on a campus network.

Current TURN support is **UDP only**. Networks that block UDP may still prevent remote control. Software is free; cloud resources, domains and relay traffic are your own costs.

## Source layout and builds

```text
apps/client/                 Qt client for Windows and Linux
src/ and include/pxc/        Shared C++ core and PC platform implementations
server/                      Account API, SQLite and WebSocket signaling
Pixel_Connection_HOS/        HarmonyOS ArkTS UI, NAPI and native decoding
deploy/                      Gateway, relay and service templates
docs/                        Build guides, usage guides and images
tests/                       Crypto, session authentication and video protocol tests
```

A headless server does not require Qt:

```bash
cmake -S . -B build -DPXC_BUILD_QT_CLIENT=OFF -DPXC_BUILD_STANDALONE_SERVER=ON
cmake --build build --target pxc-server -j
```

Set `PXC_BUILD_QT_CLIENT=ON` to build the PC client. Its executable also provides the backend via `pxc-client --server`, so a separate server executable is not required in a client installation. CMake downloads dependencies during initial configuration. HarmonyOS needs cross-compiled OpenSSL static libraries and your own signing material. See the [client build guide (Chinese)](docs/CLIENT_BUILD.md).

## Security and credentials

The repository includes no shared account, deployment password, SSH key or HarmonyOS signing certificate. Device identities are generated locally. Supply your own deployment and signing credentials and store them outside the source tree. The public Mozilla CA bundle verifies HTTPS / WSS certificates; it is not a private deployment key.

Account passwords use salted scrypt, login tokens are stored as hashes on the server, and device registration uses Ed25519 challenge signatures. Connection verification takes place inside an encrypted data channel before desktop, input and file transfer are enabled.

There are important current limitations: PC remembered passwords and connection codes use unencrypted QSettings; Windows identities are not protected by DPAPI; some HarmonyOS connection settings remain in preferences; SQLite data is not encrypted; default connection codes are short. Use a longer custom code for internet access and protect the host, credential files and backups. See [SECURITY.md (Chinese)](SECURITY.md) for the implemented protections and limitations. The project does not claim a completed security audit.

Runtime databases, device identities, signing materials, build outputs, private settings and development logs are excluded by `.gitignore`. The initial public commit was created from an audited source snapshot without the original development history. Never attach secrets or unredacted logs to public issues.

## License and contributions

Original code is licensed under [GNU GPL v3.0 only](LICENSE). Dependencies retain their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Issues and contributions are welcome. Include the client / server versions, platform, desktop session and relevant network conditions in bug reports, and remove passwords, tokens, private keys and personal information from logs and screenshots.
