import { ref, reactive, computed } from 'vue'
import { useMenuStore } from '../stores/menu'

/* ── Visibility ──
 * These start false; FloatingWindow restores its own saved visibility from
 * localStorage on mount and emits update:visible to reflect it. */
export const cliVisible = ref(false)
export const logVisible = ref(false)

/* ── Log backlog ──
 * Number of bytes the /log WS should replay on connect. Stored in localStorage. */
const BACKLOG_KEY = 'spangap.log.backlog'
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

/** docks is seeded only with the platform windows (cli, log). Consumers
 *  add their own FloatingWindows (reticulous: Map/Reticulum/TCP/UDP/LoRa)
 *  with arbitrary ids, so create the entry on first dock rather than
 *  assuming it exists — otherwise docks[id].side throws "Cannot set
 *  properties of undefined". */
function ensureDock(id: string): DockInfo {
  let d = docks[id]
  if (!d) { d = { side: null, size: 50 }; docks[id] = d }
  return d
}

export function dockWindow(id: string, side: DockSide, size: number) {
  const d = ensureDock(id)
  d.side = side
  d.size = size
  const idx = dockOrder.indexOf(id)
  if (idx >= 0) dockOrder.splice(idx, 1)
  dockOrder.push(id)
}

export function undockWindow(id: string) {
  const d = docks[id]
  if (d) d.side = null
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

import DeveloperPanel from '../panels/DeveloperPanel.vue'
import { openEditor, isPathOpen } from './editor'

const BACKLOG_PRESETS: Array<[string, number]> = [
  ['1 kB', 1024],
  ['4 kB', 4096],
  ['8 kB', 8192],
  ['16 kB', 16384],
  ['64 kB', 65536],
]

const EDIT_FILES: Array<[string, string]> = [
  ['boot',     '/state/boot'],
  ['crontab',  '/state/crontab'],
  ['net_up',   '/state/net_up'],
]

export function registerAdvanced() {
  const menu = useMenuStore()
  menu.register('advanced/cli', 'Show CLI', { type: 'action', action: () => { cliVisible.value = !cliVisible.value } })
  menu.register('advanced/log', 'Show Log', { type: 'action', action: () => { logVisible.value = !logVisible.value } })

  menu.setMenu('advanced/backlog', { label: 'Backlog Size' })
  for (const [label, bytes] of BACKLOG_PRESETS) {
    menu.register(`advanced/backlog/${bytes}`, label,
      { type: 'action', action: () => { logBacklogBytes.value = bytes; persistBacklog() } })
  }

  for (const [name, path] of EDIT_FILES) {
    menu.register(`advanced/edit/${name}`, name,
      { type: 'action', action: () => { openEditor(path, name) } },
      { disabled: () => isPathOpen(path) })
  }

  menu.register('advanced/dev', 'Developer Options', { type: 'panel', component: DeveloperPanel })
}
