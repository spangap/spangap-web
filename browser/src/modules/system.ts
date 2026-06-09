import { useMenuStore } from '../stores/menu'
import SystemPanel from '../panels/SystemPanel.vue'
import AboutPanel from '../panels/AboutPanel.vue'

export function registerSystem() {
  const menu = useMenuStore()
  /* System is a submenu so other straddles can register children under it —
   * e.g. [[ota]] adds 'settings/system/update'; the store merges containers
   * by path. */
  menu.register('settings/system/general', 'General', { type: 'panel', component: SystemPanel })
  /* About lives in a hidden 'app' group: the MenuBar's app dropdown opens it
   * by id (openPanel('app/about')), but the group never shows a menu-bar
   * button because its only leaf is hidden. */
  menu.register('app/about', 'About', { type: 'panel', component: AboutPanel }, { hidden: true })
}
