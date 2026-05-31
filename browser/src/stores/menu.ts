import { defineStore } from 'pinia'
import { ref, computed, reactive, type Component } from 'vue'

export interface MenuItem {
  id: string
  label: string
  type: 'panel' | 'toggle' | 'submenu' | 'action'
  order?: number
  component?: Component        // for type === 'panel'
  key?: string                 // for type === 'toggle' (device store dotpath)
  children?: MenuItem[]        // for type === 'submenu'
  action?: () => void          // for type === 'action'
  dynamicLabel?: () => string  // overrides `label` at render time when present
  checked?: () => boolean      // shows a leading checkmark when truthy
  disabled?: () => boolean     // greys the item out when truthy
}

export interface MenuGroup {
  id: string
  label: string
  order?: number
  items: MenuItem[]
  activeLabel?: string      // label shown in menu bar when this panel is active
  onClose?: () => void      // called when active panel's menu button is clicked (to dismiss)
  hidden?: () => boolean    // hides the entire group when truthy
}

export const useMenuStore = defineStore('menu', () => {
  const menus = reactive(new Map<string, MenuGroup>())
  const activePanel = ref<string | null>(null)

  /** Merge `incoming` into `target`, recursing into submenu children when ids match. */
  function mergeItems(target: MenuItem[], incoming: MenuItem[]) {
    for (const item of incoming) {
      const existing = target.find(t => t.id === item.id)
      if (existing && existing.type === 'submenu' && item.type === 'submenu') {
        existing.children ??= []
        mergeItems(existing.children, item.children ?? [])
        existing.children.sort((a, b) => (a.order ?? Infinity) - (b.order ?? Infinity) || a.label.localeCompare(b.label))
      } else {
        target.push(item)
      }
    }
    target.sort((a, b) => (a.order ?? Infinity) - (b.order ?? Infinity) || a.label.localeCompare(b.label))
  }

  function register(menuId: string, label: string, items: MenuItem[], options?: { activeLabel?: string, onClose?: () => void, hidden?: () => boolean }) {
    const existing = menus.get(menuId)
    if (existing) {
      mergeItems(existing.items, items)
      if (options) Object.assign(existing, options)
    } else {
      const fresh: MenuItem[] = []
      mergeItems(fresh, items)
      menus.set(menuId, { id: menuId, label, items: fresh, ...options })
    }
  }

  const sortedMenus = computed(() =>
    [...menus.values()].sort((a, b) => (a.order ?? Infinity) - (b.order ?? Infinity) || a.label.localeCompare(b.label)),
  )

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

  /** Find the component for the active panel by searching all menus (including nested submenu children). */
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

  return { menus, activePanel, sortedMenus, register, openPanel, togglePanel, closePanel, activePanelComponent, activePanelLabel }
})
