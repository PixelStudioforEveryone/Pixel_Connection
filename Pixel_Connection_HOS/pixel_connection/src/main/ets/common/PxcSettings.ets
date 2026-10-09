import { preferences } from '@kit.ArkData';

// Reserved example domain: users must configure their own server before login.
export const PXC_DEFAULT_API: string = 'https://remote.example.com';
export const PXC_DEFAULT_WS: string = 'wss://remote.example.com/signal';
export const PXC_DEFAULT_TURN: string = '';
export const PXC_DEFAULT_DEVICE_NAME: string = 'HarmonyOS 设备';

export function pxcIsExampleServerAddress(address: string): boolean {
  return /^(?:https?|wss?):\/\/(?:[a-z0-9-]+\.)*example\.(?:com|net|org)(?::[0-9]+)?(?:[/?#]|$)/.test(address.trim().toLowerCase());
}

const PREF_NAME: string = 'pxc_settings';

/**
 * preferences 封装：服务器/ICE 设置、记住的账号密码、各设备连接密钥。
 * 连接密钥以 Record<string, string> 序列化存储（deviceId → key）。
 */
export class PxcSettings {
  private store: preferences.Preferences | null = null;
  private scope: string = '';

  setAccountScope(api: string, username: string): void {
    this.scope = api.replace(/\/$/, '') + ':' + username;
    if (!this.getBool('keysScoped', false)) {
      this.putScopedString('connKeys', this.getString('connKeys', '{}'));
      this.putString('connKeys', '{}'); this.putBool('keysScoped', true);
    }
  }
  getScopedString(key: string, def: string): string { return this.getString(key + ':' + this.scope, def); }
  putScopedString(key: string, val: string): void { this.putString(key + ':' + this.scope, val); }

  async init(context: Context): Promise<void> {
    if (this.store !== null) {
      return;
    }
    this.store = await preferences.getPreferences(context, PREF_NAME);
  }

  getString(key: string, def: string): string {
    if (this.store === null) {
      return def;
    }
    const v: preferences.ValueType = this.store.getSync(key, def);
    return typeof v === 'string' ? v : def;
  }

  putString(key: string, val: string): void {
    if (this.store === null) {
      return;
    }
    this.store.putSync(key, val);
    this.store.flush();
  }

  getBool(key: string, def: boolean): boolean {
    if (this.store === null) {
      return def;
    }
    const v: preferences.ValueType = this.store.getSync(key, def);
    return typeof v === 'boolean' ? v : def;
  }

  putBool(key: string, val: boolean): void {
    if (this.store === null) {
      return;
    }
    this.store.putSync(key, val);
    this.store.flush();
  }

  getConnKey(deviceId: string): string {
    const keys: Map<string, string> = this.getConnKeyMap();
    const key: string | undefined = keys.get(deviceId);
    return key ?? '';
  }

  setConnKey(deviceId: string, key: string): void {
    const keys: Map<string, string> = this.getConnKeyMap();
    if (key === '') keys.delete(deviceId); else keys.set(deviceId, key);
    const rec: Record<string, string> = {};
    keys.forEach((v: string, k: string): void => {
      rec[k] = v;
    });
    this.putScopedString('connKeys', JSON.stringify(rec));
  }

  clearConnKeys(): void { this.putScopedString('connKeys', '{}'); }

  private getConnKeyMap(): Map<string, string> {
    const raw: string = this.getScopedString('connKeys', '{}');
    let parsed: Record<string, string> = {};
    try { parsed = JSON.parse(raw) as Record<string, string>; } catch (_) {}
    const m: Map<string, string> = new Map<string, string>();
    const ids: Array<string> = Object.keys(parsed);
    for (const id of ids) {
      m.set(id, parsed[id]);
    }
    return m;
  }
}

export const pxcSettings: PxcSettings = new PxcSettings();
