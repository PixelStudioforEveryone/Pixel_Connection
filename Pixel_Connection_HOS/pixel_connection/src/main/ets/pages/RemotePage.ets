import pxc from 'libpxc_hos.so';
import { pxcEmitter } from '../common/PxcEmitter';
import { pxcClipboard } from '../common/PxcClipboard';
import { PxcNormPoint, PxcTouchMouseMapper, pxcMouseMoveJson, pxcMouseButtonJson, pxcWheelJson } from '../common/PxcInputMapper';
import { PxcComputerKeyboard } from '../common/PxcComputerKeyboard';
import { pxcSettings } from '../common/PxcSettings';
import { PxcIcon } from '../common/PxcUi';
import { PxcControlRect, pxcUsesDesktopControls, pxcClampControlPoint, pxcDesktopControlRect,
  pxcClampPanelRect, pxcControlDockEdge, pxcPanelDockEdge, pxcDockedControlRect } from '../common/PxcDesktopControlLayout';
import { PxcFilePageParam } from './FilesPage';
import { window } from '@kit.ArkUI';
import { deviceInfo } from '@kit.BasicServicesKit';
import {
  PxcRemotePageParam,
  PxcScreenItem,
  PxcScreensUpdatedJson,
  PxcSessionStateJson,
  PxcVideoNoticeJson,
  PxcVideoStateJson,
  PxcVideoStatsJson
} from '../model/PxcModels';

// sessionState 终态：到达后自动退出远控页
const TERMINAL_STATES: Map<string, string> = new Map<string, string>([
  ['rejected', '连接被拒绝'],
  ['closed', '连接已关闭'],
  ['p2p_timeout', '连接超时']
]);

interface PxcQualityOption {
  label: string;
  width: number;
  height: number;
  bitrate: number;
}

const QUALITY_OPTIONS: Array<PxcQualityOption> = [
  { label: '1080P', width: 1920, height: 1080, bitrate: 8000 },
  { label: '2K', width: 2560, height: 1440, bitrate: 14000 },
  { label: '4K', width: 3840, height: 2160, bitrate: 24000 }
];

const INPUT_FOCUS_KEY: string = 'pxc_remote_input';
const MOUSE_WIDTH: number = 80;
const MOUSE_HEIGHT: number = 102;

@Builder
export function RemotePageBuilder(): void {
  RemotePage()
}

@Component
export struct RemotePage {
  @State deviceName: string = '远端设备';
  @State sessionStateText: string = '正在建立会话…';
  @State noticeText: string = '';
  @State noticeIsError: boolean = false;
  @State statsText: string = '';
  @State resolutionText: string = '';
  @State screens: Array<PxcScreenItem> = [];
  @State activeScreen: number = -1;
  @State panelVisible: boolean = false;
  @State desktopControls: boolean = deviceInfo.deviceType === '2in1';
  @State desktopFullscreen: boolean = false;
  @State controlX: number = -1;
  @State controlY: number = 96;
  @State controlWidth: number = 800;
  @State controlHeight: number = 600;
  @State controlDock: string = '';
  @State panelX: number = -1;
  @State panelY: number = -1;
  @State selectedQuality: string = '1080P';
  @State selectedFps: number = 30;
  @State encoderText: string = '';
  private controlDragX: number = 0;
  private controlDragY: number = 0;
  private controlDragged: boolean = false;
  private panelDragX: number = 0;
  private panelDragY: number = 0;
  private panelOffsetX: number = 0;
  private panelOffsetY: number = 0;
  @State hasVideo: boolean = false;
  @State surfaceBound: boolean = false;
  @State videoWidth: number = 0;
  @State videoHeight: number = 0;
  @State viewOnly: boolean = false;
  private remotePlatform: string = '';
  @State pointerMode: boolean = false;
  @State pointerX: number = 0.5;
  @State pointerY: number = 0.5;
  @State keyboardVisible: boolean = false;
  @State keyboardMode: string = 'system';
  @State keyboardCompact: boolean = false;
  @State landscape: boolean = true;
  @State railHeight: number = 280;
  @State stickyModifiers: boolean = false;
  @State systemText: string = '';
  @State mouseVisible: boolean = false;
  @State dragHeld: boolean = false;
  @State mousePressed: Array<string> = [];
  @State clickVisible: boolean = false;
  @State clickX: number = 0;
  @State clickY: number = 0;
  private clickTimer: number = -1;
  @State mouseX: number = 12;
  @State mouseY: number = 12;
  private mouseDragX: number = 0;
  private mouseDragY: number = 0;
  private virtualButtons: Set<string> = new Set<string>();
  private pointerTouchX: number = 0;
  private pointerTouchY: number = 0;
  private pointerDownTime: number = 0;
  private pointerMoved: boolean = false;
  private pointerStartX: number = 0;
  private pointerStartY: number = 0;
  private pointerGesture: string = '';
  private pointerLongPress: number = -1;
  private pointerWheel: number = 0;
  pathStack: NavPathStack = new NavPathStack();
  private xcc: XComponentController = new XComponentController();
  private inputMapper: PxcTouchMouseMapper = new PxcTouchMouseMapper(
    (json: string): void => {
      this.sendInput(json);
    },
    (x: number, y: number): PxcNormPoint => {
      return this.normalizePoint(x, y);
    }
  );
  private autoSwitched: boolean = false;
  private sessionEnded: boolean = false;
  // XComponent 尺寸与画面等比显示区（vp；供输入归一化与 surface 黑边）
  private boxW: number = 0;
  private boxH: number = 0;
  private videoRectX: number = 0;
  private videoRectY: number = 0;
  private videoRectW: number = 0;
  private videoRectH: number = 0;

  aboutToAppear(): void {
    this.sessionStateText = pxc.sessionAuthenticated() ? '已连接' : '正在建立会话…';
    this.pointerMode = pxcSettings.getBool('pointerMode', false);
    this.keyboardMode = pxcSettings.getString('keyboardMode', 'system');
    this.stickyModifiers = pxcSettings.getBool('stickyModifiers', false);
    const context: Context | undefined = this.getUIContext().getHostContext();
    if (context !== undefined) pxcClipboard.enable(context);
    if (context !== undefined && deviceInfo.deviceType !== '2in1') window.getLastWindow(context).then(async (win: window.Window) => {
      await win.setPreferredOrientation(window.Orientation.AUTO_ROTATION_LANDSCAPE);
      await win.setWindowLayoutFullScreen(true);
      await win.setWindowSystemBarEnable([]);
      await win.setSpecificSystemBarEnabled('navigationIndicator', false).catch(() => {});
      await win.setWindowSystemBarProperties({ navigationBarColor: '#000000', navigationBarContentColor: '#000000' });
    }).catch(() => {});
    pxcEmitter.on('sessionState', this.handleSessionState);
    pxcEmitter.on('screensUpdated', this.handleScreensUpdated);
    pxcEmitter.on('videoState', this.handleVideoState);
    pxcEmitter.on('videoStats', this.handleVideoStats);
    pxcEmitter.on('videoNotice', this.handleVideoNotice);
    pxcEmitter.on('appBackground', this.handleBackground);
  }

