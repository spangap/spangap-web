<template>
  <FloatingWindow
    id="actmon"
    :title="title"
    :visible="visible"
    :focus-token="focusToken"
    :default-geom="defaultGeom"
    :min-size="{ w: 24, h: 12 }"
    @update:visible="v => emit('update:visible', v)"
  >
    <template #default>
      <div class="actmon-body">
        <div class="actmon-graph actmon-graph-0">
          <canvas ref="c0Ref" class="actmon-canvas" />
          <div class="actmon-caption">core 0</div>
        </div>
        <div class="actmon-graph actmon-graph-1">
          <canvas ref="c1Ref" class="actmon-canvas" />
          <div class="actmon-caption">core 1</div>
        </div>
        <div class="actmon-graph actmon-graph-2">
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
  ? { x: 0, y: 0, w: 100, h: 60 }
  : { x: 20, y: 8, w: 55, h: 62 }

/* Graph geometry. Each canvas backing store is a fixed STEPS×GH buffer — one
 * column per sample, GH rows tall — and CSS stretches it to fill the window,
 * so the drawing resolution is independent of the on-screen size. */
const STEPS = 320
const GH = 100

/* Palette — identical to the on-device Activity app. */
const C_BG = '#2A2A2A', C_GRID = '#606060', C_WHITE = '#FFFFFF'
const C_RED = '#E05050', C_ORANGE = '#F08820', C_YELLOW = '#E8D040'
const LEVELS = [0, 20, 40, 60, 80, 100]

interface Sample { core0: number; core1: number; sleep: number; apbMax: number; cpuMax: number }
/* Our own rolling window — the device publishes the latest second at 1 Hz; we
 * keep the last STEPS of them here (no ring transfer from the device). */
let samples: Sample[] = []

const c0Ref = ref<HTMLCanvasElement>()
const c1Ref = ref<HTMLCanvasElement>()
const c2Ref = ref<HTMLCanvasElement>()

function ctxOf(cv: HTMLCanvasElement | undefined): { ctx: CanvasRenderingContext2D; w: number } | null {
  if (!cv) return null
  if (cv.width !== STEPS) cv.width = STEPS
  if (cv.height !== GH) cv.height = GH
  const ctx = cv.getContext('2d')
  if (!ctx) return null
  return { ctx, w: STEPS }
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

onMounted(() => {
  timer = setInterval(tick, 1000)
})

onUnmounted(() => {
  if (timer) { clearInterval(timer); timer = null }
})

/* On open the canvases gain a size — redraw the buffer we've been keeping. */
watch(() => props.visible, v => { if (v) nextTick(draw) })
</script>

<style scoped>
.actmon-body {
  position: relative;
  width: 100%;
  height: 100%;
  background: #1a1a1a;
}
/* Three bands, each 25% of the window tall, anchored at 0 / 33% / 66% from the
 * top. Height tracks the window since the parent fills it. */
.actmon-graph {
  position: absolute;
  left: 0;
  right: 0;
  height: 25%;
}
.actmon-graph-0 { top: 0; }
.actmon-graph-1 { top: 33%; }
.actmon-graph-2 { top: 66%; }
.actmon-canvas {
  display: block;
  width: 100%;
  height: 100%;
  image-rendering: pixelated;
}
.actmon-caption {
  position: absolute;
  top: 100%;
  left: 0;
  padding: 2px 0 0 6px;
  font: 11px/1.2 'SF Mono', 'Menlo', 'Consolas', monospace;
  color: #c8c8c8;
}
.actmon-caption .c-red { color: #E05050; }
.actmon-caption .c-orange { color: #F08820; }
.actmon-caption .c-yellow { color: #E8D040; }
</style>
