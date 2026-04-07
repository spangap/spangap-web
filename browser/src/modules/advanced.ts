import { ref, reactive, computed, watch } from 'vue'
import { useMenuStore } from '../stores/menu'
import { useDeviceStore } from '../stores/device'

/* ── Visibility ── */
export const cliVisible = ref(false)
export const logVisible = ref(false)

/* ── Dock system ──
 * A window can be docked to one edge (top/bottom/left/right) or floating (null).
 * Docked windows expand: top/bottom → full width, left/right → full height,
 * within the area remaining after earlier-docked windows.
 * dockOrder determines priority — first entry gets full span. */

export type DockSide = 'top' | 'bottom' | 'left' | 'right'

interface DockInfo {
  side: DockSide | null
  size: number       // % — height for top/bottom, width for left/right
}

export const docks = reactive<Record<string, DockInfo>>({
  cli: { side: null, size: 20 },
  log: { side: null, size: 70 },
})

export const dockOrder = reactive<string[]>([])

export function dockWindow(id: string, side: DockSide, size: number) {
  docks[id].side = side
  docks[id].size = size
  const idx = dockOrder.indexOf(id)
  if (idx >= 0) dockOrder.splice(idx, 1)
  dockOrder.push(id)
}

export function undockWindow(id: string) {
  docks[id].side = null
  const idx = dockOrder.indexOf(id)
  if (idx >= 0) dockOrder.splice(idx, 1)
}

/** Rect in container percentages */
export interface Rect { x: number; y: number; w: number; h: number }
interface Edges { top: number; left: number; right: number; bottom: number }

/** Compute docked window rects + remaining floating area, respecting dock order */
export const layout = computed(() => {
  const avail: Edges = { top: 0, left: 0, right: 100, bottom: 100 }
  const rects: Record<string, Rect> = {}

  for (const id of dockOrder) {
    const dock = docks[id]
    if (!dock.side) continue
    const s = dock.size

    switch (dock.side) {
      case 'bottom':
        rects[id] = { x: avail.left, y: avail.bottom - s, w: avail.right - avail.left, h: s }
        avail.bottom -= s
        break
      case 'top':
        rects[id] = { x: avail.left, y: avail.top, w: avail.right - avail.left, h: s }
        avail.top += s
        break
      case 'left':
        rects[id] = { x: avail.left, y: avail.top, w: s, h: avail.bottom - avail.top }
        avail.left += s
        break
      case 'right':
        rects[id] = { x: avail.right - s, y: avail.top, w: s, h: avail.bottom - avail.top }
        avail.right -= s
        break
    }
  }

  return { rects, floatingArea: { ...avail } }
})

/** CSS style for the video area — the rect remaining after all docked windows */
export const videoStyle = computed(() => {
  const a = layout.value.floatingArea
  return {
    position: 'absolute' as const,
    top: `${a.top}%`,
    left: `${a.left}%`,
    width: `${a.right - a.left}%`,
    height: `${a.bottom - a.top}%`,
  }
})

export function registerAdvanced() {
  useMenuStore().register('advanced', 'Advanced', 90, [
    { id: 'adv.cli', label: 'CLI', type: 'action', order: 10, action: () => { cliVisible.value = !cliVisible.value } },
    { id: 'adv.log', label: 'System Log', type: 'action', order: 20, action: () => { logVisible.value = !logVisible.value } },
  ])

  /* Restore visibility + dock state from config when device connects */
  const device = useDeviceStore()
  watch(() => device.connected, (connected) => {
    if (!connected) return
    if (device.get('s.cli.win.visible') === 1) cliVisible.value = true
    if (device.get('s.log.win.visible') === 1) logVisible.value = true

    /* Restore docks — vertical (top/bottom) before horizontal for deterministic order */
    for (const id of ['cli', 'log']) {
      const side = device.get(`s.${id}.win.dock`) as string
      const size = device.get(`s.${id}.win.dock_size`) as number
      if (side && side !== '' && typeof size === 'number') {
        dockWindow(id, side as DockSide, size)
      }
    }
  })
}