  aboutToDisappear(): void {
    pxcEmitter.off('sessionState', this.handleSessionState);
    pxcEmitter.off('screensUpdated', this.handleScreensUpdated);
    pxcEmitter.off('videoState', this.handleVideoState);
    pxcEmitter.off('videoStats', this.handleVideoStats);
    pxcEmitter.off('videoNotice', this.handleVideoNotice);
    pxcEmitter.off('appBackground', this.handleBackground);
    this.releaseInput();
    if (this.clickTimer >= 0) clearTimeout(this.clickTimer);
    pxc.clearVideoSurface();
    if (this.desktopFullscreen) {
      const desktopContext: Context | undefined = this.getUIContext().getHostContext();
      if (desktopContext !== undefined) window.getLastWindow(desktopContext).then((win: window.Window) => win.recover()).catch(() => {});
    }
    if (!this.sessionEnded) pxc.disconnect();
    const context: Context | undefined = this.getUIContext().getHostContext();
    if (context !== undefined && deviceInfo.deviceType !== '2in1') window.getLastWindow(context).then(async (win: window.Window) => {
      await win.setPreferredOrientation(window.Orientation.UNSPECIFIED);
      await win.setWindowLayoutFullScreen(false);
      await win.setWindowSystemBarEnable(['status', 'navigation']);
      await win.setSpecificSystemBarEnabled('navigationIndicator', true).catch(() => {});
      await win.setWindowSystemBarProperties({ navigationBarColor: '#ffffff', navigationBarContentColor: '#222222' });
    }).catch(() => {});
  }

  private handleSessionState = (json: string): void => {
    const d: PxcSessionStateJson = JSON.parse(json) as PxcSessionStateJson;
    const state: string = d.state ?? '';
    if (state === 'connected') {
      this.sessionStateText = '已连接';
      return;
    }
    const terminalText: string | undefined = TERMINAL_STATES.get(state);
    if (terminalText !== undefined && !this.sessionEnded) {
      this.sessionEnded = true;
      this.sessionStateText = terminalText;
      this.pathStack.pop();
    }
  };

  private handleScreensUpdated = (json: string): void => {
    const d: PxcScreensUpdatedJson = JSON.parse(json) as PxcScreensUpdatedJson;
    this.screens = d.screens ?? [];
    if (!this.autoSwitched && this.screens.length > 0) {
      this.autoSwitched = true;
      const first: PxcScreenItem = this.screens.find((s: PxcScreenItem): boolean => s.primary) ?? this.screens[0];
      this.activeScreen = first.index;
      this.inputMapper.setScreen(first.index);
      pxc.switchRemoteScreen(first.index);
    }
  };

  private handleVideoState = (json: string): void => {
    const d: PxcVideoStateJson = JSON.parse(json) as PxcVideoStateJson;
    this.videoWidth = d.width ?? 0;
    this.videoHeight = d.height ?? 0;
    if ((d.encoder ?? '') !== '') this.encoderText = d.encoder;
    if ((d.width ?? 0) > 0 && (d.height ?? 0) > 0) {
      this.resolutionText = `${d.width}x${d.height}` + ((d.fps ?? 0) > 0 ? ` @ ${d.fps}fps` : '');
    }
    this.updateVideoRect();
  };

  private handleVideoStats = (json: string): void => {
    const d: PxcVideoStatsJson = JSON.parse(json) as PxcVideoStatsJson;
    this.hasVideo = (d.fps ?? 0) > 0 || (d.pushed ?? 0) > 0;
    this.statsText = `${d.fps} fps · 收 ${d.pushed} · 丢 ${d.dropped}`;
  };

  private handleVideoNotice = (json: string): void => {
    const d: PxcVideoNoticeJson = JSON.parse(json) as PxcVideoNoticeJson;
    this.noticeText = d.message ?? '';
    this.noticeIsError = d.isError ?? false;
  };

  // -------------------------------------------------- 视频显示区（等比 + 输入归一化）

  private onBoxSizeChange(newSize: SizeOptions): void {
    const w: number = typeof newSize.width === 'number' ? newSize.width : 0;
    const h: number = typeof newSize.height === 'number' ? newSize.height : 0;
    if (w <= 0 || h <= 0) {
      return;
    }
    this.boxW = w;
    this.boxH = h;
    this.railHeight = h;
    this.controlWidth = w;
    this.controlHeight = h;
    const point = pxcClampControlPoint(this.controlX < 0 ? w - 88 : this.controlX, this.controlY, w, h);
    this.controlX = point.x;
    this.controlY = point.y;
    if (this.panelX >= 0) {
      const panel = pxcClampPanelRect(this.panelX, this.panelY, w, h);
      this.panelX = panel.x; this.panelY = panel.y;
    }
    this.followMouse();
    this.updateVideoRect();
  }

  private updateVideoRect(): void {
    if (this.boxW <= 0 || this.boxH <= 0) {
      return;
    }
    let rx: number = 0;
    let ry: number = 0;
    let rw: number = this.boxW;
    let rh: number = this.boxH;
    if (this.videoWidth > 0 && this.videoHeight > 0) {
      // 等比 contain：画面适配组件，居中留黑边
      const scale: number = Math.min(this.boxW / this.videoWidth, this.boxH / this.videoHeight);
      rw = this.videoWidth * scale;
      rh = this.videoHeight * scale;
      rx = (this.boxW - rw) / 2;
      ry = (this.boxH - rh) / 2;
    }
    this.videoRectX = rx;
    this.videoRectY = ry;
    this.videoRectW = rw;
    this.videoRectH = rh;
    if (this.surfaceBound) {
      const px: number = this.getUIContext().vp2px(1);
      const rect: SurfaceRect = {
        offsetX: rx * px,
        offsetY: ry * px,
        surfaceWidth: rw * px,
        surfaceHeight: rh * px
      };
      this.xcc.setXComponentSurfaceRect(rect);
    }
  }

  private normalizePoint(x: number, y: number): PxcNormPoint {
    let nx: number = 0;
    let ny: number = 0;
    if (this.videoRectW > 0 && this.videoRectH > 0) {
      nx = (x - this.videoRectX) / this.videoRectW;
      ny = (y - this.videoRectY) / this.videoRectH;
    } else if (this.boxW > 0 && this.boxH > 0) {
      nx = x / this.boxW;
      ny = y / this.boxH;
    }
    if (nx < 0) {
      nx = 0;
    } else if (nx > 1) {
      nx = 1;
    }
    if (ny < 0) {
      ny = 0;
    } else if (ny > 1) {
      ny = 1;
    }
    return { x: nx, y: ny };
  }

