import { useMenuStore } from '../stores/menu'
import SystemPanel from '../panels/SystemPanel.vue'
import AboutPanel from '../panels/AboutPanel.vue'

export function registerSystem() {
  const menu = useMenuStore()
  /* System is a submenu so other straddles can register children under it
   * (see [[ota]] → 'system.update'). The menu store merges submenu
   * children by id (stores/menu.ts mergeItems). */
  menu.register('settings', 'Settings', [
    { id: 'system', label: 'System', type: 'submenu',
      children: [
        { id: 'system.general', label: 'General', type: 'panel',
          component: SystemPanel },
      ],
    },
  ])
  /* About panel registered under a hidden 'app' group so the MenuBar's
   * top-level app dropdown can openPanel('about') without a separate
   * top-level Settings entry. */
  menu.register('app', 'App', [
    { id: 'about', label: 'About', type: 'panel', component: AboutPanel },
  ], { hidden: () => true })
}
