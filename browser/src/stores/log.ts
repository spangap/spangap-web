/**
 * Log store — pre-opens the `log:1` DataChannel on the shared session and
 * keeps a bounded text buffer of received bytes. The log window mounts the
 * buffer into xterm and subscribes for new lines.
 *
 * Bidirectional: console.log/info/warn/error/debug are also forwarded to the
 * device's log task so cron/file/serial fan-out captures browser output.
 *
 * WHAT THE BROWSER MAY COST THE DEVICE. Everything forwarded is bounded, in
 * three places and for three different reasons: each line is clamped to one
 * line and a few trace frames (a Vue component trace is kilobytes of nothing a
 * device log needs), repeats are counted rather than sent (a broken render
 * emits on every update), and sendLogLine caps the wire form in BYTES as a
 * backstop no caller can bypass. The device's log task receives a line on a
 * stack; an unbounded browser is how that stack overflows. None of it loses
 * anything: the console hook calls the real console first, so devtools always
 * holds the untruncated original.
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

/** Monotonic counter, bumped when the DEVICE ends the log stream while the
 *  session is otherwise healthy — i.e. the log source itself went away, not
 *  the transport under it. The log window closes on this and only this: a
 *  stream that drops with its session comes back on the next peer connection,
 *  and the window has to still be there to receive it. */
export const logStreamEnded = ref(0)

/** Session generation this DC opened in — see the close handler. */
let dcEpoch = -1

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
    dcEpoch = getSession().epoch
    /* Flush any lines queued while the channel was opening. */
    while (pendingLines.length > 0) {
      const line = pendingLines.shift()!
      try { dc!.send(line) } catch { /* drop */ }
    }
  }
  dc.onmessage = (ev) => {
    getSession().noteDcActivity()
    emitLocal(decode(ev.data))
  }
  dc.onclose = () => {
    dc = null
    logConnected.value = false
    /* Distinguish the device ending the stream from the channel going down
     * with its session (reconnect, link-down refresh, transport failure).
     * Only the former is a real end-of-stream; the latter reattaches on the
     * next peer connection. */
    if (getSession().sessionHealthy(dcEpoch)) logStreamEnded.value++
    dcEpoch = -1
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
  dcEpoch = -1
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
/* The hard bound on what any browser line may cost the device, in BYTES on the
 * wire — the unit the receiving log task actually spends, and not the same as
 * characters: one `⏎` is three bytes, so a length check in characters can be
 * out by 4×. Enforced HERE rather than only at the formatters because this is
 * the single door to the device, and it is exported — a caller added later
 * inherits the bound instead of having to remember it. */
const WIRE_MAX = 512

const utf8 = new TextEncoder()

/** Truncate to `max` UTF-8 bytes without splitting a character, and without
 *  leaving the terminal wearing a colour: an ANSI line is re-terminated. */
function clampWire(line: string, max: number): string {
  const bytes = utf8.encode(line)
  if (bytes.length <= max) return line
  const hadAnsi = line.includes('\x1b[')
  const tail = hadAnsi ? '…\x1b[0m' : '…'
  const room = max - utf8.encode(tail).length
  /* Decoding a slice that ends mid-sequence yields U+FFFD; drop it rather than
   * ship a broken character. */
  const cut = new TextDecoder('utf-8').decode(bytes.subarray(0, room)).replace(/�+$/, '')
  return cut + tail
}

export function sendLogLine(line: string) {
  line = clampWire(line, WIRE_MAX - 1)      // -1 for the newline below
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

/* ── what a browser line may cost the device ──
 *
 * A forwarded line arrives at the device's log task as ONE line, on a task with
 * a stack rather than a heap to receive it on. Browser diagnostics do not
 * respect that at all: a Vue component trace or a JS stack is kilobytes across
 * dozens of lines, and a broken render emits it again on every update — which
 * is how one bad panel becomes a stack overflow in the device's log task.
 *
 * So the forwarder states what a line may cost. The FULL text is never lost:
 * the console hook calls the real console first, so devtools always has the
 * untruncated original. What is bounded is only what travels. */
const LINE_MAX = 400      // bytes of body; a device log line, not an essay
const TRACE_LINES = 3     // stack/component-trace lines kept — enough to place a fault
const DEDUPE_MS = 3000    // a repeat inside this window is counted, not sent

/** One line, bounded, with the tail of any trace summarised rather than sent.
 *  The first few frames are kept because they are what places a fault; the
 *  remaining forty are what floods the wire. */
function clampBody(s: string): string {
  const lines = s.split('\n')
  const keep = lines.slice(0, 1 + TRACE_LINES).map(l => l.trim()).filter(Boolean)
  let out = keep.join(' ⏎ ')
  const dropped = lines.length - (1 + TRACE_LINES)
  if (dropped > 0) out += ` ⏎ …+${dropped} more`
  if (out.length > LINE_MAX) out = out.slice(0, LINE_MAX - 1) + '…'
  return out
}

/* Repeat suppression. A render that throws does it once per update, so the
 * interesting thing is that it happened and how often — not forty identical
 * copies of it. The count is flushed by the next line that differs, which is
 * also the line that gives it context. */
let lastBody = ''
let lastAt = 0
let repeats = 0

/** Returns the body to send, or null to drop it as a repeat. */
function dedupe(body: string): string | null {
  const now = Date.now()
  if (body === lastBody && now - lastAt < DEDUPE_MS) {
    repeats++
    lastAt = now
    return null
  }
  const held = repeats
  lastBody = body
  lastAt = now
  repeats = 0
  return held > 0 ? `(last line repeated ${held}×) ${body}` : body
}

/* Pre-colored line: grey timestamp + level-colored body. Device's
 * containsAnsi() check sees the escapes and passes through to ANSI consumers
 * unchanged; plain consumers + log file get a stripped version. */
function composeLine(level: string, body: string): string {
  const ts = fmtTs()
  const c = LEVEL_COLOR[level] ?? LEVEL_COLOR.I
  return `\x1b[${TS_COLOR}m${ts}\x1b[0m \x1b[${c}m${level} Browser: ${body}\x1b[0m`
}

function formatLine(level: string, args: any[]): string {
  return composeLine(level, clampBody(args.map(stringifyArg).join(' ')))
}

/** Emit a one-off system notice (e.g. link down/up) through the same path as
 *  the console hooks: it shows immediately in the local LogWindow (emitLocal)
 *  AND is forwarded to the device log task (sendLogLine), which fans it out to
 *  serial / the CLI console / the log file. Queued if the DC is down, so the
 *  "Disconnected" line still reaches the device once the link is back. */
export function logSystemNotice(text: string, level: 'I' | 'W' | 'E' = 'W') {
  const line = formatLine(level, [text])
  emitLocal(line + '\n')
  try { sendLogLine(line) } catch { /* drop */ }
}

/** Wrap window.console.{log,info,warn,error,debug} so they also stream to
 *  device. The original console method runs first (so devtools still works),
 *  then the line is forwarded. Idempotent — guard lives on globalThis so
 *  duplicate module instances (e.g. preserveSymlinks bundling the package
 *  twice) still hook console exactly once. */
const CONSOLE_HOOKED_KEY = Symbol.for('spangap.consoleHooked')
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
    /* Deduplicated on the BODY, so a repeat is recognised whatever second it
     * happened in. Dropped here rather than in formatLine: a system notice is
     * rare and deliberate, and should never be swallowed as a repeat. */
    const body = dedupe(clampBody(args.map(stringifyArg).join(' ')))
    if (body === null) return
    const line = composeLine(level, body)
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
