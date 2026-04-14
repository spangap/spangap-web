import { ref, reactive, computed } from 'vue'
import { useMenuStore } from '../stores/menu'

/* ── Visibility ──
 * These start false; FloatingWindow restores its own saved visibility from
 * localStorage on mount and emits update:visible to reflect it. */
export const cliVisible = ref(false)
export const logVisible = ref(false)

/* ── Log backlog ──
 * Number of bytes the /log WS should replay on connect. Stored in localStorage. */
const BACKLOG_KEY = 'seccam.log.backlog'
export const logBacklogBytes = ref(Number(localStorage.getItem(BACKLOG_KEY) ?? 8192) || 8192)
function persistBacklog() {
  try { localStorage.setItem(BACKLOG_KEY, String(logBacklogBytes.value)) } catch { /* ignore */ }
}

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

import DeveloperPanel from './panels/DeveloperPanel.vue'

const BACKLOG_PRESETS: Array<[string, number]> = [
  ['1 kB', 1024],
  ['4 kB', 4096],
  ['8 kB', 8192],
  ['16 kB', 16384],
  ['64 kB', 65536],
]

export function registerAdvanced() {
  useMenuStore().register('advanced', 'Advanced', 90, [
    { id: 'adv.cli', label: 'Show CLI', type: 'action', order: 10,
      action: () => { cliVisible.value = !cliVisible.value } },
    { id: 'adv.log', label: 'Show Log', type: 'action', order: 20,
      action: () => { logVisible.value = !logVisible.value } },
    { id: 'adv.backlog', label: 'Backlog Size', type: 'submenu', order: 25,
      children: BACKLOG_PRESETS.map(([label, bytes], i) => ({
        id: `adv.backlog.${bytes}`,
        label,
        type: 'action' as const,
        order: (i + 1) * 10,
        action: () => { logBacklogBytes.value = bytes; persistBacklog() },
      })),
    },
    { id: 'adv.dev', label: 'Developer Options', type: 'panel', order: 30, component: DeveloperPanel },
  ])
}
