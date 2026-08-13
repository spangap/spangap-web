<template>
  <FloatingWindow
    id="actmon"
    :title="title"
    :visible="visible"
    :focus-token="focusToken"
    :default-geom="defaultGeom"
    :min-size="{ w: 24, h: 16 }"
    :chromeless="collapsed"
    flush
    @update:visible="v => emit('update:visible', v)"
  >
    <template #default>
      <div ref="bodyEl" class="actmon-body" :class="{ 'actmon-body--collapsed': collapsed }">
        <div class="actmon-graph g-c0">
          <canvas ref="c0Ref" class="actmon-canvas" />
          <div class="actmon-captions"><div class="actmon-caption">core 0</div></div>
        </div>
        <div class="actmon-graph g-c1">
          <canvas ref="c1Ref" class="actmon-canvas" />
          <div class="actmon-captions"><div class="actmon-caption">core 1</div></div>
        </div>
        <div ref="pwrGraphRef" class="actmon-graph g-pwr">
          <canvas ref="c2Ref" class="actmon-canvas" />
          <div v-if="pwrDragging || pwrHover" class="actmon-tooltip" :style="pwrPos">{{ pwrTip }}</div>
          <div class="actmon-est actmon-est--drag" :style="pwrPos"
               @pointerdown="onPwrDown" @mouseenter="pwrHover = true" @mouseleave="pwrHover = false">{{ maText }}</div>
          <div class="actmon-captions">
            <div class="actmon-caption">power mgmt: <span class="c-red">CPU_MAX</span>, <span class="c-orange">APB_MAX</span>, <span class="c-yellow">APB_MIN</span>. No bar: SLEEP</div>
          </div>
        </div>
        <div class="actmon-graph g-pkt">
          <canvas ref="pktRef" class="actmon-canvas" />
          <div v-if="pktPeak.show" ref="pktPeakRef" class="actmon-peak">{{ pktPeak.text }}</div>
        </div>
        <div ref="netGraphRef" class="actmon-graph g-traf">
          <canvas ref="trafRef" class="actmon-canvas" />
          <div v-if="trafPeak.show" ref="trafPeakRef" class="actmon-peak">{{ trafPeak.text }}</div>
          <div v-if="netDragging || netHover" class="actmon-tooltip" :style="netPos">{{ netTip }}</div>
          <div class="actmon-est actmon-est--drag" :style="netPos"
               @pointerdown="onNetDown" @mouseenter="netHover = true" @mouseleave="netHover = false">{{ wifiMaText }}</div>
          <div class="actmon-captions">
            <div class="actmon-caption"><span class="c-in">IN</span> / <span class="c-out">OUT</span></div>
          </div>
        </div>
      </div>
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted, nextTick, type Ref } from 'vue'
import FloatingWindow from './FloatingWindow.vue'
import { useDeviceStore } from '../stores/device'
import { useChromelessFit } from '../lib/chromelessFit'
import { getSession } from '../lib/webrtc-session'

const props = defineProps<{
  visible: boolean
  title: string
  focusToken?: number
}>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const device = useDeviceStore()

const isPhoneInit = window.matchMedia?.('(max-width: 599px)').matches ?? false
const defaultGeom = isPhoneInit
  ? { x: 0, y: 0, w: 100, h: 82 }
  : { x: 20, y: 6, w: 55, h: 84 }

const STEPS = 320
const GH = 100

const C_WHITE = '#FFFFFF'
const C_RED = '#E05050', C_ORANGE = '#F08820', C_YELLOW = '#E8D040'
const C_BLACK = '#000000'
const C_BLUE = '#4088E8'          // traffic out
const C_IN = '#E8D040'            // traffic in (yellow)
const C_MIX = '#46C05A'           // in + out overlap (green)

/* Both graph buffers start empty at app open and fill live, one sample a second,
 * from the mirrored sys.stats.* keys (see tick). */
interface Sample { core0: number; core1: number; sleep: number; apbMax: number; cpuMax: number }
const samples: Sample[] = []

interface NetSample { bIn: number; bOut: number; pIn: number; pOut: number }
const netSamples: NetSample[] = []

