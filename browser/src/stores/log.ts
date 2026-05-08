/**
 * Log store — pre-opens the `log:1` DataChannel on the shared session and
 * keeps a bounded text buffer of received bytes. The log window mounts the
 * buffer into xterm and subscribes for new lines.
 *
 * Bidirectional: console.log/info/warn/error/debug are also forwarded to the
 * device's log task so cron/file/serial fan-out captures browser output.
 */
import { ref } from 'vue'
import { getSession } from '../lib/webrtc-session'
import { logBacklogBytes } from '../modules/advanced'

const BUFFER_BYTES_CAP = 256 * 1024

let dc: RTCDataChannel | null = null
let unregisterBuilder: (() => void) | null = null
let started = false

const subscribers = new Set<(text: string) => void>()
let buffer = ''
let bufferBytes = 0

/* Queue lines that were generated before the DC finished opening.
 * Bounded to avoid unbounded memory if the DC never opens. */
const PENDING_MAX = 128
const pendingLines: string[] = []

/** Reactive flag — true when DC is connected. */
export const logConnected = ref(false)

function appendBuffer(text: string) {
  buffer += text
  bufferBytes += text.length
  if (bufferBytes > BUFFER_BYTES_CAP) {
    /* Drop oldest until back under cap, snapping to next line boundary. */
    const drop = bufferBytes - BUFFER_BYTES_CAP
    const cut = buffer.indexOf('\n', drop)
    const k = cut >= 0 ? cut + 1 : drop
    buffer = buffer.slice(k)
    bufferBytes -= k
  }
}

function emitLocal(text: string) {
  appendBuffer(text)
  for (const cb of subscribers) cb(text)
}

function decode(data: any): string {
  if (typeof data === 'string') return data
  if (data instanceof ArrayBuffer) return new TextDecoder().decode(data)
  return new TextDecoder().decode((data as Uint8Array).buffer)
}

function buildChannel(pc: RTCPeerConnection) {
  if (dc) { try { dc.onclose = null; dc.close() } catch { /* */ } }
  try {
    /* Backlog size driven by Advanced → Backlog Size submenu (persisted in
     * localStorage). New value takes effect on next session reconnect. */
    const b = logBacklogBytes.value
    dc = pc.createDataChannel('log:1', {
      ordered: true,
      protocol: b > 0 ? JSON.stringify({ backlog: b }) : '',
    })
  } catch (e) {
    console.error('[log] createDataChannel failed:', e)
    dc = null
    return
  }
  dc.binaryType = 'arraybuffer'
  dc.onopen = () => {
    logConnected.value = true
    /* Flush any lines queued while the channel was opening. */
    while (pendingLines.length > 0) {
      const line = pendingLines.shift()!
      try { dc!.send(line) } catch { /* drop */ }
    }
  }
  dc.onmessage = (ev) => {
    emitLocal(decode(ev.data))
  }
  dc.onclose = () => {
    dc = null
    logConnected.value = false
  }
  dc.onerror = () => { /* onclose follows */ }
}

/** Begin pre-connecting the log DC. Call once on app boot, after auth check. */
export function startLogStream() {
  if (started) return
  started = true
  unregisterBuilder = getSession().registerChannel(buildChannel)
  getSession().connect()
}

/** Stop preconnecting (useful for hot-reload tests; rarely needed). */
export function stopLogStream() {
  if (unregisterBuilder) { unregisterBuilder(); unregisterBuilder = null }
  if (dc) { try { dc.close() } catch { /* */ } }
  dc = null
  started = false
  logConnected.value = false
}

/** Return the buffered text accumulated so far. */
export function getLogBuffer(): string { return buffer }

/** Subscribe to incoming chunks. Returns an unsubscribe function. */
export function subscribeLog(cb: (text: string) => void): () => void {
  subscribers.add(cb)
  return () => { subscribers.delete(cb) }
}

