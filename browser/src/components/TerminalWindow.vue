<template>
  <div
    v-show="visible"
    ref="windowRef"
    class="term-window"
    :style="windowStyle"
    @mousedown="bringToFront"
  >
    <!-- Resize handles -->
    <div class="resize-handle resize-n" @mousedown.prevent="startResize('n', $event)" />
    <div class="resize-handle resize-s" @mousedown.prevent="startResize('s', $event)" />
    <div class="resize-handle resize-e" @mousedown.prevent="startResize('e', $event)" />
    <div class="resize-handle resize-w" @mousedown.prevent="startResize('w', $event)" />
    <div class="resize-handle resize-ne" @mousedown.prevent="startResize('ne', $event)" />
    <div class="resize-handle resize-nw" @mousedown.prevent="startResize('nw', $event)" />
    <div class="resize-handle resize-se" @mousedown.prevent="startResize('se', $event)" />
    <div class="resize-handle resize-sw" @mousedown.prevent="startResize('sw', $event)" />

    <!-- Title bar -->
    <div
      class="term-titlebar"
      :class="{ 'term-titlebar-flash': flashing }"
      @mousedown.prevent="startDrag($event)"
    >
      <div class="term-close" @mousedown.stop @click="$emit('update:visible', false)" />
      <span class="term-title">{{ title }}</span>
      <div class="term-zoom-controls" @mousedown.stop>
        <span class="term-zoom-btn" @click="zoomOut">-</span>
        <span class="term-zoom-btn" @click="zoomIn">+</span>
      </div>
    </div>

    <!-- Terminal container -->
    <div ref="termRef" class="term-body" />
  </div>
</template>

<script lang="ts">
/* Shared z-counter across all TerminalWindow instances */
let zCounter = 1000
</script>

<script setup lang="ts">
import { ref, watch, onMounted, onUnmounted, nextTick, computed } from 'vue'
import { Terminal } from '@xterm/xterm'
import { FitAddon } from '@xterm/addon-fit'
import { deviceWssBase } from '../lib/epl'
import { useDeviceStore } from '../stores/device'
import { docks, layout, dockWindow, undockWindow, type DockSide } from '../modules/advanced'
import '@xterm/xterm/css/xterm.css'

const props = defineProps<{
  visible: boolean
  title: string
  endpoint: string
  readonly?: boolean
  configPrefix: string    // 'cli' or 'log' — also used as dock ID
}>()

const emit = defineEmits<{
  'update:visible': [value: boolean]
}>()

const device = useDeviceStore()
const dockId = computed(() => props.configPrefix)

/* ── refs ── */
const windowRef = ref<HTMLElement>()
const termRef = ref<HTMLElement>()

/* ── z-order ── */
const zIndex = ref(zCounter)
function bringToFront() { zIndex.value = ++zCounter }

/* ── geometry ── */
const BASE_FONT = 14
const MIN_PCT_W = 10
const MIN_PCT_H = 8
const DOCK_THRESH = 1.5

/* Floating geometry (stored in config, used when not docked) */
const pctX = ref(0)
const pctY = ref(0)
const pctW = ref(50)
const pctH = ref(50)
const zoom = ref(0)

/* Pre-dock size to restore on undock (the dimension that gets expanded) */
let preDockW = 0
let preDockH = 0

const fontSize = computed(() => Math.max(8, BASE_FONT + zoom.value * 2))
const isDocked = computed(() => docks[dockId.value]?.side !== null)

const windowStyle = computed(() => {
  const rect = isDocked.value ? layout.value.rects[dockId.value] : null
  if (rect) {
    return {
      left: `${rect.x}%`, top: `${rect.y}%`,
      width: `${rect.w}%`, height: `${rect.h}%`,
      zIndex: zIndex.value,
    }
  }
  return {
    left: `${pctX.value}%`, top: `${pctY.value}%`,
    width: `${pctW.value}%`, height: `${pctH.value}%`,
    zIndex: zIndex.value,
  }
})

/* ── clamp floating window to floating area ── */
function clamp() {
  const a = layout.value.floatingArea
  const aw = a.right - a.left
  const ah = a.bottom - a.top
  pctW.value = Math.min(aw, Math.max(MIN_PCT_W, pctW.value))
  pctH.value = Math.min(ah, Math.max(MIN_PCT_H, pctH.value))
  pctX.value = Math.min(a.right - pctW.value, Math.max(a.left, pctX.value))
  pctY.value = Math.min(a.bottom - pctH.value, Math.max(a.top, pctY.value))
}

/* ── default positions ── */
const DEFAULTS: Record<string, { x: number; y: number; w: number; h: number }> = {
  cli: { x: 12.5, y: 77.5, w: 75, h: 20 },
  log: { x: 12.5, y: 2.5, w: 75, h: 70 },
}

