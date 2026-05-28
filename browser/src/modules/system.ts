import { useMenuStore } from '../stores/menu'
import SystemPanel from '../panels/SystemPanel.vue'
import AboutPanel from '../panels/AboutPanel.vue'

export function registerSystem() {
  const menu = useMenuStore()
  /* System is a submenu so other straddles can register children under it
   * (see [[ota]] → 'system.update'). The menu store merges submenu
   * children by id (stores/menu.ts mergeItems). */
  menu.register('settings', 'Settings', 10, [
    { id: 'system', label: 'System', type: 'submenu', order: 10,
      children: [
        { id: 'system.general', label: 'General', type: 'panel', order: 10,
          component: SystemPanel },
      ],
    },
  ])
  /* About panel registered under a hidden 'app' group so the MenuBar's
   * top-level app dropdown can openPanel('about') without a separate
   * top-level Settings entry. */
  menu.register('app', 'App', -100, [
    { id: 'about', label: 'About', type: 'panel', order: 0, component: AboutPanel },
  ], { hidden: () => true })
}
