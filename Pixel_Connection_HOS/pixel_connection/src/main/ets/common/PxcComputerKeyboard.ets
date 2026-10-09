import { PxcKeyboard } from './PxcInputMapper';

interface PxcKeyCap { label: string; key: number; shift: boolean; weight: number; }
function cap(label: string, key: number, shift: boolean = false, weight: number = 1): PxcKeyCap {
  return { label: label, key: key, shift: shift, weight: weight };
}
function asciiRow(text: string): Array<PxcKeyCap> {
  return text.split('').map((letter: string): PxcKeyCap => cap(letter, letter.toUpperCase().charCodeAt(0)));
}
const LETTER_ROWS: Array<Array<PxcKeyCap>> = [
  asciiRow('1234567890'), asciiRow('qwertyuiop'), asciiRow('asdfghjkl'),
  [cap('z', 90), cap('x', 88), cap('c', 67), cap('v', 86), cap('b', 66), cap('n', 78), cap('m', 77), cap('⌫', 0x01000003, false, 1.6)],
  [cap('Esc', 0x01000000), cap('Tab', 0x01000001), cap(',', 44), cap('空格', 32, false, 4), cap('.', 46), cap('Enter', 0x01000004, false, 2)]
];
const FUNCTION_ROWS: Array<Array<PxcKeyCap>> = [
  [cap('!', 49, true), cap('@', 50, true), cap('#', 51, true), cap('$', 52, true), cap('%', 53, true), cap('^', 54, true), cap('&', 55, true), cap('*', 56, true), cap('(', 57, true), cap(')', 48, true)],
  [cap('F1', 0x01000030), cap('F2', 0x01000031), cap('F3', 0x01000032), cap('F4', 0x01000033), cap('F5', 0x01000034), cap('F6', 0x01000035), cap('F7', 0x01000036), cap('F8', 0x01000037), cap('F9', 0x01000038), cap('F10', 0x01000039), cap('F11', 0x0100003a), cap('F12', 0x0100003b)],
  [cap('PrtSc', 0x01000009), cap('ScrLk', 0x01000026), cap('Pause', 0x01000008), cap('`', 96), cap('-', 45), cap('=', 61), cap('[', 91), cap(']', 93), cap('\\', 92), cap(';', 59), cap("'", 39), cap('/', 47)],
  [cap('Insert', 0x01000006), cap('Home', 0x01000010), cap('PgUp', 0x01000016), cap('Caps', 0x01000024), cap('↑', 0x01000013)],
  [cap('Delete', 0x01000007), cap('End', 0x01000011), cap('PgDn', 0x01000017), cap('←', 0x01000012), cap('↓', 0x01000015), cap('→', 0x01000014)]
];
const MODIFIERS: Array<PxcKeyCap> = [cap('Ctrl', 0x01000021), cap('Shift', 0x01000020), cap('Alt', 0x01000023), cap('Win', 0x01000022)];

@Component
export struct PxcComputerKeyboard {
  keyboard: PxcKeyboard = new PxcKeyboard((): void => {});
  @State heldKeys: Array<number> = [];
  @State page: number = 0;
  @State sticky: boolean = false;
  @Prop compact: boolean = false;
  onStickyChanged: (value: boolean) => void = (): void => {};

  private toggleModifier(key: number): void {
    this.keyboard.key(key, !this.keyboard.isHeld(key));
    this.heldKeys = MODIFIERS.filter((k: PxcKeyCap): boolean => this.keyboard.isHeld(k.key)).map((k: PxcKeyCap): number => k.key);
  }
  private pressKey(key: PxcKeyCap): void {
    const temporaryShift: boolean = key.shift && !this.keyboard.isHeld(0x01000020);
    if (temporaryShift) this.keyboard.key(0x01000020, true);
    this.keyboard.tap(key.key);
    if (temporaryShift) this.keyboard.key(0x01000020, false);
    if (!this.sticky) { this.keyboard.release(); this.heldKeys = []; }
  }
  @Builder
  KeyRows(rows: Array<Array<PxcKeyCap>>) {
    Column({ space: 4 }) {
      ForEach(rows, (row: Array<PxcKeyCap>, index: number) => {
        Row({ space: 4 }) {
          ForEach(row, (key: PxcKeyCap) => {
            Button(key.label, { type: ButtonType.Normal }).layoutWeight(key.weight).height(this.compact ? 25 : 34)
              .fontSize(key.label.length > 3 ? 10 : 12).fontColor('#edf3ff').backgroundColor('#364458').borderRadius(5).padding(0)
              .onClick(() => { this.pressKey(key); })
          }, (key: PxcKeyCap): string => key.label)
        }.width('100%')
      })
    }.width('100%')
  }
  build() {
    Column({ space: 6 }) {
      Row({ space: 5 }) {
        Toggle({ type: ToggleType.Checkbox, isOn: this.sticky }).width(20).height(20).selectedColor('#4385ff')
          .onChange((v: boolean) => { this.sticky = v; this.onStickyChanged(v); if (!v) { this.keyboard.release(); this.heldKeys = []; } })
        Text('组合键保持').fontSize(11).fontColor('#acb8cd').margin({ right: 6 })
        ForEach(MODIFIERS, (key: PxcKeyCap) => {
          Button(key.label, { type: ButtonType.Normal }).layoutWeight(1).height(28).fontSize(12).borderRadius(6)
            .backgroundColor(this.heldKeys.includes(key.key) ? '#4385ff' : '#364458').onClick(() => { this.toggleModifier(key.key); })
        })
      }.width('100%')
      Swiper() { this.KeyRows(LETTER_ROWS); this.KeyRows(FUNCTION_ROWS) }
        .loop(false).index(this.page).onChange((page: number) => { this.page = page; })
        .indicator(false).width('100%').height(this.compact ? 145 : 190)
      Row({ space: 12 }) {
        Text('字母区').fontSize(11).fontColor(this.page === 0 ? '#71a7ff' : '#8998ad').onClick(() => { this.page = 0; })
        Text('‹  左右滑动翻页  ›').fontSize(10).fontColor('#8998ad')
        Text('功能区').fontSize(11).fontColor(this.page === 1 ? '#71a7ff' : '#8998ad').onClick(() => { this.page = 1; })
      }
    }.width('100%').height(this.compact ? 206 : 252).padding({ left: 8, right: 8, bottom: 6 }).backgroundColor('#1e293a')
  }
}
