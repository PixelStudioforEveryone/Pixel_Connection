import pxc from 'libpxc_hos.so';
import { pxcLoginPersistence } from '../common/PxcLoginPersistence';
import { pxcServerDescription } from '../common/PxcServerStatus';
import { pxcSettings, PXC_DEFAULT_API, PXC_DEFAULT_WS, PXC_DEFAULT_DEVICE_NAME } from '../common/PxcSettings';
import { PxcIcon } from '../common/PxcUi';
import { pxcEmitter } from '../common/PxcEmitter';
import { PxcIdentityJson } from '../model/PxcModels';

@Builder
export function SettingsPageBuilder(): void { SettingsPage() }
@Component
export struct SettingsPage {
  build() { NavDestination() { SettingsContent() }.title('设置') }
}

@Component
export struct SettingsContent {
  @State section: string = '常规';
  @State apiUrl: string = '';
  @State serverDescription: string = '正在确认服务器来源…';
  @State serverCheckedApi: string = '';
  private serverProbe: number = 0;

  private async checkServer(): Promise<void> {
    const generation = ++this.serverProbe;
    const api = this.apiUrl.trim();
    const description = await pxcServerDescription(api);
    if (generation === this.serverProbe) { this.serverDescription = description; this.serverCheckedApi = api; }
  }
  @State wsUrl: string = '';
  @State stunUrl: string = '';
  @State turnUrl: string = '';
  @State turnUser: string = '';
  @State turnPass: string = '';
  @State deviceName: string = '';
  @State savedHint: string = '';
  @State rememberKeys: boolean = true;
  @State confirmExit: boolean = true;
  @State disconnectBackground: boolean = false;
  @State previewEnabled: boolean = true;
  @State pointerMode: boolean = false;
  @State stickyModifiers: boolean = false;
  @State keyboardMode: string = 'system';
  @State localDeviceId: string = '';
  @State localPassword: string = '';
  @State showLocalPassword: boolean = false;
  @State editingPassword: boolean = false;
  @State newPassword: string = '';
  @State confirmPassword: string = '';
  @State passwordHint: string = '';
  @State passwordError: boolean = false;
  @Prop username: string = '';
  @Prop allowLogout: boolean = false;
  onLogout: () => void = (): void => {};

  aboutToAppear(): void {
    const identity = pxc.getLocalIdentity();
    this.localDeviceId = identity.deviceId; this.localPassword = identity.connectionKey;
    pxcEmitter.on('connectionKeyChanged', this.handleIdentityChanged);
    pxcEmitter.on('deviceEnrolled', this.handleIdentityChanged);
    this.apiUrl = pxcSettings.getString('apiUrl', PXC_DEFAULT_API);
    this.checkServer();
    this.wsUrl = pxcSettings.getString('wsUrl', PXC_DEFAULT_WS);
    this.stunUrl = pxcSettings.getString('stunUrl', '');
    this.turnUrl = pxcSettings.getString('turnUrl', '');
    this.turnUser = pxcSettings.getString('turnUser', ''); this.turnPass = pxcSettings.getString('turnPass', '');
    this.deviceName = pxcSettings.getString('deviceName', PXC_DEFAULT_DEVICE_NAME);
    this.rememberKeys = pxcSettings.getBool('rememberKeys', true); this.confirmExit = pxcSettings.getBool('confirmExit', true);
    this.disconnectBackground = pxcSettings.getBool('disconnectBackground', false);
    this.previewEnabled = pxcSettings.getBool('previewEnabled', true); this.pointerMode = pxcSettings.getBool('pointerMode', false);
    this.stickyModifiers = pxcSettings.getBool('stickyModifiers', false); this.keyboardMode = pxcSettings.getString('keyboardMode', 'system');
  }

  aboutToDisappear(): void {
    ++this.serverProbe;
    pxcEmitter.off('connectionKeyChanged', this.handleIdentityChanged);
    pxcEmitter.off('deviceEnrolled', this.handleIdentityChanged);
    this.clearPasswordEditor(); this.showLocalPassword = false;
  }

  private handleIdentityChanged = (json: string): void => {
    const identity: PxcIdentityJson = JSON.parse(json) as PxcIdentityJson;
    this.localDeviceId = identity.deviceId; this.localPassword = identity.connectionKey;
  };

  private clearPasswordEditor(): void {
    this.editingPassword = false; this.newPassword = ''; this.confirmPassword = '';
  }

