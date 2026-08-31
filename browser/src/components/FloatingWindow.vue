<template>
  <div
    v-show="shown"
    ref="windowRef"
    class="fw"
    :class="{ 'fw--compact': compact, 'fw--autoheight': autoHeight && !compact }"
    :style="windowStyle"
    @mousedown.capture="onRootMouseDown"
    @click.capture="onRootClickCapture"
  >
    <!-- Resize handles — suppressed in compact mode: the window is full-screen
         there and neither draggable nor resizable. -->
    <template v-if="(vResize || canResizeH) && !compact">
      <div v-if="vResize" class="fw-resize fw-resize-n" @pointerdown.prevent="startResize('n', $event)" />
      <div v-if="vResize" class="fw-resize fw-resize-s" @pointerdown.prevent="startResize('s', $event)" />
      <div v-if="canResizeH" class="fw-resize fw-resize-e" @pointerdown.prevent="startResize('e', $event)" />
      <div v-if="canResizeH" class="fw-resize fw-resize-w" @pointerdown.prevent="startResize('w', $event)" />
      <div v-if="vResize && canResizeH" class="fw-resize fw-resize-ne" @pointerdown.prevent="startResize('ne', $event)" />
      <div v-if="vResize && canResizeH" class="fw-resize fw-resize-nw" @pointerdown.prevent="startResize('nw', $event)" />
      <div v-if="vResize && canResizeH" class="fw-resize fw-resize-se" @pointerdown.prevent="startResize('se', $event)" />
      <div v-if="vResize && canResizeH" class="fw-resize fw-resize-sw" @pointerdown.prevent="startResize('sw', $event)" />
    </template>

    <!-- Titlebar — suppressed in chromeless mode; the close control moves into
         the body as a floating dot and the window is pinned always-on-top. -->
    <div
      v-if="!chromeless"
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
    <div
      ref="bodyRef"
      class="fw-body"
      :class="{ 'fw-body--chromeless': chromeless, 'fw-body--flush': flush }"
      @pointerdown="onBodyPointerDown"
    >
      <div v-if="chromeless" class="fw-close fw-close-float" @pointerdown.stop @click="close" />
      <slot :size="bodySize" />
    </div>
  </div>
</template>

<script lang="ts">
/* Shared z-counter across all FloatingWindow instances. Every raise takes the
 * next value off it, so a window's counter value IS its place in the stack —
 * which is why that value (not the composed z-index) is what persists. */
let zCounter = 1000
/* Which window currently holds the top of the counter. Pressing the front-most
 * window again must not mint a rank: the order is already right, and burning a
 * rank per click would march the counter toward the reserved band and rewrite
 * the persisted stack for no visible effect. */
let zTopId = ''
/* Chromeless windows are pinned above every normal window. They still stack
 * among themselves off this same counter, just lifted into a reserved band. */
const ON_TOP_Z = 2_000_000
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
  /** Height follows the body's content instead of a fixed % (and vertical
   *  resize is disabled). Width/position stay user-controllable. For panels
   *  whose natural height is intrinsic (e.g. fixed-height stacked graphs). */
  autoHeight?: boolean
  defaultGeom?: Geom
  minSize?: MinSize
  /** Monotonic "raise me" nonce. Bumping it brings the window to the front
   *  even when it's already visible (a fresh open is raised by the visibility
   *  watch). Menu "show" actions increment this. */
  focusToken?: number
  /** Optional body aspect ratio (width / height). When > 0, resizing keeps the
   *  body proportional to it (e.g. a device screen mirror). 0 = free resize. */
  aspect?: number
  /** Strip the titlebar: the close control becomes a floating dot in the
   *  top-left of the body and the window is pinned above all normal windows.
   *  For panels that collapse to a chromeless tile when space is tight. */
  chromeless?: boolean
  /** Remove the body's inner padding so content sits edge-to-edge. For panels
   *  that manage their own insets (e.g. full-bleed graphs). */
  flush?: boolean
}>(), {
  canResizeV: true,
  canResizeH: true,
  autoHeight: false,
  defaultGeom: () => ({ x: 25, y: 25, w: 50, h: 50 }),
  minSize: () => ({ w: 10, h: 8 }),
  focusToken: 0,
  aspect: 0,
  chromeless: false,
  flush: false,
})

const emit = defineEmits<{
  'update:visible': [value: boolean]
}>()