/* ── load/save config ── */
function loadConfig() {
  const d = DEFAULTS[props.configPrefix] ?? DEFAULTS.cli
  const pre = `s.${props.configPrefix}.win`
  pctX.value = (device.get(`${pre}.x`) as number) ?? d.x
  pctY.value = (device.get(`${pre}.y`) as number) ?? d.y
  pctW.value = (device.get(`${pre}.w`) as number) ?? d.w
  pctH.value = (device.get(`${pre}.h`) as number) ?? d.h
  zoom.value = (device.get(`${pre}.zoom`) as number) ?? 0
  preDockW = pctW.value
  preDockH = pctH.value
  if (!isDocked.value) clamp()
}

let saveTimer: ReturnType<typeof setTimeout> | null = null
function saveConfig() {
  if (saveTimer) clearTimeout(saveTimer)
  saveTimer = setTimeout(() => {
    saveTimer = null
    const pre = `s.${props.configPrefix}.win`
    /* Always save floating geometry */
    device.set(`${pre}.x`, Math.round(pctX.value * 10) / 10)
    device.set(`${pre}.y`, Math.round(pctY.value * 10) / 10)
    device.set(`${pre}.w`, Math.round(pctW.value * 10) / 10)
    device.set(`${pre}.h`, Math.round(pctH.value * 10) / 10)
    device.set(`${pre}.zoom`, zoom.value)
    /* Save dock state */
    const dock = docks[dockId.value]
    device.set(`${pre}.dock`, dock.side ?? '')
    device.set(`${pre}.dock_size`, dock.size)
  }, 500)
}

/* ── container dimensions ── */
function containerSize(): { cw: number; ch: number } {
  const el = windowRef.value?.parentElement
  return { cw: el?.clientWidth ?? 1, ch: el?.clientHeight ?? 1 }
}
function containerRect(): DOMRect | undefined {
  return windowRef.value?.parentElement?.getBoundingClientRect()
}

/* ── zoom ── */
function zoomIn() {
  zoom.value = Math.min(zoom.value + 1, 10)
  if (term) { term.options.fontSize = fontSize.value; fitAddon?.fit() }
  saveConfig()
}
function zoomOut() {
  zoom.value = Math.max(zoom.value - 1, -5)
  if (term) { term.options.fontSize = fontSize.value; fitAddon?.fit() }
  saveConfig()
}

/* ── detect which container edge the window touches ── */
function detectDockEdge(): DockSide | null {
  if (pctY.value + pctH.value >= 100 - DOCK_THRESH) return 'bottom'
  if (pctY.value <= DOCK_THRESH) return 'top'
  if (pctX.value <= DOCK_THRESH) return 'left'
  if (pctX.value + pctW.value >= 100 - DOCK_THRESH) return 'right'
  return null
}

/* ── dock / undock ── */
function performDock(side: DockSide) {
  preDockW = pctW.value
  preDockH = pctH.value
  const size = (side === 'top' || side === 'bottom') ? pctH.value : pctW.value
  dockWindow(dockId.value, side, size)
  saveConfig()
  nextTick(() => fitAddon?.fit())
}

function performUndock(e: MouseEvent) {
  const dock = docks[dockId.value]
  const wasSide = dock.side
  /* Restore pre-dock size in the dimension that was expanded */
  if (wasSide === 'top' || wasSide === 'bottom') {
    pctW.value = preDockW || pctW.value
    pctH.value = dock.size
  } else if (wasSide === 'left' || wasSide === 'right') {
    pctH.value = preDockH || pctH.value
    pctW.value = dock.size
  }
  undockWindow(dockId.value)
  /* Position window centered on mouse cursor */
  const cr = containerRect()
  const { cw, ch } = containerSize()
  if (cr) {
    pctX.value = (e.clientX - cr.left) / cw * 100 - pctW.value / 2
    pctY.value = (e.clientY - cr.top) / ch * 100 - 2
  }
  clamp()
}

/* ── drag ── */
const MIN_DRAG_PX = 8  /* minimum pixels moved to count as a real drag (not just a click) */
let dragStartX = 0, dragStartY = 0, dragOffX = 0, dragOffY = 0
let dragMoved = false
let wasDockedOnDragStart = false

