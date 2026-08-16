import { useMenuStore } from '../stores/menu'
import AboutPanel from '../panels/AboutPanel.vue'

export function registerSystem() {
  /* About lives in a hidden 'app' group: the MenuBar's app dropdown opens it
   * by id (openPanel('app/about')), but the group never shows a menu-bar
   * button because its only leaf is hidden.
   *
   * The System settings pane is not here any more — spangap-net names that node
   * and this straddle contributes its Backup & Recovery rows to it from its own
   * `settings:` block, so neither has a component to register. */
  useMenuStore().register('app/about', 'About', { type: 'panel', component: AboutPanel }, { hidden: true })
}
