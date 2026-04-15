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
import { deviceWssBase } from '../lib/epl'
import FloatingWindow from './FloatingWindow.vue'
import '@xterm/xterm/css/xterm.css'

const props = defineProps<{
  visible: boolean
  title: string
  endpoint: string
  readonly?: boolean
  configPrefix: string   // 'cli' or 'log' — window id + localStorage namespace
  backlogBytes?: number  // only meaningful for readonly log endpoint
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

/* ── terminal + WS ── */
let term: Terminal | null = null
let fitAddon: FitAddon | null = null
let ws: WebSocket | null = null
let resizeObserver: ResizeObserver | null = null
let reconnectTimer: ReturnType<typeof setTimeout> | null = null
let reconnectDelay = 1000
let wasConnected = false
let shouldReconnect = false
let atBottom = true

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
    term.onData((data: string) => {
      if (ws?.readyState === WebSocket.OPEN) ws.send(data)
    })
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

function scheduleReconnect() {
  if (!shouldReconnect || reconnectTimer) return
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null
    if (shouldReconnect) connectWs()
  }, reconnectDelay)
  reconnectDelay = Math.min(reconnectDelay * 2, 5000)
}

function wsUrl(): string {
  let url = `${deviceWssBase()}${props.endpoint}`
  /* Backlog only on first connect — reconnects pick up live from here. */
  if (!wasConnected && props.backlogBytes && props.backlogBytes > 0) {
    const sep = url.includes('?') ? '&' : '?'
    url += `${sep}backlog=${props.backlogBytes}`
  }
  return url
}

function connectWs() {
  if (ws) return
  shouldReconnect = true
  try {
    ws = new WebSocket(wsUrl())
  } catch {
    ws = null
    scheduleReconnect()
    return
  }
  ws.binaryType = 'arraybuffer'
  ws.onopen = () => {
    atBottom = true
    reconnectDelay = 1000
    if (wasConnected) {
      term?.writeln('\r\n\x1b[32m── reconnected ──\x1b[0m')
    } else {
      term?.clear()
    }
    wasConnected = true
  }
  ws.onmessage = (ev) => {
    if (!term) return
    term.write(typeof ev.data === 'string' ? ev.data : new TextDecoder().decode(ev.data))
    const buf = term.buffer.active
    atBottom = buf.viewportY >= buf.baseY
    if (!atBottom) fwRef.value?.flashTitleBar()
  }
  ws.onclose = () => {
    ws = null
    if (wasConnected && shouldReconnect) {
      term?.writeln('\r\n\x1b[31m── connection lost, reconnecting… ──\x1b[0m')
    }
    scheduleReconnect()
  }
  ws.onerror = () => { try { ws?.close() } catch { /* ignore */ } }
}

function disconnectWs() {
  shouldReconnect = false
  if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null }
  if (ws) { ws.onclose = null; try { ws.close() } catch { /* ignore */ } ws = null }
  wasConnected = false
  reconnectDelay = 1000
}

/* ── lifecycle ── */

function showWindow() {
  createTerminal()
  connectWs()
  setTimeout(() => {
    fitAddon?.fit()
    if (!props.readonly) term?.focus()
  }, 50)
}

watch(() => props.visible, (vis) => {
  if (vis) nextTick(showWindow)
  else { disconnectWs(); destroyTerminal() }
})

onMounted(() => {
  if (props.visible) nextTick(showWindow)
})

onUnmounted(() => {
  disconnectWs()
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
