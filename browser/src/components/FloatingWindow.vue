<template>
  <div
    v-show="visible"
    ref="windowRef"
    class="fw"
    :style="windowStyle"
    @mousedown="bringToFront"
  >
    <!-- Resize handles (conditionally rendered) -->
    <template v-if="canResizeV || canResizeH">
      <div v-if="canResizeV" class="fw-resize fw-resize-n" @mousedown.prevent="startResize('n', $event)" />
      <div v-if="canResizeV" class="fw-resize fw-resize-s" @mousedown.prevent="startResize('s', $event)" />
      <div v-if="canResizeH" class="fw-resize fw-resize-e" @mousedown.prevent="startResize('e', $event)" />
      <div v-if="canResizeH" class="fw-resize fw-resize-w" @mousedown.prevent="startResize('w', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-ne" @mousedown.prevent="startResize('ne', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-nw" @mousedown.prevent="startResize('nw', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-se" @mousedown.prevent="startResize('se', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-sw" @mousedown.prevent="startResize('sw', $event)" />
    </template>

    <!-- Titlebar -->
    <div
      class="fw-titlebar"
      :class="{ 'fw-titlebar-flash': flashing }"
      @mousedown.prevent="startDrag($event)"
    >
      <div class="fw-close" @mousedown.stop @click="close" />
      <span class="fw-title">{{ title }}</span>
      <div class="fw-titlebar-right" @mousedown.stop>
        <slot name="titlebar-right" />
      </div>
    </div>

    <!-- Body -->
    <div ref="bodyRef" class="fw-body">
      <slot :size="bodySize" />
    </div>
  </div>
</template>

<script lang="ts">
/* Shared z-counter across all FloatingWindow instances */
let zCounter = 1000
</script>

<script setup lang="ts">
import { ref, reactive, watch, onMounted, onUnmounted, nextTick, computed } from 'vue'
import { docks, layout, dockWindow, undockWindow, type DockSide } from '../modules/advanced'

interface Geom { x: number; y: number; w: number; h: number }
interface MinSize { w: number; h: number }

const props = withDefaults(defineProps<{
  id: string
  title: string
  visible: boolean
  canResizeV?: boolean
  canResizeH?: boolean
  canDock?: boolean
  defaultGeom?: Geom
  minSize?: MinSize
}>(), {
  canResizeV: true,
  canResizeH: true,
  canDock: true,
  defaultGeom: () => ({ x: 25, y: 25, w: 50, h: 50 }),
  minSize: () => ({ w: 10, h: 8 }),
})

const emit = defineEmits<{
  'update:visible': [value: boolean]
}>()

defineExpose({ bringToFront, flashTitleBar })

/* ── refs ── */
const windowRef = ref<HTMLElement>()
const bodyRef = ref<HTMLElement>()

/* ── z-order ── */
const zIndex = ref(zCounter)
function bringToFront() { zIndex.value = ++zCounter }

/* ── geometry ── */
const DOCK_THRESH = 1.5

const pctX = ref(props.defaultGeom.x)
const pctY = ref(props.defaultGeom.y)
const pctW = ref(props.defaultGeom.w)
const pctH = ref(props.defaultGeom.h)

let preDockW = pctW.value
let preDockH = pctH.value

const isDocked = computed(() => props.canDock && docks[props.id]?.side != null)

const windowStyle = computed(() => {
  const rect = isDocked.value ? layout.value.rects[props.id] : null
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

/* ── body size exposed to slot ── */
const bodySize = reactive({ w: 0, h: 0 })
let resizeObserver: ResizeObserver | null = null

/* ── persistence ── */
const STORAGE_KEY = `seccam.win.${props.id}`

interface StoredState {
  x: number; y: number; w: number; h: number
  visible: boolean
  dock: DockSide | null
  dockSize: number
}

function loadState(): void {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (!raw) return
    const s = JSON.parse(raw) as Partial<StoredState>
    if (typeof s.x === 'number') pctX.value = s.x
    if (typeof s.y === 'number') pctY.value = s.y
    if (typeof s.w === 'number') pctW.value = s.w
    if (typeof s.h === 'number') pctH.value = s.h
    preDockW = pctW.value
    preDockH = pctH.value
    if (props.canDock && s.dock && typeof s.dockSize === 'number') {
      dockWindow(props.id, s.dock as DockSide, s.dockSize)
    }
    if (typeof s.visible === 'boolean' && s.visible !== props.visible) {
      emit('update:visible', s.visible)
    }
  } catch { /* corrupt JSON — ignore */ }
}

let saveTimer: ReturnType<typeof setTimeout> | null = null
function saveState(): void {
  if (saveTimer) clearTimeout(saveTimer)
  saveTimer = setTimeout(() => {
    saveTimer = null
    const dock = props.canDock ? docks[props.id] : null
    const s: StoredState = {
      x: Math.round(pctX.value * 10) / 10,
      y: Math.round(pctY.value * 10) / 10,
      w: Math.round(pctW.value * 10) / 10,
      h: Math.round(pctH.value * 10) / 10,
      visible: props.visible,
      dock: (dock?.side as DockSide) ?? null,
      dockSize: dock?.size ?? 0,
    }
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(s)) }
    catch { /* quota — ignore */ }
  }, 500)
}