const c0Ref = ref<HTMLCanvasElement>()
const c1Ref = ref<HTMLCanvasElement>()
const c2Ref = ref<HTMLCanvasElement>()
const trafRef = ref<HTMLCanvasElement>()
const pktRef = ref<HTMLCanvasElement>()
const trafPeakRef = ref<HTMLElement>()
const pktPeakRef = ref<HTMLElement>()

const maText = ref('')            // CPU/PM current estimate (selected window)
const wifiMaText = ref('')        // Wi-Fi current estimate (selected window)
const pwrTip = ref('')            // "3 min avg 14% CPU_MAX, …"
const netTip = ref('')            // "3 min avg ~0.3% tx, ~1.1% rx"

interface Peak { show: boolean; text: string; frac: number }
const trafPeak = ref<Peak>({ show: false, text: '', frac: 0 })
const pktPeak = ref<Peak>({ show: false, text: '', frac: 0 })

/* ── draggable averaging window (shared by both pills) ── */
const AVG_WINDOWS = [300, 240, 180, 120, 60, 30]
function windowFromFrac(f: number): number {
  let best = AVG_WINDOWS[0], bd = Infinity
  for (const w of AVG_WINDOWS) {
    const d = Math.abs((1 - w / 300) - f)
    if (d < bd) { bd = d; best = w }
  }
  return best
}
function windowLabel(secs: number): string {
  return secs < 60 ? `${secs} sec` : `${secs / 60} min`
}
function usePillDrag(win: Ref<number>, graph: Ref<HTMLElement | undefined>, dragging: Ref<boolean>) {
  const pos = computed(() => {
    const f = 1 - win.value / 300
    return { left: `${f * 100}%`, transform: `translateX(${-f * 100}%)` }
  })
  function onMove(e: PointerEvent) {
    const g = graph.value
    if (!g) return
    const r = g.getBoundingClientRect()
    win.value = windowFromFrac(Math.max(0, Math.min(0.9, (e.clientX - r.left) / r.width)))
  }
  function onUp() {
    dragging.value = false
    window.removeEventListener('pointermove', onMove)
    window.removeEventListener('pointerup', onUp)
  }
  function onDown(e: PointerEvent) {
    e.stopPropagation(); e.preventDefault()      // don't start a window drag / text select
    dragging.value = true
    window.addEventListener('pointermove', onMove)
    window.addEventListener('pointerup', onUp)
  }
  return { pos, onDown }
}

const pwrWindow = ref(300), pwrDragging = ref(false), pwrHover = ref(false)
const pwrGraphRef = ref<HTMLElement>()
const { pos: pwrPos, onDown: onPwrDown } = usePillDrag(pwrWindow, pwrGraphRef, pwrDragging)

const netWindow = ref(300), netDragging = ref(false), netHover = ref(false)
const netGraphRef = ref<HTMLElement>()
const { pos: netPos, onDown: onNetDown } = usePillDrag(netWindow, netGraphRef, netDragging)

/* ── format helpers ── */
function formatRate(bytesPerSec: number): string {
  const bps = bytesPerSec * 8
  if (bps >= 1e6) return `${(bps / 1e6).toFixed(1)} Mbps`
  if (bps >= 1e3) return `${Math.round(bps / 1e3)} kbps`
  return `${Math.round(bps)} bps`
}
function formatPkts(pps: number): string {
  if (pps >= 1e4) return `${Math.round(pps / 1e3)}k pps`
  if (pps >= 1e3) return `${(pps / 1e3).toFixed(1)}k pps`
  return `${Math.round(pps)} pps`
}
function formatMa(ma10: number): string {
  const ma = ma10 / 10
  return `~${ma < 10 ? ma.toFixed(1) : String(Math.round(ma))} mA`
}

function ctxOf(cv: HTMLCanvasElement | undefined): { ctx: CanvasRenderingContext2D; w: number } | null {
  if (!cv) return null
  if (cv.width !== STEPS) cv.width = STEPS
  if (cv.height !== GH) cv.height = GH
  const ctx = cv.getContext('2d')
  if (!ctx) return null
  return { ctx, w: STEPS }
}

