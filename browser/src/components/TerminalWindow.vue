<template>
  <FloatingWindow
    ref="fwRef"
    :id="configPrefix"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :default-dock="defaultDock"
    :min-size="{ w: 10, h: 8 }"
    @update:visible="onVisibleChange"
  >
    <template #titlebar-right>
      <span class="term-zoom-btn" @click="zoomOut">-</span>
      <span class="term-zoom-btn" @click="zoomIn">+</span>
    </template>

    <template #default="{ size }">
      <div ref="termRef" class="term-body" :data-sz="`${size.w}x${size.h}`" />
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, watch, onMounted, onUnmounted, nextTick, computed } from 'vue'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { getSession } from '../lib/webrtc-session'
import FloatingWindow from './FloatingWindow.vue'
import '@xterm/xterm/css/xterm.css'

const props = defineProps<{
  visible: boolean
  title: string
  /** DataChannel label to open on the shared session (e.g. `cli:1`, `log:1`). */
  dcLabel: string
  /** DCEP protocol string — used by `log:1` to request a custom backlog size
   *  like `{"backlog":65536}`. Leave empty for default. */
  dcProtocol?: string
  readonly?: boolean
  configPrefix: string   // 'cli' or 'log' — window id + localStorage namespace
}>()

const emit = defineEmits<{
  'update:visible': [value: boolean]
}>()

function onVisibleChange(v: boolean) { emit('update:visible', v) }

/* ── defaults + zoom persistence ── */
/* Phone-sized initial layout: full-width, half-screen tall, docked top. */
const isPhoneInit = window.matchMedia?.('(max-width: 599px)').matches ?? false
const DEFAULTS: Record<string, { x: number; y: number; w: number; h: number }> = {
  cli: { x: 12.5, y: 77.5, w: 75, h: 20 },
  log: { x: 12.5, y: 2.5, w: 75, h: 70 },
}
const defaultGeom = isPhoneInit
  ? { x: 0, y: 0, w: 100, h: 50 }
  : (DEFAULTS[props.configPrefix] ?? DEFAULTS.cli)
const defaultDock = isPhoneInit
  ? { side: 'top' as const, size: 50 }
  : null

const BASE_FONT = 14
const ZOOM_KEY = `spangap.win.${props.configPrefix}.zoom`
/* 3 stops below default (font ≈ 8px) so the small docked window holds
 * useful CLI / log output on a phone. Stored value wins when present. */
const DEFAULT_ZOOM = isPhoneInit ? -3 : 0
const storedZoom = localStorage.getItem(ZOOM_KEY)
const zoom = ref(storedZoom !== null ? (Number(storedZoom) || 0) : DEFAULT_ZOOM)
const fontSize = computed(() => Math.max(8, BASE_FONT + zoom.value * 2))

function persistZoom() {
  try { localStorage.setItem(ZOOM_KEY, String(zoom.value)) } catch { /* ignore */ }
}

function zoomIn() {
  zoom.value = Math.min(zoom.value + 1, 10)
  if (term) { term.options.fontSize = fontSize.value; fitAddon?.fit() }
  persistZoom()
}
function zoomOut() {
  zoom.value = Math.max(zoom.value - 1, -5)
  if (term) { term.options.fontSize = fontSize.value; fitAddon?.fit() }
  persistZoom()
}

/* ── refs ── */
const fwRef = ref<InstanceType<typeof FloatingWindow>>()
const termRef = ref<HTMLElement>()

/* ── terminal + DC ── */
let term: Terminal | null = null
let fitAddon: FitAddon | null = null
let dc: RTCDataChannel | null = null
let resizeObserver: ResizeObserver | null = null
let unregisterBuilder: (() => void) | null = null
let wasConnected = false
let atBottom = true

/* ── CLI input: dumb terminal ─────────────────────────────────────────────
 * The device runs this DataChannel client in CLI_ANSI mode: the device line
 * editor (or, during an interactive ssh shell, the remote pty) owns echo, line
 * editing, history and tab-completion, emitting ANSI that xterm renders. We
 * echo nothing locally — every keystroke is forwarded verbatim and all received
 * bytes go straight to xterm. (No more local line editor / private raw-mode
 * OSC: the far end is always the echoer, so there's nothing to toggle.)
 *
 * Lossless send: bytes wait in `pending` and flush, in order, once the DC is
 * open, so a key pressed during a reconnect is never lost. */
let pending = ''
function flushPending() {
  if (!pending || !dc || dc.readyState !== 'open') return
  try { dc.send(pending); pending = '' } catch { /* keep buffered; retry on open */ }
}
function rawSend(data: string) { pending += data; flushPending() }

function onInput(data: string) {
  /* A lone control byte (Enter, Backspace, Ctrl-key) is emitted synchronously
   * from xterm's keydown and can overtake printable letters that xterm emits on
   * its own deferred setTimeout(0) under an IME — yielding "t<cr>op" for a typed
   * "top<cr>". Re-defer it by one macrotask so it lands after those letters
   * (same-delay timers fire FIFO). Paste / escape sequences (multi-byte) carry
   * their own order and run immediately. */
  if (data.length === 1 && (data.charCodeAt(0) < 0x20 || data.charCodeAt(0) === 0x7f))
    setTimeout(() => rawSend(data), 0)
  else
    rawSend(data)
}

