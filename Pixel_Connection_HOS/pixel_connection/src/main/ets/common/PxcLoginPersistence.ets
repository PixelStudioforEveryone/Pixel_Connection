import { asset } from '@kit.AssetStoreKit';
import { util } from '@kit.ArkTS';
import pxc from 'libpxc_hos.so';
import { pxcSettings, PXC_DEFAULT_API, PXC_DEFAULT_WS } from './PxcSettings';
import { pxcEmitter } from './PxcEmitter';
import { PxcLoginResultJson } from '../model/PxcModels';

export interface PxcSavedLogin {
  apiUrl: string;
  wsUrl: string;
  identifier: string;
  password: string;
  token: string;
  username: string;
}

const PASSWORD_ALIAS: string = 'pxc.login.password.v1';
const SESSION_ALIAS: string = 'pxc.login.session.v1';

// Secrets live in the system Asset Store, never in Preferences or event logs.
export class PxcLoginPersistence {
  private initialized: boolean = false;
  private legacyAutoLogin: boolean = false;
  private pendingPassword: PxcSavedLogin | undefined = undefined;

  init(): void {
    if (this.initialized) { return; }
    this.initialized = true;
    if (!pxcSettings.getBool('loginOptionsSeparated', false)) {
      this.legacyAutoLogin = pxcSettings.getBool('rememberPassword', false);
      pxcSettings.putBool('keepLoggedIn', this.legacyAutoLogin);
      pxcSettings.putBool('loginOptionsSeparated', true);
    }
    const legacyPassword: string = pxcSettings.getString('savedPassword', '');
    if (legacyPassword !== '') {
      if (pxcSettings.getBool('rememberPassword', false)) {
        this.remember(pxcSettings.getString('savedIdentifier', ''), legacyPassword);
      }
      pxcSettings.putString('savedPassword', '');
    }
    pxcEmitter.on('loginResult', (json: string): void => {
      const result: PxcLoginResultJson = JSON.parse(json) as PxcLoginResultJson;
      if (result.generation !== undefined && result.generation !== pxc.getLoginSession().generation) { return; }
      if (result.ok) {
        const pending = this.pendingPassword;
        if (pending !== undefined && !this.remember(pending.identifier, pending.password)) {
          pxcEmitter.dispatch('loginPersistenceNotice', '系统未能保存密码，请下次重新输入');
        }
        this.saveSession();
      }
      else if (result.restored && result.invalidSession) { this.remove(SESSION_ALIAS); }
      this.pendingPassword = undefined;
    });
  }

  private bytes(value: string): Uint8Array {
    return new util.TextEncoder().encodeInto(value);
  }

  private record(): PxcSavedLogin {
    return { apiUrl: pxcSettings.getString('apiUrl', PXC_DEFAULT_API),
      wsUrl: pxcSettings.getString('wsUrl', PXC_DEFAULT_WS),
      identifier: '', password: '', token: '', username: '' };
  }

  private write(alias: string, record: PxcSavedLogin): boolean {
    try {
      const attributes: asset.AssetMap = new Map<asset.Tag, asset.Value>();
      attributes.set(asset.Tag.ALIAS, this.bytes(alias));
      attributes.set(asset.Tag.SECRET, this.bytes(JSON.stringify(record)));
      attributes.set(asset.Tag.ACCESSIBILITY, asset.Accessibility.DEVICE_UNLOCKED);
      attributes.set(asset.Tag.CONFLICT_RESOLUTION, asset.ConflictResolution.OVERWRITE);
      asset.addSync(attributes);
      return true;
    } catch (_) { return false; }
  }

  private read(alias: string, apiUrl: string, wsUrl: string): PxcSavedLogin | undefined {
    try {
      const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
      query.set(asset.Tag.ALIAS, this.bytes(alias));
      query.set(asset.Tag.RETURN_TYPE, asset.ReturnType.ALL);
      const records: Array<asset.AssetMap> = asset.querySync(query);
      if (records.length === 0) { return undefined; }
      const bytes: Uint8Array = records[0].get(asset.Tag.SECRET) as Uint8Array;
      const record: PxcSavedLogin = JSON.parse(util.TextDecoder.create('utf-8').decodeToString(bytes)) as PxcSavedLogin;
      if (record.apiUrl !== apiUrl || record.wsUrl !== wsUrl) { return undefined; }
      return record;
    } catch (_) { return undefined; }
  }

  private remove(alias: string): void {
    try {
      const query: asset.AssetMap = new Map<asset.Tag, asset.Value>();
      query.set(asset.Tag.ALIAS, this.bytes(alias));
      asset.removeSync(query);
    } catch (_) { /* Missing records are already cleared. Preference flags prevent reuse. */ }
  }

  savedPassword(apiUrl: string, wsUrl: string): PxcSavedLogin | undefined {
    if (!pxcSettings.getBool('rememberPassword', false)) { return undefined; }
    return this.read(PASSWORD_ALIAS, apiUrl, wsUrl);
  }

  session(): PxcSavedLogin | undefined {
    if (!pxcSettings.getBool('keepLoggedIn', false)) { return undefined; }
    const current: PxcSavedLogin = this.record();
    return this.read(SESSION_ALIAS, current.apiUrl, current.wsUrl);
  }

  legacyLogin(): PxcSavedLogin | undefined {
    if (!this.legacyAutoLogin) { return undefined; }
    this.legacyAutoLogin = false;
    const current: PxcSavedLogin = this.record();
    return this.savedPassword(current.apiUrl, current.wsUrl);
  }

  setOptions(rememberPassword: boolean, keepLoggedIn: boolean): void {
    pxcSettings.putBool('rememberPassword', rememberPassword);
    pxcSettings.putBool('keepLoggedIn', keepLoggedIn);
    if (!rememberPassword) { this.remove(PASSWORD_ALIAS); }
    if (!keepLoggedIn) { this.remove(SESSION_ALIAS); }
  }

  beginLogin(identifier: string, password: string, rememberPassword: boolean, keepLoggedIn: boolean): void {
    this.setOptions(rememberPassword, keepLoggedIn);
    const pending = this.record();
    pending.identifier = identifier;
    pending.password = password;
    this.pendingPassword = pending;
  }

  remember(identifier: string, password: string): boolean {
    if (!pxcSettings.getBool('rememberPassword', false)) { return true; }
    const record: PxcSavedLogin = this.record();
    record.identifier = identifier;
    record.password = password;
    pxcSettings.putString('savedIdentifier', identifier);
    const saved: boolean = this.write(PASSWORD_ALIAS, record);
    if (!saved) { pxcSettings.putBool('rememberPassword', false); }
    return saved;
  }

  private saveSession(): void {
    if (!pxcSettings.getBool('keepLoggedIn', false)) { return; }
    const current = pxc.getLoginSession();
    if (current.token === '' || current.username === '') { return; }
    const record: PxcSavedLogin = this.record();
    record.token = current.token;
    record.username = current.username;
    if (!this.write(SESSION_ALIAS, record)) {
      pxcSettings.putBool('keepLoggedIn', false);
      pxcEmitter.dispatch('loginPersistenceNotice', '系统未能保存登录状态，请下次重新登录');
    }
  }

  clear(): void {
    this.pendingPassword = undefined;
    this.setOptions(false, false);
    pxcSettings.putString('savedPassword', '');
    this.legacyAutoLogin = false;
  }
}

export const pxcLoginPersistence: PxcLoginPersistence = new PxcLoginPersistence();
