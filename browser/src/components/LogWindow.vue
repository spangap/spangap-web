<template>
  <FloatingWindow
    ref="fwRef"
    id="log"
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
import FloatingWindow from './FloatingWindow.vue'
import { getLogBuffer, subscribeLog } from '../stores/log'
import '@xterm/xterm/css/xterm.css'

const props = defineProps<{
  visible: boolean
  title: string
}>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()
function onVisibleChange(v: boolean) { emit('update:visible', v) }

/* Phone-sized initial layout: full-width, half-screen tall, docked top.
 * Sampled once at load — Quasar's reactive `screen` would over-engineer
 * a one-time default. */
const isPhoneInit = window.matchMedia?.('(max-width: 599px)').matches ?? false
const defaultGeom = isPhoneInit
  ? { x: 0, y: 0, w: 100, h: 50 }
  : { x: 12.5, y: 2.5, w: 75, h: 70 }
const defaultDock = isPhoneInit
  ? { side: 'top' as const, size: 50 }
  : null

const BASE_FONT = 14
const ZOOM_KEY = 'diptych.win.log.zoom'
/* Phone default starts 3 stops down (font ≈ 8px) so a packed log fits
 * in the half-screen window. Stored value, when present, wins. */
const DEFAULT_ZOOM = isPhoneInit ? -3 : 0
const stored = localStorage.getItem(ZOOM_KEY)
const zoom = ref(stored !== null ? (Number(stored) || 0) : DEFAULT_ZOOM)
const fontSize = computed(() => Math.max(8, BASE_FONT + zoom.value * 2))

function persistZoom() {
  try { localStorage.setItem(ZOOM_KEY, String(zoom.value)) } catch { /* */ }
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

const fwRef = ref<InstanceType<typeof FloatingWindow>>()
const termRef = ref<HTMLElement>()

let term: Terminal | null = null
let fitAddon: FitAddon | null = null
let resizeObserver: ResizeObserver | null = null
let unsubscribe: (() => void) | null = null
let atBottom = true

function createTerminal() {
  if (term || !termRef.value) return
  term = new Terminal({
    fontSize: fontSize.value,
    fontFamily: "'SF Mono', 'Menlo', 'Monaco', 'Consolas', 'Liberation Mono', 'Courier New', monospace",
    theme: {
      background: '#000000',
      foreground: '#e0e0e0',
      cursor: '#000000',
      selectionBackground: 'rgba(255,255,255,0.25)',
    },
    cursorBlink: false,
    cursorInactiveStyle: 'none',
    disableStdin: true,
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

  /* Initial paste of buffered backlog accumulated before the window opened. */
  const backlog = getLogBuffer()
  if (backlog.length > 0) term.write(backlog)

  /* Stream new lines as they arrive. */
  unsubscribe = subscribeLog((text) => {
    if (!term) return
    term.write(text)
    const buf = term.buffer.active
    atBottom = buf.viewportY >= buf.baseY
    if (!atBottom) fwRef.value?.flashTitleBar()
  })

  resizeObserver = new ResizeObserver(() => fitAddon?.fit())
  resizeObserver.observe(termRef.value)
}

function destroyTerminal() {
  if (unsubscribe) { unsubscribe(); unsubscribe = null }
  resizeObserver?.disconnect()
  resizeObserver = null
  term?.dispose()
  term = null
  fitAddon = null
}

function showWindow() {
  createTerminal()
  setTimeout(() => fitAddon?.fit(), 50)
}

watch(() => props.visible, (vis) => {
  if (vis) nextTick(showWindow)
  else destroyTerminal()
})

onMounted(() => { if (props.visible) nextTick(showWindow) })
onUnmounted(destroyTerminal)
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
