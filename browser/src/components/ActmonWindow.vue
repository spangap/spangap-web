<template>
  <FloatingWindow
    id="actmon"
    :title="title"
    :visible="visible"
    :focus-token="focusToken"
    :default-geom="defaultGeom"
    :min-size="{ w: 24, h: 8 }"
    auto-height
    @update:visible="v => emit('update:visible', v)"
  >
    <template #default>
      <div ref="bodyRef" class="actmon-body">
        <div class="actmon-graph">
          <canvas ref="c0Ref" class="actmon-canvas" />
          <div class="actmon-caption">core 0</div>
        </div>
        <div class="actmon-graph">
          <canvas ref="c1Ref" class="actmon-canvas" />
          <div class="actmon-caption">core 1</div>
        </div>
        <div class="actmon-graph">
          <canvas ref="c2Ref" class="actmon-canvas" />
          <div class="actmon-caption">power mgmt: <span class="c-red">CPU_MAX</span>, <span class="c-orange">APB_MAX</span>, <span class="c-yellow">APB_MIN</span>. Rest is SLEEP</div>
        </div>
      </div>
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, watch, onMounted, onUnmounted, nextTick } from 'vue'
import FloatingWindow from './FloatingWindow.vue'
import { useDeviceStore } from '../stores/device'

const props = defineProps<{
  visible: boolean
  title: string
  focusToken?: number
}>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const device = useDeviceStore()

const isPhoneInit = window.matchMedia?.('(max-width: 599px)').matches ?? false
const defaultGeom = isPhoneInit
  ? { x: 0, y: 0, w: 100, h: 50 }
  : { x: 20, y: 8, w: 55, h: 62 }

/* Graph geometry. STEPS is the horizontal resolution — one column per second,
 * the window's full width divided into this many steps. GH is each graph's
 * pixel height (matches the on-device 50 px band). */
const STEPS = 100
const GH = 50

/* Palette — identical to the on-device Activity app. */
const C_BG = '#2A2A2A', C_GRID = '#606060', C_WHITE = '#FFFFFF'
const C_RED = '#E05050', C_ORANGE = '#F08820', C_YELLOW = '#E8D040'
const LEVELS = [0, 20, 40, 60, 80, 100]

interface Sample { core0: number; core1: number; sleep: number; apbMax: number; cpuMax: number }
/* Our own rolling window — the device publishes the latest second at 1 Hz; we
 * keep the last STEPS of them here (no ring transfer from the device). */
let samples: Sample[] = []

const bodyRef = ref<HTMLElement>()
const c0Ref = ref<HTMLCanvasElement>()
const c1Ref = ref<HTMLCanvasElement>()
const c2Ref = ref<HTMLCanvasElement>()

function ctxOf(cv: HTMLCanvasElement | undefined): { ctx: CanvasRenderingContext2D; w: number } | null {
  if (!cv) return null
  const w = cv.clientWidth
  if (w <= 0) return null
  const dpr = window.devicePixelRatio || 1
  cv.width = Math.max(1, Math.round(w * dpr))
  cv.height = Math.round(GH * dpr)
  const ctx = cv.getContext('2d')
  if (!ctx) return null
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
  return { ctx, w }
}

function drawGrid(ctx: CanvasRenderingContext2D, w: number) {
  ctx.fillStyle = C_BG
  ctx.fillRect(0, 0, w, GH)
  ctx.fillStyle = C_GRID
  for (const p of LEVELS) {
    const y = Math.min(Math.round(GH - (p / 100) * GH), GH - 1)
    ctx.fillRect(0, y, w, 1)
  }
}

function drawCore(cv: HTMLCanvasElement | undefined, pick: (s: Sample) => number) {
  const c = ctxOf(cv)
  if (!c) return
  const { ctx, w } = c
  drawGrid(ctx, w)
  const step = w / STEPS
  const n = samples.length
  ctx.fillStyle = C_WHITE
  for (let i = 0; i < n; i++) {
    const p = pick(samples[i])
    if (p <= 0) continue
    let h = (p / 100) * GH
    if (h < 1) h = 1                       // any nonzero % shows at least a pixel
    ctx.fillRect((STEPS - n + i) * step, GH - h, Math.ceil(step), h)
  }
}

function drawState(cv: HTMLCanvasElement | undefined) {
  const c = ctxOf(cv)
  if (!c) return
  const { ctx, w } = c
  drawGrid(ctx, w)
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

function draw() {
  drawCore(c0Ref.value, s => s.core0)
  drawCore(c1Ref.value, s => s.core1)
  drawState(c2Ref.value)
}

function tick() {
  const g = (k: string) => Number(device.get('sys.stats.' + k) ?? 0)
  samples.push({
    core0: g('cpu_pct.0'), core1: g('cpu_pct.1'),
    sleep: g('SLEEP'), apbMax: g('APB_MAX'), cpuMax: g('CPU_MAX'),
  })
  if (samples.length > STEPS) samples.shift()
  if (props.visible) draw()
}

let timer: ReturnType<typeof setInterval> | null = null
let ro: ResizeObserver | null = null

onMounted(() => {
  timer = setInterval(tick, 1000)
  ro = new ResizeObserver(() => { if (props.visible) draw() })
  if (bodyRef.value) ro.observe(bodyRef.value)
})

onUnmounted(() => {
  if (timer) { clearInterval(timer); timer = null }
  if (ro) { ro.disconnect(); ro = null }
})

/* On open the canvases gain a size — redraw the buffer we've been keeping. */
watch(() => props.visible, v => { if (v) nextTick(draw) })
</script>

<style scoped>
.actmon-body {
  width: 100%;
  background: #1a1a1a;
  padding: 10px 0;
  display: flex;
  flex-direction: column;
  gap: 12px;
}
.actmon-graph { width: 100%; }
.actmon-canvas {
  display: block;
  width: 100%;
  height: 50px;
}
.actmon-caption {
  padding: 2px 0 0 6px;
  font: 11px/1.2 'SF Mono', 'Menlo', 'Consolas', monospace;
  color: #c8c8c8;
}
.actmon-caption .c-red { color: #E05050; }
.actmon-caption .c-orange { color: #F08820; }
.actmon-caption .c-yellow { color: #E8D040; }
</style>
