import pxc from 'libpxc_hos.so';
import { fileIo, picker, fileUri } from '@kit.CoreFileKit';
import { pxcEmitter } from '../common/PxcEmitter';
import { pxcClipboard } from '../common/PxcClipboard';
import { PxcIcon } from '../common/PxcUi';

interface RemoteFile { name: string; path: string; directory: boolean; size: number; modified: string; }
interface FileMessage { op: string; path?: string; entries?: Array<RemoteFile>; error?: string; }
interface FileProgress { id: string; name: string; total: number; transferred: number; state: string; done: boolean; }
export interface PxcFilePageParam { deviceName: string; ownSession: boolean; }
@Builder
export function FilesPageBuilder(): void { FilesPage() }

@Component
export struct FilesPage {
  pathStack: NavPathStack = new NavPathStack();
  @State deviceName: string = '远端设备';
  @State ready: boolean = false;
  @State remotePath: string = '';
  @State entries: Array<RemoteFile> = [];
  @State selected: string = '';
  @State localUri: string = '';
  @State localName: string = '';
  @State localSize: number = 0;
  @State message: string = '';
  @State transferId: string = '';
  @State progress: number = 0;
  @State transferText: string = '暂无传输任务';
  @State newDirectory: string = '';
  @State wide: boolean = false;
  @State received: Array<string> = [];
  private ownSession: boolean = true;
  private readyTimer: number = -1;
  private cachePath: string = '';
  private saveUri: string = '';
  private receiveName: string = '';

