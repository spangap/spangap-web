import { useMenuStore } from '../stores/menu'
import SystemPanel from '../panels/SystemPanel.vue'
import AboutPanel from '../panels/AboutPanel.vue'

export function registerSystem() {
  const menu = useMenuStore()
  menu.register('settings', 'Settings', 10, [
    { id: 'system', label: 'System', type: 'panel', order: 10, component: SystemPanel },
  ])
  /* About panel registered under a hidden 'app' group so the MenuBar's
   * top-level "Seccam" dropdown can openPanel('about') without a separate
   * top-level Settings entry. */
  menu.register('app', 'App', -100, [
    { id: 'about', label: 'About', type: 'panel', order: 0, component: AboutPanel },
  ], { hidden: () => true })
}