function createTerminal() {
  if (term || !termRef.value) return
  term = new Terminal({
    fontSize: fontSize.value,
    fontFamily: "'SF Mono', 'Menlo', 'Monaco', 'Consolas', 'Liberation Mono', 'Courier New', monospace",
    theme: {
      background: '#000000',
      foreground: '#e0e0e0',
      cursor: props.readonly ? '#000000' : '#e0e0e0',
      selectionBackground: 'rgba(255,255,255,0.25)',
    },
    cursorBlink: !props.readonly,
    cursorInactiveStyle: props.readonly ? 'none' : 'outline',
    disableStdin: !!props.readonly,
    scrollback: 10000,
    convertEol: true,
  })
  fitAddon = new FitAddon()
  term.loadAddon(fitAddon)
  term.open(termRef.value)
  fitAddon.fit()

  term.onScroll(() => {
    const buf = term!.buffer.active
    atBottom = buf.viewportY >= buf.baseY
  })

  if (!props.readonly) {
    term.onData((data: string) => onInput(data))
  }

  resizeObserver = new ResizeObserver(() => fitAddon?.fit())
  resizeObserver.observe(termRef.value)
}

function destroyTerminal() {
  resizeObserver?.disconnect()
  resizeObserver = null
  term?.dispose()
  term = null
  fitAddon = null
}

/** Channel builder: called by the shared session on every fresh PC,
 *  BEFORE createOffer. Ensures our `log:1` / `cli:1` DC ships with the
 *  offer SDP's m=application line. */
function buildChannel(pc: RTCPeerConnection) {
  if (dc) { try { dc.onclose = null; dc.close() } catch { /* */ } }
  try {
    dc = pc.createDataChannel(props.dcLabel, {
      ordered: true,
      protocol: props.dcProtocol ?? '',
    })
  } catch (e) {
    console.error(`[${props.configPrefix}] createDataChannel failed:`, e)
    dc = null
    return
  }
  dc.binaryType = 'arraybuffer'
  dc.onopen = () => {
    atBottom = true
    if (wasConnected) {
      term?.writeln('\r\n\x1b[32m── reconnected ──\x1b[0m')
    } else {
      term?.clear()
    }
    wasConnected = true
    flushPending()   // send any key pressed while the channel was down
  }
  dc.onmessage = (ev) => {
    if (!term) return
    const raw = typeof ev.data === 'string'
      ? ev.data
      : new TextDecoder().decode(ev.data instanceof ArrayBuffer
          ? ev.data
          : (ev.data as Uint8Array).buffer)
    term.write(raw)
    const buf = term.buffer.active
    atBottom = buf.viewportY >= buf.baseY
    if (!atBottom) fwRef.value?.flashTitleBar()
  }
  dc.onclose = () => {
    dc = null
    /* Device closed just this channel (e.g. the CLI exited on `exit`) while
     * the session itself is still up: the DC is gone for good, so close the
     * window. When the channel instead drops because the whole session is
     * tearing down to reconnect, state has already left 'connected' by the
     * time this queued onclose runs — keep the window so it reattaches its
     * DC on the next peer connection. */
    if (getSession().state === 'connected') {
      emit('update:visible', false)
      return
    }
    if (wasConnected) {
      term?.writeln('\r\n\x1b[31m── channel closed ──\x1b[0m')
    }
  }
  dc.onerror = () => { /* onclose follows */ }
}

function attachSession() {
  if (unregisterBuilder) return
  unregisterBuilder = getSession().registerChannel(buildChannel)
  getSession().connect()
}

function detachSession() {
  if (unregisterBuilder) { unregisterBuilder(); unregisterBuilder = null }
  flushPending()
  if (dc) {
    const d = dc
    dc = null
    d.onclose = null
    d.onmessage = null
    try { d.close() } catch { /* */ }
  }
  wasConnected = false
}

/* ── lifecycle ── */

function showWindow() {
  createTerminal()
  attachSession()
  setTimeout(() => {
    fitAddon?.fit()
    if (!props.readonly) term?.focus()
  }, 50)
}

watch(() => props.visible, (vis) => {
  if (vis) nextTick(showWindow)
  else { detachSession(); destroyTerminal() }
})

onMounted(() => {
  if (props.visible) nextTick(showWindow)
})

onUnmounted(() => {
  detachSession()
  destroyTerminal()
})
</script>

<style scoped>
.term-zoom-btn {
  width: 18px; height: 18px; display: flex; align-items: center; justify-content: center;
  border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
}
.term-zoom-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }

.term-body { width: 100%; height: 100%; }

.term-body :deep(.xterm-viewport) { overflow-y: scroll !important; }
.term-body :deep(.xterm-viewport::-webkit-scrollbar) { width: 10px; }
.term-body :deep(.xterm-viewport::-webkit-scrollbar-track) { background: rgba(255,255,255,0.05); }
.term-body :deep(.xterm-viewport::-webkit-scrollbar-thumb) { background: rgba(255,255,255,0.2); border-radius: 5px; }
.term-body :deep(.xterm-viewport::-webkit-scrollbar-thumb:hover) { background: rgba(255,255,255,0.35); }
</style>
