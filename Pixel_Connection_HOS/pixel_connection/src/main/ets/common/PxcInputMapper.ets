/**
 * pxc 输入事件构造与映射：
 *  - OHOS KeyCode（multimodalInput）→ Qt::Key（被控端统一键码，见
 *    原仓库 src/platform/windows/input_injector_win.cpp 的 qt_key_to_vk）
 *  - pxc 协议 4 类输入事件 JSON（对照 apps/client/session_wire.h 注释）：
 *      {"type":"mouse_move","screen":0,"x":0.5,"y":0.5}
 *      {"type":"mouse_button","button":"left","pressed":true}
 *      {"type":"wheel","dy":1}
 *      {"type":"key","key":65,"text":"a","pressed":true,"mods":0}
 *  - PxcTouchMouseMapper：触屏手势→鼠标事件状态机 + 物理鼠标/滚轮/键盘直通
 */

export interface PxcNormPoint {
  x: number;
  y: number;
}

// ---------------------------------------------------------------- 键码映射

// OHOS KeyCode → Qt::Key（字母数字标点同 ASCII；特殊键为 Qt 0x010000xx 段）
const KEY_TO_QT: Map<number, number> = new Map<number, number>([
  // 数字 0-9（KEYCODE_0=2000..KEYCODE_9=2009）
  [2000, 0x30], [2001, 0x31], [2002, 0x32], [2003, 0x33], [2004, 0x34],
  [2005, 0x35], [2006, 0x36], [2007, 0x37], [2008, 0x38], [2009, 0x39],
  // 字母 A-Z（KEYCODE_A=2017..KEYCODE_Z=2042）
  [2017, 0x41], [2018, 0x42], [2019, 0x43], [2020, 0x44], [2021, 0x45],
  [2022, 0x46], [2023, 0x47], [2024, 0x48], [2025, 0x49], [2026, 0x4A],
  [2027, 0x4B], [2028, 0x4C], [2029, 0x4D], [2030, 0x4E], [2031, 0x4F],
  [2032, 0x50], [2033, 0x51], [2034, 0x52], [2035, 0x53], [2036, 0x54],
  [2037, 0x55], [2038, 0x56], [2039, 0x57], [2040, 0x58], [2041, 0x59],
  [2042, 0x5A],
  // 方向键（DPAD）
  [2012, 0x01000013],  // Up
  [2013, 0x01000015],  // Down
  [2014, 0x01000012],  // Left
  [2015, 0x01000014],  // Right
  // 标点
  [2043, 0x2C],        // ,   KEYCODE_COMMA
  [2044, 0x2E],        // .   KEYCODE_PERIOD
  [2056, 0x60],        // `   KEYCODE_GRAVE
  [2057, 0x2D],        // -   KEYCODE_MINUS
  [2058, 0x3D],        // =   KEYCODE_EQUALS
  [2059, 0x5B],        // [   KEYCODE_LEFT_BRACKET
  [2060, 0x5D],        // ]   KEYCODE_RIGHT_BRACKET
  [2061, 0x5C],        // \   KEYCODE_BACKSLASH
  [2062, 0x3B],        // ;   KEYCODE_SEMICOLON
  [2063, 0x27],        // '   KEYCODE_APOSTROPHE
  [2064, 0x2F],        // /   KEYCODE_SLASH
  // 功能区
  [2049, 0x01000001],  // Tab
  [2050, 0x20],        // Space
  [2054, 0x01000004],  // Enter → Qt Key_Return
  [2055, 0x01000003],  // Backspace（KEYCODE_DEL）
  [2068, 0x01000016],  // PageUp
  [2069, 0x01000017],  // PageDown
  [2070, 0x01000000],  // Escape
  [2071, 0x01000007],  // Delete（KEYCODE_FORWARD_DEL）
  [2080, 0x01000008],  // Pause/Break（KEYCODE_BREAK）
  [2081, 0x01000010],  // Home（KEYCODE_MOVE_HOME）
  [2082, 0x01000011],  // End（KEYCODE_MOVE_END）
  [2083, 0x01000006],  // Insert
  [2074, 0x01000024],  // CapsLock
  [2075, 0x01000026],  // ScrollLock
  [2102, 0x01000025],  // NumLock
  // 修饰键（左右合并，被控端 VK 同为通用 VK_SHIFT/VK_CONTROL/VK_MENU）
  [2047, 0x01000020],  // Shift Left
  [2048, 0x01000020],  // Shift Right
  [2045, 0x01000023],  // Alt Left
  [2046, 0x01000023],  // Alt Right
  [2072, 0x01000021],  // Ctrl Left
  [2073, 0x01000021],  // Ctrl Right
  [2076, 0x01000022],  // Meta Left
  [2077, 0x01000022],  // Meta Right
  // F1-F12（KEYCODE_F1=2090..KEYCODE_F12=2101 → Qt 0x01000030+）
  [2090, 0x01000030], [2091, 0x01000031], [2092, 0x01000032], [2093, 0x01000033],
  [2094, 0x01000034], [2095, 0x01000035], [2096, 0x01000036], [2097, 0x01000037],
  [2098, 0x01000038], [2099, 0x01000039], [2100, 0x0100003A], [2101, 0x0100003B]
]);

