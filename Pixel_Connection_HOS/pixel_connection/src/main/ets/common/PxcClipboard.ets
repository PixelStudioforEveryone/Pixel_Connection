import { pasteboard } from '@kit.BasicServicesKit';
import pxc from 'libpxc_hos.so';
import { pxcEmitter } from './PxcEmitter';
import { PxcSessionStateJson } from '../model/PxcModels';

interface ClipboardText { text: string; }
interface ClipboardInput { type: string; key?: number; mods?: number; pressed?: boolean; }

// Only the authenticated foreground session can read or write the local text clipboard.
class PxcClipboard {
  private initialized: boolean = false;
  private foreground: boolean = true;
  private enabled: boolean = false;
  private allowed: boolean = false;
  private applying: boolean = false;
  private epoch: number = 0;
  private lastText: string | undefined = undefined;
  private board: pasteboard.SystemPasteboard = pasteboard.getSystemPasteboard();
  private writes: Promise<void> = Promise.resolve();
  private inputs: Promise<void> = Promise.resolve();
  private sessionRevision: number = 0;

  init(): void {
    if (this.initialized) return;
    this.initialized = true;
    this.board.on('update', (): void => { this.epoch++; if (!this.applying) this.syncLocal(); });
    pxcEmitter.on('clipboardReady', (): void => { this.syncLocal(); });
    pxcEmitter.on('sessionState', (json: string): void => {
      const state = (JSON.parse(json) as PxcSessionStateJson).state;
      if (state === 'authenticated') { this.sessionRevision++; this.lastText = undefined; this.syncLocal(); }
      if (state === 'closed' || state === 'rejected' || state === 'p2p_timeout') {
        this.epoch++; this.sessionRevision++; this.lastText = undefined;
      }
    });
    // PasteButton grants expire when the app leaves the foreground.
    pxcEmitter.on('appBackground', (): void => { this.foreground = false; this.allowed = false; this.epoch++; });
    pxcEmitter.on('appForeground', (): void => { this.foreground = true; this.syncLocal(); });
    pxcEmitter.on('clipboardText', (json: string): void => {
      if (!this.enabled || !this.foreground || !pxc.sessionAuthenticated()) return;
      const text = (JSON.parse(json) as ClipboardText).text;
      if (typeof text !== 'string' || text === this.lastText) return;
      const epoch = ++this.epoch;
      this.lastText = text;
      this.writes = this.writes.then(async (): Promise<void> => {
        if (epoch !== this.epoch || !this.foreground || !pxc.sessionAuthenticated()) return;
        this.applying = true;
        try {
          const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, text);
          const property = data.getProperty(); property.shareOption = pasteboard.ShareOption.LOCALDEVICE;
          data.setProperty(property);
          await this.board.setData(data);
        }
        catch (_) { this.lastText = undefined; }
        finally { this.applying = false; }
      });
    });
  }

  async enable(_context: Context): Promise<void> {
    this.init();
    // Writing remote text needs no restricted READ_PASTEBOARD signing ACL.
    this.enabled = true;
  }

  pasteAuthorized(): void {
    if (!this.foreground || !pxc.sessionAuthenticated()) return;
    this.allowed = true;
    // Called only after a successful system PasteButton authorization.
    this.sendInput(JSON.stringify({ type: 'key', key: 86, mods: 0x04000000, pressed: true }));
    this.sendInput(JSON.stringify({ type: 'key', key: 86, mods: 0x04000000, pressed: false }));
  }

  sendInput(json: string): void {
    const revision = this.sessionRevision;
    // Preserve key-up order while reading the current phone clipboard for Ctrl+V.
    this.inputs = this.inputs.then(async (): Promise<void> => {
      if (revision !== this.sessionRevision || !pxc.sessionAuthenticated()) return;
      const input = JSON.parse(json) as ClipboardInput;
      if (input.type === 'key' && input.key === 86 && input.pressed &&
          ((input.mods ?? 0) & 0x04000000) !== 0 && this.allowed && this.foreground) {
        try {
          const data = await this.board.getData();
          if (revision !== this.sessionRevision || !this.foreground || !this.allowed || !pxc.sessionAuthenticated()) return;
          if (data.hasMimeType(pasteboard.MIMETYPE_TEXT_PLAIN)) {
            const text = data.getPrimaryText();
            if (pxc.sendClipboardText(text)) this.lastText = text;
          }
        } catch (_) {}
      }
      if (revision === this.sessionRevision && pxc.sessionAuthenticated()) pxc.sendInputEvent(json);
    }).catch(() => {});
  }

  private async syncLocal(): Promise<void> {
    if (!this.allowed || !this.foreground || this.applying || !pxc.sessionAuthenticated()) return;
    const epoch = this.epoch;
    try {
      const data = await this.board.getData();
      if (epoch !== this.epoch || !this.foreground || !pxc.sessionAuthenticated() || this.applying) return;
      if (!data.hasMimeType(pasteboard.MIMETYPE_TEXT_PLAIN)) return;
      const text = data.getPrimaryText();
      if (text !== this.lastText && pxc.sendClipboardText(text)) this.lastText = text;
    } catch (_) { /* Denied clipboard access must not prevent remote control. */ }
  }
}

export const pxcClipboard: PxcClipboard = new PxcClipboard();
