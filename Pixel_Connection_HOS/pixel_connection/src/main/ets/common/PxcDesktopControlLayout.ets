export interface PxcControlPoint { x: number; y: number; }
export interface PxcControlRect { x: number; y: number; width: number; height: number; }

export function pxcUsesDesktopControls(deviceType: string, width: number): boolean {
  return deviceType === '2in1' || (deviceType === 'tablet' && width >= 840);
}

export function pxcClampControlPoint(x: number, y: number, width: number, height: number): PxcControlPoint {
  const maxX: number = Math.max(0, width - 52);
  const maxY: number = Math.max(0, height - 52);
  return { x: Math.min(maxX, Math.max(Math.min(8, maxX), x)),
    y: Math.min(maxY, Math.max(Math.min(8, maxY), y)) };
}

export function pxcDesktopControlRect(point: PxcControlPoint, width: number, height: number): PxcControlRect {
  const panelWidth: number = Math.max(0, Math.min(320, width - 16));
  const panelHeight: number = Math.max(0, Math.min(620, height - 16));
  const anchorX: number = point.x + 52 + panelWidth <= width - 8 ? point.x + 52 : point.x - panelWidth - 8;
  return { x: Math.max(8, Math.min(width - panelWidth - 8, anchorX)),
    y: Math.max(8, Math.min(height - panelHeight - 8, point.y)), width: panelWidth, height: panelHeight };
}

export function pxcClampPanelRect(x: number, y: number, width: number, height: number): PxcControlRect {
  const size: PxcControlRect = pxcDesktopControlRect({ x: 0, y: 0 }, width, height);
  return { x: Math.max(8, Math.min(width - size.width - 8, x)),
    y: Math.max(8, Math.min(height - size.height - 8, y)), width: size.width, height: size.height };
}

export function pxcControlDockEdge(point: PxcControlPoint, width: number, height: number): string {
  const distances: number[] = [point.x, width - point.x - 44, point.y, height - point.y - 44];
  const edges: string[] = ['left', 'right', 'top', 'bottom'];
  let nearest: number = 0;
  for (let i: number = 1; i < distances.length; i++) if (distances[i] < distances[nearest]) nearest = i;
  return distances[nearest] <= 24 ? edges[nearest] : '';
}

export function pxcPanelDockEdge(rect: PxcControlRect, width: number, height: number,
  offsetX: number, offsetY: number): string {
  // A short viewport already clamps the panel vertically. Only the drag axis
  // decides docking, so moving sideways does not accidentally dock at the top.
  if (Math.abs(offsetX) >= Math.abs(offsetY)) {
    if (offsetX < 0 && rect.x <= 24) return 'left';
    if (offsetX > 0 && width - rect.x - rect.width <= 24) return 'right';
  } else {
    if (offsetY < 0 && rect.y <= 24) return 'top';
    if (offsetY > 0 && height - rect.y - rect.height <= 24) return 'bottom';
  }
  return '';
}

export function pxcDockedControlRect(point: PxcControlPoint, edge: string, width: number, height: number): PxcControlRect {
  const vertical: boolean = edge === 'left' || edge === 'right';
  const w: number = vertical ? 14 : 44;
  const h: number = vertical ? 44 : 14;
  return { x: edge === 'left' ? 0 : edge === 'right' ? width - w : Math.max(0, Math.min(width - w, point.x)),
    y: edge === 'top' ? 0 : edge === 'bottom' ? height - h : Math.max(0, Math.min(height - h, point.y)),
    width: w, height: h };
}
