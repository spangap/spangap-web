/**
 * Base URL for WebSocket connections to the device — always same-origin.
 *
 * Production: the device serves the SPA over TLS, so this is `wss://<device>`.
 * `spangap dev`: the SPA is served by the Vite dev server, which reverse-proxies
 * `/webrtc` (and `/auth`, `/state`) to the device (see web-interface's
 * quasar.config.ts). So we talk plain `ws` to the dev origin and the proxy
 * upgrades to the device's `wss` — which keeps the browser same-origin, so the
 * session cookie and auth Just Work instead of being stranded cross-origin.
 */
export function deviceWssBase(): string {
  if (typeof window === 'undefined') return 'wss://device.local'
  const loc = window.location
  const proto = loc.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${loc.host}` // loc.host carries the port when present
}
