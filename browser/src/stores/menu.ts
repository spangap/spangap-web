import { defineStore } from 'pinia'
import { ref, computed, reactive, type Component } from 'vue'

/**
 * Path-based menu registry (mirrors the LCD's lcdRegisterSettings).
 *
 * Callers register one leaf at a slash-path; the store auto-creates the
 * intermediate group/submenu containers (title-cased from their id) and folds
 * concurrent registrations together by path. e.g.
 *
 *   register('settings/system/general', 'General', { type: 'panel', component })
 *   register('settings/network/wifi',   'WiFi',    { type: 'panel', component })
 *
 * both land under one "Settings" menu, "System"/"Network" submenus merged by id.
 * First segment = top-level menu-bar group, last = the leaf, middle = submenus.
 * Minimum path is `group/leaf` (>= 2 segments).
 *
 * Placement (opts.placement, default 0) orders siblings — menus among menus,
 * items among items:
 *   > 0  left/top block, ascending      (1 is furthest left/top)
 *   = 0  middle block, alphabetic        (the unstated default)
 *   < 0  right/bottom block, ascending  (-1 is furthest right/bottom)
 * Ties within a block sort alphabetically by label. Containers default to 0;
 * override a container's label/placement with setMenu().
 */

export type MenuLeaf =
  | { type: 'panel';  component: Component }  // settings pane shown in the panel area
  | { type: 'toggle'; key: string }           // device-store dotpath, rendered as a switch
  | { type: 'action'; action: () => void }    // fire-and-forget menu action

export interface RegisterOpts {
  placement?: number           // sibling ordering preference (see comparator); default 0
  dynamicLabel?: () => string  // overrides `label` at render time when present
  checked?: () => boolean      // shows a leading checkmark when truthy
  disabled?: () => boolean     // greys the item out when truthy
  hidden?: boolean | (() => boolean)  // openable by id but not rendered (e.g. the About pane)
}

/** A node in the menu tree — a leaf when `type !== 'submenu'`, else a container. */
export interface MenuItem {
  id: string                   // full slash-path (stable id for openPanel)
  label: string
  placement: number
  type: 'panel' | 'toggle' | 'submenu' | 'action'
  component?: Component        // type === 'panel'
  key?: string                 // type === 'toggle'
  action?: () => void          // type === 'action'
  children?: MenuItem[]        // type === 'submenu'
  dynamicLabel?: () => string
  checked?: () => boolean
  disabled?: () => boolean
  hidden?: boolean | (() => boolean)
}

/** A top-level menu-bar group (the first path segment). */
export interface MenuGroup {
  id: string
  label: string
  placement: number
  items: MenuItem[]
  hidden?: boolean | (() => boolean)  // hide the whole group (e.g. when no SD card)
}

/** Bucket placements: positive first, unstated/0 middle, negative last. */
function placeRank(p: number): number {
  return p > 0 ? 0 : p < 0 ? 2 : 1
}

/** Sibling order: by bucket, then ascending value within bucket, then by label. */
function byPlacement(a: { placement: number; label: string }, b: { placement: number; label: string }): number {
  const ra = placeRank(a.placement), rb = placeRank(b.placement)
  if (ra !== rb) return ra - rb
  if (a.placement !== b.placement) return a.placement - b.placement
  return a.label.localeCompare(b.label)
}

function titleCase(s: string): string {
  return s ? s[0].toUpperCase() + s.slice(1) : s
}

function splitPath(path: string): string[] {
  return path.split('/').filter(Boolean)
}

