import { reactive, computed } from 'vue'

/**
 * Window manager — the registry of every FloatingWindow on screen and the
 * arbiter of which one is "focused" (front-most).
 *
 * On desktop, windows overlap and z-order alone decides what's on top; this
 * registry just mirrors that so other UI (e.g. the mobile switcher) can see
 * the live set. On a phone (see useCompact) the registry earns its keep: the
 * floating paradigm collapses to a single full-screen window, and that window
 * is `focusedWindowId` — the visible window with the highest z. Bringing any
 * window to front (menu action, switcher tap) makes it the one shown.
 *
 * Kept as a plain reactive module rather than a Pinia store to sit alongside
 * the existing dock/layout system in modules/advanced.ts, and so FloatingWindow
 * can drive it without a setup-time store handle.
 */

interface WinMeta {
  id: string
  title: string
  visible: boolean
  z: number
}

const wins = reactive(new Map<string, WinMeta>())

/* Per-window "raise me" nonce. focusWindow() bumps it; FloatingWindow watches
 * its own entry and calls bringToFront(). This is the centralized focus path
 * the mobile switcher uses — distinct from each window's app-supplied
 * focus-token prop, but with the same effect. */
const focusReq = reactive<Record<string, number>>({})

export function registerWindow(id: string, title: string, z: number): void {
  const w = wins.get(id)
  if (w) { w.title = title }
  else wins.set(id, { id, title, visible: false, z })
}

export function unregisterWindow(id: string): void {
  wins.delete(id)
  delete focusReq[id]
}

export function setWindowTitle(id: string, title: string): void {
  const w = wins.get(id)
  if (w) w.title = title
}

export function setWindowVisible(id: string, visible: boolean): void {
  const w = wins.get(id)
  if (w) w.visible = visible
}

export function setWindowZ(id: string, z: number): void {
  const w = wins.get(id)
  if (w) w.z = z
}

/** Request that a window be raised to the front (and thus, on a phone, shown). */
export function focusWindow(id: string): void {
  focusReq[id] = (focusReq[id] ?? 0) + 1
}

/** Read a window's focus nonce — FloatingWindow watches this to know it was
 *  asked to raise itself via focusWindow(). */
export function windowFocusReq(id: string): number {
  return focusReq[id] ?? 0
}

/** The front-most currently-visible window, or null when none are open. In
 *  compact mode this is the single window painted full-screen. */
export const focusedWindowId = computed<string | null>(() => {
  let best: WinMeta | null = null
  for (const w of wins.values()) {
    if (!w.visible) continue
    if (!best || w.z > best.z) best = w
  }
  return best ? best.id : null
})

/** Visible windows, front-most first — the list backing the mobile switcher. */
export const openWindows = computed<WinMeta[]>(() =>
  [...wins.values()].filter(w => w.visible).sort((a, b) => b.z - a.z),
)