  // -------------------------------------------------- 视频链路

  private onVideoSurfaceLoad(): void {
    const surfaceId: string = this.xcc.getXComponentSurfaceId();
    if (surfaceId === '') {
      this.noticeText = '获取视频 Surface 失败';
      this.noticeIsError = true;
      return;
    }
    const w: number = this.videoWidth > 0 ? this.videoWidth : 1280;
    const h: number = this.videoHeight > 0 ? this.videoHeight : 720;
    const ok: boolean = pxc.setVideoSurface(surfaceId, w, h);
    this.surfaceBound = ok;
    if (!ok) {
      this.noticeText = '视频解码器启动失败';
      this.noticeIsError = true;
      return;
    }
    this.updateVideoRect();
    this.refocusInput();
    pxc.requestRemoteScreens();
    pxc.requestRemoteKeyframe();
  }

  private refocusInput(): void {
    this.getUIContext().getFocusController().requestFocus(INPUT_FOCUS_KEY);
  }

  // -------------------------------------------------- 控制面板动作

  private doSwitchScreen(index: number): void {
    this.releaseInput();
    this.activeScreen = index;
    this.inputMapper.setScreen(index);
    pxc.switchRemoteScreen(index);
    this.refocusInput();
  }

  private doApplyQuality(opt: PxcQualityOption): void {
    this.selectedQuality = opt.label;
    pxc.setRemoteVideoConfig(opt.width, opt.height, this.selectedFps, opt.bitrate);
    pxc.requestRemoteKeyframe();
    this.refocusInput();
  }

  private doApplyFps(fps: number): void {
    this.selectedFps = fps;
    pxc.setRemoteVideoConfig(0, 0, fps, 0);
    this.refocusInput();
  }

  private toggleDesktopPanel(): void {
    if (this.controlDragged) { this.controlDragged = false; return; }
    if (this.controlDock !== '') {
      if (this.controlDock === 'left') this.controlX = 8;
      if (this.controlDock === 'right') this.controlX = this.controlWidth - 52;
      if (this.controlDock === 'top') this.controlY = 8;
      if (this.controlDock === 'bottom') this.controlY = this.controlHeight - 52;
      this.controlDock = ''; this.panelX = -1; this.panelY = -1;
    }
    this.releaseInput();
    this.panelVisible = !this.panelVisible;
    if (this.panelVisible) pxc.requestRemoteScreens();
    else this.refocusInput();
  }

  private desktopPanelRect(): PxcControlRect {
    if (this.panelX >= 0) return pxcClampPanelRect(this.panelX, this.panelY, this.controlWidth, this.controlHeight);
    return pxcDesktopControlRect({ x: this.controlX, y: this.controlY }, this.controlWidth, this.controlHeight);
  }

  private desktopButtonRect(): PxcControlRect {
    if (this.controlDock !== '') return pxcDockedControlRect({ x: this.controlX, y: this.controlY },
      this.controlDock, this.controlWidth, this.controlHeight);
    return { x: this.controlX, y: this.controlY, width: 44, height: 44 };
  }

  private finishControlDrag(): void {
    this.controlDock = pxcControlDockEdge({ x: this.controlX, y: this.controlY }, this.controlWidth, this.controlHeight);
    this.panelX = -1; this.panelY = -1;
    // Keep the trailing pointer-up from opening the panel after a drag.
    setTimeout(() => { this.controlDragged = false; }, 150);
    this.refocusInput();
  }

  private moveDesktopPanel(event: GestureEvent): void {
    this.panelOffsetX = event.offsetX; this.panelOffsetY = event.offsetY;
    const rect = pxcClampPanelRect(this.panelDragX + event.offsetX, this.panelDragY + event.offsetY,
      this.controlWidth, this.controlHeight);
    this.panelX = rect.x; this.panelY = rect.y;
    const button = pxcClampControlPoint(rect.x + rect.width + 8, rect.y, this.controlWidth, this.controlHeight);
    this.controlX = button.x; this.controlY = button.y;
  }

  private finishPanelDrag(): void {
    const rect = this.desktopPanelRect();
    const edge = pxcPanelDockEdge(rect, this.controlWidth, this.controlHeight, this.panelOffsetX, this.panelOffsetY);
    if (edge !== '') {
      this.controlDock = edge; this.panelVisible = false;
      this.controlX = rect.x; this.controlY = rect.y;
      this.panelX = -1; this.panelY = -1;
      this.refocusInput();
    }
  }

  private async toggleDesktopFullscreen(): Promise<void> {
    const context: Context | undefined = this.getUIContext().getHostContext();
    if (context === undefined) return;
    this.releaseInput();
    try {
      const win = await window.getLastWindow(context);
      if (this.desktopFullscreen) await win.recover();
      else await win.maximize(window.MaximizePresentation.ENTER_IMMERSIVE);
      this.desktopFullscreen = !this.desktopFullscreen;
    } catch (_) { this.noticeText = '当前窗口不支持切换全屏'; this.noticeIsError = true; }
  }

  private doDisconnect(): void {
    if (pxcSettings.getBool('confirmExit', true)) {
      this.getUIContext().showAlertDialog({ title: '断开远程桌面', message: '确定结束当前连接？',
        primaryButton: { value: '继续控制', action: (): void => {} }, secondaryButton: { value: '断开', action: (): void => {
          this.releaseInput(); pxc.disconnect();
        } } });
    } else { this.releaseInput(); pxc.disconnect(); }
  }

