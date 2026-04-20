<template>
  <FloatingWindow
    ref="fwRef"
    :id="configPrefix"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
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
  /** CLI input: coalesce keystrokes within this many ms into one DC message.
   *  0 = send each keystroke immediately. Default 50ms per the device plan. */
  coalesceMs?: number
}>()

const emit = defineEmits<{
  'update:visible': [value: boolean]
}>()

function onVisibleChange(v: boolean) { emit('update:visible', v) }

/* ── defaults + zoom persistence ── */
const DEFAULTS: Record<string, { x: number; y: number; w: number; h: number }> = {
  cli: { x: 12.5, y: 77.5, w: 75, h: 20 },
  log: { x: 12.5, y: 2.5, w: 75, h: 70 },
}
const defaultGeom = DEFAULTS[props.configPrefix] ?? DEFAULTS.cli

const BASE_FONT = 14
const ZOOM_KEY = `seccam.win.${props.configPrefix}.zoom`
const zoom = ref(Number(localStorage.getItem(ZOOM_KEY) ?? 0) || 0)
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

/* CLI keystroke coalescing: accumulate inputs for N ms, flush as one DC
   message. Matches the device's packet-mode semantics. */
let sendBuf: string[] = []
let sendTimer: ReturnType<typeof setTimeout> | null = null
function flushSend() {
  if (sendTimer) { clearTimeout(sendTimer); sendTimer = null }
  if (sendBuf.length === 0) return
  if (dc && dc.readyState === 'open') {
    try { dc.send(sendBuf.join('')) } catch { /* drop if DC died */ }
  }
  sendBuf = []
}
function sendData(data: string) {
  const ms = props.coalesceMs ?? 50
  if (ms <= 0) {
    if (dc && dc.readyState === 'open') {
      try { dc.send(data) } catch { /* */ }
    }
    return
  }
  sendBuf.push(data)
  /* Schedule a flush only if none pending — do NOT reset on each key. With
   * reset semantics, sustained typing would push the flush out indefinitely
   * and a DC reopen during that window would orphan the buffer. */
  if (!sendTimer) sendTimer = setTimeout(flushSend, ms)
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
    term.onData((data: string) => sendData(data))
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
  }
  dc.onmessage = (ev) => {
    if (!term) return
    const text = typeof ev.data === 'string'
      ? ev.data
      : new TextDecoder().decode(ev.data instanceof ArrayBuffer
          ? ev.data
          : (ev.data as Uint8Array).buffer)
    term.write(text)
    const buf = term.buffer.active
    atBottom = buf.viewportY >= buf.baseY
    if (!atBottom) fwRef.value?.flashTitleBar()
  }
  dc.onclose = () => {
    dc = null
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
  flushSend()
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