defineExpose({ bringToFront, flashTitleBar, fitBodyPx })

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

/* Vertical resize is meaningless when the height tracks content. */
const vResize = computed(() => props.canResizeV && !props.autoHeight)

/* ── z-order ──
 * The rank is the raw counter value and is what the stored record carries; the
 * chromeless on-top band is added at paint time instead of being baked in,
 * because `chromeless` is a live prop a panel flips as its layout collapses —
 * a stored number with the band already in it would bring the window back into
 * the wrong band whenever it was restored in the other mode. */
const zRank = ref(zCounter)
const zIndex = computed(() => (props.chromeless ? ON_TOP_Z : 0) + zRank.value)

function bringToFront() {
  if (zTopId !== props.id) {
    zTopId = props.id
    zRank.value = ++zCounter
    saveState()               /* the new order is part of the persisted layout */
  }
  /* Re-report even when the rank stood still: a chromeless flip changes the
   * composed z without touching the rank, and the manager mirrors composed z. */
  setWindowZ(props.id, zIndex.value)
}
/* The band follows the prop on its own; the raise is what a flip adds — a panel
 * that collapses to its chromeless tile, or expands back, is the window the user
 * is working in, so it comes forward within whichever band it lands in. */
watch(() => props.chromeless, () => bringToFront())

/* ── click-to-focus swallowing ──
 * A plain click on a background (non-front) window only raises and focuses it —
 * the click is swallowed so it does NOT also actuate the content under the
 * pointer (a button, a link, a message action). The window still takes keyboard
 * focus: the mousedown is left to reach the terminal / input beneath, which
 * focuses it, so the CLI accepts typing immediately. A drag that selects text is
 * not a click — if the pointer moved or a text selection is present, the
 * interaction passes through untouched. Only the raising click (window not
 * front-most at press time) is ever swallowed; interacting with the already-
 * focused window behaves normally. */