  private sendInput(json: string): void {
      if (!this.viewOnly && !this.sessionEnded && pxc.sessionAuthenticated()) pxcClipboard.sendInput(json);
  }
  private handleBackground = (): void => {
    this.releaseInput();
    this.keyboardVisible = false;
    this.mouseVisible = false;
    if (pxcSettings.getBool('disconnectBackground', false)) pxc.disconnect();
  };
  private releaseInput(): void {
    this.inputMapper.reset(true);
    for (const button of this.virtualButtons) this.sendInput(pxcMouseButtonJson(button, false));
    this.virtualButtons.clear(); this.mousePressed = []; this.dragHeld = false;
    if (this.pointerLongPress >= 0) clearTimeout(this.pointerLongPress);
    this.pointerLongPress = -1; this.pointerGesture = ''; this.pointerWheel = 0;
  }
  private clickPointer(button: string): void {
    this.showClickFeedback(this.pointerX, this.pointerY);
    this.sendInput(pxcMouseMoveJson(this.pointerX, this.pointerY, Math.max(0, this.activeScreen)));
    this.sendInput(pxcMouseButtonJson(button, true)); this.sendInput(pxcMouseButtonJson(button, false));
  }
  private virtualButton(button: string, event: TouchEvent): void {
    if (this.viewOnly) return;
    if (event.type === TouchType.Down) {
      this.sendInput(pxcMouseMoveJson(this.pointerX, this.pointerY, Math.max(0, this.activeScreen)));
      this.virtualButtons.add(button); this.sendInput(pxcMouseButtonJson(button, true));
      this.mousePressed = Array.from(this.virtualButtons);
      this.showClickFeedback(this.pointerX, this.pointerY);
    } else if (event.type === TouchType.Up || event.type === TouchType.Cancel) {
      if (this.virtualButtons.delete(button)) this.sendInput(pxcMouseButtonJson(button, false));
      this.mousePressed = Array.from(this.virtualButtons);
      if (button === 'left') this.dragHeld = false;
    }
  }
  private showClickFeedback(x: number, y: number): void {
    if (this.clickTimer >= 0) clearTimeout(this.clickTimer);
    this.clickX = x; this.clickY = y; this.clickVisible = true;
    this.clickTimer = setTimeout(() => { this.clickVisible = false; this.clickTimer = -1; }, 220);
  }
  private followMouse(): void {
    this.mouseX = Math.max(0, Math.min(Math.max(0, this.boxW - MOUSE_WIDTH),
      this.videoRectX + this.pointerX * this.videoRectW + 12));
    this.mouseY = Math.max(0, Math.min(Math.max(0, this.boxH - MOUSE_HEIGHT),
      this.videoRectY + this.pointerY * this.videoRectH + 12));
  }
  private handleTouch(event: TouchEvent, relative: boolean = false): void {
    if (this.viewOnly) return;
    if (!this.pointerMode && !relative) {
      if (event.touches.length > 0) {
        const p: PxcNormPoint = this.normalizePoint(event.touches[0].x, event.touches[0].y);
        this.pointerX = p.x; this.pointerY = p.y;
        if (event.type === TouchType.Down) this.showClickFeedback(p.x, p.y);
        if (this.mouseVisible) this.followMouse();
      }
      this.inputMapper.onTouch(event); return;
    }
    const touches: Array<TouchObject> = event.touches;
    if (event.type === TouchType.Cancel) { this.releaseInput(); return; }
    if (event.type === TouchType.Down && touches.length > 0) {
      if (this.pointerLongPress >= 0) clearTimeout(this.pointerLongPress);
      this.pointerTouchX = touches[0].windowX; this.pointerTouchY = touches[0].windowY;
      this.pointerStartX = this.pointerTouchX; this.pointerStartY = this.pointerTouchY;
      this.pointerMoved = false; this.pointerDownTime = Date.now();
      this.pointerGesture = touches.length >= 3 ? 'middle' : touches.length === 2 ? 'scroll' : 'move';
      this.pointerWheel = 0;
      if (this.pointerGesture === 'move') {
        this.pointerLongPress = setTimeout(() => {
          if (!this.pointerMoved && this.pointerGesture === 'move') { this.clickPointer('right'); this.pointerGesture = 'consumed'; }
        }, 600);
      }
    } else if (event.type === TouchType.Move && touches.length > 0) {
      const dx: number = touches[0].windowX - this.pointerTouchX;
      const dy: number = touches[0].windowY - this.pointerTouchY;
      this.pointerTouchX = touches[0].windowX; this.pointerTouchY = touches[0].windowY;
      if (Math.hypot(touches[0].windowX - this.pointerStartX, touches[0].windowY - this.pointerStartY) > 8) this.pointerMoved = true;
      if (this.pointerGesture === 'move') {
        this.pointerX = Math.max(0, Math.min(1, this.pointerX + dx / Math.max(1, this.videoRectW)));
        this.pointerY = Math.max(0, Math.min(1, this.pointerY + dy / Math.max(1, this.videoRectH)));
        this.sendInput(pxcMouseMoveJson(this.pointerX, this.pointerY, Math.max(0, this.activeScreen)));
        if (this.mouseVisible) this.followMouse();
      } else if (this.pointerGesture === 'scroll') {
        this.pointerWheel += dy;
        while (Math.abs(this.pointerWheel) >= 32) {
          const sign: number = this.pointerWheel > 0 ? 1 : -1;
          this.sendInput(pxcWheelJson(-sign)); this.pointerWheel -= sign * 32;
        }
      }
    } else if (event.type === TouchType.Up) {
      if (this.pointerLongPress >= 0) clearTimeout(this.pointerLongPress);
      const changed: Set<number> = new Set<number>(event.changedTouches.map((t: TouchObject): number => t.id));
      if (touches.filter((t: TouchObject): boolean => !changed.has(t.id)).length !== 0) return;
      if (!this.pointerMoved && Date.now() - this.pointerDownTime < 600) {
        if (this.pointerGesture === 'move' && !this.dragHeld) this.clickPointer('left');
        if (this.pointerGesture === 'middle') this.clickPointer('middle');
      }
      this.pointerGesture = '';
    }
  }
  private toggleKeyboard(): void {
    this.releaseInput(); this.keyboardVisible = !this.keyboardVisible;
    if (this.keyboardVisible) { this.mouseVisible = false; this.panelVisible = false; }
    if (!this.keyboardVisible) this.refocusInput();
  }
  private changeKeyboard(mode: string): void {
    this.releaseInput(); this.keyboardMode = mode; pxcSettings.putString('keyboardMode', mode);
    if (mode === 'computer') this.refocusInput();
  }
  private sendText(): void {
    if (this.systemText === '' || this.viewOnly) return;
    this.releaseInput();
    if (this.systemText.length > 4096) { this.noticeText = '一次最多发送 4096 个字符'; return; }
    this.sendInput(`{"type":"text","text":${JSON.stringify(this.systemText)}}`); this.systemText = '';
  }
  private shortcut(keys: Array<number>): void {
    if (this.viewOnly) return;
    this.releaseInput();
    for (const key of keys) this.inputMapper.keyboard.key(key, true);
    for (let i: number = keys.length - 1; i >= 0; i--) this.inputMapper.keyboard.key(keys[i], false);
    this.refocusInput();
  }
  private rotateDisplay(): void {
    const context: Context | undefined = this.getUIContext().getHostContext();
    if (context !== undefined && deviceInfo.deviceType !== '2in1') window.getLastWindow(context).then((win: window.Window) => {
      win.setPreferredOrientation(this.boxW > this.boxH ? window.Orientation.PORTRAIT : window.Orientation.LANDSCAPE);
    }).catch(() => { this.noticeText = '当前设备不支持切换方向'; });
  }

