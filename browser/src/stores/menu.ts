import { defineStore } from 'pinia'
import { ref, computed, reactive, type Component } from 'vue'

export interface MenuItem {
  id: string
  label: string
  type: 'panel' | 'toggle' | 'submenu' | 'action'
  order: number
  component?: Component        // for type === 'panel'
  key?: string                 // for type === 'toggle' (device store dotpath)
  children?: MenuItem[]        // for type === 'submenu'
  action?: () => void          // for type === 'action'
  dynamicLabel?: () => string  // overrides `label` at render time when present
  checked?: () => boolean      // shows a leading checkmark when truthy
}

export interface MenuGroup {
  id: string
  label: string
  order: number
  items: MenuItem[]
  activeLabel?: string      // label shown in menu bar when this panel is active
  onClose?: () => void      // called when active panel's menu button is clicked (to dismiss)
  hidden?: () => boolean    // hides the entire group when truthy
}

export const useMenuStore = defineStore('menu', () => {
  const menus = reactive(new Map<string, MenuGroup>())
  const activePanel = ref<string | null>(null)

  function register(menuId: string, label: string, order: number, items: MenuItem[], options?: { activeLabel?: string, onClose?: () => void, hidden?: () => boolean }) {
    const existing = menus.get(menuId)
    if (existing) {
      existing.items.push(...items)
      existing.items.sort((a, b) => a.order - b.order)
      if (options) Object.assign(existing, options)
    } else {
      menus.set(menuId, { id: menuId, label, order, items: [...items].sort((a, b) => a.order - b.order), ...options })
    }
  }

  const sortedMenus = computed(() =>
    [...menus.values()].sort((a, b) => a.order - b.order),
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

  /** Find the component for the active panel by searching all menus (including submenu children). */
  const activePanelComponent = computed<Component | null>(() => {
    if (!activePanel.value) return null
    for (const menu of menus.values()) {
      for (const item of menu.items) {
        if (item.id === activePanel.value && item.component) return item.component
        if (item.children) {
          const child = item.children.find(c => c.id === activePanel.value)
          if (child?.component) return child.component
        }
      }
    }
    return null
  })

  const activePanelLabel = computed<string>(() => {
    if (!activePanel.value) return ''
    for (const menu of menus.values()) {
      for (const item of menu.items) {
        if (item.id === activePanel.value) return item.label
        if (item.children) {
          const child = item.children.find(c => c.id === activePanel.value)
          if (child) return child.label
        }
      }
    }
    return ''
  })

  return { menus, activePanel, sortedMenus, register, openPanel, togglePanel, closePanel, activePanelComponent, activePanelLabel }
})