/* Persist on visibility changes. */
watch(() => props.visible, saveState)

/* ── close ── */
function close() { emit('update:visible', false) }

/* ── clamp ── */
function clamp() {
  const a = layout.value.floatingArea
  const aw = a.right - a.left
  const ah = a.bottom - a.top
  pctW.value = Math.min(aw, Math.max(props.minSize.w, pctW.value))
  pctH.value = Math.min(ah, Math.max(props.minSize.h, pctH.value))
  pctX.value = Math.min(a.right - pctW.value, Math.max(a.left, pctX.value))
  pctY.value = Math.min(a.bottom - pctH.value, Math.max(a.top, pctY.value))
}

/* Re-clamp when the floating area changes (another window docked/resized) */
watch(() => layout.value.floatingArea, () => {
  if (!isDocked.value && props.visible) clamp()
}, { deep: true })

/* ── container dimensions ── */
function containerSize(): { cw: number; ch: number } {
  const el = windowRef.value?.parentElement
  return { cw: el?.clientWidth ?? 1, ch: el?.clientHeight ?? 1 }
}
function containerRect(): DOMRect | undefined {
  return windowRef.value?.parentElement?.getBoundingClientRect()
}

/* ── dock edge detect ── */
function detectDockEdge(): DockSide | null {
  if (!props.canDock) return null
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
  dockWindow(props.id, side, size)
  saveState()
}

function performUndock(e: MouseEvent) {
  const dock = docks[props.id]
  const wasSide = dock?.side
  if (wasSide === 'top' || wasSide === 'bottom') {
    pctW.value = preDockW || pctW.value
    pctH.value = dock.size
  } else if (wasSide === 'left' || wasSide === 'right') {
    pctH.value = preDockH || pctH.value
    pctW.value = dock.size
  }
  undockWindow(props.id)
  const cr = containerRect()
  const { cw, ch } = containerSize()
  if (cr) {
    pctX.value = (e.clientX - cr.left) / cw * 100 - pctW.value / 2
    pctY.value = (e.clientY - cr.top) / ch * 100 - 2
  }
  clamp()
}

/* ── drag ── */
const MIN_DRAG_PX = 8
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
  pctW.value = Math.min(100, Math.max(props.minSize.w, pctW.value))
  pctH.value = Math.min(100, Math.max(props.minSize.h, pctH.value))
  pctX.value = Math.min(100 - pctW.value, Math.max(0, pctX.value))
  pctY.value = Math.min(100 - pctH.value, Math.max(0, pctY.value))
}
function endDrag() {
  window.removeEventListener('mousemove', onDrag)
  window.removeEventListener('mouseup', endDrag)
  if (dragMoved) {
    const edge = detectDockEdge()
    if (edge) performDock(edge)
    else { clamp(); saveState() }
  } else if (wasDockedOnDragStart) {
    clamp(); saveState()
  } else {
    saveState()
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
    const side = docks[props.id].side
    const allowed = side === 'bottom' ? 'n' : side === 'top' ? 's' : side === 'left' ? 'e' : 'w'
    if (!edge.includes(allowed as string)) return
  }
  resizeEdge = edge
  resizeStartX = e.clientX
  resizeStartY = e.clientY
  resizeStartW = isDocked.value ? docks[props.id].size : pctW.value
  resizeStartH = isDocked.value ? docks[props.id].size : pctH.value
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
    const dock = docks[props.id]
    if (dock.side === 'bottom') dock.size = Math.max(props.minSize.h, resizeStartH - dy)
    else if (dock.side === 'top') dock.size = Math.max(props.minSize.h, resizeStartH + dy)
    else if (dock.side === 'right') dock.size = Math.max(props.minSize.w, resizeStartW - dx)
    else if (dock.side === 'left') dock.size = Math.max(props.minSize.w, resizeStartW + dx)
  } else {
    if (resizeEdge.includes('e')) pctW.value = Math.max(props.minSize.w, resizeStartW + dx)
    if (resizeEdge.includes('w')) {
      const nw = Math.max(props.minSize.w, resizeStartW - dx)
      pctX.value = resizeStartPX + resizeStartW - nw
      pctW.value = nw
    }
    if (resizeEdge.includes('s')) pctH.value = Math.max(props.minSize.h, resizeStartH + dy)
    if (resizeEdge.includes('n')) {
      const nh = Math.max(props.minSize.h, resizeStartH - dy)
      pctY.value = resizeStartPY + resizeStartH - nh
      pctH.value = nh
    }
    clamp()
  }
}
function endResize() {
  window.removeEventListener('mousemove', onResize)
  window.removeEventListener('mouseup', endResize)
  saveState()
}