  @Builder
  ControlPanel() {
    Column({ space: 12 }) {
      if (!this.desktopControls) {
      Text('控制面板')
        .fontSize(15)
        .fontWeight(FontWeight.Bold)
        .fontColor('#222222')
      }

      if (this.resolutionText !== '' && !this.desktopControls) {
        Text(`远端分辨率 ${this.resolutionText}`)
          .fontSize(12)
          .fontColor('#888888')
      }

      if (this.screens.length > 0 && !this.desktopControls) {
        Column({ space: 6 }) {
          Text('屏幕')
            .fontSize(12)
            .fontColor('#666666')
            .width('100%')
          Column({ space: 6 }) {
            ForEach(this.screens, (scr: PxcScreenItem) => {
              Text(`${scr.index} · ${scr.name}`)
                .fontSize(12)
                .padding({ left: 10, right: 10, top: 6, bottom: 6 })
                .borderRadius(8)
                .fontColor(scr.index === this.activeScreen ? Color.White : '#444444')
                .backgroundColor(scr.index === this.activeScreen ? this.desktopControls ? '#4f7cff' : '#1f6feb' : '#f2f3f5')
                .onClick(() => {
                  this.doSwitchScreen(scr.index);
                })
            }, (scr: PxcScreenItem): string => `screen-${scr.index}`)
          }
          .width('100%')
        }
        .width('100%')
        .alignItems(HorizontalAlign.Start)
      }

      Column({ space: 6 }) {
        Text('画质')
          .fontSize(12)
          .fontColor('#666666')
          .width('100%')
        Row({ space: 8 }) {
          ForEach(QUALITY_OPTIONS, (opt: PxcQualityOption) => {
            Text(opt.label)
              .fontSize(13).height(34)
              .padding({ left: 18, right: 18 })
              .borderRadius(8)
              .fontColor(this.selectedQuality === opt.label ? Color.White : '#444444')
              .backgroundColor(this.selectedQuality === opt.label ? this.desktopControls ? '#4f7cff' : '#1f6feb' : '#f2f3f5')
              .onClick(() => {
                this.doApplyQuality(opt);
              })
          }, (opt: PxcQualityOption): string => `quality-${opt.label}`)
        }.width('100%')
        Text('帧率').fontSize(12).fontColor('#666666').width('100%')
        Row({ space: 8 }) {
          Text('30 帧')
            .fontSize(13).height(34)
            .padding({ left: 18, right: 18 })
            .borderRadius(8)
            .fontColor(this.selectedFps === 30 ? Color.White : '#444444')
            .backgroundColor(this.selectedFps === 30 ? this.desktopControls ? '#4f7cff' : '#1f6feb' : '#f2f3f5')
            .onClick(() => {
              this.doApplyFps(30);
            })
          Text('60 帧')
            .fontSize(13).height(34)
            .padding({ left: 18, right: 18 })
            .borderRadius(8)
            .fontColor(this.selectedFps === 60 ? Color.White : '#444444')
            .backgroundColor(this.selectedFps === 60 ? this.desktopControls ? '#4f7cff' : '#1f6feb' : '#f2f3f5')
            .onClick(() => {
              this.doApplyFps(60);
            })
        }
        .width('100%')
      }
      .width('100%')
      .alignItems(HorizontalAlign.Start)

      Button('请求关键帧', { type: ButtonType.Normal })
        .borderRadius(8)
        .fontSize(13)
        .height(34)
        .width('100%')
        .backgroundColor(this.desktopControls ? '#4f7cff' : '#1f6feb')
        .onClick(() => {
          pxc.requestRemoteKeyframe();
          this.refocusInput();
        })
    }
    .width('100%')
    .padding(this.desktopControls ? 0 : 14)
    .borderRadius({ topLeft: 16, topRight: 16 })
    .backgroundColor(Color.White)
  }

  @Builder
  DesktopControlButton() {
    Stack() {
      if (this.controlDock === '') {
        Column({ space: 3 }) {
          Row({ space: 3 }) {
            Rect().width(8).height(8).radius(2).fill(Color.White)
            Rect().width(8).height(8).radius(2).fill(Color.White)
          }
          Row({ space: 3 }) {
            Rect().width(8).height(8).radius(2).fill(Color.White)
            Rect().width(8).height(8).radius(2).fill(Color.White)
          }
        }.hitTestBehavior(HitTestMode.None)
      } else {
        Column() {}.width(this.controlDock === 'left' || this.controlDock === 'right' ? 3 : 16)
          .height(this.controlDock === 'left' || this.controlDock === 'right' ? 16 : 3)
          .backgroundColor(Color.White).borderRadius(2).hitTestBehavior(HitTestMode.None)
      }
    }
      .id('pxc_desktop_control_button').accessibilityText('控制中心，拖动可移动')
      .width(this.desktopButtonRect().width).height(this.desktopButtonRect().height).linearGradient({ angle: 135, colors: [['#5d8bff', 0], ['#3562e8', 1]] }).borderRadius(24)
      .border({ width: 1, color: 'rgba(255,255,255,0.7)' })
      .shadow({ radius: 10, color: '#40000000', offsetY: 2 })
      .position({ x: this.desktopButtonRect().x, y: this.desktopButtonRect().y })
      .onClick(() => { this.toggleDesktopPanel(); })
      .gesture(PanGesture({ distance: 6 }).onActionStart(() => {
        const rect = this.desktopButtonRect();
        const origin = pxcClampControlPoint(rect.x, rect.y, this.controlWidth, this.controlHeight);
        this.controlX = origin.x; this.controlY = origin.y;
        this.controlDragX = origin.x; this.controlDragY = origin.y;
        this.controlDock = '';
        this.controlDragged = true; this.panelVisible = false; this.releaseInput();
      }).onActionUpdate((event: GestureEvent) => {
        const point = pxcClampControlPoint(this.controlDragX + event.offsetX, this.controlDragY + event.offsetY,
          this.controlWidth, this.controlHeight);
        this.controlX = point.x; this.controlY = point.y;
      }).onActionEnd(() => { this.finishControlDrag(); })
        .onActionCancel(() => { this.controlDragged = false; this.refocusInput(); }))
  }

