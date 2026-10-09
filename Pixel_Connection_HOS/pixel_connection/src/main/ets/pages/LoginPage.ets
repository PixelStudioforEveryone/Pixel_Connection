import pxc from 'libpxc_hos.so';
import { pxcLoginPersistence } from '../common/PxcLoginPersistence';
import { pxcServerDescription } from '../common/PxcServerStatus';
import { pxcEmitter } from '../common/PxcEmitter';
import {
  pxcSettings,
  PXC_DEFAULT_API,
  PXC_DEFAULT_WS,
  PXC_DEFAULT_DEVICE_NAME
} from '../common/PxcSettings';
import { PxcLoginResultJson, PxcRegisterResultJson } from '../model/PxcModels';

@Component
export struct LoginPage {
  @State loginMode: boolean = true;
  @State identifier: string = '';
  @State password: string = '';
  @State regUsername: string = '';
  @State regEmail: string = '';
  @State regPassword: string = '';
  @State regConfirm: string = '';
  @State rememberPwd: boolean = false;
  @State keepLoggedIn: boolean = false;
  @State busy: boolean = false;
  @State errorText: string = '';
  @State advancedOpen: boolean = false;
  @State apiUrl: string = PXC_DEFAULT_API;
  @State wsUrl: string = PXC_DEFAULT_WS;
  @State serverDescription: string = '正在确认服务器来源…';
  private serverProbe: number = 0;

  private async checkServer(): Promise<void> {
    const generation: number = ++this.serverProbe;
    const api: string = this.apiUrl.trim();
    this.serverDescription = '正在确认服务器来源…';
    const description: string = await pxcServerDescription(api);
    if (generation === this.serverProbe && api === this.apiUrl.trim()) { this.serverDescription = description; }
  }

  aboutToAppear(): void {
    this.rememberPwd = pxcSettings.getBool('rememberPassword', false);
    this.keepLoggedIn = pxcSettings.getBool('keepLoggedIn', false);
    this.apiUrl = pxcSettings.getString('apiUrl', PXC_DEFAULT_API);
    this.wsUrl = pxcSettings.getString('wsUrl', PXC_DEFAULT_WS);
    this.checkServer();
    const saved = pxcLoginPersistence.savedPassword(this.apiUrl, this.wsUrl);
    if (saved !== undefined) {
      this.identifier = saved.identifier;
      this.password = saved.password;
    }
    pxcEmitter.on('loginResult', this.handleLoginResult);
    pxcEmitter.on('registerResult', this.handleRegisterResult);
  }

  aboutToDisappear(): void {
    ++this.serverProbe;
    pxcEmitter.off('loginResult', this.handleLoginResult);
    pxcEmitter.off('registerResult', this.handleRegisterResult);
  }

  private handleLoginResult = (json: string): void => {
    const d: PxcLoginResultJson = JSON.parse(json) as PxcLoginResultJson;
    if (!this.busy || (d.generation !== undefined && d.generation !== pxc.getLoginSession().generation)) { return; }
    this.busy = false;
    if (d.ok === true) {
      this.errorText = '';
    } else {
      this.errorText = d.error ?? '登录失败';
    }
  };

  private handleRegisterResult = (json: string): void => {
    const d: PxcRegisterResultJson = JSON.parse(json) as PxcRegisterResultJson;
    this.busy = false;
    if (d.ok === true) {
      this.loginMode = true;
      this.identifier = this.regUsername;
      this.password = '';
      this.errorText = '注册成功，请登录';
    } else {
      this.errorText = d.message;
    }
  };

  private applyServerConfig(): boolean {
    const api: string = this.apiUrl.trim();
    const ws: string = this.wsUrl.trim();
    if (api === '' || ws === '') {
      this.errorText = '账号 API 与信令服务器地址不能为空';
      return false;
    }
    pxcSettings.putString('apiUrl', api);
    pxcSettings.putString('wsUrl', ws);
    const hostContext: Context | undefined = this.getUIContext().getHostContext();
    if (hostContext === undefined) {
      this.errorText = '无法获取应用上下文';
      return false;
    }
    const deviceName: string = pxcSettings.getString('deviceName', PXC_DEFAULT_DEVICE_NAME);
    pxc.configure(api, ws, hostContext.filesDir, deviceName);
    return true;
  }

  private submitLogin(): void {
    if (this.identifier === '' || this.password === '') {
      this.errorText = '请输入账号和密码';
      return;
    }
    if (!this.applyServerConfig()) {
      return;
    }
    this.busy = true;
    this.errorText = '';
    pxcLoginPersistence.beginLogin(this.identifier, this.password, this.rememberPwd, this.keepLoggedIn);
    pxc.login(this.identifier, this.password);
  }

  private submitRegister(): void {
    if (this.regUsername === '' || this.regPassword === '') {
      this.errorText = '请输入用户名和密码';
      return;
    }
    if (this.regPassword !== this.regConfirm) {
      this.errorText = '两次输入的密码不一致';
      return;
    }
    if (!this.applyServerConfig()) {
      return;
    }
    this.busy = true;
    this.errorText = '';
    pxc.registerAccount(this.regUsername, this.regEmail, this.regPassword);
  }