/* ── titlebar flash ── */
const flashing = ref(false)
let flashTimer: ReturnType<typeof setTimeout> | null = null

function flashTitleBar() {
  if (!props.visible) return
  flashing.value = true
  if (flashTimer) clearTimeout(flashTimer)
  flashTimer = setTimeout(() => { flashing.value = false }, 800)
}

/* ── lifecycle ── */
onMounted(() => {
  loadState()
  if (!isDocked.value) clamp()

  resizeObserver = new ResizeObserver(() => {
    const el = bodyRef.value
    if (!el) return
    bodySize.w = el.clientWidth
    bodySize.h = el.clientHeight
  })
  if (bodyRef.value) resizeObserver.observe(bodyRef.value)

  nextTick(() => {
    if (bodyRef.value) {
      bodySize.w = bodyRef.value.clientWidth
      bodySize.h = bodyRef.value.clientHeight
    }
  })
})

onUnmounted(() => {
  resizeObserver?.disconnect()
  resizeObserver = null
  if (saveTimer) { clearTimeout(saveTimer); saveTimer = null }
  if (flashTimer) { clearTimeout(flashTimer); flashTimer = null }
})

watch(() => props.visible, (vis) => {
  if (vis) {
    bringToFront()
    if (!isDocked.value) clamp()
  } else if (props.canDock && docks[props.id]?.side) {
    /* Window hidden while docked — release the dock slot so other windows
     * can reclaim the space. The chosen side+size stays in localStorage and
     * loadState() restores it on the next mount. */
    undockWindow(props.id)
  }
})
</script>

<style scoped>
.fw {
  position: absolute;
  border: 1px solid white;
  border-radius: 6px;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  background: #000000;
}

.fw-titlebar {
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
.fw-titlebar:active { cursor: grabbing; }

.fw-titlebar-flash { animation: titleFlash 0.8s ease; }
@keyframes titleFlash {
  0%   { background-color: #282828; }
  30%  { background-color: #64401e; }
  100% { background-color: #282828; }
}

.fw-close {
  width: 12px; height: 12px; border-radius: 50%;
  background: #ff5f57; cursor: pointer; flex-shrink: 0;
}
.fw-close:hover { background: #ff3b30; }

.fw-title {
  flex: 1; text-align: center; font-size: 12px; font-weight: 500;
  color: rgba(255,255,255,0.7); font-family: system-ui;
}

.fw-titlebar-right { display: flex; gap: 4px; flex-shrink: 0; }

.fw-body { flex: 1; overflow: hidden; padding: 0 5px; }

.fw-resize { position: absolute; z-index: 10; }
.fw-resize-n  { top: -3px; left: 8px; right: 8px; height: 6px; cursor: n-resize; }
.fw-resize-s  { bottom: -3px; left: 8px; right: 8px; height: 6px; cursor: s-resize; }
.fw-resize-e  { right: -3px; top: 8px; bottom: 8px; width: 6px; cursor: e-resize; }
.fw-resize-w  { left: -3px; top: 8px; bottom: 8px; width: 6px; cursor: w-resize; }
.fw-resize-ne { top: -3px; right: -3px; width: 12px; height: 12px; cursor: ne-resize; }
.fw-resize-nw { top: -3px; left: -3px; width: 12px; height: 12px; cursor: nw-resize; }
.fw-resize-se { bottom: -3px; right: -3px; width: 12px; height: 12px; cursor: se-resize; }
.fw-resize-sw { bottom: -3px; left: -3px; width: 12px; height: 12px; cursor: sw-resize; }
</style>