  @Builder
  DesktopControlWindow() {
    Column({ space: 10 }) {
      Row() {
        Text('控制中心').fontColor('#1f2329').fontSize(15).fontWeight(FontWeight.Bold).layoutWeight(1).height(32)
          .id('pxc_desktop_control_drag').accessibilityText('拖动控制中心')
          .gesture(PanGesture({ distance: 6 }).onActionStart(() => {
            const rect = this.desktopPanelRect(); this.panelDragX = rect.x; this.panelDragY = rect.y;
            this.panelOffsetX = 0; this.panelOffsetY = 0; this.releaseInput();
          }).onActionUpdate((event: GestureEvent) => { this.moveDesktopPanel(event); })
            .onActionEnd(() => { this.finishPanelDrag(); })
            .onActionCancel(() => { this.refocusInput(); }))
        PxcIcon({ name: 'close', pixelSize: 20 }).padding(6).accessibilityText('关闭控制中心')
          .onClick(() => { this.panelVisible = false; this.refocusInput(); })
      }.width('100%')
      Scroll() {
        Column({ space: 10 }) {
          Text('屏幕切换').fontSize(12).fontColor('#8a9099').width('100%')
          if (this.screens.length === 0) {
            Text('正在获取屏幕列表…').fontSize(12).fontColor('#8a9099').width('100%')
          } else {
            Column({ space: 6 }) {
              ForEach(this.screens, (screen: PxcScreenItem) => {
                Text(`${screen.name}${screen.primary ? ' · 主屏幕' : ''}  ${screen.width}×${screen.height}`)
                  .fontSize(12).width('100%').height(34).padding({ left: 12, right: 12 }).borderRadius(8)
                  .fontColor('#1f2329')
                  .border({ width: 1, color: screen.index === this.activeScreen ? '#4f7cff' : '#eceef1' })
                  .backgroundColor(screen.index === this.activeScreen ? '#eef3ff' : '#ffffff')
                  .onClick(() => { this.doSwitchScreen(screen.index); })
              }, (screen: PxcScreenItem): string => `desktop-screen-${screen.index}`)
            }.width('100%')
          }
          Button('刷新屏幕列表', { type: ButtonType.Normal }).fontSize(12).height(30)
            .fontColor('#4f7cff').backgroundColor(Color.Transparent).borderRadius(8)
            .onClick(() => { pxc.requestRemoteScreens(); })
          this.ControlPanel()
          Text('解码加速').fontSize(12).fontColor('#8a9099').width('100%')
          Text('鸿蒙系统自动选择解码器').fontSize(13).width('100%').padding(10)
            .backgroundColor('#f2f3f5').borderRadius(8)
          Text(this.resolutionText || '等待视频状态…').fontSize(12).width('100%')
          Text(`${this.sessionStateText}${this.encoderText ? ' · ' + this.encoderText : ''}\n${this.statsText}`)
            .fontSize(11).fontColor('#8a9099').width('100%').padding(10).backgroundColor('#f7f8fa').borderRadius(8)
          if (this.noticeText !== '') {
            Text(this.noticeText).fontSize(12).fontColor(this.noticeIsError ? '#c82f42' : '#64748b').width('100%')
          }
          Row() {
            Text('仅观看').layoutWeight(1)
            Toggle({ type: ToggleType.Switch, isOn: this.viewOnly }).onChange((value: boolean) => {
              this.releaseInput(); this.viewOnly = value;
            })
          }.width('100%')
          Row({ space: 8 }) {
            Button('文件传输', { type: ButtonType.Normal }).fontSize(13).height(34).borderRadius(8)
              .fontColor('#1f2329').backgroundColor('#f2f3f5').layoutWeight(1).onClick(() => {
              this.releaseInput(); this.panelVisible = false;
              const param: PxcFilePageParam = { deviceName: this.deviceName, ownSession: false };
              this.pathStack.pushPath({ name: 'files', param: param });
            })
            PasteButton().fontSize(13).padding(10).enabled(!this.viewOnly && !this.sessionEnded)
              .onClick((_event: ClickEvent, result: PasteButtonOnClickResult) => {
                if (result === PasteButtonOnClickResult.SUCCESS) pxcClipboard.pasteAuthorized();
              })
          }.width('100%')
        }.width('100%').alignItems(HorizontalAlign.Start).padding({ bottom: 4 })
      }.width('100%').layoutWeight(1).scrollBar(BarState.Auto).align(Alignment.Top)
      Row({ space: 8 }) {
        Button(this.desktopFullscreen ? '退出全屏' : '全屏', { type: ButtonType.Normal })
          .fontSize(13).height(34).borderRadius(8).fontColor(Color.White).backgroundColor('#4f7cff').layoutWeight(1)
          .onClick(() => { this.toggleDesktopFullscreen(); })
        Button('断开', { type: ButtonType.Normal }).fontSize(13).height(34).borderRadius(8)
          .layoutWeight(1).fontColor(Color.White).backgroundColor('#e5484d')
          .onClick(() => { this.doDisconnect(); })
      }.width('100%')
    }.id('pxc_desktop_control_window').padding({ left: 18, right: 18, top: 14, bottom: 14 }).backgroundColor(Color.White)
      .borderRadius(14)
      .border({ width: 1, color: '#eceef1' }).shadow({ radius: 16, color: '#40000000', offsetY: 4 })
      .width(this.desktopPanelRect().width)
      .height(this.desktopPanelRect().height)
      .position({
        x: this.desktopPanelRect().x,
        y: this.desktopPanelRect().y
      })
  }

  @Builder
  DockButton(icon: string, label: string, enabled: boolean, action: () => void) {
    Column({ space: 3 }) {
      PxcIcon({ name: icon, pixelSize: 21, tint: enabled ? '#d8e5fa' : '#607086' })
      Text(label).fontSize(10).fontColor(enabled ? '#182333' : '#718092')
    }.layoutWeight(1).height(this.landscape ? 44 : 56).justifyContent(FlexAlign.Center).enabled(enabled).onClick(action)
  }