const CLICK_SLOP = 4
let raiseArmed = false
let downX = 0, downY = 0
function onRootMouseDown(e: MouseEvent) {
  raiseArmed = focusedWindowId.value !== props.id
  downX = e.clientX
  downY = e.clientY
  bringToFront()
}
function onRootClickCapture(e: MouseEvent) {
  if (!raiseArmed) return
  raiseArmed = false
  const moved = Math.abs(e.clientX - downX) > CLICK_SLOP ||
                Math.abs(e.clientY - downY) > CLICK_SLOP
  const sel = window.getSelection()
  if (moved || (sel && !sel.isCollapsed)) return   /* a select/drag — leave it */
  e.stopPropagation()
  e.preventDefault()
}

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
  if (props.autoHeight) {
    return {
      left: `${pctX.value}%`, top: `${pctY.value}%`,
      width: `${pctW.value}%`, height: 'auto', maxHeight: '92%',
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

/* ── persistence ──
 * The stored record is the window's geometry, its place in the stack, and the
 * user's intent to have it open, and it is the whole of what a page load
 * restores — one key per window, so nothing has to agree with anything else
 * and a window that is never mounted simply never takes part. `visible`
 * only ever moves for a reason the user would recognise (the close dot, a dock
 * launch, an app deliberately dismissing its own window) — a window whose
 * content depends on a live link must ride out a link drop rather than lower
 * `visible`, or the drop would rewrite the layout that comes back next load. */
const STORAGE_KEY = `spangap.win.${props.id}`
/* True once a saved geometry has been restored — fitBodyPx respects a user's
 * chosen size and only auto-sizes a window that has never been placed. */
let hadStored = false

/* Armed while the restore's own `update:visible` is still travelling out to the
 * owning ref and back in as a prop change. Coming back is not opening: raising
 * on that echo would renumber every restored window in mount order and throw
 * away the stack the reload exists to bring back. */
let restoringVisible = false

interface StoredState {
  x: number; y: number; w: number; h: number
  /** Raise-counter rank — see the z-order section. */
  z: number
  visible: boolean
}

function loadState(): void {
  try {
    const raw = localStorage.getItem(STORAGE_KEY)
    if (raw) {
      hadStored = true
      const s = JSON.parse(raw) as Partial<StoredState>
      if (typeof s.x === 'number') pctX.value = s.x
      if (typeof s.y === 'number') pctY.value = s.y
      if (typeof s.w === 'number') pctW.value = s.w
      if (typeof s.h === 'number') pctH.value = s.h
      /* Stacking. Only the ordering between ranks matters, so windows restore
       * independently: one that has never been opened has no rank and keeps the
       * default, and a key belonging to a window that no longer exists is never
       * read by anyone. A rank at or above the reserved band is not one this
       * code minted — taking it would pin the window above every chromeless
       * panel for good, so leave such a record's stacking at the default. */
      if (typeof s.z === 'number' && s.z > 0 && s.z < ON_TOP_Z) {
        zRank.value = s.z
        /* Keep the counter ahead of every restored rank, so the first genuine
         * raise still lands in front of the whole restored stack. */
        if (s.z >= zCounter) { zCounter = s.z; zTopId = props.id }
        setWindowZ(props.id, zIndex.value)
      }
      if (typeof s.visible === 'boolean' && s.visible !== props.visible) {
        emit('update:visible', s.visible)
        restoringVisible = s.visible
      }
    }
    /* Unknown legacy fields (dock/dockSize from the removed docking feature)
     * are simply ignored — no migration needed (per project convention). */
  } catch { /* corrupt JSON — ignore */ }
}

let saveTimer: ReturnType<typeof setTimeout> | null = null
let pendingSave: StoredState | null = null

function writeState(s: StoredState): void {
  try { localStorage.setItem(STORAGE_KEY, JSON.stringify(s)) }
  catch { /* quota — ignore */ }
}

function saveState(): void {
  pendingSave = {
    x: Math.round(pctX.value * 10) / 10,
    y: Math.round(pctY.value * 10) / 10,
    w: Math.round(pctW.value * 10) / 10,
    h: Math.round(pctH.value * 10) / 10,
    z: zRank.value,
    visible: props.visible,
  }
  if (saveTimer) clearTimeout(saveTimer)
  saveTimer = setTimeout(flushState, 500)
}

/* Write out a debounced save immediately. The debounce coalesces a drag or a
 * resize into one write; a reload landing inside that window would otherwise
 * restore the state from before the move, so the page-hide path flushes. */
function flushState(): void {
  if (saveTimer) { clearTimeout(saveTimer); saveTimer = null }
  const s = pendingSave
  pendingSave = null
  if (s) writeState(s)
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

/* ── aspect lock (opt-in via props.aspect) ──
 * Geometry is in % of the container; the body is the window minus its chrome
 * (titlebar/borders). chromePctH() is that chrome height as a % of the container,
 * so the aspect math constrains the BODY, not the whole window. */
function chromePctH(): number {
  const el = windowRef.value, b = bodyRef.value
  if (!el || !b) return 0
  const { ch } = containerSize()
  return Math.max(0, el.clientHeight - b.clientHeight) / ch * 100
}

/* Keep the body proportional to props.aspect. `horizontal` = the drag changed
 * width (derive height); otherwise the drag changed height (derive width). */
function applyAspect(horizontal: boolean): void {
  if (!props.aspect) return
  const { cw, ch } = containerSize()
  const cph = chromePctH()
  if (horizontal) {
    const bottom = pctY.value + pctH.value
    pctH.value = pctW.value * cw / (ch * props.aspect) + cph
    if (resizeEdge.includes('n')) pctY.value = bottom - pctH.value   /* keep bottom anchored */
  } else {
    pctW.value = (pctH.value - cph) * ch * props.aspect / cw
  }
}

/* Size the window so its BODY is ~w×h device pixels (the screen's native size),
 * proportional to props.aspect. No-op once the user has placed the window. */
function fitBodyPx(w: number, h: number): void {
  if (hadStored) return
  const { cw, ch } = containerSize()
  pctW.value = Math.min(90, Math.max(props.minSize.w, w / cw * 100))
  applyAspect(true)                 /* derive height from width + chrome */
  if (pctH.value > 90) { pctH.value = 90; applyAspect(false) }
  /* Centre the freshly-sized window. */
  pctX.value = Math.max(0, (100 - pctW.value) / 2)
  pctY.value = Math.max(0, (100 - pctH.value) / 2)
  clamp()
  saveState()
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

/* Chromeless has no titlebar, so the body itself is the drag handle — a press
 * anywhere on it moves the window, except where a child claims the pointer
 * (the close dot, or interactive slot content that stops propagation). */
function onBodyPointerDown(e: PointerEvent) {
  if (!props.chromeless) return
  e.preventDefault()
  startDrag(e)
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

  /* Each edge anchors the opposite edge and clamps the moving edge to the
   * container. When the moving edge hits the boundary the window stops growing
   * — it does NOT push the anchored edge past where the drag started. */
  const minW = props.minSize.w, minH = props.minSize.h
  if (resizeEdge.includes('e')) {                     /* left anchored, right → pointer */
    pctW.value = Math.min(100 - resizeStartPX, Math.max(minW, resizeStartW + dx))
  }
  if (resizeEdge.includes('w')) {                     /* right anchored, left → pointer */
    const right = resizeStartPX + resizeStartW
    const x = Math.max(0, Math.min(right - minW, resizeStartPX + dx))
    pctX.value = x
    pctW.value = right - x
  }
  if (resizeEdge.includes('s')) {                     /* top anchored, bottom → pointer */
    pctH.value = Math.min(100 - resizeStartPY, Math.max(minH, resizeStartH + dy))
  }
  if (resizeEdge.includes('n')) {                     /* bottom anchored, top → pointer */
    const bottom = resizeStartPY + resizeStartH
    const y = Math.max(0, Math.min(bottom - minH, resizeStartPY + dy))
    pctY.value = y
    pctH.value = bottom - y
  }
  /* Aspect lock: a horizontal-edge drag drives width→height, a vertical-edge
   * drag drives height→width, so the mirror stays proportional either way. */
  applyAspect(resizeEdge.includes('e') || resizeEdge.includes('w'))
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
  /* Watchers queued by the restore have all run by the time a nextTick callback
   * fires, so the echo is over: any later open is a real one and must raise. */
  nextTick(() => { restoringVisible = false })

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

  /* `pagehide` rather than `beforeunload`: it fires on every way a document
   * stops being shown, including the mobile paths that skip beforeunload. */
  window.addEventListener('pagehide', flushState)
})

onUnmounted(() => {
  window.removeEventListener('pagehide', flushState)
  resizeObserver?.disconnect()
  resizeObserver = null
  /* A window being unmounted is being taken off screen by its owner, which
   * may already have dropped this record (a per-instance window drops its
   * orphaned geometry on close). Discard the debounced write rather than
   * resurrect the key. */
  if (saveTimer) { clearTimeout(saveTimer); saveTimer = null }
  pendingSave = null
  if (flashTimer) { clearTimeout(flashTimer); flashTimer = null }
  /* Release the top-of-counter claim: an id that comes back (a per-instance
   * window reopened) restores whatever rank it had, and a stale claim would
   * make the first click on it a no-op instead of a raise. */
  if (zTopId === props.id) zTopId = ''
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
  if (!vis) return
  clamp()
  /* Opening a window puts it in front — except when this is the restore's own
   * echo, where the window is being put back exactly where the user left it,
   * stacking included. */
  if (restoringVisible) { restoringVisible = false; return }
  bringToFront()
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

/* Chromeless: the close dot floats over the top-left of the body (the first
 * graph). A subtle ring keeps it legible against any graph colour. Shown only
 * while the window is hovered, so nothing overlays the content otherwise. */
.fw-close-float {
  position: absolute; top: 6px; left: 6px; z-index: 12;
  box-shadow: 0 0 0 1px rgba(0,0,0,0.6);
  opacity: 0;
  transition: opacity 0.12s ease;
  pointer-events: none;
}
.fw:hover .fw-close-float {
  opacity: 1;
  pointer-events: auto;
}

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
/* With no titlebar the body is the whole window — round its top corners too,
 * drop the padding so content goes edge-to-edge, and make it the drag surface
 * (grab cursor; touch drags move, not scroll). */
.fw-body--chromeless {
  position: relative;
  padding: 0;
  border-top-left-radius: 5px;
  border-top-right-radius: 5px;
  cursor: grab;
  touch-action: none;
}
.fw-body--chromeless:active { cursor: grabbing; }
/* Content-height mode: basis auto (not the flex:1 basis:0 that collapses in an
   auto-height column); scroll only if the body exceeds the window's max-height. */
.fw--autoheight .fw-body { flex: 1 1 auto; overflow: auto; }

/* Edge-to-edge body: panel manages its own insets. */
.fw-body--flush { padding: 0; }

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
