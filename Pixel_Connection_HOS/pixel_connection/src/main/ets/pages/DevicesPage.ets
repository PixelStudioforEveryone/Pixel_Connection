import pxc from 'libpxc_hos.so';
import { pxcLoginPersistence } from '../common/PxcLoginPersistence';
import { BusinessError } from '@kit.BasicServicesKit';
import { pasteboard } from '@kit.BasicServicesKit';
import { fileIo } from '@kit.CoreFileKit';
import { PxcFilePageParam } from './FilesPage';
import { pxcEmitter } from '../common/PxcEmitter';
import { pxcSettings } from '../common/PxcSettings';
import { PxcIcon, pxcWallpaper, pxcSameDevice, pxcCanControl } from '../common/PxcUi';
import { SettingsContent } from './SettingsPage';
import { PxcDeviceItem, PxcDevicesUpdatedJson, PxcErrorJson, PxcIdentityJson,
  PxcRemotePageParam, PxcSessionStateJson, PxcSignalingOnlineJson, PxcWallpaperJson } from '../model/PxcModels';

@Component
export struct DevicesPage {
  @State username: string = '';
  @State localEnrolled: boolean = false;
  @State localDeviceId: string = '';
  @State localConnKey: string = '';
  @State devices: Array<PxcDeviceItem> = [];
  @State refreshing: boolean = false;
  @State signalOnline: boolean = false;
  @State message: string = '';
  @State tab: string = '设备';
  @State selectedId: string = '';
  @State wide: boolean = false;
  @State wallpaperRevision: number = 0;
  @State showKeyInput: boolean = false;
  @State keyInputText: string = '';
  @State partnerId: string = '';
  @State recentIds: Array<string> = [];
  @State keyTargetName: string = '';
  @State sessionBusy: boolean = false;
  @State menuVisible: boolean = false;
  private targetId: string = '';
  private targetName: string = '';
  private intent: string = '';
  private fileOnly: boolean = false;
  private viewOnly: boolean = false;
  private remotePushed: boolean = false;
  private previewTimer: number = -1;
  private previewQueue: Array<PxcDeviceItem> = [];
  private previewAttempted: Set<string> = new Set<string>();
  pathStack: NavPathStack = new NavPathStack();
  onLoggedOut: () => void = (): void => {};
  onBackHandlerReady: (handler: () => boolean) => void = (): void => {};

  private handlePageBack = (): boolean => {
    if (this.showKeyInput) { this.showKeyInput = false; this.keyInputText = ''; return true; }
    if (this.menuVisible) { this.menuVisible = false; return true; }
    if (this.selectedId !== '') { this.selectedId = ''; return true; }
    if (this.tab !== '设备') { this.selectTab('设备'); return true; }
    return false;
  };

  aboutToAppear(): void {
    this.onBackHandlerReady(this.handlePageBack);
    pxcEmitter.on('devicesUpdated', this.handleDevicesUpdated);
    pxcEmitter.on('deviceListFailed', this.handleDeviceListFailed);
    pxcEmitter.on('deviceEnrolled', this.handleDeviceEnrolled);
    pxcEmitter.on('connectionKeyChanged', this.handleConnectionKeyChanged);
    pxcEmitter.on('deviceEnrollFailed', this.handleDeviceListFailed);
    pxcEmitter.on('signalingOnline', this.handleSignalingOnline);
    pxcEmitter.on('sessionState', this.handleSessionState);
    pxcEmitter.on('keyVerified', this.handleKeyVerified);
    pxcEmitter.on('wallpaperReady', this.handleWallpaper);
    pxcEmitter.on('appBackground', this.handleBackground);
    try { this.recentIds = JSON.parse(pxcSettings.getScopedString('recentIds', '[]')) as Array<string>; } catch (_) {}
    pxc.refreshDevices();
  }

  aboutToDisappear(): void {
    this.onBackHandlerReady((): boolean => false);
    pxcEmitter.off('devicesUpdated', this.handleDevicesUpdated);
    pxcEmitter.off('deviceListFailed', this.handleDeviceListFailed);
    pxcEmitter.off('deviceEnrolled', this.handleDeviceEnrolled);
    pxcEmitter.off('connectionKeyChanged', this.handleConnectionKeyChanged);
    pxcEmitter.off('deviceEnrollFailed', this.handleDeviceListFailed);
    pxcEmitter.off('signalingOnline', this.handleSignalingOnline);
    pxcEmitter.off('sessionState', this.handleSessionState);
    pxcEmitter.off('keyVerified', this.handleKeyVerified);
    pxcEmitter.off('wallpaperReady', this.handleWallpaper);
    pxcEmitter.off('appBackground', this.handleBackground);
    this.clearPreviewTimer();
  }