/** OHOS KeyCode → Qt::Key；未映射返回 0（调用方丢弃该键）。 */
export function pxcMapKeyCode(keyCode: number): number {
  const mapped: number | undefined = KEY_TO_QT.get(keyCode);
  return mapped ?? 0;
}

/** 可打印 ASCII 区（0x20-0x7E）返回对应字符，其余返回空串（被控端用 text 做布局兜底）。 */
export function pxcQtKeyToText(qtKey: number): string {
  if (qtKey >= 0x20 && qtKey <= 0x7E) {
    return String.fromCharCode(qtKey);
  }
  return '';
}

// ---------------------------------------------------------------- 事件 JSON

function pxcFmt(v: number): string {
  return (Math.round(v * 10000) / 10000).toString();
}

export function pxcMouseMoveJson(nx: number, ny: number, screen: number): string {
  return `{"type":"mouse_move","x":${pxcFmt(nx)},"y":${pxcFmt(ny)},"screen":${screen}}`;
}

export function pxcMouseButtonJson(button: string, pressed: boolean): string {
  return `{"type":"mouse_button","button":"${button}","pressed":${pressed}}`;
}

export function pxcWheelJson(dy: number): string {
  return `{"type":"wheel","dy":${dy}}`;
}

export function pxcKeyJson(qtKey: number, text: string, pressed: boolean, mods: number = 0): string {
  return `{"type":"key","key":${qtKey},"text":${JSON.stringify(text)},` +
    `"pressed":${pressed},"mods":${mods}}`;
}

/** Shared physical/virtual key state; modifiers are real key-down/up events, not labels. */
export class PxcKeyboard {
  private held: Set<number> = new Set<number>();
  private send: (json: string) => void;
  constructor(send: (json: string) => void) { this.send = send; }

  modifiers(): number {
    let value: number = 0;
    if (this.held.has(0x01000020)) value |= 0x02000000;
    if (this.held.has(0x01000021)) value |= 0x04000000;
    if (this.held.has(0x01000023)) value |= 0x08000000;
    if (this.held.has(0x01000022)) value |= 0x10000000;
    return value;
  }

  isHeld(key: number): boolean { return this.held.has(key); }

  key(key: number, pressed: boolean): void {
    if (pressed) this.held.add(key);
    else if (!this.held.delete(key)) return;
    const shifted: boolean = this.held.has(0x01000020);
    let text: string = pxcQtKeyToText(key);
    if (key >= 65 && key <= 90 && !shifted) text = text.toLowerCase();
    this.send(pxcKeyJson(key, text, pressed, this.modifiers()));
  }

  tap(key: number): void { this.key(key, true); this.key(key, false); }

  release(): void {
    const keys: Array<number> = Array.from(this.held);
    for (const key of keys) this.key(key, false);
  }
}

// ---------------------------------------------------------------- 手势状态机

