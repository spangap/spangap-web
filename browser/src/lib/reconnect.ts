/**
 * Shared reconnect timing for all WebSocket/streaming connections.
 *
 * Strategy (from first disconnect):
 *   - First 2 attempts: 100ms  (near-instant recovery)
 *   - Up to 1 minute:   1s
 *   - Up to 1 hour:     5s
 *   - After 1 hour:     30s
 *
 * reset() returns to fast retries (call on successful connect or user action).
 */

export class ReconnectTimer {
  private timer: ReturnType<typeof setTimeout> | null = null
  private attempt = 0
  private startMs = 0

  /** Schedule a reconnect. Calls `fn` after the appropriate delay. */
  schedule(fn: () => void) {
    this.clear()
    if (this.attempt === 0) this.startMs = Date.now()
    this.attempt++
    const delay = this.delay()
    this.timer = setTimeout(() => {
      this.timer = null
      fn()
    }, delay)
  }

  /** Cancel any pending reconnect. */
  clear() {
    if (this.timer) { clearTimeout(this.timer); this.timer = null }
  }

  /** Reset to fast retries (call on successful connect or user interaction). */
  reset() {
    this.clear()
    this.attempt = 0
    this.startMs = 0
  }

  private delay(): number {
    if (this.attempt <= 2) return 100
    const elapsed = Date.now() - this.startMs
    if (elapsed < 60_000) return 1000
    if (elapsed < 3600_000) return 5000
    return 30_000
  }
}