/* Four subtle greyscale quarter-bands with a sawtooth ramp — replaces gridlines. */
function drawBands(ctx: CanvasRenderingContext2D, w: number) {
  const q = GH / 4
  for (let i = 0; i < 4; i++) {
    const yTop = GH - (i + 1) * q
    const g = ctx.createLinearGradient(0, yTop + q, 0, yTop)
    g.addColorStop(0, '#242424')
    g.addColorStop(1, '#313131')
    ctx.fillStyle = g
    ctx.fillRect(0, yTop, w, q)
  }
}

function drawCore(cv: HTMLCanvasElement | undefined, pick: (s: Sample) => number) {
  const c = ctxOf(cv)
  if (!c) return
  const { ctx, w } = c
  drawBands(ctx, w)
  const step = w / STEPS
  const n = samples.length
  ctx.fillStyle = C_WHITE
  for (let i = 0; i < n; i++) {
    const p = pick(samples[i])
    if (p <= 0) continue
    let h = (p / 100) * GH
    if (h < 1) h = 1
    ctx.fillRect((STEPS - n + i) * step, GH - h, Math.ceil(step), h)
  }
}

function drawState(cv: HTMLCanvasElement | undefined) {
  const c = ctxOf(cv)
  if (!c) return
  const { ctx, w } = c
  drawBands(ctx, w)
  const step = w / STEPS
  const n = samples.length
  for (let i = 0; i < n; i++) {
    const s = samples[i]
    let apbMin = 100 - s.sleep - s.apbMax - s.cpuMax
    if (apbMin < 0) apbMin = 0
    const x = (STEPS - n + i) * step, bw = Math.ceil(step)
    const segs: Array<[string, number]> = [[C_RED, s.cpuMax], [C_ORANGE, s.apbMax], [C_YELLOW, apbMin]]
    let acc = 0
    for (const [col, val] of segs) {
      if (val > 0) {
        const yTop = GH - ((acc + val) / 100) * GH
        const yBot = GH - (acc / 100) * GH
        ctx.fillStyle = col
        ctx.fillRect(x, yTop, bw, yBot - yTop)
      }
      acc += val
    }
  }
}

/* in (blue) over out (yellow), green overlap; auto-scaled to the window peak. */
function drawTraffic(cv: HTMLCanvasElement | undefined, peak: typeof trafPeak,
                     pickOut: (s: NetSample) => number, pickIn: (s: NetSample) => number,
                     fmt: (v: number) => string) {
  const c = ctxOf(cv)
  if (!c) return
  const { ctx, w } = c
  drawBands(ctx, w)
  const n = netSamples.length
  let max = 0, peakIdx = -1, peakVal = 0
  for (let i = 0; i < n; i++) {
    const v = Math.max(pickOut(netSamples[i]), pickIn(netSamples[i]))
    if (v > max) { max = v; peakIdx = i; peakVal = v }
  }
  if (max <= 0) { peak.value = { show: false, text: '', frac: 0 }; return }
  const step = w / STEPS
  for (let i = 0; i < n; i++) {
    const out = pickOut(netSamples[i]) / max * GH
    const inn = pickIn(netSamples[i]) / max * GH
    if (out <= 0 && inn <= 0) continue
    const x = (STEPS - n + i) * step, bw = Math.ceil(step)
    const lo = Math.min(out, inn), hi = Math.max(out, inn)
    if (lo > 0) { ctx.fillStyle = C_MIX; ctx.fillRect(x, GH - lo, bw, lo) }
    if (hi > lo) { ctx.fillStyle = out >= inn ? C_YELLOW : C_BLUE; ctx.fillRect(x, GH - hi, bw, hi - lo) }
  }
  const frac = peakIdx >= 0 ? (STEPS - n + peakIdx) / STEPS : 1
  peak.value = { show: true, text: fmt(peakVal), frac }
}

/* Place a peak label just to the right of its column; flip to the left only when
 * it wouldn't fit on the right (measured, so it hugs the peak either way). */
function placePeak(el: HTMLElement | undefined, frac: number) {
  if (!el || !el.parentElement) return
  const gw = el.parentElement.clientWidth
  const peakX = frac * gw
  const gap = 3
  if (peakX + gap + el.offsetWidth <= gw) { el.style.left = `${peakX + gap}px`; el.style.right = 'auto' }
  else { el.style.right = `${Math.max(0, gw - peakX + gap)}px`; el.style.left = 'auto' }
}
function placePeaks() {
  if (trafPeak.value.show) placePeak(trafPeakRef.value, trafPeak.value.frac)
  if (pktPeak.value.show) placePeak(pktPeakRef.value, pktPeak.value.frac)
}