// 触屏手势：
//   单指按下/移动/抬起 → 左键按下/移动/抬起（按下前先同步位置，同桌面端）
//   长按（600ms 未移动）→ 右键单击
//   双指纵向拖动      → 滚轮（每 40vp 一格）
//   三指轻点          → 中键单击
const MODE_IDLE = 0;        // 无触摸
const MODE_DRAG = 1;        // 单指拖拽（左键按下中）
const MODE_CONSUMED = 2;    // 长按已消费（等抬起，不再发左键抬起）
const MODE_SCROLL = 3;      // 双指滚动
const MODE_IGNORE = 4;      // 手势结束后的残余手指，全部抬起前忽略
const MODE_TAP3 = 5;        // 三指轻点判定中

const LONG_PRESS_MS = 600;
const DRAG_SLOP_VP2 = 64;       // 8vp 位移平方：超过视为拖动，取消长按
const TAP3_SLOP_VP2 = 256;      // 16vp
const TAP3_MAX_MS = 300;
const SCROLL_NOTCH_VP = 40;     // 双指每滑 40vp = 远端滚轮一格
const WHEEL_NOTCH_VP = 64;      // 物理滚轮轴值累计（vp），≈一格；真机校准
const MOVE_THROTTLE_MS = 16;    // 移动节流，同桌面端

export class PxcTouchMouseMapper {
  keyboard: PxcKeyboard;
  private physicalKeys: Map<number, number> = new Map<number, number>();
  private physicalButtons: Set<string> = new Set<string>();
  private send: (json: string) => void;
  private normalize: (x: number, y: number) => PxcNormPoint;
  private screen: number = 0;

  private mode: number = MODE_IDLE;
  private downX: number = 0;
  private downY: number = 0;
  private dragMoved: boolean = false;
  private tap3Moved: boolean = false;
  private tap3Time: number = 0;
  private longPressTimer: number = -1;
  private lastAvgY: number = 0;
  private scrollAcc: number = 0;
  private wheelAcc: number = 0;

  // 移动节流（pending 非负表示有待发位置；按下/抬起前强制冲刷）
  private pendingX: number = -1;
  private pendingY: number = -1;
  private lastMoveMs: number = 0;

  constructor(send: (json: string) => void, normalize: (x: number, y: number) => PxcNormPoint) {
    this.send = send;
    this.normalize = normalize;
    this.keyboard = new PxcKeyboard(send);
  }

  setScreen(screen: number): void {
    this.screen = screen;
  }

  // -------------------------------------------------- 触屏

  onTouch(event: TouchEvent): void {
    if (event.type === TouchType.Down) {
      this.onDown(event.touches);
      return;
    }
    if (event.type === TouchType.Move) {
      this.onMove(event.touches);
      return;
    }
    if (event.type === TouchType.Up) {
      const changed: Set<number> = new Set<number>(event.changedTouches.map((t: TouchObject): number => t.id));
      this.onUp(event.touches.filter((t: TouchObject): boolean => !changed.has(t.id)));
      return;
    }
    // Cancel：异常中断，按「释放全部按键」清理
    this.reset(true);
  }

  private onDown(touches: Array<TouchObject>): void {
    const n: number = touches.length;
    if (n === 1) {
      const t: TouchObject = touches[0];
      // 先把光标同步到按下点，再发左键按下（保证点击落在正确位置）
      const p: PxcNormPoint = this.normalize(t.x, t.y);
      this.pendingX = p.x;
      this.pendingY = p.y;
      this.flushMove(true);
      this.send(pxcMouseButtonJson('left', true));
      this.mode = MODE_DRAG;
      this.downX = t.x;
      this.downY = t.y;
      this.dragMoved = false;
      this.clearLongPressTimer();
      this.longPressTimer = setTimeout(() => {
        this.onLongPress();
      }, LONG_PRESS_MS);
      return;
    }
    if (n === 2) {
      this.cancelDrag();
      this.clearLongPressTimer();
      this.mode = MODE_SCROLL;
      this.lastAvgY = PxcTouchMouseMapper.avgY(touches);
      this.scrollAcc = 0;
      return;
    }
    // 三指及以上：进入三指点按判定
    this.cancelDrag();
    this.clearLongPressTimer();
    this.mode = MODE_TAP3;
    this.tap3Moved = false;
    this.tap3Time = Date.now();
    this.downX = PxcTouchMouseMapper.avgX(touches);
    this.downY = PxcTouchMouseMapper.avgY(touches);
  }