  private handleDevicesUpdated = (json: string): void => {
    const d: PxcDevicesUpdatedJson = JSON.parse(json) as PxcDevicesUpdatedJson;
    this.devices = d.devices ?? []; this.refreshing = false;
    this.previewQueue = this.devices.filter((dev: PxcDeviceItem): boolean => {
      if (!pxcCanControl(dev, this.localDeviceId) || this.previewAttempted.has(dev.deviceId) ||
        pxcSettings.getConnKey(dev.deviceId) === '') return false;
      try { return !fileIo.accessSync(pxc.cachedWallpaper(dev.deviceId)); } catch (_) { return true; }
    });
    this.nextPreview();
  };
  private handleDeviceListFailed = (json: string): void => {
    this.refreshing = false; this.message = (JSON.parse(json) as PxcErrorJson).error ?? '操作失败';
  };
  private handleDeviceEnrolled = (json: string): void => {
    const d: PxcIdentityJson = JSON.parse(json) as PxcIdentityJson;
    this.localEnrolled = true; this.localDeviceId = d.deviceId; this.localConnKey = d.connectionKey;
    this.message = '已添加本机，本机只作为控制端使用';
  };
  private handleConnectionKeyChanged = (json: string): void => {
    const d: PxcIdentityJson = JSON.parse(json) as PxcIdentityJson;
    this.localDeviceId = d.deviceId; this.localConnKey = d.connectionKey;
  };
  private handleSignalingOnline = (json: string): void => {
    this.signalOnline = (JSON.parse(json) as PxcSignalingOnlineJson).online;
  };
  private handleSessionState = (json: string): void => {
    const d: PxcSessionStateJson = JSON.parse(json) as PxcSessionStateJson;
    if (!this.targetId || !pxcSameDevice(d.deviceId, this.targetId)) return;
    if (d.state === 'authenticated' && (this.intent === 'control' || this.intent === 'file') && !this.remotePushed) {
      this.remotePushed = true; this.sessionBusy = true;
      const device: PxcDeviceItem | undefined = this.devices.find((dev: PxcDeviceItem): boolean => pxcSameDevice(dev.deviceId, this.targetId));
      const param: PxcRemotePageParam = { deviceName: this.targetName, deviceId: this.targetId, viewOnly: this.viewOnly, platform: device?.platform ?? '' };
      this.recentIds = [this.targetId].concat(this.recentIds.filter((id: string): boolean => !pxcSameDevice(id, this.targetId))).slice(0, 8);
      pxcSettings.putScopedString('recentIds', JSON.stringify(this.recentIds));
      const fileParam: PxcFilePageParam = { deviceName: this.targetName, ownSession: true };
      this.pathStack.pushDestination({ name: this.fileOnly ? 'files' : 'remote', param: this.fileOnly ? fileParam : param }).catch((err: BusinessError) => {
        this.message = '无法打开远控页面'; this.remotePushed = false; pxc.disconnect();
      });
    } else if (d.state === 'closed' || d.state === 'rejected' || d.state === 'p2p_timeout') {
      const wasPreview: boolean = this.intent === 'preview';
      this.intent = ''; this.targetId = ''; this.sessionBusy = false; this.remotePushed = false;
      this.clearPreviewTimer();
      if (!wasPreview) this.message = d.reason ?? (d.state === 'rejected' ? '连接被拒绝，请检查设备状态或连接密钥' : '会话已结束');
      if (wasPreview) setTimeout(() => { this.nextPreview(); }, 250);
    } else if (this.intent === 'control') {
      this.message = d.state === 'authenticating' ? '正在验证连接密钥…' : '正在建立连接…';
    }
  };
  private handleKeyVerified = (json: string): void => {
    const d: PxcIdentityJson = JSON.parse(json) as PxcIdentityJson;
    if (pxcSettings.getBool('rememberKeys', true)) pxcSettings.setConnKey(d.deviceId, d.connectionKey);
  };
  private handleWallpaper = (json: string): void => {
    const d: PxcWallpaperJson = JSON.parse(json) as PxcWallpaperJson;
    this.wallpaperRevision++;
    if (this.intent === 'preview' && pxcSameDevice(d.deviceId, this.targetId)) pxc.disconnect();
  };
  private handleBackground = (): void => {
    this.previewQueue = [];
    if (this.intent === 'preview') pxc.disconnect();
  };
  private clearPreviewTimer(): void {
    if (this.previewTimer >= 0) clearTimeout(this.previewTimer);
    this.previewTimer = -1;
  }
  private nextPreview(): void {
    if (this.intent !== '' || !pxcSettings.getBool('previewEnabled', true) || this.previewQueue.length === 0) return;
    const dev: PxcDeviceItem = this.previewQueue.shift()!;
    this.previewAttempted.add(dev.deviceId);
    this.intent = 'preview'; this.targetId = dev.deviceId; this.targetName = dev.name;
    pxc.connectToDevice(dev.deviceId, pxcSettings.getConnKey(dev.deviceId));
    this.previewTimer = setTimeout(() => { if (this.intent === 'preview') pxc.disconnect(); }, 12000);
  }
  private doLogout(): void {
    this.previewQueue = []; this.clearPreviewTimer(); pxc.logout();
    pxcLoginPersistence.clear();
    this.pathStack.clear(); this.onLoggedOut();
  }
  private selectTab(tab: string): void { this.tab = tab; this.selectedId = ''; this.menuVisible = false; }
  private selected(): PxcDeviceItem {
    const found: PxcDeviceItem | undefined = this.devices.find((d: PxcDeviceItem): boolean => d.deviceId === this.selectedId);
    return found ?? { deviceId: this.selectedId, name: this.selectedId, platform: '', online: false, publicIp: '', lastSeen: '' };
  }
  private connectDevice(dev: PxcDeviceItem, onlyView: boolean = false, files: boolean = false): void {
    if (!pxcCanControl(dev, this.localDeviceId)) { this.message = '设备离线、本机或仅支持控制其他设备'; return; }
    if (this.intent === 'control' || this.intent === 'file') return;
    this.fileOnly = files;
    this.previewQueue = []; this.viewOnly = onlyView;
    const key: string = pxcSettings.getBool('rememberKeys', true) ? pxcSettings.getConnKey(dev.deviceId) : '';
    if (this.intent === 'preview') {
      this.intent = ''; this.targetId = ''; this.clearPreviewTimer(); pxc.disconnect();
    }
    this.targetId = dev.deviceId; this.targetName = dev.name;
    if (key !== '') setTimeout(() => { this.startConnection(key); }, 250);
    else { this.keyTargetName = dev.name; this.keyInputText = ''; this.showKeyInput = true; }
  }
  private startConnection(key: string): void {
    if (!this.targetId) return;
    this.intent = this.fileOnly ? 'file' : 'control'; this.sessionBusy = true; this.remotePushed = false;
    this.message = '正在建立安全连接…'; pxc.connectToDevice(this.targetId, key);
  }
  private connectPartner(): void {
    const id: string = this.partnerId.trim();
    if (!id || pxcSameDevice(id, this.localDeviceId)) { this.message = '请输入其他电脑的设备 ID'; return; }
    const known: PxcDeviceItem | undefined = this.devices.find((d: PxcDeviceItem): boolean => pxcSameDevice(d.deviceId, id));
    this.connectDevice(known ?? { deviceId: id, name: id, platform: 'computer', online: true, publicIp: '', lastSeen: '' });
  }