  private switchMode(toRegister: boolean): void {
    this.loginMode = !toRegister;
    this.errorText = '';
    this.busy = false;
  }

  build() {
    Column({ space: 12 }) {
      Text('Pixel Connection')
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
        .margin({ top: 48 })
      Text('HarmonyOS 主控端')
        .fontSize(14)
        .fontColor('#888888')

      if (this.loginMode) {
        TextInput({ text: this.identifier, placeholder: '用户名 / 邮箱' })
          .id('pxc_login_identifier')
          .width('85%')
          .onChange((v: string) => {
            this.identifier = v;
          })
        TextInput({ text: this.password, placeholder: '密码' })
          .id('pxc_login_password')
          .type(InputType.Password)
          .width('85%')
          .onChange((v: string) => {
            this.password = v;
          })
        Row({ space: 8 }) {
          Toggle({ type: ToggleType.Checkbox, isOn: this.rememberPwd })
            .id('pxc_login_remember_password')
            .enabled(!this.busy)
            .onChange((isOn: boolean) => {
              this.rememberPwd = isOn;
            })
          Text('记住密码')
            .fontSize(14)
          Blank()
          Toggle({ type: ToggleType.Checkbox, isOn: this.keepLoggedIn })
            .id('pxc_login_keep_logged_in')
            .enabled(!this.busy)
            .onChange((isOn: boolean) => {
              this.keepLoggedIn = isOn;
            })
          Text('保持登录状态')
            .fontSize(14)
        }
        .width('85%')

        Button('登录')
          .id('pxc_login_submit')
          .width('85%')
          .enabled(!this.busy)
          .onClick(() => {
            this.submitLogin();
          })
        Text('没有账号？点此注册')
          .fontSize(14)
          .fontColor('#1f6feb')
          .onClick(() => {
            this.switchMode(true);
          })
      } else {
        Text('注册账号')
          .fontSize(20)
          .fontWeight(FontWeight.Medium)
          .margin({ top: 8 })
        TextInput({ text: this.regUsername, placeholder: '用户名' })
          .width('85%')
          .onChange((v: string) => {
            this.regUsername = v;
          })
        TextInput({ text: this.regEmail, placeholder: '邮箱（可选）' })
          .width('85%')
          .onChange((v: string) => {
            this.regEmail = v;
          })
        TextInput({ text: this.regPassword, placeholder: '密码' })
          .type(InputType.Password)
          .width('85%')
          .onChange((v: string) => {
            this.regPassword = v;
          })
        TextInput({ text: this.regConfirm, placeholder: '确认密码' })
          .type(InputType.Password)
          .width('85%')
          .onChange((v: string) => {
            this.regConfirm = v;
          })

        Button('注册')
          .width('85%')
          .enabled(!this.busy)
          .onClick(() => {
            this.submitRegister();
          })
        Text('已有账号？点此登录')
          .fontSize(14)
          .fontColor('#1f6feb')
          .onClick(() => {
            this.switchMode(false);
          })
      }

      Text(this.advancedOpen ? '收起高级设置 ▲' : '高级设置 ▼')
        .fontSize(14)
        .fontColor('#1f6feb')
        .margin({ top: 4 })
        .onClick(() => {
          this.advancedOpen = !this.advancedOpen;
        })

      if (this.advancedOpen) {
        Column({ space: 8 }) {
          Text('账号 API')
            .fontSize(12)
            .fontColor('#666666')
            .width('100%')
          TextInput({ text: this.apiUrl, placeholder: 'http://192.168.1.10:29910' })
            .width('100%')
            .fontSize(14)
            .onChange((v: string) => {
              this.apiUrl = v;
              this.serverDescription = '地址已修改，请检测服务器';
            })
          Button('检测服务器').height(34).fontSize(13).onClick(() => { this.checkServer(); })
          Text('信令服务器')
            .fontSize(12)
            .fontColor('#666666')
            .width('100%')
          TextInput({ text: this.wsUrl, placeholder: 'ws://192.168.1.10:9910' })
            .width('100%')
            .fontSize(14)
            .onChange((v: string) => {
              this.wsUrl = v;
            })
        }
        .width('85%')
        .padding(12)
        .borderRadius(10)
        .backgroundColor('#f2f3f5')
      }

      if (this.busy) {
        LoadingProgress()
          .width(28)
          .height(28)
      }
      if (this.errorText !== '') {
        Text(this.errorText)
          .fontSize(14)
          .fontColor('#e5484d')
          .textAlign(TextAlign.Center)
          .width('85%')
      }

      Blank()
      Text(`账号 API: ${this.apiUrl}\n信令: ${this.wsUrl}`)
        .fontSize(12)
        .fontColor('#667085')
        .textAlign(TextAlign.Center)
        .margin({ bottom: 4 })
      Text(this.serverDescription).fontSize(12).fontColor('#667085')
        .textAlign(TextAlign.Center).width('85%').margin({ bottom: 18 })
    }
    .width('100%')
    .height('100%')
    .alignItems(HorizontalAlign.Center)
    .backgroundColor('#ffffff')
  }
}