  private onMove(touches: Array<TouchObject>): void {
    if (this.mode === MODE_DRAG && touches.length === 1) {
      const t: TouchObject = touches[0];
      if (!this.dragMoved) {
        const dx: number = t.x - this.downX;
        const dy: number = t.y - this.downY;
        if (dx * dx + dy * dy > DRAG_SLOP_VP2) {
          this.dragMoved = true;
          this.clearLongPressTimer();
        }
      }
      const p: PxcNormPoint = this.normalize(t.x, t.y);
      this.pendingX = p.x;
      this.pendingY = p.y;
      this.flushMove(false);
      return;
    }
    if (this.mode === MODE_SCROLL && touches.length === 2) {
      const ay: number = PxcTouchMouseMapper.avgY(touches);
      this.scrollAcc += ay - this.lastAvgY;
      this.lastAvgY = ay;
      while (Math.abs(this.scrollAcc) >= SCROLL_NOTCH_VP) {
        const sign: number = this.scrollAcc > 0 ? 1 : -1;
        this.scrollAcc -= sign * SCROLL_NOTCH_VP;
        // 手指上滑（avgY 减小）→ 远端滚轮向上（dy 正）
        this.send(pxcWheelJson(-sign));
      }
      return;
    }
    if (this.mode === MODE_TAP3 && touches.length >= 3) {
      const dx: number = PxcTouchMouseMapper.avgX(touches) - this.downX;
      const dy: number = PxcTouchMouseMapper.avgY(touches) - this.downY;
      if (dx * dx + dy * dy > TAP3_SLOP_VP2) {
        this.tap3Moved = true;
      }
    }
  }

  private onUp(touches: Array<TouchObject>): void {
    const remaining: number = touches.length;
    if (this.mode === MODE_DRAG) {
      if (remaining === 0) {
        this.clearLongPressTimer();
        this.flushMove(true);
        this.send(pxcMouseButtonJson('left', false));
        this.mode = MODE_IDLE;
      }
      return;
    }
    if (this.mode === MODE_CONSUMED) {
      if (remaining === 0) {
        this.mode = MODE_IDLE;
      }
      return;
    }
    if (this.mode === MODE_SCROLL) {
      if (remaining <= 1) {
        this.scrollAcc = 0;
        this.mode = remaining === 0 ? MODE_IDLE : MODE_IGNORE;
      }
      return;
    }
    if (this.mode === MODE_TAP3) {
      if (remaining === 0) {
        const held: number = Date.now() - this.tap3Time;
        if (!this.tap3Moved && held < TAP3_MAX_MS) {
          this.send(pxcMouseButtonJson('middle', true));
          this.send(pxcMouseButtonJson('middle', false));
        }
        this.mode = MODE_IDLE;
      }
      return;
    }
    if (this.mode === MODE_IGNORE && remaining === 0) {
      this.mode = MODE_IDLE;
    }
  }

  private onLongPress(): void {
    if (this.mode !== MODE_DRAG || this.dragMoved) {
      return;
    }
    this.mode = MODE_CONSUMED;
    // 长按 = 右键单击：先释放已按下的左键
    this.send(pxcMouseButtonJson('left', false));
    this.send(pxcMouseButtonJson('right', true));
    this.send(pxcMouseButtonJson('right', false));
  }

  private cancelDrag(): void {
    if (this.mode === MODE_DRAG) {
      this.send(pxcMouseButtonJson('left', false));
    }
  }

  private clearLongPressTimer(): void {
    if (this.longPressTimer >= 0) {
      clearTimeout(this.longPressTimer);
      this.longPressTimer = -1;
    }
  }

