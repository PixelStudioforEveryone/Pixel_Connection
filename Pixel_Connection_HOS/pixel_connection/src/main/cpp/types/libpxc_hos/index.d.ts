/**
 * 本机设备身份（configure / addThisDevice 返回值）。
 * enrolled=false 表示尚未「添加本机」（无连接密钥）。
 */
export interface LocalIdentity {
  deviceId: string;
  connectionKey: string;
  enrolled: boolean;
}

export const version: () => string;

/**
 * Phase 1 冒烟测试：原生 POSIX socket 明文 HTTP GET。
 * @param host - 目标主机
 * @param port - 目标端口
 * @param path - 请求路径
 * @returns 结果描述（收到响应: HTTP/1.1 ... / 连接超时 / 无响应 等）
 */
export const httpSmokeTest: (host: string, port: number, path: string) => string;

/**
 * 注册全局事件监听（可重复调用，替换旧监听）。
 * 事件名与 JSON 载荷见 native 侧 hos_controller.h 注释：
 * log / loginResult / registerResult / devicesUpdated / deviceListFailed /
 * deviceEnrolled / deviceEnrollFailed / signalingOnline / sessionState /
 * keyVerified / screensUpdated / videoState / videoNotice / fileEvent
 */
export const setEventListener: (callback: (event: string, json: string) => void) => void;

/**
 * 初始化：设置 API/信令地址与沙箱目录，并加载已有身份。
 * 必须最先调用（login 依赖它创建的 API 客户端）。
 * @param apiUrl - 例 "http://192.168.1.10:29910"
 * @param wsUrl - 例 "ws://192.168.1.10:9910"
 * @param filesDir - 应用沙箱文件目录（identity.json 存放处）
 * @param deviceName - 信令上线时上报的设备名
 */
export const configure: (apiUrl: string, wsUrl: string, filesDir: string, deviceName: string) => LocalIdentity;

/**
 * 「添加本机」：生成 Ed25519 身份 + 连接密钥并落盘，随后自动尝试信令上线。
 * 需先 login。结果经 deviceEnrolled / deviceEnrollFailed 事件通知。
 */
export const addThisDevice: () => LocalIdentity;

/** Read current persisted device identity without reconfiguring the connection. */
export const getLocalIdentity: () => LocalIdentity;
/** Change the local device verification password (5–128 characters), preserve device ID. */
export const setConnectionKey: (key: string) => boolean;

/** 账号登录，结果经 loginResult 事件。需先 configure。 */
export const login: (identifier: string, password: string) => void;
export interface LoginSession { token: string; username: string; generation: number; }
/** Validate an Asset Store session with the configured account server. */
export const restoreLogin: (token: string, username: string) => void;
/** Only for encrypted Asset Store persistence; never log this value. */
export const getLoginSession: () => LoginSession;
export const cancelLoginAttempt: () => void;

/** 账号注册，结果经 registerResult 事件。 */
export const registerAccount: (username: string, email: string, password: string) => void;

/** 登出并停信令。 */
export const logout: () => void;

/** 拉取设备列表，结果经 devicesUpdated / deviceListFailed 事件。 */
export const refreshDevices: () => void;

/** 设置 ICE 服务器（STUN/TURN），连接前调用。 */
export const setIceServers: (stunUrl: string, turnUrl: string, turnUser: string, turnPass: string) => void;

/**
 * 发起 P2P 连接（主控端）。状态经 sessionState 事件：
 * connecting / authenticating / authenticated / connected / rejected / closed / p2p_timeout
 * 认证成功另有 keyVerified 事件（deviceId + connectionKey，供记住密钥）。
 */
export const connectToDevice: (deviceId: string, connectionKey: string) => void;

/** 断开会话。 */
export const disconnect: () => void;
export const fileChannelReady: () => boolean;
export const fileCommand: (json: string) => boolean;
export const uploadFile: (fd: number, name: string, remoteDirectory: string) => string;
export const downloadFile: (fd: number, remotePath: string, name: string) => string;
export const cancelFileTransfer: (id: string) => void;

/** 当前会话是否已通过 PXC 认证。 */
export const sessionAuthenticated: () => boolean;

/** 请求远端屏幕列表，结果经 screensUpdated 事件。 */
export const requestRemoteScreens: () => void;
/** Authenticated wallpaper request; wallpaperReady contains the sandbox cache path. */
export const requestRemoteWallpaper: () => void;
/** Account/server-scoped cache filename, which may not exist before the first connection. */
export const cachedWallpaper: (deviceId: string) => string;

/** 切换远端屏幕。 */
export const switchRemoteScreen: (index: number) => void;

/** 设置远端视频参数（0 表示不修改该项，由服务端定）。 */
export const setRemoteVideoConfig: (width: number, height: number, fps: number, bitrateKbps: number) => void;

/** 请求远端立即编码一个关键帧。 */
export const requestRemoteKeyframe: () => void;

/**
 * 发送输入事件（归一化坐标 JSON，native 侧包装为 pxc 协议）。
 * 例：'{"type":"mouse_move","x":0.5,"y":0.5,"screen":0}'
 */
export const sendInputEvent: (payloadJson: string) => void;
/** Authenticated text clipboard sync, UTF-8 limit 64 KiB. */
export const sendClipboardText: (text: string) => boolean;

/**
 * 绑定视频解码输出 Surface（RemotePage 的 XComponent）。
 * surfaceId 来自 XComponentController.getXComponentSurfaceId()；
 * width/height 为远端视频尺寸提示（videoState 事件给出，可传 0 用默认）。
 * 成功后 ch-video 重组帧将硬解直渲到该 Surface。
 */
export const setVideoSurface: (surfaceId: string, width: number, height: number) => boolean;

/** 解绑视频 Surface 并停止解码器（RemotePage 退出时调用）。 */
export const clearVideoSurface: () => void;
