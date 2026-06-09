import { useMenuStore } from '../stores/menu'
import NetworkPanel from '../panels/NetworkPanel.vue'

/**
 * Network → WiFi only. Panels for UPnP / WireGuard / DuckDNS / ACME live
 * in their own straddles ([[upnp]] / [[wg]] / [[duckdns]] / [[acme]]) and
 * register themselves under the same `settings/network` submenu — the menu
 * store merges containers by path.
 */
export function registerNetwork() {
  useMenuStore().register('settings/network/wifi', 'WiFi', { type: 'panel', component: NetworkPanel })
}