  // -------------------------------------------------- 物理鼠标 / 滚轮 / 键盘

  onMouseEvent(event: MouseEvent): void {
    if (event.action === MouseAction.Move) {
      const p: PxcNormPoint = this.normalize(event.x, event.y);
      this.pendingX = p.x;
      this.pendingY = p.y;
      this.flushMove(false);
      return;
    }
    if (event.action === MouseAction.Press || event.action === MouseAction.Release) {
      const p: PxcNormPoint = this.normalize(event.x, event.y);
      this.pendingX = p.x;
      this.pendingY = p.y;
      this.flushMove(true);
      const pressed: boolean = event.action === MouseAction.Press;
      if (event.button === MouseButton.Left) {
        if (pressed) this.physicalButtons.add('left'); else this.physicalButtons.delete('left');
        this.send(pxcMouseButtonJson('left', pressed));
      } else if (event.button === MouseButton.Right) {
        if (pressed) this.physicalButtons.add('right'); else this.physicalButtons.delete('right');
        this.send(pxcMouseButtonJson('right', pressed));
      } else if (event.button === MouseButton.Middle) {
        if (pressed) this.physicalButtons.add('middle'); else this.physicalButtons.delete('middle');
        this.send(pxcMouseButtonJson('middle', pressed));
      }
    }
  }

  onAxisEvent(event: AxisEvent): void {
    const v: number = event.getVerticalAxisValue();
    this.wheelAcc += v;
    while (Math.abs(this.wheelAcc) >= WHEEL_NOTCH_VP) {
      const sign: number = this.wheelAcc > 0 ? 1 : -1;
      this.wheelAcc -= sign * WHEEL_NOTCH_VP;
      // 轴值正 = 滚轮向上 → 远端 dy 正（方向待真机校准）
      this.send(pxcWheelJson(sign));
    }
  }

  onKeyEvent(event: KeyEvent): void {
    const qtKey: number = pxcMapKeyCode(event.keyCode);
    if (qtKey === 0) {
      return;
    }
    if (event.type === KeyType.Down) {
      this.physicalKeys.set(event.keyCode, qtKey);
      this.keyboard.key(qtKey, true);
    } else {
      this.physicalKeys.delete(event.keyCode);
      // Left/right modifiers share a Qt key; release it only after both physical keys are up.
      if (!Array.from(this.physicalKeys.values()).includes(qtKey)) this.keyboard.key(qtKey, false);
    }
  }

  // -------------------------------------------------- 清理

  /** 释放所有已按下按键、清理定时器；页面销毁/会话结束时调用。 */
  reset(releaseButtons: boolean): void {
    this.clearLongPressTimer();
    if (releaseButtons) {
      this.cancelDrag();
      for (const button of this.physicalButtons) this.send(pxcMouseButtonJson(button, false));
      this.physicalButtons.clear();
      this.physicalKeys.clear();
      this.keyboard.release();
    }
    this.mode = MODE_IDLE;
    this.scrollAcc = 0;
    this.wheelAcc = 0;
    this.pendingX = -1;
    this.pendingY = -1;
  }

  // -------------------------------------------------- 内部工具

  private flushMove(force: boolean): void {
    if (this.pendingX < 0) {
      return;
    }
    const now: number = Date.now();
    if (!force && now - this.lastMoveMs < MOVE_THROTTLE_MS) {
      return;
    }
    this.lastMoveMs = now;
    this.send(pxcMouseMoveJson(this.pendingX, this.pendingY, this.screen));
    this.pendingX = -1;
    this.pendingY = -1;
  }

  private static avgX(touches: Array<TouchObject>): number {
    let sum: number = 0;
    for (const t of touches) {
      sum += t.x;
    }
    return touches.length > 0 ? sum / touches.length : 0;
  }

  private static avgY(touches: Array<TouchObject>): number {
    let sum: number = 0;
    for (const t of touches) {
      sum += t.y;
    }
    return touches.length > 0 ? sum / touches.length : 0;
  }
}
