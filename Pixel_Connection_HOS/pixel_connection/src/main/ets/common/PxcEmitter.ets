import pxc from 'libpxc_hos.so';

export type PxcEventHandler = (json: string) => void;

/**
 * native 事件总线：包一层 pxc.setEventListener，
 * 页面按事件名订阅/退订（可在多页面并存，互不干扰）。
 */
export class PxcEmitter {
  private handlers: Map<string, Array<PxcEventHandler>> = new Map<string, Array<PxcEventHandler>>();
  private initialized: boolean = false;

  init(): void {
    if (this.initialized) {
      return;
    }
    this.initialized = true;
    pxc.setEventListener((event: string, json: string): void => {
      this.dispatch(event, json);
    });
  }

  on(event: string, handler: PxcEventHandler): void {
    const list: Array<PxcEventHandler> | undefined = this.handlers.get(event);
    if (list === undefined) {
      const newList: Array<PxcEventHandler> = [handler];
      this.handlers.set(event, newList);
      return;
    }
    list.push(handler);
  }

  off(event: string, handler: PxcEventHandler): void {
    const list: Array<PxcEventHandler> | undefined = this.handlers.get(event);
    if (list === undefined) {
      return;
    }
    const idx: number = list.indexOf(handler);
    if (idx >= 0) {
      list.splice(idx, 1);
    }
  }

  dispatch(event: string, json: string): void {
    const list: Array<PxcEventHandler> | undefined = this.handlers.get(event);
    if (list === undefined) {
      return;
    }
    for (const handler of list) {
      handler(json);
    }
  }
}

export const pxcEmitter: PxcEmitter = new PxcEmitter();
