import { useMenuStore } from '../stores/menu'
import SystemPanel from '../panels/SystemPanel.vue'
import AboutPanel from '../panels/AboutPanel.vue'

export function registerSystem() {
  const menu = useMenuStore()
  /* Settings is the left-most menu group. */
  menu.setMenu('settings', { placement: 1 })
  /* System is a single panel registered straight at the submenu path, so the
   * Settings dropdown shows "System" as a one-click item (no nested General).
   * OTA is omitted from this build; were a straddle to add a second System
   * child it would need to move back to a `settings/system/<leaf>` submenu. */
  menu.register('settings/system', 'System', { type: 'panel', component: SystemPanel }, { placement: 1 })
  /* About lives in a hidden 'app' group: the MenuBar's app dropdown opens it
   * by id (openPanel('app/about')), but the group never shows a menu-bar
   * button because its only leaf is hidden. */
  menu.register('app/about', 'About', { type: 'panel', component: AboutPanel }, { hidden: true })
}
