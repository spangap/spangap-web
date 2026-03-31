import { useMenuStore } from '../stores/menu'
import SystemPanel from './panels/SystemPanel.vue'

export function registerSystem() {
  useMenuStore().register('settings', 'Settings', 10, [
    { id: 'system', label: 'System', type: 'panel', order: 10, component: SystemPanel },
  ])
}