  private saveLocalPassword(): void {
    const key: string = this.newPassword.trim();
    this.passwordError = true;
    if (Array.from(key).length < 5 || Array.from(key).length > 128) {
      this.passwordHint = '验证密码须为 5–128 个字符'; return;
    }
    if (key !== this.confirmPassword.trim()) { this.passwordHint = '两次输入的密码不一致'; return; }
    try {
      if (!pxc.setConnectionKey(key)) {
        this.passwordHint = '无法保存验证密码，请确认本机已加入设备列表'; return;
      }
      this.localPassword = key; this.showLocalPassword = false;
      this.clearPasswordEditor(); this.passwordError = false;
      this.passwordHint = '本机验证密码已修改，信令验证信息已更新';
    } catch (_) { this.passwordHint = '修改失败，请稍后重试'; }
  }

  private saveSettings(): void {
    if (!/^https?:\/\/[^\s]+$/.test(this.apiUrl.trim()) || !/^wss?:\/\/[^\s]+$/.test(this.wsUrl.trim())) {
      this.savedHint = '请输入完整的 http(s) 账号 API 和 ws(s) 信令地址'; return;
    }
    pxcSettings.putString('apiUrl', this.apiUrl.trim()); pxcSettings.putString('wsUrl', this.wsUrl.trim());
    pxcSettings.putString('stunUrl', this.stunUrl.trim()); pxcSettings.putString('turnUrl', this.turnUrl.trim());
    pxcSettings.putString('turnUser', this.turnUser); pxcSettings.putString('turnPass', this.turnPass);
    pxcSettings.putString('deviceName', this.deviceName.trim());
    pxc.setIceServers(this.stunUrl.trim(), this.turnUrl.trim(), this.turnUser, this.turnPass);
    this.savedHint = '已保存。ICE 配置立即生效；账号服务器与设备名在重新登录后生效';
  }

  @Builder
  SettingSwitch(title: string, hint: string, value: boolean, changed: (value: boolean) => void) {
    Row({ space: 12 }) {
      Column({ space: 5 }) {
        Text(title).fontSize(16).fontColor('#192b40')
        Text(hint).fontSize(12).fontColor('#7c899c')
      }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      Toggle({ type: ToggleType.Switch, isOn: value }).selectedColor('#4385ff').onChange(changed)
    }.width('100%').padding(18).backgroundColor('rgba(255,255,255,0.88)').backgroundBlurStyle(BlurStyle.COMPONENT_THIN).borderRadius(18)
      .border({ width: 1, color: 'rgba(255,255,255,0.8)' })
  }