function startDrag(e: MouseEvent) {
  bringToFront()
  wasDockedOnDragStart = isDocked.value
  if (isDocked.value) performUndock(e)
  dragStartX = e.clientX
  dragStartY = e.clientY
  dragOffX = pctX.value
  dragOffY = pctY.value
  dragMoved = false
  window.addEventListener('mousemove', onDrag)
  window.addEventListener('mouseup', endDrag)
}
function onDrag(e: MouseEvent) {
  const dx = e.clientX - dragStartX
  const dy = e.clientY - dragStartY
  if (!dragMoved && dx * dx + dy * dy >= MIN_DRAG_PX * MIN_DRAG_PX) dragMoved = true
  const { cw, ch } = containerSize()
  pctX.value = dragOffX + dx / cw * 100
  pctY.value = dragOffY + dy / ch * 100
  /* Clamp to full container during drag (not floating area — we need edge detection) */
  pctW.value = Math.min(100, Math.max(MIN_PCT_W, pctW.value))
  pctH.value = Math.min(100, Math.max(MIN_PCT_H, pctH.value))
  pctX.value = Math.min(100 - pctW.value, Math.max(0, pctX.value))
  pctY.value = Math.min(100 - pctH.value, Math.max(0, pctY.value))
}
function endDrag() {
  window.removeEventListener('mousemove', onDrag)
  window.removeEventListener('mouseup', endDrag)
  if (dragMoved) {
    const edge = detectDockEdge()
    if (edge) {
      performDock(edge)
    } else {
      clamp()
      saveConfig()
    }
  } else if (wasDockedOnDragStart) {
    /* Click without drag on a docked window — stay undocked at current position */
    clamp()
    saveConfig()
  } else {
    saveConfig()
  }
}

/* ── resize ── */
type Edge = 'n' | 's' | 'e' | 'w' | 'ne' | 'nw' | 'se' | 'sw'
let resizeEdge: Edge = 's'
let resizeStartX = 0, resizeStartY = 0
let resizeStartW = 0, resizeStartH = 0
let resizeStartPX = 0, resizeStartPY = 0

function startResize(edge: Edge, e: MouseEvent) {
  bringToFront()
  if (isDocked.value) {
    /* When docked, only allow resizing the inner edge */
    const side = docks[dockId.value].side
    const allowed = side === 'bottom' ? 'n' : side === 'top' ? 's' : side === 'left' ? 'e' : 'w'
    if (!edge.includes(allowed)) return
  }
  resizeEdge = edge
  resizeStartX = e.clientX
  resizeStartY = e.clientY
  resizeStartW = isDocked.value ? docks[dockId.value].size : pctW.value
  resizeStartH = isDocked.value ? docks[dockId.value].size : pctH.value
  resizeStartPX = pctX.value
  resizeStartPY = pctY.value
  window.addEventListener('mousemove', onResize)
  window.addEventListener('mouseup', endResize)
}
function onResize(e: MouseEvent) {
  const { cw, ch } = containerSize()
  const dx = (e.clientX - resizeStartX) / cw * 100
  const dy = (e.clientY - resizeStartY) / ch * 100

  if (isDocked.value) {
    /* Docked resize: only change dock.size */
    const dock = docks[dockId.value]
    if (dock.side === 'bottom') dock.size = Math.max(MIN_PCT_H, resizeStartH - dy)
    else if (dock.side === 'top') dock.size = Math.max(MIN_PCT_H, resizeStartH + dy)
    else if (dock.side === 'right') dock.size = Math.max(MIN_PCT_W, resizeStartW - dx)
    else if (dock.side === 'left') dock.size = Math.max(MIN_PCT_W, resizeStartW + dx)
  } else {
    /* Floating resize */
    if (resizeEdge.includes('e')) pctW.value = Math.max(MIN_PCT_W, resizeStartW + dx)
    if (resizeEdge.includes('w')) {
      const nw = Math.max(MIN_PCT_W, resizeStartW - dx)
      pctX.value = resizeStartPX + resizeStartW - nw
      pctW.value = nw
    }
    if (resizeEdge.includes('s')) pctH.value = Math.max(MIN_PCT_H, resizeStartH + dy)
    if (resizeEdge.includes('n')) {
      const nh = Math.max(MIN_PCT_H, resizeStartH - dy)
      pctY.value = resizeStartPY + resizeStartH - nh
      pctH.value = nh
    }
    clamp()
  }
  fitAddon?.fit()
}
function endResize() {
  window.removeEventListener('mousemove', onResize)
  window.removeEventListener('mouseup', endResize)
  saveConfig()
}

/* ── re-clamp floating windows when floating area changes (another window docked/resized) ── */
watch(() => layout.value.floatingArea, () => {
  if (!isDocked.value && props.visible) {
    clamp()
    nextTick(() => fitAddon?.fit())
  }
}, { deep: true })

/* ── title bar flash ── */
const flashing = ref(false)
let flashTimer: ReturnType<typeof setTimeout> | null = null
let atBottom = true

function flashTitleBar() {
  if (atBottom || !props.visible) return
  flashing.value = true
  if (flashTimer) clearTimeout(flashTimer)
  flashTimer = setTimeout(() => { flashing.value = false }, 800)
}

