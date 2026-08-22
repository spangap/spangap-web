/**
 * title — the tab title as device state.
 *
 * A device window is "<host> - Web UI", led by ❌ while the link is down,
 * so the tab strip says at a glance which windows have lost their device —
 * the glyph survives a tab squeezed too narrow for any text. No project
 * name: the tab strip is for telling windows apart.
 *
 * Until a hostname is known the served title is left alone. It is the
 * per-project brand from the consumer's index.html, which the pre-auth pages
 * (Login/Setup) use for their heading — captured here at first import,
 * before the device title takes over.
 */
export const servedTitle = document.title || 'Device'

export function setDeviceTitle(host: string | undefined, down: boolean) {
  if (!host) return // pre-sync: the served title stands
  const t = `${host} - Web UI`
  document.title = down ? `❌ ${t}` : t
}