  build() {
    Column({ space: 16 }) {
      Row() {
        Text('设置').fontSize(28).fontWeight(FontWeight.Bold).fontColor('#18283d').layoutWeight(1)
        if (this.allowLogout) {
          Button('退出登录', { type: ButtonType.Normal }).fontSize(14).height(38).fontColor('#db4b54').backgroundColor('#ffedf0').borderRadius(10)
            .onClick(() => {
              this.getUIContext().showAlertDialog({ title: '退出登录', message: '断开当前会话并清除保存的登录密码？',
                primaryButton: { value: '取消', action: (): void => {} }, secondaryButton: { value: '退出登录', action: this.onLogout } });
            })
        }
      }.width('100%')
      Scroll() {
        Row({ space: 6 }) {
          ForEach(['常规', '安全', '键盘', '鼠标', '网络'], (tab: string) => {
            Text(tab).fontSize(15).padding({ left: 12, right: 12, top: 10, bottom: 10 }).borderRadius(10)
              .fontColor(this.section === tab ? '#4385ff' : '#7c899c').backgroundColor(this.section === tab ? '#e5eeff' : Color.Transparent)
              .onClick(() => {
                this.section = tab; this.savedHint = ''; this.passwordHint = '';
                this.clearPasswordEditor(); this.showLocalPassword = false;
              })
          })
        }
      }.scrollable(ScrollDirection.Horizontal).scrollBar(BarState.Off).width('100%').height(42)
      Scroll() {
        Column({ space: 14 }) {
          if (this.section === '常规') {
            Text(this.username === '' ? 'Pixel Connection' : `当前账号：${this.username}`).fontSize(16).width('100%')
            TextInput({ text: this.deviceName, placeholder: '本机名称' }).onChange((v: string) => { this.deviceName = v; })
            Text('鸿蒙客户端只负责控制其他设备，不接收桌面控制请求。').fontSize(13).fontColor('#7c899c').width('100%')
            this.SettingSwitch('加载设备壁纸', '仅连接已记住密钥的在线电脑，缓存真实桌面壁纸', this.previewEnabled,
              (v: boolean): void => { this.previewEnabled = v; pxcSettings.putBool('previewEnabled', v); })
            Button('保存设备名称').width('100%').onClick(() => { this.saveSettings(); })
          } else if (this.section === '安全') {
            Column({ space: 12 }) {
              Row({ space: 10 }) {
                PxcIcon({ name: 'shield' }); Text('本机验证密码').fontSize(18).fontColor('#192b40').layoutWeight(1)
                Button(this.editingPassword ? '取消' : '修改', { type: ButtonType.Normal })
                  .id('pxc_local_password_edit').height(34).fontSize(13).fontColor('#1f6feb').backgroundColor('#e5eeff').borderRadius(10)
                  .enabled(this.localPassword !== '').onClick(() => {
                    this.passwordHint = ''; this.showLocalPassword = false;
                    if (this.editingPassword) this.clearPasswordEditor(); else this.editingPassword = true;
                  })
              }.width('100%')
              Text(`本机设备 ID：${this.localDeviceId || '尚未添加本机'}`).fontSize(13).fontColor('#52647c').width('100%')
              Row({ space: 12 }) {
                Text(this.localPassword === '' ? '加入设备列表后生成' : this.showLocalPassword ? this.localPassword : '••••••••')
                  .fontSize(18).fontColor('#24364b').layoutWeight(1).maxLines(2).wordBreak(WordBreak.BREAK_ALL)
                if (this.localPassword !== '') {
                  Text(this.showLocalPassword ? '隐藏' : '显示').fontSize(13).fontColor('#1f6feb').padding(8)
                    .onClick(() => { this.showLocalPassword = !this.showLocalPassword; })
                }
              }.width('100%')
              Text('设备验证密码保存在本机，与账号登录密码独立。鸿蒙当前仅作控制端，不接受远程桌面控制。')
                .fontSize(12).fontColor('#52647c').width('100%')
              if (this.localPassword === '') {
                Button('把本机加入设备列表').width('100%').onClick(() => { pxc.addThisDevice(); })
              }
              if (this.editingPassword) {
                TextInput({ text: this.newPassword, placeholder: '新验证密码（5–128 个字符）' })
                  .id('pxc_local_password_new').type(InputType.Password).maxLength(128)
                  .fontColor('#192b40').placeholderColor('#667085').backgroundColor('#eef1f5')
                  .onChange((v: string) => { this.newPassword = v; this.passwordHint = ''; })
                TextInput({ text: this.confirmPassword, placeholder: '再次输入新验证密码' })
                  .id('pxc_local_password_confirm').type(InputType.Password).maxLength(128)
                  .fontColor('#192b40').placeholderColor('#667085').backgroundColor('#eef1f5')
                  .onChange((v: string) => { this.confirmPassword = v; this.passwordHint = ''; })
                Button('保存验证密码').id('pxc_local_password_save').width('100%')
                  .enabled(this.newPassword.trim() !== '' && this.confirmPassword.trim() !== '')
                  .onClick(() => { this.saveLocalPassword(); })
              }
              if (this.passwordHint !== '') {
                Text(this.passwordHint).fontSize(13).fontColor(this.passwordError ? '#c82f42' : '#1f6feb').width('100%')
              }
            }.width('100%').padding(18).backgroundColor(Color.White).borderRadius(18)
            Row({ space: 10 }) { PxcIcon({ name: 'shield' }); Text('连接保护').fontSize(18) }.width('100%')
            Text('远控须通过设备密钥认证，控制和视频通过 DTLS 加密通道传输。').fontSize(13).fontColor('#7c899c').width('100%')
            this.SettingSwitch('记住已验证的连接密钥', '关闭后，每次连接都需要输入密钥', this.rememberKeys,
              (v: boolean): void => { this.rememberKeys = v; pxcSettings.putBool('rememberKeys', v); if (!v) pxcSettings.clearConnKeys(); })
            this.SettingSwitch('断开前确认', '避免误触退出远程桌面', this.confirmExit,
              (v: boolean): void => { this.confirmExit = v; pxcSettings.putBool('confirmExit', v); })
            this.SettingSwitch('切入后台时断开', '离开应用时结束远控；始终释放已按下的键鼠', this.disconnectBackground,
              (v: boolean): void => { this.disconnectBackground = v; pxcSettings.putBool('disconnectBackground', v); })
            Button('清除所有记住的设备密钥').fontColor('#db4b54').backgroundColor('#ffedf0').width('100%').onClick(() => {
              this.getUIContext().showAlertDialog({ title: '清除连接密钥', message: '下次连接需要重新输入电脑端密钥。',
                primaryButton: { value: '取消', action: (): void => {} }, secondaryButton: { value: '清除', action: (): void => {
                  pxcSettings.clearConnKeys(); this.savedHint = '已清除设备密钥';
                } } });
            })
            Button('清除已保存密码和登录状态').width('100%').onClick(() => {
              pxcLoginPersistence.clear(); this.savedHint = '已清除，下次启动需重新登录';
            })
          } else if (this.section === '键盘') {
            Row({ space: 10 }) {
              Button('系统输入法').layoutWeight(1).backgroundColor(this.keyboardMode === 'system' ? '#4385ff' : '#9aa8be')
                .onClick(() => { this.keyboardMode = 'system'; pxcSettings.putString('keyboardMode', 'system'); })
              Button('电脑键盘').layoutWeight(1).backgroundColor(this.keyboardMode === 'computer' ? '#4385ff' : '#9aa8be')
                .onClick(() => { this.keyboardMode = 'computer'; pxcSettings.putString('keyboardMode', 'computer'); })
            }.width('100%')
            this.SettingSwitch('保持组合键', 'Ctrl / Shift / Alt / Win 持续按住，再点一次释放', this.stickyModifiers,
              (v: boolean): void => { this.stickyModifiers = v; pxcSettings.putBool('stickyModifiers', v); })
            Text('电脑键盘左右滑动切换字母区与功能区。关闭键盘、切屏、切入后台和断开时释放所有按键。系统输入法先完成输入，再点发送，避免拼音候选重复发送。')
              .fontSize(14).fontColor('#7c899c').width('100%')
          } else if (this.section === '鼠标') {
            this.SettingSwitch('默认使用鼠标指针', '手指相对移动指针，轻点左键；关闭时触摸位置即点击位置', this.pointerMode,
              (v: boolean): void => { this.pointerMode = v; pxcSettings.putBool('pointerMode', v); })
            Text('虚拟鼠标提供左键、右键、中键、滚轮和按住拖拽。拖动手柄放在远程画面外，面板可以移位和收起，方便点击边缘。双指滑动滚轮，长按右键，三指轻点中键。')
              .fontSize(14).fontColor('#7c899c').width('100%')
          } else {
            Text(this.serverCheckedApi === this.apiUrl.trim() ? this.serverDescription : '地址已修改，请检测服务器')
              .fontSize(13).fontColor('#35465c').width('100%').copyOption(CopyOptions.InApp)
            Text('地址不同但服务器标识相同，表示连接同一个账号库。本机 127.0.0.1 与服务器局域网地址可以指向同一服务。')
              .fontSize(13).fontColor('#7c899c').width('100%')
            Button('检测服务器').onClick(() => { this.checkServer(); })
            TextInput({ text: this.apiUrl, placeholder: '账号 API（http://主机:29910）' }).onChange((v: string) => { this.apiUrl = v; })
            TextInput({ text: this.wsUrl, placeholder: '信令（ws://主机:9910）' }).onChange((v: string) => { this.wsUrl = v; })
            TextInput({ text: this.stunUrl, placeholder: 'STUN 地址，可留空' }).onChange((v: string) => { this.stunUrl = v; })
            TextInput({ text: this.turnUrl, placeholder: 'TURN（turn://主机:3478）' }).onChange((v: string) => { this.turnUrl = v; })
            TextInput({ text: this.turnUser, placeholder: 'TURN 账号' }).onChange((v: string) => { this.turnUser = v; })
            TextInput({ text: this.turnPass, placeholder: 'TURN 密码' }).type(InputType.Password).onChange((v: string) => { this.turnPass = v; })
            Button('保存服务器设置').width('100%').onClick(() => { this.saveSettings(); })
          }
          if (this.savedHint !== '') { Text(this.savedHint).fontSize(13).fontColor('#4385ff').width('100%') }
        }.width('100%').alignItems(HorizontalAlign.Start).padding({ bottom: 24 })
      }.width('100%').layoutWeight(1).align(Alignment.Top).scrollBar(BarState.Auto)
    }.width('100%').height('100%').padding(20).backgroundColor('#f3f6fa')
  }
}
