import { reactive } from 'vue'

export interface EditorEntry {
  id: string
  path: string
  title: string
  visible: boolean
}

let counter = 0
export const editors = reactive<EditorEntry[]>([])

export function openEditor(path: string, title: string) {
  counter++
  editors.push({ id: `editor-${counter}`, path, title, visible: true })
}

export function isPathOpen(path: string): boolean {
  return editors.some(e => e.path === path)
}

export function closeEditor(id: string) {
  const idx = editors.findIndex(e => e.id === id)
  if (idx < 0) return
  editors.splice(idx, 1)
  /* Each editor window gets a unique id, so its persisted geometry is
   * orphaned once the window closes. Drop it so localStorage doesn't grow. */
  try { localStorage.removeItem(`seccam.win.${id}`) } catch { /* */ }
}
