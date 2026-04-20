/**
 * Base URL for WebSocket connections to the camera (always wss — device serves TLS).
 * Dev server: ?host=<ip-or-hostname>&port=<443|…> (defaults to 443).
 * Served from device: same-origin host/port from window.location.
 */
export function deviceWssBase(): string {
  if (typeof window === 'undefined') return 'wss://seccam.local'
  const loc = window.location
  const params = new URLSearchParams(loc.search)
  const host = params.get('host') || loc.hostname
  const portStr = params.get('port') || loc.port || '443'
  const port = parseInt(portStr, 10) || 443
  if (port === 443) return `wss://${host}`
  return `wss://${host}:${port}`
}