  @Builder
  RightRail() {
    Scroll() {
    Column({ space: 4 }) {
      this.DockButton('gear', '操作', true, (): void => { this.releaseInput(); this.panelVisible = !this.panelVisible; this.keyboardVisible = false; })
      this.DockButton('keyboard', '键盘', !this.viewOnly, (): void => { this.toggleKeyboard(); })
      this.DockButton('mouse', '鼠标', !this.viewOnly, (): void => {
        this.releaseInput(); this.mouseVisible = !this.mouseVisible;
        this.keyboardVisible = false; this.panelVisible = false;
        this.followMouse();
      })
      this.DockButton('file', '文件', true, (): void => {
        this.releaseInput(); this.mouseVisible = false; this.keyboardVisible = false;
        const param: PxcFilePageParam = { deviceName: this.deviceName, ownSession: false };
        this.pathStack.pushPath({ name: 'files', param: param });
      })
      this.DockButton('devices', '桌面', !this.viewOnly, (): void => { this.shortcut([0x01000022, 68]); })
      this.DockButton('grid', '窗口', !this.viewOnly, (): void => { this.shortcut(this.remotePlatform.toLowerCase().includes('linux') ? [0x01000022] : [0x01000022, 0x01000001]); })
      Column({ space: 4 }) {
        PxcIcon({ name: 'move', pixelSize: 20, tint: '#8eb4ed' }); Text('拖动').fontSize(10).fontColor('#8eb4ed')
      }.height(56).justifyContent(FlexAlign.Center).enabled(this.mouseVisible)
        .gesture(PanGesture().onActionStart(() => { this.mouseDragX = this.pointerX; this.mouseDragY = this.pointerY; })
          .onActionUpdate((event: GestureEvent) => {
            this.pointerX = Math.max(0, Math.min(1, this.mouseDragX + event.offsetX / Math.max(1, this.videoRectW)));
            this.pointerY = Math.max(0, Math.min(1, this.mouseDragY + event.offsetY / Math.max(1, this.videoRectH)));
            this.sendInput(pxcMouseMoveJson(this.pointerX, this.pointerY, Math.max(0, this.activeScreen)));
            this.followMouse();
          }))
    }.width(56).height(Math.max(236, this.railHeight - 8)).padding(6).backgroundColor('rgba(246,249,255,0.8)').backgroundBlurStyle(BlurStyle.COMPONENT_THIN).borderRadius(28).margin(4)
      .border({ width: 1, color: 'rgba(255,255,255,0.9)' })
    }.width(64).height('100%').align(Alignment.Top).scrollBar(BarState.Off)
  }

  @Builder
  MousePanel() {
    Column({ space: 4 }) {
      Row({ space: 3 }) {
        Column() {}.width(26).height(34).border({ width: 1.4, color: '#111111' })
          .backgroundColor(this.mousePressed.includes('left') ? '#3b82f6' : Color.Transparent)
          .borderRadius({ topLeft: 20, bottomLeft: 4 }).accessibilityText('鼠标左键')
          .onTouch((event: TouchEvent) => { this.virtualButton('left', event); })
        Column() {}.width(12).height(26).border({ width: 1.4, color: '#111111' }).borderRadius(6)
          .backgroundColor(this.mousePressed.includes('middle') ? '#3b82f6' : Color.Transparent)
          .accessibilityText('鼠标中键和滚轮').onTouch((event: TouchEvent) => { this.virtualButton('middle', event); })
          .gesture(PanGesture({ direction: PanDirection.Vertical }).onActionUpdate((event: GestureEvent) => {
            const steps: number = Math.trunc(event.offsetY / 24);
            const delta: number = steps - this.pointerWheel;
            if (delta !== 0) { this.sendInput(pxcWheelJson(-delta)); this.pointerWheel = steps; }
          }).onActionStart(() => { this.pointerWheel = 0; }).onActionEnd(() => { this.pointerWheel = 0; }))
        Column() {}.width(26).height(34).border({ width: 1.4, color: '#111111' })
          .backgroundColor(this.mousePressed.includes('right') ? '#3b82f6' : Color.Transparent)
          .borderRadius({ topRight: 20, bottomRight: 4 }).accessibilityText('鼠标右键')
          .onTouch((event: TouchEvent) => { this.virtualButton('right', event); })
      }.width('100%').justifyContent(FlexAlign.Center)
      Stack() {
        PxcIcon({ name: 'move', pixelSize: 20 }).hitTestBehavior(HitTestMode.None)
      }.width('100%').height(30).accessibilityText('滑动移动鼠标')
        .onTouch((event: TouchEvent) => { this.handleTouch(event, true); })
      Row() {
        PxcIcon({ name: 'pointer', pixelSize: 16 }).padding(3).borderRadius(8)
          .border({ width: this.dragHeld ? 1.5 : 0, color: '#111111' }).accessibilityText('切换左键按住拖拽')
          .onClick(() => {
            this.dragHeld = !this.dragHeld;
            this.sendInput(pxcMouseMoveJson(this.pointerX, this.pointerY, Math.max(0, this.activeScreen)));
            if (this.dragHeld) this.virtualButtons.add('left'); else this.virtualButtons.delete('left');
            this.mousePressed = Array.from(this.virtualButtons);
            this.sendInput(pxcMouseButtonJson('left', this.dragHeld));
          })
        Blank()
        PxcIcon({ name: 'close', pixelSize: 16 }).padding(3).accessibilityText('关闭虚拟鼠标')
          .onClick(() => { this.releaseInput(); this.mouseVisible = false; })
      }.width('100%')
    }.width(MOUSE_WIDTH).height(MOUSE_HEIGHT).padding(5).backgroundColor('rgba(244,248,255,0.85)')
      .backgroundBlurStyle(BlurStyle.COMPONENT_THIN).borderRadius(25)
      .border({ width: 1, color: 'rgba(255,255,255,0.7)' })
      .position({ x: this.mouseX, y: this.mouseY }).shadow({ radius: 12, color: '#50000000', offsetY: 3 })
  }
  @Builder
  KeyboardPanel() {
    Column({ space: 8 }) {
      Row({ space: 10 }) {
        Text('输入法').fontSize(13).fontColor(this.keyboardMode === 'system' ? '#74a8ff' : '#aab8ce').onClick(() => { this.changeKeyboard('system'); })
        Text('电脑键盘').fontSize(13).fontColor(this.keyboardMode === 'computer' ? '#74a8ff' : '#aab8ce').onClick(() => { this.changeKeyboard('computer'); })
        PasteButton().fontSize(13).fontColor(Color.White).backgroundColor('#334155')
          .padding({ left: 12, right: 12, top: 8, bottom: 8 }).enabled(!this.viewOnly && !this.sessionEnded)
          .onClick((_event: ClickEvent, result: PasteButtonOnClickResult) => {
            if (result === PasteButtonOnClickResult.SUCCESS) pxcClipboard.pasteAuthorized();
          })
        Blank(); PxcIcon({ name: 'close', pixelSize: 20 }).padding(4).backgroundColor('#edf3ff').borderRadius(14).onClick(() => { this.toggleKeyboard(); })
      }.width('100%').padding({ left: 16, right: 16, top: 10 })
      if (this.keyboardMode === 'computer') {
        PxcComputerKeyboard({ keyboard: this.inputMapper.keyboard, sticky: this.stickyModifiers, compact: this.keyboardCompact,
          onStickyChanged: (v: boolean): void => { this.stickyModifiers = v; pxcSettings.putBool('stickyModifiers', v); } })
      } else {
        Row({ space: 8 }) {
          TextInput({ text: this.systemText, placeholder: '使用系统输入法，完成后发送到远端' }).id('pxc_ime_input')
            .fontColor(Color.White).placeholderColor(Color.White).backgroundColor('#334155').caretColor(Color.White)
            .layoutWeight(1).height(42).onChange((text: string) => { this.systemText = text; }).onSubmit(() => { this.sendText(); })
          Button('发送').height(38).fontSize(13).enabled(this.systemText !== '').onClick(() => { this.sendText(); })
        }.width('100%').padding({ left: 12, right: 12, bottom: 10 })
      }
    }.width('100%').height(this.keyboardMode === 'computer' ? (this.keyboardCompact ? 244 : 290) : 100).backgroundColor('#1e293a')
  }

