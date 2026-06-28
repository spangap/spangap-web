<template>
  <div
    v-show="shown"
    ref="windowRef"
    class="fw"
    :class="{ 'fw--compact': compact }"
    :style="windowStyle"
    @mousedown="bringToFront"
  >
    <!-- Resize handles — suppressed in compact mode: the window is full-screen
         there and neither draggable nor resizable. -->
    <template v-if="(canResizeV || canResizeH) && !compact">
      <div v-if="canResizeV" class="fw-resize fw-resize-n" @pointerdown.prevent="startResize('n', $event)" />
      <div v-if="canResizeV" class="fw-resize fw-resize-s" @pointerdown.prevent="startResize('s', $event)" />
      <div v-if="canResizeH" class="fw-resize fw-resize-e" @pointerdown.prevent="startResize('e', $event)" />
      <div v-if="canResizeH" class="fw-resize fw-resize-w" @pointerdown.prevent="startResize('w', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-ne" @pointerdown.prevent="startResize('ne', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-nw" @pointerdown.prevent="startResize('nw', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-se" @pointerdown.prevent="startResize('se', $event)" />
      <div v-if="canResizeV && canResizeH" class="fw-resize fw-resize-sw" @pointerdown.prevent="startResize('sw', $event)" />
    </template>

    <!-- Titlebar -->
    <div
      class="fw-titlebar"
      :class="{ 'fw-titlebar-flash': flashing }"
      @pointerdown.prevent="startDrag($event)"
    >
      <div class="fw-close" @pointerdown.stop @click="close" />
      <span class="fw-title">{{ title }}</span>
      <div class="fw-titlebar-right" @pointerdown.stop>
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
import { useCompact } from '../lib/viewport'
import {
  registerWindow, unregisterWindow, setWindowTitle, setWindowVisible,
  setWindowZ, focusedWindowId, windowFocusReq,
} from '../lib/windows'

interface Geom { x: number; y: number; w: number; h: number }
interface MinSize { w: number; h: number }

const props = withDefaults(defineProps<{
  id: string
  title: string
  visible: boolean
  canResizeV?: boolean
  canResizeH?: boolean
  defaultGeom?: Geom
  minSize?: MinSize
  /** Monotonic "raise me" nonce. Bumping it brings the window to the front
   *  even when it's already visible (a fresh open is raised by the visibility
   *  watch). Menu "show" actions increment this. */
  focusToken?: number
}>(), {
  canResizeV: true,
  canResizeH: true,
  defaultGeom: () => ({ x: 25, y: 25, w: 50, h: 50 }),
  minSize: () => ({ w: 10, h: 8 }),
  focusToken: 0,
})

const emit = defineEmits<{
  'update:visible': [value: boolean]
}>()

defineExpose({ bringToFront, flashTitleBar })

/* ── refs ── */
const windowRef = ref<HTMLElement>()
const bodyRef = ref<HTMLElement>()

/* ── compact (phone) mode ──
 * One shared signal flips the whole shell to single-window. In compact mode
 * this window paints full-screen and is shown only while it's the focused
 * (front-most visible) window; the window manager arbitrates that across all
 * instances. Switching windows = bringing another to front (menu/switcher). */
const compact = useCompact()
const shown = computed(() =>
  props.visible && (!compact.value || focusedWindowId.value === props.id),
)

/* ── z-order ── */
const zIndex = ref(zCounter)
function bringToFront() { zIndex.value = ++zCounter; setWindowZ(props.id, zIndex.value) }

/* ── geometry ──
 * Windows are pure floating (desktop) / full-screen (phone). Docking was
 * removed, so the placement area is always the whole container. */
const pctX = ref(props.defaultGeom.x)
const pctY = ref(props.defaultGeom.y)
const pctW = ref(props.defaultGeom.w)
const pctH = ref(props.defaultGeom.h)

