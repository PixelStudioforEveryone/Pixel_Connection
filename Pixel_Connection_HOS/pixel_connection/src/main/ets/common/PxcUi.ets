import { fileIo } from '@kit.CoreFileKit';
import pxc from 'libpxc_hos.so';
import { PxcDeviceItem } from '../model/PxcModels';

export function pxcSameDevice(a: string, b: string): boolean {
  return a.replace(/[-\s]/g, '').toUpperCase() === b.replace(/[-\s]/g, '').toUpperCase();
}

export function pxcCanControl(device: PxcDeviceItem, localId: string): boolean {
  const platform: string = device.platform.toLowerCase().replace(/[-_\s]/g, '');
  return device.online && !pxcSameDevice(device.deviceId, localId) &&
    !platform.includes('harmonyos') && !platform.startsWith('openharmony') && platform !== 'ohos';
}

export function pxcWallpaper(deviceId: string): string | Resource {
  const path: string = pxc.cachedWallpaper(deviceId);
  try {
    if (fileIo.accessSync(path)) return 'file://' + path;
  } catch (_) {}
  return $r('app.media.wallpaper_placeholder');
}

export function pxcIcon(name: string): Resource {
  switch (name) {
    case 'devices': return $r('app.media.icon_devices');
    case 'assist': return $r('app.media.icon_assist');
    case 'gear': return $r('app.media.icon_gear');
    case 'keyboard': return $r('app.media.icon_keyboard');
    case 'mouse': return $r('app.media.icon_mouse');
    case 'file': return $r('app.media.icon_file');
    case 'view': return $r('app.media.icon_view');
    case 'back': return $r('app.media.icon_back');
    case 'chevron': return $r('app.media.icon_chevron');
    case 'more': return $r('app.media.icon_more');
    case 'grid': return $r('app.media.icon_grid');
    case 'shield': return $r('app.media.icon_shield');
    case 'close': return $r('app.media.icon_close');
    case 'plus': return $r('app.media.icon_plus');
    case 'move': return $r('app.media.icon_move');
    case 'copy': return $r('app.media.icon_copy');
    case 'network': return $r('app.media.icon_network');
    case 'power': return $r('app.media.icon_power');
    case 'rotate': return $r('app.media.icon_rotate');
    case 'pointer': return $r('app.media.icon_pointer');
    case 'eye': return $r('app.media.icon_eye');
    default: return $r('app.media.icon_devices');
  }
}

@Component
export struct PxcIcon {
  @Prop name: string = 'devices';
  @Prop pixelSize: number = 24;
  @Prop tint: ResourceColor = '#24364b';
  build() {
    // SVG paths supply black strokes and fill="none". fillColor would fill enclosed outlines.
    Image(pxcIcon(this.name)).width(this.pixelSize).height(this.pixelSize)
  }
}