/** Send a preformatted line to the device log. Adds a trailing newline if
 *  missing. If the DC isn't open yet (early boot, reconnect), the line is
 *  queued and flushed when the channel opens. Bounded queue. */
export function sendLogLine(line: string) {
  if (!line.endsWith('\n')) line += '\n'
  if (dc && dc.readyState === 'open') {
    try { dc.send(line) } catch { /* drop */ }
    return
  }
  if (pendingLines.length >= PENDING_MAX) pendingLines.shift()
  pendingLines.push(line)
}

/* ── Browser console hooks ────────────────────────────────────────────── */

const MONTHS = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec']

/* Match device-side colors (s.log.colors.* defaults). */
const TS_COLOR = '0;90'
const LEVEL_COLOR: Record<string, string> = {
  E: '0;31', W: '0;33', I: '0;32', D: '0;37', V: '0;90',
}

function fmtTs(d = new Date()): string {
  const pad = (n: number, w = 2) => String(n).padStart(w, '0')
  return `${MONTHS[d.getMonth()]} ${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}.${pad(d.getMilliseconds(), 3)}`
}

function stringifyArg(a: any): string {
  if (typeof a === 'string') return a
  if (a instanceof Error) return a.stack || `${a.name}: ${a.message}`
  try { return JSON.stringify(a) } catch { return String(a) }
}

/* Pre-colored line: grey timestamp + level-colored body. Device's
 * containsAnsi() check sees the escapes and passes through to ANSI consumers
 * unchanged; plain consumers + log file get a stripped version. */
function formatLine(level: string, args: any[]): string {
  const ts = fmtTs()
  const body = `${level} Browser: ${args.map(stringifyArg).join(' ')}`
  const c = LEVEL_COLOR[level] ?? LEVEL_COLOR.I
  return `\x1b[${TS_COLOR}m${ts}\x1b[0m \x1b[${c}m${body}\x1b[0m`
}

/** Wrap window.console.{log,info,warn,error,debug} so they also stream to
 *  device. The original console method runs first (so devtools still works),
 *  then the line is forwarded. Idempotent — guard lives on globalThis so
 *  duplicate module instances (e.g. preserveSymlinks bundling the package
 *  twice) still hook console exactly once. */
const CONSOLE_HOOKED_KEY = Symbol.for('diptych.consoleHooked')
type HookedHolder = { [CONSOLE_HOOKED_KEY]?: boolean }
const hookedHolder = globalThis as unknown as HookedHolder
export function installConsoleHooks() {
  if (hookedHolder[CONSOLE_HOOKED_KEY]) return
  hookedHolder[CONSOLE_HOOKED_KEY] = true
  const orig = {
    log:   console.log.bind(console),
    info:  console.info.bind(console),
    warn:  console.warn.bind(console),
    error: console.error.bind(console),
    debug: console.debug.bind(console),
  }
  function emit(level: string, args: any[]) {
    const line = formatLine(level, args)
    /* Local echo: appears in the LogWindow without device round-trip
     * (device-side fan-out skips the source slot, so this line wouldn't
     * come back to us anyway). */
    emitLocal(line + '\n')
    /* Send to device so file/serial/other consumers also pick it up. */
    try { sendLogLine(line) } catch { /* drop */ }
  }
  function hook(name: keyof typeof orig, level: string) {
    console[name] = (...args: any[]) => {
      orig[name](...args)
      try { emit(level, args) } catch { /* drop */ }
    }
  }
  hook('log',   'I')
  hook('info',  'I')
  hook('warn',  'W')
  hook('error', 'E')
  hook('debug', 'D')

  /* Surface unhandled rejections + window errors. */
  window.addEventListener('error', (ev) => {
    try { emit('E', [ev.message, ev.filename + ':' + ev.lineno]) } catch { /* */ }
  })
  window.addEventListener('unhandledrejection', (ev) => {
    try { emit('E', ['unhandled rejection:', ev.reason]) } catch { /* */ }
  })
}
