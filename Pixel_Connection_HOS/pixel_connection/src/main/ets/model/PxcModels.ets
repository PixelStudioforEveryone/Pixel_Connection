// native 事件 JSON 载荷模型（字段名与 hos_controller.h 事件表逐字对齐，camelCase）

export interface PxcDeviceItem {
  deviceId: string;
  name: string;
  publicIp: string;
  lastSeen: string;
  platform: string;
  online: boolean;
}

export interface PxcDevicesUpdatedJson {
  devices: Array<PxcDeviceItem>;
}

export interface PxcLoginResultJson {
  ok: boolean;
  username?: string;
  error?: string;
  restored?: boolean;
  invalidSession?: boolean;
  generation?: number;
}

export interface PxcRegisterResultJson {
  ok: boolean;
  message: string;
}

// deviceEnrolled / keyVerified 共用 {deviceId, connectionKey}
export interface PxcIdentityJson {
  deviceId: string;
  connectionKey: string;
}

export interface PxcSessionStateJson {
    deviceId: string;
    state: string;
    reason?: string;
}

export interface PxcSignalingOnlineJson {
  online: boolean;
  deviceId?: string;
}

export interface PxcErrorJson {
  error?: string;
}

// screensUpdated：screens 数组元素（对照被控端 screen_list 回复）
export interface PxcScreenItem {
  index: number;
  name: string;
  width: number;
  height: number;
  primary: boolean;
}

export interface PxcScreensUpdatedJson {
  screens: Array<PxcScreenItem>;
}

// videoState：width/height/fps 为 0 表示远端尚未出流或已停止
export interface PxcVideoStateJson {
  width: number;
  height: number;
  fps: number;
  screen: number;
  encoder: string;
  error: string;
}

// videoStats：HosVideoReceiver 每 1s 上报
export interface PxcVideoStatsJson {
  fps: number;
  pushed: number;
  dropped: number;
}

export interface PxcVideoNoticeJson {
  message: string;
  isError: boolean;
}

// RemotePage 路由参数
export interface PxcRemotePageParam {
  deviceName: string;
  deviceId: string;
  viewOnly: boolean;
  platform: string;
}

export interface PxcWallpaperJson {
  deviceId: string;
  path: string;
}
