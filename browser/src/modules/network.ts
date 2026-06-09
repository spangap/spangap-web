import { useMenuStore } from '../stores/menu'
import NetworkPanel from '../panels/NetworkPanel.vue'

/**
 * Settings → Internet → WiFi. Panels for UPnP / WireGuard / DuckDNS / ACME
 * live in their own straddles ([[upnp]] / [[wg]] / [[duckdns]] / [[acme]])
 * and register themselves under the same `settings/network` submenu — the
 * menu store merges containers by path. The submenu keeps the `network` id
 * (so those straddles need no change) but is labelled "Internet"; WiFi is
 * lifted to the front of the dropdown.
 */
export function registerNetwork() {
  const menu = useMenuStore()
  menu.setMenu('settings/network', { label: 'Internet', placement: 2 })
  menu.register('settings/network/wifi', 'WiFi', { type: 'panel', component: NetworkPanel }, { placement: 1 })
}
