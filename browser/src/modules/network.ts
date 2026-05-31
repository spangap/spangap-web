import { useMenuStore } from '../stores/menu'
import NetworkPanel from '../panels/NetworkPanel.vue'

/**
 * Network → WiFi only. Panels for UPnP / WireGuard / DuckDNS / ACME live
 * in their own straddles ([[upnp]] / [[wg]] / [[duckdns]] / [[acme]]) and
 * register themselves under the same `network` submenu — the menu store
 * merges submenu children by id (see stores/menu.ts mergeItems).
 */
export function registerNetwork() {
  useMenuStore().register('settings', 'Settings', [
    {
      id: 'network', label: 'Network', type: 'submenu',
      children: [
        { id: 'network.wifi', label: 'WiFi', type: 'panel',
          component: NetworkPanel },
      ],
    },
  ])
}
