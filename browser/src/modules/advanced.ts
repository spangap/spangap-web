import { ref } from 'vue'
import { useMenuStore } from '../stores/menu'
import { registerApp } from '../lib/apps'

/* ── Visibility ──
 * These start false; FloatingWindow restores its own saved visibility from
 * localStorage on mount and emits update:visible to reflect it. */
export const cliVisible = ref(false)
export const logVisible = ref(false)
/* Settings is now a first-class app window (the gear dock icon), not a drawer. */
export const settingsVisible = ref(false)

/* ── Focus nonces ──
 * Bumped by the show* helpers to raise an already-open window to the front.
 * MainLayout binds these to each window's `focus-token` prop. */
export const cliFocus = ref(0)
export const logFocus = ref(0)
export const settingsFocus = ref(0)

/* Dock launch actions: only ever show + raise, never hide. */
export function showCli() { cliVisible.value = true; cliFocus.value++ }
export function showLog() { logVisible.value = true; logFocus.value++ }
export function showSettings() { settingsVisible.value = true; settingsFocus.value++ }

/* ── Log backlog ──
 * Number of bytes the /log WS should replay on connect. Stored in localStorage. */
const BACKLOG_KEY = 'spangap.log.backlog'
export const logBacklogBytes = ref(Number(localStorage.getItem(BACKLOG_KEY) ?? 8192) || 8192)
function persistBacklog() {
  try { localStorage.setItem(BACKLOG_KEY, String(logBacklogBytes.value)) } catch { /* ignore */ }
}

/* Window docking was removed — windows are pure floating (desktop) / full-screen
 * (phone). The old dock store (docks/dockOrder/dockWindow/undockWindow/layout)
 * lived here; the bottom app Dock (Dock.vue + lib/apps.ts) replaces it. */

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

  /* Platform dock apps. Settings (the gear) hosts the whole settings tree as a
   * window now; CLI and System Log are the terminal windows. Placement orders
   * them in the dock: Settings pinned first, CLI/Log next. */
  registerApp({ id: 'settings', label: 'Settings', icon: 'gear', open: showSettings,
                placement: 1, isOpen: () => settingsVisible.value })
  registerApp({ id: 'cli', label: 'CLI', icon: 'cli', open: showCli,
                placement: 2, isOpen: () => cliVisible.value })
  registerApp({ id: 'log', label: 'System Log', icon: 'log', open: showLog,
                placement: 3, isOpen: () => logVisible.value })

  /* #if 0 — Backlog Size / Edit / Developer Options removed from the menu.
   * The backing code (presets, editor, DeveloperPanel) is kept but no longer
   * registered. Re-enable by changing the guard back to a registration. */
  if (false) {
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
  /* #endif */
}