/* ── device liveness (frozen ts → solid black, scale hidden) ── */
const inactive = ref(false)
let lastTs = NaN
let staleTicks = 0
const STALE_LIMIT = 3

function drawInactive(cv: HTMLCanvasElement | undefined) {
  const c = ctxOf(cv)
  if (!c) return
  c.ctx.fillStyle = C_BLACK
  c.ctx.fillRect(0, 0, c.w, GH)
}

function draw() {
  if (inactive.value) {
    drawInactive(c0Ref.value); drawInactive(c1Ref.value); drawInactive(c2Ref.value)
    drawInactive(pktRef.value); drawInactive(trafRef.value)
    trafPeak.value = { show: false, text: '', frac: 0 }
    pktPeak.value = { show: false, text: '', frac: 0 }
    return
  }
  drawCore(c0Ref.value, s => s.core0)
  drawCore(c1Ref.value, s => s.core1)
  drawState(c2Ref.value)
  drawTraffic(pktRef.value, pktPeak, s => s.pOut, s => s.pIn, formatPkts)
  drawTraffic(trafRef.value, trafPeak, s => s.bOut, s => s.bIn, formatRate)
}
function redraw() { draw(); nextTick(placePeaks) }

function updateAvg() {
  const pw = pwrWindow.value
  const gp = (k: string) => Number(device.get(`sys.stats.avg.w${pw}.${k}`) ?? 0)
  maText.value = formatMa(gp('ma_x10'))
  pwrTip.value = `${windowLabel(pw)} avg ${gp('cpu_max')}% CPU_MAX, ${gp('apb_max')}% APB_MAX, ${gp('apb_min')}% APB_MIN`

  const nw = netWindow.value
  const gn = (k: string) => Number(device.get(`sys.stats.avg.net.w${nw}.${k}`) ?? 0)
  wifiMaText.value = formatMa(gn('ma_x10'))
  netTip.value = `${windowLabel(nw)} avg ~${(gn('tx_x10') / 10).toFixed(1)}% tx, ~${(gn('rx_x10') / 10).toFixed(1)}% rx`
}

function tick() {
  if (props.visible) device.set('sys.stats.web_actmon', 1)   /* heartbeat while open */

  const g = (k: string) => Number(device.get('sys.stats.' + k) ?? 0)
  const ts = g('ts')
  const fresh = ts !== lastTs
  if (fresh) { staleTicks = 0; lastTs = ts }
  else staleTicks++
  inactive.value = staleTicks >= STALE_LIMIT

  /* Only append on a fresh heartbeat — a frozen device must not pollute the
   * graph buffers with repeated last-values (the graphs go black while frozen). */
  if (fresh) {
    samples.push({
      core0: g('cpu_pct.0'), core1: g('cpu_pct.1'),
      sleep: g('SLEEP'), apbMax: g('APB_MAX'), cpuMax: g('CPU_MAX'),
    })
    if (samples.length > STEPS) samples.shift()

    netSamples.push({
      bIn: g('net.bytes_in'), bOut: g('net.bytes_out'),
      pIn: g('net.pkts_in'), pOut: g('net.pkts_out'),
    })
    if (netSamples.length > STEPS) netSamples.shift()
  }

  updateAvg()
  if (props.visible) redraw()
}

/* ── responsive collapse (measures the caption blocks) ── */
const bodyEl = ref<HTMLElement>()
const GAPS = [0.05, 0.05, 0.14, 0.08]

function captionsFit(availH: number): boolean {
  const body = bodyEl.value
  if (!body) return true
  const blocks = Array.from(body.querySelectorAll<HTMLElement>('.actmon-captions'))
  if (!blocks.length) return true
  for (let i = 0; i < blocks.length; i++) {
    const gap = GAPS[i] ?? GAPS[GAPS.length - 1]
    if (blocks[i].offsetHeight > gap * availH) return false
    for (const ln of blocks[i].querySelectorAll<HTMLElement>('.actmon-caption'))
      if (ln.scrollWidth > ln.clientWidth + 1) return false
  }
  return true
}

