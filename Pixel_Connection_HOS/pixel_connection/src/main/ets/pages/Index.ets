import pxc, { LocalIdentity } from 'libpxc_hos.so';
import { pxcLoginPersistence } from '../common/PxcLoginPersistence';
import { pxcEmitter } from '../common/PxcEmitter';
import { pxcClipboard } from '../common/PxcClipboard';
import { preparePxcTrust } from '../common/PxcTrust';
import {
  pxcSettings,
  PXC_DEFAULT_API,
  PXC_DEFAULT_WS,
  PXC_DEFAULT_DEVICE_NAME
} from '../common/PxcSettings';
import { PxcLoginResultJson } from '../model/PxcModels';
import { LoginPage } from './LoginPage';
import { DevicesPage } from './DevicesPage';

@Entry
@Component
struct Index {
  @State pageMode: string = 'loading';
  @State loggedInUser: string = '';
  @State localEnrolled: boolean = false;
  @State localDeviceId: string = '';
  @State localConnKey: string = '';
  @State autoLoggingIn: boolean = false;
  private pathStack: NavPathStack = new NavPathStack();
  private devicesBack: () => boolean = (): boolean => false;

  onBackPress(): boolean {
    // NavDestination owns remote/file pages. Inline device details need their own back handler.
    if (this.pageMode === 'devices' && this.pathStack.size() === 0) { return this.devicesBack(); }
    return false;
  }

  aboutToAppear(): void {
    pxcEmitter.init();
    pxcClipboard.init();
    pxcEmitter.on('loginResult', this.handleLoginResult);
    pxcEmitter.on('loginPersistenceNotice', this.handlePersistenceNotice);
    this.bootstrap();
  }

  private handleLoginResult = (json: string): void => {
    const d: PxcLoginResultJson = JSON.parse(json) as PxcLoginResultJson;
    if (d.generation !== undefined && d.generation !== pxc.getLoginSession().generation) { return; }
    if (d.ok === true) {
      this.loggedInUser = d.username ?? '';
      pxcSettings.setAccountScope(pxcSettings.getString('apiUrl', PXC_DEFAULT_API), this.loggedInUser);
      this.autoLoggingIn = false;
      this.pageMode = 'devices';
    } else {
      if (this.autoLoggingIn) {
        this.autoLoggingIn = false;
        this.pageMode = 'login';
        this.getUIContext().getPromptAction().showToast({ message: d.invalidSession ? '登录状态已失效，请重新登录' : '暂时无法恢复登录，请检查服务器或重新登录' });
      }
    }
  };

  private handlePersistenceNotice = (message: string): void => {
    this.getUIContext().getPromptAction().showToast({ message: message });
  };

  aboutToDisappear(): void {
    pxcEmitter.off('loginResult', this.handleLoginResult);
    pxcEmitter.off('loginPersistenceNotice', this.handlePersistenceNotice);
  }

  private async bootstrap(): Promise<void> {
    const hostContext: Context | undefined = this.getUIContext().getHostContext();
    if (hostContext === undefined) {
      this.pageMode = 'login';
      return;
    }
    await pxcSettings.init(hostContext);
    pxcLoginPersistence.init();
    preparePxcTrust(hostContext);

    const apiUrl: string = pxcSettings.getString('apiUrl', PXC_DEFAULT_API);
    const wsUrl: string = pxcSettings.getString('wsUrl', PXC_DEFAULT_WS);
    const stunUrl: string = pxcSettings.getString('stunUrl', '');
    const turnUrl: string = pxcSettings.getString('turnUrl', '');
    const turnUser: string = pxcSettings.getString('turnUser', '');
    const turnPass: string = pxcSettings.getString('turnPass', '');
    const deviceName: string = pxcSettings.getString('deviceName', PXC_DEFAULT_DEVICE_NAME);

    pxc.setIceServers(stunUrl, turnUrl, turnUser, turnPass);
    const identity: LocalIdentity = pxc.configure(apiUrl, wsUrl, hostContext.filesDir, deviceName);
    this.localEnrolled = identity.enrolled;
    this.localDeviceId = identity.deviceId;
    this.localConnKey = identity.connectionKey;

    const session = pxcLoginPersistence.session();
    const legacy = pxcLoginPersistence.legacyLogin();
    if (session !== undefined || (legacy !== undefined && legacy.identifier !== '' && legacy.password !== '')) {
      this.autoLoggingIn = true;
      if (session !== undefined) { pxc.restoreLogin(session.token, session.username); }
      else if (legacy !== undefined) { pxc.login(legacy.identifier, legacy.password); }
      setTimeout(() => {
        if (this.autoLoggingIn) {
          pxc.cancelLoginAttempt();
          this.autoLoggingIn = false;
          this.pageMode = 'login';
        }
      }, 20000);
    } else {
      this.pageMode = 'login';
    }
  }

  private switchToLogin(): void {
    this.pageMode = 'login';
  }

  build() {
    Navigation(this.pathStack) {
      if (this.pageMode === 'loading') {
        Column({ space: 16 }) {
          LoadingProgress()
            .width(48)
            .height(48)
          if (this.autoLoggingIn) {
            Text('正在自动登录…')
              .fontSize(14)
              .fontColor('#888888')
          } else {
            Text('正在连接服务器…')
              .fontSize(14)
              .fontColor('#888888')
          }
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
      } else if (this.pageMode === 'login') {
        LoginPage()
      } else {
        DevicesPage({
          onBackHandlerReady: (handler: () => boolean): void => { this.devicesBack = handler; },
          pathStack: this.pathStack,
          username: this.loggedInUser,
          localEnrolled: this.localEnrolled,
          localDeviceId: this.localDeviceId,
          localConnKey: this.localConnKey,
          onLoggedOut: (): void => {
            this.switchToLogin();
          }
        })
      }
    }
    .mode(NavigationMode.Stack)
    .hideTitleBar(true)
    .backgroundColor('#ffffff')
  }
}