  build() {
    NavDestination() {
      Column() {
        Row() {
        Stack() {
        XComponent({ id: 'pxc_remote_video', type: XComponentType.SURFACE, libraryname: '', controller: this.xcc })
          .onLoad(() => {
            this.onVideoSurfaceLoad();
          })
          .onSizeChange((oldSize: SizeOptions, newSize: SizeOptions) => {
            this.onBoxSizeChange(newSize);
          })
          .width('100%').height('100%').hitTestBehavior(HitTestMode.None)
        Column() {}
          .backgroundColor(Color.Transparent)
          .onTouch((event: TouchEvent) => {
            this.handleTouch(event);
          })
          .onMouse((event: MouseEvent) => {
            this.inputMapper.onMouseEvent(event);
          })
          .onAxisEvent((event: AxisEvent) => {
            this.inputMapper.onAxisEvent(event);
          })
          .onKeyEvent((event: KeyEvent) => {
            this.inputMapper.onKeyEvent(event);
          })
          .focusable(true)
          .id(INPUT_FOCUS_KEY)
          .width('100%')
          .height('100%')
        if (this.clickVisible && !this.viewOnly) {
          Column() {}.width(22).height(22).backgroundColor('rgba(59,130,246,0.28)').borderRadius(11).border({ width: 2, color: '#3b82f6' })
            .position({ x: this.videoRectX + this.clickX * this.videoRectW - 11,
              y: this.videoRectY + this.clickY * this.videoRectH - 11 }).hitTestBehavior(HitTestMode.None)
        }
        if ((this.pointerMode || this.mouseVisible) && !this.viewOnly) {
          PxcIcon({ name: 'pointer', pixelSize: 22, tint: Color.White })
            .position({ x: this.videoRectX + this.pointerX * this.videoRectW, y: this.videoRectY + this.pointerY * this.videoRectH })
            .hitTestBehavior(HitTestMode.None)
        }
        if (this.mouseVisible && !this.viewOnly) { this.MousePanel() }

        if (this.panelVisible && !this.desktopControls) {
          Scroll() {
            Column({ space: 12 }) {
              Row() {
                Text('操作与显示').fontSize(18).fontWeight(FontWeight.Bold).layoutWeight(1)
                PxcIcon({ name: 'close' }).onClick(() => { this.panelVisible = false; this.refocusInput(); })
              }.width('100%')
              Text(this.deviceName).fontSize(15).fontWeight(FontWeight.Medium).width('100%')
              Text(this.sessionStateText + ' · ' + this.statsText).fontSize(11).fontColor('#7c899c').width('100%')
              if (this.noticeText !== '') {
                Text(this.noticeText).fontSize(12).fontColor(this.noticeIsError ? '#c82f42' : '#64748b').width('100%')
              }
              Button('断开连接').fontColor(Color.White).backgroundColor('#e54855').width('100%').onClick(() => { this.doDisconnect(); })
              Row() { Text('仅观看').layoutWeight(1); Toggle({ type: ToggleType.Switch, isOn: this.viewOnly }).onChange((v: boolean) => {
                this.releaseInput(); this.viewOnly = v; this.keyboardVisible = false; this.mouseVisible = false;
              }) }.width('100%')
              Row() { Text('鼠标指针模式').layoutWeight(1); Toggle({ type: ToggleType.Switch, isOn: this.pointerMode }).enabled(!this.viewOnly).onChange((v: boolean) => {
                this.releaseInput(); this.pointerMode = v;
              }) }.width('100%')
              Button('切换横竖屏').width('100%').onClick(() => { this.rotateDisplay(); })
              this.ControlPanel()
            }.width('100%').padding(14)
          }.width('88%').constraintSize({ maxWidth: 360 }).height('100%').backgroundColor('#f3f6fa').align(Alignment.End)
        }

        if (!this.hasVideo && this.noticeText === '') {
          Column({ space: 10 }) {
            LoadingProgress()
              .width(28)
              .height(28)
              .color(Color.White)
            Text('等待视频流…')
              .fontSize(13)
              .fontColor('rgba(255,255,255,0.85)')
          }
          .hitTestBehavior(HitTestMode.None)
        }

        if (this.noticeText !== '' && this.noticeIsError) {
          Text(this.noticeText)
            .fontSize(13)
            .fontColor(Color.White)
            .padding({ left: 14, right: 14, top: 8, bottom: 8 })
            .borderRadius(10)
            .backgroundColor(this.noticeIsError ? 'rgba(180,40,40,0.85)' : 'rgba(0,0,0,0.6)')
            .margin({ bottom: 120 })
            .hitTestBehavior(HitTestMode.None)
        }
        if (this.desktopControls) {
          if (this.panelVisible) {
            Column() {}.width('100%').height('100%').backgroundColor(Color.Transparent)
              .onClick(() => { this.panelVisible = false; this.refocusInput(); })
            this.DesktopControlWindow()
          }
          this.DesktopControlButton()
        }
        }.height('100%').layoutWeight(1).clip(true)
        if (!this.desktopControls) { this.RightRail() }
        }.width('100%').layoutWeight(1)
        if (this.keyboardVisible && !this.viewOnly) { this.KeyboardPanel() }
      }
      .width('100%')
      .height('100%')
      .backgroundColor(Color.Black)
      .expandSafeArea([SafeAreaType.SYSTEM], [SafeAreaEdge.TOP, SafeAreaEdge.BOTTOM])
      .onSizeChange((oldSize: SizeOptions, newSize: SizeOptions) => {
        this.keyboardCompact = Number(newSize.height) < 600;
        this.landscape = Number(newSize.width) > Number(newSize.height);
        this.desktopControls = pxcUsesDesktopControls(deviceInfo.deviceType, Number(newSize.width));
      })
    }
    .hideTitleBar(true)
    .onReady((ctx: NavDestinationContext) => {
      this.pathStack = ctx.pathStack;
      const p: PxcRemotePageParam | undefined =
        ctx.pathInfo.param as PxcRemotePageParam | undefined;
      if (p !== undefined && p.deviceName !== undefined && p.deviceName !== '') {
        this.deviceName = p.deviceName;
        this.viewOnly = p.viewOnly;
        this.remotePlatform = p.platform;
      }
    })
    .onShown(() => {
      this.refocusInput();
    })
    .onBackPressed((): boolean => { this.doDisconnect(); return true; })
  }
}