/* ── terminal + WS ── */
let term: Terminal | null = null
let fitAddon: FitAddon | null = null
let ws: WebSocket | null = null
let resizeObserver: ResizeObserver | null = null

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

function connectWs() {
  if (ws) return
  ws = new WebSocket(`${deviceWssBase()}${props.endpoint}`)
  ws.binaryType = 'arraybuffer'
  ws.onopen = () => { term?.clear(); atBottom = true }
  ws.onmessage = (ev) => {
    if (!term) return
    term.write(typeof ev.data === 'string' ? ev.data : new TextDecoder().decode(ev.data))
    const buf = term.buffer.active
    atBottom = buf.viewportY >= buf.baseY
    if (!atBottom) flashTitleBar()
  }
  ws.onclose = () => { ws = null }
  ws.onerror = () => { ws?.close() }
}

function disconnectWs() {
  if (ws) { ws.onclose = null; ws.close(); ws = null }
}

/* ── lifecycle ── */
watch(() => props.visible, (vis) => {
  device.set(`s.${props.configPrefix}.win.visible`, vis ? 1 : 0)
  if (vis) {
    loadConfig()
    bringToFront()
    nextTick(() => {
      createTerminal()
      connectWs()
      setTimeout(() => {
        fitAddon?.fit()
        if (!props.readonly) term?.focus()
      }, 50)
    })
  } else {
    disconnectWs()
    destroyTerminal()
  }
})

onMounted(() => {
  if (props.visible) {
    loadConfig()
    nextTick(() => {
      createTerminal()
      connectWs()
      setTimeout(() => {
        fitAddon?.fit()
        if (!props.readonly) term?.focus()
      }, 50)
    })
  }
})

onUnmounted(() => {
  disconnectWs()
  destroyTerminal()
  if (saveTimer) clearTimeout(saveTimer)
  if (flashTimer) clearTimeout(flashTimer)
})
</script>

<style scoped>
.term-window {
  position: absolute;
  border: 1px solid white;
  border-radius: 6px;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  background: #000000;
}

.term-titlebar {
  display: flex;
  align-items: center;
  height: 28px;
  min-height: 28px;
  padding: 0 10px;
  background: #282828;
  cursor: grab;
  user-select: none;
  transition: background-color 0.3s ease;
}
.term-titlebar:active { cursor: grabbing; }

.term-titlebar-flash { animation: titleFlash 0.8s ease; }
@keyframes titleFlash {
  0%   { background-color: #282828; }
  30%  { background-color: #64401e; }
  100% { background-color: #282828; }
}

.term-close {
  width: 12px; height: 12px; border-radius: 50%;
  background: #ff5f57; cursor: pointer; flex-shrink: 0;
}
.term-close:hover { background: #ff3b30; }

.term-title {
  flex: 1; text-align: center; font-size: 12px; font-weight: 500;
  color: rgba(255,255,255,0.7); font-family: system-ui;
}

.term-zoom-controls { display: flex; gap: 4px; flex-shrink: 0; }
.term-zoom-btn {
  width: 18px; height: 18px; display: flex; align-items: center; justify-content: center;
  border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
}
.term-zoom-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }

.term-body { flex: 1; overflow: hidden; padding: 0 5px; }

.resize-handle { position: absolute; z-index: 10; }
.resize-n  { top: -3px; left: 8px; right: 8px; height: 6px; cursor: n-resize; }
.resize-s  { bottom: -3px; left: 8px; right: 8px; height: 6px; cursor: s-resize; }
.resize-e  { right: -3px; top: 8px; bottom: 8px; width: 6px; cursor: e-resize; }
.resize-w  { left: -3px; top: 8px; bottom: 8px; width: 6px; cursor: w-resize; }
.resize-ne { top: -3px; right: -3px; width: 12px; height: 12px; cursor: ne-resize; }
.resize-nw { top: -3px; left: -3px; width: 12px; height: 12px; cursor: nw-resize; }
.resize-se { bottom: -3px; right: -3px; width: 12px; height: 12px; cursor: se-resize; }
.resize-sw { bottom: -3px; left: -3px; width: 12px; height: 12px; cursor: sw-resize; }

.term-body :deep(.xterm-viewport) { overflow-y: scroll !important; }
.term-body :deep(.xterm-viewport::-webkit-scrollbar) { width: 10px; }
.term-body :deep(.xterm-viewport::-webkit-scrollbar-track) { background: rgba(255,255,255,0.05); }
.term-body :deep(.xterm-viewport::-webkit-scrollbar-thumb) { background: rgba(255,255,255,0.2); border-radius: 5px; }
.term-body :deep(.xterm-viewport::-webkit-scrollbar-thumb:hover) { background: rgba(255,255,255,0.35); }
</style>