  @Builder
  Wallpaper(id: string) {
    ForEach([this.wallpaperRevision], (revision: number) => {
      Image(pxcWallpaper(id)).objectFit(ImageFit.Cover).width('100%').height('100%')
    }, (revision: number): string => `${id}-${revision}`)
  }
  @Builder
  DeviceCard(dev: PxcDeviceItem) {
    Row({ space: 16 }) {
      Stack({ alignContent: Alignment.BottomStart }) {
        this.Wallpaper(dev.deviceId)
        Text(dev.online ? '● 在线' : '● 离线').fontSize(10).fontColor(dev.online ? '#91ffd7' : '#d7e1ed')
          .padding({ left: 8, right: 8, top: 3, bottom: 3 }).backgroundColor('rgba(18,30,45,0.7)').borderRadius(7).margin(8)
      }.width('36%').height(92).borderRadius(10).clip(true)
      Column({ space: 10 }) {
        Text(pxcSameDevice(dev.deviceId, this.localDeviceId) ? `${dev.name}（本机）` : dev.name)
          .fontSize(17).fontWeight(FontWeight.Medium).fontColor('#192b40').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
        Row({ space: 7 }) { PxcIcon({ name: 'devices', pixelSize: 17, tint: '#7c899c' }); Text(dev.platform || '电脑').fontSize(12).fontColor('#7c899c') }
      }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      PxcIcon({ name: 'chevron', pixelSize: 18 })
    }.width('100%').padding(12).backgroundColor('rgba(255,255,255,0.88)').backgroundBlurStyle(BlurStyle.COMPONENT_THIN).borderRadius(18)
      .border({ width: 1, color: 'rgba(255,255,255,0.8)' }).shadow({ radius: 8, color: '#080f2544', offsetY: 3 })
      .onClick(() => { this.selectedId = dev.deviceId; this.menuVisible = false; })
  }
  @Builder
  DeviceList() {
    Column({ space: 18 }) {
      Row() {
        Text('可控设备').fontSize(28).fontWeight(FontWeight.Bold).layoutWeight(1)
        Button('全部设备', { type: ButtonType.Normal }).height(34).fontSize(12).borderRadius(17).fontColor('#52647c').backgroundColor('#e8eef5')
          .onClick(() => { this.refreshing = true; pxc.refreshDevices(); })
      }.width('100%')
      if (!this.localEnrolled) {
        Button('添加本机到账号').width('100%').onClick(() => { pxc.addThisDevice(); })
      }
      Scroll() {
        Column({ space: 12 }) {
          ForEach([true, false], (online: boolean) => {
            Text(`${online ? '在线设备' : '离线设备'} ${this.devices.filter((d: PxcDeviceItem): boolean => d.online === online).length}`)
              .fontSize(13).fontColor('#7c899c').width('100%').margin({ top: 6 })
            ForEach(this.devices.filter((d: PxcDeviceItem): boolean => d.online === online), (dev: PxcDeviceItem) => {
              this.DeviceCard(dev)
            }, (dev: PxcDeviceItem): string => dev.deviceId)
          })
          if (this.devices.length === 0) { Text(this.refreshing ? '正在获取设备…' : '暂无设备，登录其他客户端并添加设备后会出现在这里').fontColor('#7c899c').padding(28) }
        }.width('100%').padding({ bottom: 20 })
      }.layoutWeight(1).width('100%').align(Alignment.Top).scrollBar(BarState.Auto)
    }.width('100%').height('100%').padding(20)
  }
  @Builder
  DetailTool(icon: string, title: string, enabled: boolean, action: () => void) {
    Column({ space: 8 }) {
      PxcIcon({ name: icon, pixelSize: 24, tint: enabled ? '#24364b' : '#acb6c5' })
      Text(title).fontSize(12).fontColor(enabled ? '#24364b' : '#acb6c5')
    }.layoutWeight(1).padding({ top: 16, bottom: 16 }).enabled(enabled).onClick(action)
  }
  @Builder
  DeviceDetail() {
    Column({ space: 18 }) {
      Row({ space: 10 }) {
        PxcIcon({ name: 'back' }).onClick(() => { this.selectedId = ''; })
        Text(this.selected().online ? '● 在线' : '● 离线').fontSize(11).fontColor(this.selected().online ? '#30c49a' : '#a5afbc')
          .padding({ left: 12, right: 12, top: 6, bottom: 6 }).backgroundColor('#20272e').borderRadius(16)
        Text(this.selected().name).fontSize(22).fontWeight(FontWeight.Bold).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis }).layoutWeight(1)
        PxcIcon({ name: 'more' }).onClick(() => { this.menuVisible = !this.menuVisible; })
      }.width('100%')
      if (this.menuVisible) {
        Row({ space: 12 }) {
          Text('设备 ID：' + this.selectedId).fontSize(12).layoutWeight(1)
          Button('复制 ID').height(32).fontSize(12).onClick(() => { pasteboard.getSystemPasteboard().setData(pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, this.selectedId)); })
          Button('忘记密钥').height(32).fontSize(12).onClick(() => { pxcSettings.setConnKey(this.selectedId, ''); this.message = '已忘记该设备连接密钥'; })
        }.width('100%').padding(12).backgroundColor(Color.White).borderRadius(12)
      }
      Scroll() {
        Column({ space: 20 }) {
          Column() {
            Stack({ alignContent: Alignment.Bottom }) {
              this.Wallpaper(this.selectedId)
              Row() {
                Text('进入桌面').fontSize(20).fontWeight(FontWeight.Medium).fontColor(Color.White)
                Blank(); PxcIcon({ name: 'chevron' }).padding(5).backgroundColor('rgba(255,255,255,0.85)').borderRadius(18)
              }.width('100%').padding(22).backgroundColor('rgba(16,25,40,0.58)')
                .opacity(pxcCanControl(this.selected(), this.localDeviceId) && !this.sessionBusy ? 1 : 0.45)
            }.width('100%').aspectRatio(16 / 9).clip(true)
              .enabled(pxcCanControl(this.selected(), this.localDeviceId) && !this.sessionBusy)
              .onClick(() => { this.connectDevice(this.selected()); })
            Row() {
              this.DetailTool('file', '文件传输', pxcCanControl(this.selected(), this.localDeviceId), (): void => { this.connectDevice(this.selected(), false, true); })
              this.DetailTool('view', '观看模式', pxcCanControl(this.selected(), this.localDeviceId) && !this.sessionBusy,
                (): void => { this.connectDevice(this.selected(), true); })
              this.DetailTool('keyboard', '终端', false, (): void => {})
              this.DetailTool('grid', '更多', true, (): void => { this.menuVisible = !this.menuVisible; })
            }.width('100%').backgroundColor(Color.White)
          }.width('100%').borderRadius(18).clip(true)
          Text('快速启动').fontSize(16).fontWeight(FontWeight.Medium).width('100%')
          Column({ space: 8 }) {
            PxcIcon({ name: 'devices', pixelSize: 30, tint: '#96a5b8' })
            Text(pxcCanControl(this.selected(), this.localDeviceId) ? '连接后使用键盘和鼠标操作远端应用' : '该设备当前无法进入桌面')
              .fontSize(13).fontColor('#7c899c').textAlign(TextAlign.Center)
          }.width('100%').height(144).justifyContent(FlexAlign.Center).borderRadius(18)
            .border({ width: 1, color: '#c9d2de', style: BorderStyle.Dashed })
        }.width('100%').padding({ bottom: 20 })
      }.width('100%').layoutWeight(1).align(Alignment.Top).scrollBar(BarState.Auto)
    }.width('100%').height('100%').padding(20)
  }
  @Builder
  Assistance() {
    Scroll() {
      Column({ space: 20 }) {
        Text('远程协助').fontSize(28).fontWeight(FontWeight.Bold).width('100%')
        Column({ space: 12 }) {
          Text('本设备').fontSize(18).fontWeight(FontWeight.Medium).width('100%')
          Text('HarmonyOS 控制端').fontSize(13).fontColor('#7c899c').width('100%')
          Text(this.localDeviceId || '尚未添加本机').fontSize(22).fontWeight(FontWeight.Medium).width('100%')
          Text('本设备不接受远程桌面控制。请在被控电脑的「开始协助」页面查看 ID 和连接密钥。').fontSize(13).fontColor('#7c899c').width('100%')
        }.padding(20).width('100%').backgroundColor(Color.White).borderRadius(18)
        Column({ space: 16 }) {
          Text('远控伙伴设备').fontSize(18).fontWeight(FontWeight.Medium).width('100%')
          TextInput({ text: this.partnerId, placeholder: '伙伴的设备 ID' }).onChange((v: string) => { this.partnerId = v; })
          Button('连接').width('100%').enabled(this.partnerId.trim() !== '' && !this.sessionBusy).onClick(() => { this.connectPartner(); })
          Row() {
            Text('最近连接').fontSize(14).layoutWeight(1)
            Text('清除记录').fontSize(12).fontColor('#7c899c').onClick(() => { this.recentIds = []; pxcSettings.putScopedString('recentIds', '[]'); })
          }.width('100%')
          ForEach(this.recentIds, (id: string) => {
            Text(id).fontSize(14).fontColor('#4385ff').width('100%').padding(10).backgroundColor('#f3f6fa').borderRadius(8)
              .onClick(() => { this.partnerId = id; })
          })
        }.padding(20).width('100%').backgroundColor(Color.White).borderRadius(18)
      }.padding(20).width('100%')
    }.width('100%').height('100%').align(Alignment.Top).scrollBar(BarState.Auto)
  }
  @Builder
  NavigationItem(name: string, icon: string) {
    Column({ space: 5 }) {
      PxcIcon({ name: icon, tint: this.tab === name ? '#4385ff' : '#7c899c' })
      Text(name).fontSize(11).fontColor(this.tab === name ? '#4385ff' : '#7c899c')
    }.layoutWeight(1).height(58).justifyContent(FlexAlign.Center).onClick(() => { this.selectTab(name); })
  }
  @Builder
  SidebarItem(name: string, icon: string) {
    Row({ space: 12 }) {
      PxcIcon({ name: icon, tint: this.tab === name ? '#4385ff' : '#52647c' })
      Text(name).fontSize(15).fontColor('#24364b')
    }.width('100%').padding(14).borderRadius(12).backgroundColor(this.tab === name && this.selectedId === '' ? '#e5eeff' : Color.Transparent)
      .onClick(() => { this.selectTab(name); })
  }
  build() {
    Stack() {
      Column() {
      Row() {
        if (this.wide) {
          Column({ space: 12 }) {
            Row({ space: 10 }) { Image($r('app.media.pxc_app_icon')).width(28).height(28); Text('Pixel远程').fontSize(16).fontWeight(FontWeight.Bold) }.width('100%').padding({ top: 16, bottom: 16 })
            Text('我的设备').fontSize(12).fontColor('#7c899c').width('100%')
            Scroll() {
              Column({ space: 6 }) {
                ForEach(this.devices, (dev: PxcDeviceItem) => {
                  Row({ space: 10 }) { PxcIcon({ name: 'devices', pixelSize: 20 }); Text(dev.name).maxLines(1).fontSize(14).layoutWeight(1); Text('●').fontColor(dev.online ? '#30c49a' : '#a6b0bf').fontSize(10) }
                    .width('100%').padding(12).borderRadius(10).backgroundColor(this.selectedId === dev.deviceId ? '#e8eef7' : Color.Transparent)
                    .onClick(() => { this.tab = '设备'; this.selectedId = dev.deviceId; })
                })
              }.width('100%')
            }.width('100%').constraintSize({ maxHeight: 280 })
            this.SidebarItem('设备', 'grid')
            this.SidebarItem('远程协助', 'assist')
            Blank(); this.SidebarItem('设置', 'gear')
          }.width(232).height('100%').padding(16).backgroundColor(Color.White)
        }
        Column() {
          if (this.tab === '设置') { SettingsContent({ username: this.username, allowLogout: true, onLogout: (): void => { this.doLogout(); } }) }
          else if (this.tab === '远程协助') { this.Assistance() }
          else if (this.selectedId !== '') { this.DeviceDetail() }
          else { this.DeviceList() }
        }.layoutWeight(1).height('100%')
      }.width('100%').layoutWeight(1)
      Column() {
        Text(this.message || (this.signalOnline ? '● 信令在线' : '○ 信令未连接')).fontSize(11).fontColor('#7c899c').maxLines(1).padding(6).width('100%')
        if (!this.wide) {
          Row() { this.NavigationItem('设备', 'devices'); this.NavigationItem('远程协助', 'assist'); this.NavigationItem('设置', 'gear') }
            .width('calc(100% - 24vp)').height(64).backgroundColor('rgba(255,255,255,0.8)').backgroundBlurStyle(BlurStyle.COMPONENT_THIN)
            .borderRadius(32).padding({ bottom: 4 }).margin({ left: 12, right: 12, bottom: 6 })
            .border({ width: 1, color: 'rgba(255,255,255,0.85)' }).shadow({ radius: 14, color: '#14253650', offsetY: 2 })
        }
      }.width('100%').height(this.wide ? 28 : 92)
      }.width('100%').height('100%')
      if (this.showKeyInput) {
        Column().width('100%').height('100%').backgroundColor('rgba(10,23,40,0.4)').onClick(() => { this.showKeyInput = false; })
        Column({ space: 16 }) {
          PxcIcon({ name: 'shield', pixelSize: 32, tint: '#4385ff' })
          Text(`连接 ${this.keyTargetName}`).fontSize(19).fontWeight(FontWeight.Medium)
          TextInput({ text: this.keyInputText, placeholder: '请输入电脑端的连接密钥' }).type(InputType.Password).onChange((v: string) => { this.keyInputText = v; })
          Row({ space: 10 }) {
            Button('取消').layoutWeight(1).backgroundColor('#9aa8be').onClick(() => { this.showKeyInput = false; })
            Button('安全连接').layoutWeight(1).enabled(this.keyInputText.trim() !== '').onClick(() => { this.showKeyInput = false; this.startConnection(this.keyInputText.trim()); })
          }.width('100%')
        }.width('88%').height(270).constraintSize({ maxWidth: 420 }).padding(24).backgroundColor(Color.White).borderRadius(22)
      }
    }.width('100%').height('100%').backgroundColor('#f3f6fa').onSizeChange((oldSize: SizeOptions, size: SizeOptions) => { this.wide = Number(size.width) >= 840; })
  }
}