const windowStyle = computed(() => {
  /* Compact: full-bleed, geometry ignored. Keep the live z so the focused
   * window still paints above any sibling that's mid-transition. */
  if (compact.value) {
    return { left: '0%', top: '0%', width: '100%', height: '100%', zIndex: zIndex.value }
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
const STORAGE_KEY = `spangap.win.${props.id}`

interface StoredState {
  x: number; y: number; w: number; h: number
  visible: boolean
}

function loadState(): void {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (raw) {
      const s = JSON.parse(raw) as Partial<StoredState>
      if (typeof s.x === 'number') pctX.value = s.x
      if (typeof s.y === 'number') pctY.value = s.y
      if (typeof s.w === 'number') pctW.value = s.w
      if (typeof s.h === 'number') pctH.value = s.h
      if (typeof s.visible === 'boolean' && s.visible !== props.visible) {
        emit('update:visible', s.visible)
      }
    }
    /* Unknown legacy fields (dock/dockSize from the removed docking feature)
     * are simply ignored — no migration needed (per project convention). */
  } catch { /* corrupt JSON — ignore */ }
}

let saveTimer: ReturnType<typeof setTimeout> | null = null
function saveState(): void {
  const snapshot: StoredState = {
    x: Math.round(pctX.value * 10) / 10,
    y: Math.round(pctY.value * 10) / 10,
    w: Math.round(pctW.value * 10) / 10,
    h: Math.round(pctH.value * 10) / 10,
    visible: props.visible,
  }
  if (saveTimer) clearTimeout(saveTimer)
  saveTimer = setTimeout(() => {
    saveTimer = null
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(snapshot)) }
    catch { /* quota — ignore */ }
  }, 500)
}

/* Persist on visibility changes. */
watch(() => props.visible, saveState)

/* ── close ── */
function close() { emit('update:visible', false) }

/* ── clamp ── */
function clamp() {
  pctW.value = Math.min(100, Math.max(props.minSize.w, pctW.value))
  pctH.value = Math.min(100, Math.max(props.minSize.h, pctH.value))
  pctX.value = Math.min(100 - pctW.value, Math.max(0, pctX.value))
  pctY.value = Math.min(100 - pctH.value, Math.max(0, pctY.value))
}

/* ── container dimensions ── */
function containerSize(): { cw: number; ch: number } {
  const el = windowRef.value?.parentElement
  return { cw: el?.clientWidth ?? 1, ch: el?.clientHeight ?? 1 }
}
/* ── drag ── */
const MIN_DRAG_PX = 8
let dragStartX = 0, dragStartY = 0, dragOffX = 0, dragOffY = 0
let dragMoved = false

let dragPointerId = -1