  aboutToAppear(): void {
    const context = this.getUIContext().getHostContext();
    if (context !== undefined) pxcClipboard.enable(context);
    pxcEmitter.on('fileEvent', this.handleFile);
    pxcEmitter.on('fileProgress', this.handleProgress);
    pxcEmitter.on('sessionState', this.handleSession);
    this.readyTimer = setInterval(() => {
      const ready: boolean = pxc.fileChannelReady();
      if (ready && !this.ready) this.list('~');
      this.ready = ready;
    }, 400);
  }
  aboutToDisappear(): void {
    pxcEmitter.off('fileEvent', this.handleFile); pxcEmitter.off('fileProgress', this.handleProgress);
    pxcEmitter.off('sessionState', this.handleSession);
    if (this.readyTimer >= 0) clearInterval(this.readyTimer);
    if (this.transferId !== '') pxc.cancelFileTransfer(this.transferId);
    this.clearCache();
    if (this.ownSession) pxc.disconnect();
  }
  private handleSession = (json: string): void => {
    const state: string = (JSON.parse(json) as FileProgress).state;
    if (state === 'closed' || state === 'rejected' || state === 'p2p_timeout') {
      this.ready = false; this.message = '连接已结束，请返回设备页面重新连接';
    }
  };
  private handleFile = (json: string): void => {
    const d: FileMessage = JSON.parse(json) as FileMessage;
    if (d.op === 'list_result') {
      if (d.error) { this.message = d.error; return; }
      this.entries = d.entries ?? []; this.remotePath = d.path ?? ''; this.selected = ''; this.message = '';
    } else if (d.op === 'mkdir_result') {
      if (d.error) this.message = d.error; else this.list(d.path ?? this.remotePath);
    }
  };
  private handleProgress = (json: string): void => {
    const p: FileProgress = JSON.parse(json) as FileProgress;
    if (this.transferId !== p.id) return;
    this.progress = p.total > 0 ? Math.min(100, p.transferred * 100 / p.total) : 0;
    this.transferText = `${p.name} · ${p.state} · ${this.bytes(p.transferred)} / ${this.bytes(p.total)}`;
    if (p.done) {
      this.transferId = '';
      if (p.state === '已完成') {
        this.progress = 100;
        if (this.cachePath !== '') {
          this.received = [this.receiveName].concat(this.received).slice(0, 10);
          this.exportDownload();
        }
        this.list(this.remotePath);
      } else { this.message = p.state; this.clearCache(); }
    }
  };
  private clearCache(): void {
    if (this.cachePath !== '') {
      try { fileIo.unlinkSync(this.cachePath); } catch (_) {}
      this.cachePath = ''; this.saveUri = '';
    }
  }
  private async exportDownload(): Promise<void> {
    const path: string = this.cachePath;
    const uri: string = this.saveUri;
    if (path === '' || uri === '') return;
    try {
      const destination: fileIo.File = fileIo.openSync(uri, fileIo.OpenMode.WRITE_ONLY);
      try { fileIo.copyFileSync(path, destination.fd); } finally { fileIo.closeSync(destination); }
      this.transferText = `${this.receiveName} · 已保存到所选位置`;
      this.clearCache();
    } catch (_) {
      this.message = '文件已下载到应用缓存，保存失败，请点重新保存';
    }
  }
  private async saveAgain(): Promise<void> {
    try {
      const uris: Array<string> = await new picker.DocumentViewPicker().save({ newFileNames: [this.receiveName] });
      if (uris.length > 0) { this.saveUri = uris[0]; await this.exportDownload(); }
    } catch (_) { this.message = '保存已取消'; }
  }
  private bytes(value: number): string {
    return value < 1024 ? `${value} B` : value < 1048576 ? `${(value / 1024).toFixed(1)} KB` : `${(value / 1048576).toFixed(1)} MB`;
  }
  private list(path: string): void {
    if (!pxc.fileCommand(JSON.stringify({ op: 'list', path: path }))) this.message = '文件通道未就绪';
  }
  private up(): void {
    let parent: string = this.remotePath.replace(/[/\\]+[^/\\]+[/\\]*$/, '');
    if (/^[a-zA-Z]:$/.test(parent)) parent += '/';
    this.list(parent || '/');
  }
  private async choose(): Promise<void> {
    try {
      const uris: Array<string> = await new picker.DocumentViewPicker().select({ maxSelectNumber: 1 });
      if (uris.length === 0) return;
      const file: fileIo.File = fileIo.openSync(uris[0], fileIo.OpenMode.READ_ONLY);
      try { this.localSize = fileIo.statSync(file.fd).size; } finally { fileIo.closeSync(file); }
      this.localUri = uris[0]; this.localName = new fileUri.FileUri(uris[0]).name;
    } catch (_) { this.message = '未选择文件或没有读取权限'; }
  }
  private upload(): void {
    if (!this.localUri || this.transferId || !this.ready) return;
    try {
      const file: fileIo.File = fileIo.openSync(this.localUri, fileIo.OpenMode.READ_ONLY);
      try { this.transferId = pxc.uploadFile(file.fd, this.localName, this.remotePath); }
      finally { fileIo.closeSync(file); }
      this.progress = 0; this.transferText = this.transferId ? `${this.localName} · 准备上传` : '无法启动上传';
    } catch (_) { this.message = '无法打开手机文件'; }
  }
  private async download(): Promise<void> {
    const file: RemoteFile | undefined = this.entries.find((f: RemoteFile): boolean => f.path === this.selected && !f.directory);
    if (!file || this.transferId || !this.ready || this.cachePath !== '') return;
    try {
      const uris: Array<string> = await new picker.DocumentViewPicker().save({ newFileNames: [file.name] });
      if (uris.length === 0 || !pxc.fileChannelReady()) return;
      const context: Context | undefined = this.getUIContext().getHostContext();
      if (!context) return;
      this.cachePath = `${context.cacheDir}/pxc-download-${Date.now()}.part`;
      this.saveUri = uris[0]; this.receiveName = file.name;
      const local: fileIo.File = fileIo.openSync(this.cachePath, fileIo.OpenMode.CREATE | fileIo.OpenMode.READ_WRITE);
      try { this.transferId = pxc.downloadFile(local.fd, file.path, file.name); } finally { fileIo.closeSync(local); }
      this.progress = 0;
      if (!this.transferId) this.clearCache();
    } catch (_) { this.clearCache(); this.message = '无法启动下载或保存已取消'; }
  }
  @Builder
  LocalPane() {
    Column({ space: 12 }) {
      Row({ space: 8 }) { PxcIcon({ name: 'devices', pixelSize: 20 }); Text('手机文件').fontSize(18).fontWeight(FontWeight.Bold) }.width('100%')
      Button('选择手机文件').width('100%').enabled(this.transferId === '').onClick(() => { this.choose(); })
      Text(this.localName || '尚未选择文件').fontSize(14).maxLines(2).width('100%')
      if (this.localUri) { Text(this.bytes(this.localSize)).fontSize(12).fontColor('#64748b').width('100%') }
      Button('上传到当前远端目录').width('100%').enabled(this.ready && this.localUri !== '' && this.remotePath !== '' && this.transferId === '')
        .onClick(() => { this.upload(); })
      if (this.received.length > 0) {
        Text('最近下载').fontSize(14).width('100%')
        ForEach(this.received, (name: string) => { Text(name).fontSize(12).width('100%').maxLines(1) })
      }
    }.width('100%').padding(14).backgroundColor('rgba(255,255,255,0.8)').backgroundBlurStyle(BlurStyle.COMPONENT_THIN).borderRadius(18)
  }
  @Builder
  RemotePane() {
    Column({ space: 10 }) {
      Row({ space: 8 }) {
        PxcIcon({ name: 'file', pixelSize: 20 }); Text('远端文件').fontSize(18).fontWeight(FontWeight.Bold).layoutWeight(1)
        Button('上级').enabled(this.ready).onClick(() => { this.up(); })
        Button('主页').enabled(this.ready).onClick(() => { this.list('~'); })
      }.width('100%')
      Row({ space: 8 }) {
        TextInput({ text: this.remotePath, placeholder: '输入远端目录地址' }).layoutWeight(1).onChange((v: string) => { this.remotePath = v; })
          .onSubmit(() => { this.list(this.remotePath); })
        Button('刷新').enabled(this.ready).onClick(() => { this.list(this.remotePath); })
      }.width('100%')
      List({ space: 4 }) {
        ForEach(this.entries, (file: RemoteFile) => {
          ListItem() {
            Row({ space: 10 }) {
              PxcIcon({ name: file.directory ? 'file' : 'copy', pixelSize: 20 })
              Column({ space: 3 }) {
                Text(file.name).fontSize(14).maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis }).width('100%')
                Text(file.directory ? '文件夹' : this.bytes(file.size)).fontSize(11).fontColor('#64748b').width('100%')
              }.layoutWeight(1)
              if (this.selected === file.path) { PxcIcon({ name: 'pointer', pixelSize: 16 }) }
            }.width('100%').padding(10).borderRadius(10).backgroundColor(this.selected === file.path ? '#dbeafe' : '#ffffff')
              .onClick(() => { if (file.directory) this.list(file.path); else this.selected = file.path; })
          }
        }, (file: RemoteFile): string => file.path)
      }.width('100%').layoutWeight(1)
      Row({ space: 8 }) {
        TextInput({ text: this.newDirectory, placeholder: '新文件夹名称' }).layoutWeight(1).onChange((v: string) => { this.newDirectory = v; })
        Button('新建').enabled(this.ready && this.newDirectory.trim() !== '').onClick(() => {
          pxc.fileCommand(JSON.stringify({ op: 'mkdir', path: this.remotePath, name: this.newDirectory.trim() })); this.newDirectory = '';
        })
      }.width('100%')
      Button('下载选中文件').width('100%').enabled(this.ready && this.selected !== '' && this.transferId === '' && this.cachePath === '')
        .onClick(() => { this.download(); })
    }.width('100%').height('100%').padding(14).backgroundColor('#ffffff').borderRadius(18)
  }
  build() {
    NavDestination() {
      Column({ space: 12 }) {
        Row({ space: 12 }) {
          PxcIcon({ name: 'back' }).padding(6).onClick(() => { this.pathStack.pop(); })
          Text('文件传输').fontSize(23).fontWeight(FontWeight.Bold).layoutWeight(1)
          Text(this.deviceName).fontSize(13).maxLines(1)
        }.width('100%')
        if (this.wide) {
          Row({ space: 12 }) { Column() { this.LocalPane() }.width('32%'); Column() { this.RemotePane() }.layoutWeight(1) }.layoutWeight(1).width('100%')
        } else {
          this.LocalPane()
          Column() { this.RemotePane() }.layoutWeight(1).width('100%')
        }
        Column({ space: 8 }) {
          Text(this.message || (this.ready ? '文件通道已认证' : '等待文件通道…')).fontSize(12).fontColor('#64748b').width('100%')
          Text(this.transferText).fontSize(12).width('100%').maxLines(2)
          Progress({ value: this.progress, total: 100, type: ProgressType.Linear }).width('100%')
          if (this.transferId !== '') { Button('取消传输').onClick(() => { pxc.cancelFileTransfer(this.transferId); }) }
          if (this.cachePath !== '' && this.transferId === '') { Button('重新保存').onClick(() => { this.saveAgain(); }) }
        }.padding(12).width('100%').backgroundColor('#ffffff').borderRadius(14)
      }.width('100%').height('100%').padding(12).backgroundColor('#f3f6fb')
        .onSizeChange((oldSize: SizeOptions, size: SizeOptions) => { this.wide = Number(size.width) >= 650; })
    }.hideTitleBar(true).onReady((ctx: NavDestinationContext) => {
      this.pathStack = ctx.pathStack;
      const param: PxcFilePageParam = ctx.pathInfo.param as PxcFilePageParam;
      if (param) { this.deviceName = param.deviceName; this.ownSession = param.ownSession; }
    })
  }
}