const { collapsed, evaluate } = useChromelessFit({ el: bodyEl, fits: captionsFit })

let timer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  timer = setInterval(tick, 1000)
  getSession().connect()
})
onUnmounted(() => {
  if (timer) { clearInterval(timer); timer = null }
  device.set('sys.stats.web_actmon', 0)
})

watch(() => props.visible, v => {
  device.set('sys.stats.web_actmon', v ? 1 : 0)
  if (v) { lastTs = NaN; staleTicks = 0; inactive.value = false; nextTick(() => { redraw(); evaluate() }) }
})

/* Recompute pill text + tooltip immediately when either window is dragged. */
watch([pwrWindow, netWindow], updateAvg)
</script>

<style scoped>
.actmon-body {
  position: relative;
  width: 100%;
  height: 100%;
  background: #000;
}

/* Five stacked graphs — core0/core1 are half the power graph's height. Order:
 * CPU, then packets, then traffic (bottom). */
.actmon-graph { position: absolute; left: 0; right: 0; }
.g-c0   { top: 0;   height: 8%; }
.g-c1   { top: 13%; height: 8%; }
.g-pwr  { top: 26%; height: 13%; }
.g-pkt  { top: 53%; height: 17%; }
.g-traf { top: 75%; height: 17%; }

.actmon-canvas { display: block; width: 100%; height: 100%; image-rendering: pixelated; }

/* Captions: one clipped nowrap line each; the block stacks its lines below the graph. */
.actmon-captions { position: absolute; top: 100%; left: 0; right: 0; }
.actmon-caption {
  padding: 2px 0 0 6px;
  font: 11px/1.2 'SF Mono', 'Menlo', 'Consolas', monospace;
  color: #c8c8c8;
  white-space: nowrap;
  overflow: hidden;
}
.actmon-caption .c-red { color: #E05050; }
.actmon-caption .c-orange { color: #F08820; }
.actmon-caption .c-yellow { color: #E8D040; }
.actmon-caption .c-in { color: #4088E8; }
.actmon-caption .c-out { color: #E8D040; }

/* Estimate pill — bottom-left over its own graph; draggable to pick the window. */
.actmon-est {
  position: absolute;
  bottom: 3px;
  padding: 0 4px;
  border-radius: 3px;
  background: rgba(0, 0, 0, 0.5);
  font: 10px/1.4 'SF Mono', 'Menlo', 'Consolas', monospace;
  color: #e0e0e0;
  white-space: nowrap;
}
.actmon-est--drag { cursor: ew-resize; touch-action: none; }

/* Tooltip above a pill — the averaged breakdown, seen while dragging. */
.actmon-tooltip {
  position: absolute;
  bottom: 22px;
  padding: 2px 6px;
  border-radius: 4px;
  background: rgba(20, 20, 20, 0.92);
  box-shadow: 0 0 0 1px rgba(255, 255, 255, 0.15);
  font: 10px/1.3 'SF Mono', 'Menlo', 'Consolas', monospace;
  color: #e8e8e8;
  white-space: nowrap;
  pointer-events: none;
  z-index: 5;
}

/* Floating peak label on the wifi graphs (positioned imperatively). */
.actmon-peak {
  position: absolute;
  top: 3px;
  left: 0;
  padding: 0 4px;
  border-radius: 3px;
  background: rgba(0, 0, 0, 0.5);
  font: 10px/1.4 'SF Mono', 'Menlo', 'Consolas', monospace;
  color: #e8e8e8;
  white-space: nowrap;
  pointer-events: none;
}

/* Collapsed: drop the caption text; graphs fill the height; pills stay. */
.actmon-body--collapsed .actmon-captions { visibility: hidden; }
.actmon-body--collapsed .g-c0   { top: 0;   height: 18%; }
.actmon-body--collapsed .g-c1   { top: 20%; height: 18%; }
.actmon-body--collapsed .g-pwr  { top: 40%; height: 18%; }
.actmon-body--collapsed .g-pkt  { top: 60%; height: 18%; }
.actmon-body--collapsed .g-traf { top: 80%; height: 18%; }
</style>