function startDrag(e: PointerEvent) {
  bringToFront()
  /* Full-screen in compact mode: the titlebar is a header, not a drag handle. */
  if (compact.value) return
  /* Only react to primary pointer (left-click for mouse, single-finger
   * for touch). Multi-touch / right-click drag would mis-track. */
  if (!e.isPrimary) return
  dragStartX = e.clientX
  dragStartY = e.clientY
  dragOffX = pctX.value
  dragOffY = pctY.value
  dragMoved = false
  dragPointerId = e.pointerId
  ;(e.currentTarget as Element | null)?.setPointerCapture?.(e.pointerId)
  window.addEventListener('pointermove', onDrag)
  window.addEventListener('pointerup', endDrag)
  window.addEventListener('pointercancel', endDrag)
}
function onDrag(e: PointerEvent) {
  if (e.pointerId !== dragPointerId) return
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
function endDrag(e: PointerEvent) {
  if (e.pointerId !== dragPointerId) return
  dragPointerId = -1
  window.removeEventListener('pointermove', onDrag)
  window.removeEventListener('pointerup', endDrag)
  window.removeEventListener('pointercancel', endDrag)
  clamp()
  saveState()
}

/* ── resize ── */
type Edge = 'n' | 's' | 'e' | 'w' | 'ne' | 'nw' | 'se' | 'sw'
let resizeEdge: Edge = 's'
let resizeStartX = 0, resizeStartY = 0
let resizeStartW = 0, resizeStartH = 0
let resizeStartPX = 0, resizeStartPY = 0

let resizePointerId = -1

function startResize(edge: Edge, e: PointerEvent) {
  bringToFront()
  if (!e.isPrimary) return
  resizeEdge = edge
  resizeStartX = e.clientX
  resizeStartY = e.clientY
  resizeStartW = pctW.value
  resizeStartH = pctH.value
  resizeStartPX = pctX.value
  resizeStartPY = pctY.value
  resizePointerId = e.pointerId
  ;(e.currentTarget as Element | null)?.setPointerCapture?.(e.pointerId)
  window.addEventListener('pointermove', onResize)
  window.addEventListener('pointerup', endResize)
  window.addEventListener('pointercancel', endResize)
}
function onResize(e: PointerEvent) {
  if (e.pointerId !== resizePointerId) return
  const { cw, ch } = containerSize()
  const dx = (e.clientX - resizeStartX) / cw * 100
  const dy = (e.clientY - resizeStartY) / ch * 100

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
function endResize(e: PointerEvent) {
  if (e.pointerId !== resizePointerId) return
  resizePointerId = -1
  window.removeEventListener('pointermove', onResize)
  window.removeEventListener('pointerup', endResize)
  window.removeEventListener('pointercancel', endResize)
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
  registerWindow(props.id, props.title, zIndex.value)
  setWindowVisible(props.id, props.visible)
  loadState()
  clamp()

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
  unregisterWindow(props.id)
})

/* Keep the window manager's mirror of this window in sync. */
watch(() => props.title, (t) => setWindowTitle(props.id, t))

/* A menu "show" action bumps focusToken to raise an already-open window.
 * (Opening a hidden window is raised by the visibility watch below.) The
 * mobile switcher uses focusWindow() instead, which bumps this nonce. */
watch(() => props.focusToken, () => { bringToFront() })
watch(() => windowFocusReq(props.id), () => { if (props.visible) bringToFront() })

watch(() => props.visible, (vis) => {
  setWindowVisible(props.id, vis)
  if (vis) {
    bringToFront()
    clamp()
  }
})
</script>

<style scoped>
.fw {
  position: absolute;
  /* Two-stroke outline: 1px black ring directly around the window
   * content, then a 1px white ring outside that (via box-shadow so it
   * follows border-radius and doesn't affect layout). The black is
   * what gives the editor a visible edge against light backgrounds; the
   * white keeps the original visual identity against the dark page. */
  border: 1px solid #000;
  box-shadow: 0 0 0 1px #fff;
  border-radius: 6px;
  display: flex;
  flex-direction: column;
  background: #000000;
  /* `overflow: visible` lets resize handles extend past the window edge
   * so the outside-of-border hit zone is clickable. Inner clipping is
   * handled per-child via their own border-radius / overflow rules. */
}

/* Compact (phone): the window fills the whole usable area. No chrome that
 * implies it floats — square corners, no outline ring — and a taller,
 * touch-friendly header. Geometry comes from the inline full-screen style. */
.fw--compact {
  border: none;
  box-shadow: none;
  border-radius: 0;
}
.fw--compact .fw-titlebar {
  height: 44px;
  min-height: 44px;
  cursor: default;
  border-radius: 0;
  /* Drag is disabled in compact, so let normal touch behaviour through. */
  touch-action: auto;
}
.fw--compact .fw-titlebar:active { cursor: default; }
/* Bigger close hit-target for touch (the dot stays the visual size). */
.fw--compact .fw-close {
  width: 16px; height: 16px;
  margin: -10px; padding: 10px;
  background-clip: content-box;
}
.fw--compact .fw-title { font-size: 15px; }

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
  /* Prevent the browser from interpreting touch drags as scroll/pinch
   * so pointermove can drive the window on touch screens. */
  touch-action: none;
  /* Round the title bar's top corners to match the outer border, since
   * the outer .fw no longer clips. */
  border-top-left-radius: 5px;
  border-top-right-radius: 5px;
  /* Above the resize handles so the close button + drag still take
   * priority inside the title-bar area. */
  position: relative;
  z-index: 11;
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

.fw-body {
  flex: 1; overflow: hidden; padding: 0 5px;
  /* Match the outer rounded border now that .fw doesn't clip. */
  border-bottom-left-radius: 5px;
  border-bottom-right-radius: 5px;
}

/* Resize handles are invisible but generously sized so they're easy to
 * grab on touch and with a mouse. Each handle straddles the window border
 * — half outside, half inside — and the title bar (z-index: 11) wins over
 * the inside half within its area, so the close/drag area still works.
 * Bottom corners are larger because nothing competes for space there. */
.fw-resize { position: absolute; z-index: 10; touch-action: none; }
.fw-resize-n  { top: -10px;    left: 16px; right: 16px; height: 20px; cursor: n-resize; }
.fw-resize-s  { bottom: -10px; left: 16px; right: 16px; height: 20px; cursor: s-resize; }
.fw-resize-e  { right: -10px;  top: 16px;  bottom: 16px; width: 20px; cursor: e-resize; }
.fw-resize-w  { left: -10px;   top: 16px;  bottom: 16px; width: 20px; cursor: w-resize; }
.fw-resize-ne { top: -10px;    right: -10px; width: 24px; height: 24px; cursor: ne-resize; }
.fw-resize-nw { top: -10px;    left: -10px;  width: 24px; height: 24px; cursor: nw-resize; }
.fw-resize-se { bottom: -10px; right: -10px; width: 28px; height: 28px; cursor: se-resize; }
.fw-resize-sw { bottom: -10px; left: -10px;  width: 28px; height: 28px; cursor: sw-resize; }
</style>