export const useMenuStore = defineStore('menu', () => {
  const menus = reactive(new Map<string, MenuGroup>())
  const activePanel = ref<string | null>(null)

  function ensureGroup(id: string): MenuGroup {
    let group = menus.get(id)
    if (!group) {
      group = { id, label: titleCase(id), placement: 0, items: [] }
      menus.set(id, group)
    }
    return group
  }

  /** Find or create the submenu container at `prefix` among `siblings`. */
  function ensureSubmenu(siblings: MenuItem[], prefix: string, seg: string): MenuItem {
    let node = siblings.find(n => n.id === prefix)
    if (!node) {
      node = { id: prefix, label: titleCase(seg), placement: 0, type: 'submenu', children: [] }
      siblings.push(node)
    }
    node.children ??= []
    return node
  }

  /** Re-sort every level of a group's tree by placement. */
  function sortTree(group: MenuGroup) {
    const rec = (items: MenuItem[]) => {
      items.sort(byPlacement)
      for (const it of items) if (it.children) rec(it.children)
    }
    rec(group.items)
  }

  function register(path: string, label: string, leaf: MenuLeaf, opts: RegisterOpts = {}) {
    const segs = splitPath(path)
    if (segs.length < 2) throw new Error(`menu path needs at least group/leaf: "${path}"`)

    const group = ensureGroup(segs[0])
    let siblings = group.items
    let prefix = segs[0]
    for (let i = 1; i < segs.length - 1; i++) {
      prefix += '/' + segs[i]
      siblings = ensureSubmenu(siblings, prefix, segs[i]).children!
    }

    const leafPath = segs.join('/')
    const existing = siblings.find(n => n.id === leafPath)
    const node: MenuItem = existing ?? { id: leafPath, label, placement: 0, type: leaf.type }
    node.label = label
    node.type = leaf.type
    node.placement = opts.placement ?? 0
    node.dynamicLabel = opts.dynamicLabel
    node.checked = opts.checked
    node.disabled = opts.disabled
    node.hidden = opts.hidden
    node.component = leaf.type === 'panel' ? leaf.component : undefined
    node.key = leaf.type === 'toggle' ? leaf.key : undefined
    node.action = leaf.type === 'action' ? leaf.action : undefined
    if (!existing) siblings.push(node)

    sortTree(group)
  }

  /**
   * Remove the leaf or submenu (with its subtree) at `path`; prunes any
   * containers left empty, including the group. A single-segment path removes a
   * whole group. Inert when the path isn't registered.
   */
  function unregister(path: string) {
    const segs = splitPath(path)
    if (!segs.length) return
    const group = menus.get(segs[0])
    if (!group) return
    if (segs.length === 1) { menus.delete(segs[0]); return }

    // Walk to the target's parent, recording the container chain for pruning.
    const chain: MenuItem[] = []
    let siblings = group.items
    let prefix = segs[0]
    for (let i = 1; i < segs.length - 1; i++) {
      prefix += '/' + segs[i]
      const node = siblings.find(n => n.id === prefix && n.children)
      if (!node) return
      chain.push(node)
      siblings = node.children!
    }

    const targetPath = segs.join('/')
    const idx = siblings.findIndex(n => n.id === targetPath)
    if (idx < 0) return
    siblings.splice(idx, 1)

    // Prune ancestor submenus that just became empty, deepest first.
    for (let i = chain.length - 1; i >= 0; i--) {
      const node = chain[i]
      if (node.children && node.children.length === 0) {
        const parent = i === 0 ? group.items : chain[i - 1].children!
        const pIdx = parent.findIndex(n => n.id === node.id)
        if (pIdx >= 0) parent.splice(pIdx, 1)
      } else break
    }
    if (group.items.length === 0) menus.delete(group.id)
  }

  /** Override a container's (group or submenu) label, placement, and/or hidden
   *  state, creating it if absent. The path's intermediate segments are
   *  auto-created. */
  function setMenu(path: string, opts: { label?: string; placement?: number; hidden?: boolean | (() => boolean) }) {
    const segs = splitPath(path)
    if (!segs.length) return
    const group = ensureGroup(segs[0])
    if (segs.length === 1) {
      if (opts.label !== undefined) group.label = opts.label
      if (opts.placement !== undefined) group.placement = opts.placement
      if (opts.hidden !== undefined) group.hidden = opts.hidden
      return
    }
    let siblings = group.items
    let prefix = segs[0]
    for (let i = 1; i < segs.length; i++) {
      prefix += '/' + segs[i]
      const node = ensureSubmenu(siblings, prefix, segs[i])
      if (i === segs.length - 1) {
        if (opts.label !== undefined) node.label = opts.label
        if (opts.placement !== undefined) node.placement = opts.placement
        if (opts.hidden !== undefined) node.hidden = opts.hidden
      }
      siblings = node.children!
    }
    sortTree(group)
  }

  const sortedMenus = computed(() => [...menus.values()].sort(byPlacement))

  function openPanel(id: string) {
    activePanel.value = id
  }

  function togglePanel(id: string) {
    activePanel.value = activePanel.value === id ? null : id
  }

  function closePanel() {
    activePanel.value = null
  }

  function findItem(items: MenuItem[], id: string): MenuItem | null {
    for (const item of items) {
      if (item.id === id) return item
      if (item.children) {
        const found = findItem(item.children, id)
        if (found) return found
      }
    }
    return null
  }

  /** Component for the active panel, searched across all menus (incl. hidden). */
  const activePanelComponent = computed<Component | null>(() => {
    if (!activePanel.value) return null
    for (const menu of menus.values()) {
      const item = findItem(menu.items, activePanel.value)
      if (item?.component) return item.component
    }
    return null
  })

  const activePanelLabel = computed<string>(() => {
    if (!activePanel.value) return ''
    for (const menu of menus.values()) {
      const item = findItem(menu.items, activePanel.value)
      if (item) return item.label
    }
    return ''
  })

  return { menus, activePanel, sortedMenus, register, unregister, setMenu, openPanel, togglePanel, closePanel, activePanelComponent, activePanelLabel }
})
