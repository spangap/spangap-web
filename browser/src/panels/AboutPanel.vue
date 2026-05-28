<template>
  <div class="q-gutter-y-md">
    <PanelHeading>{{ progName }}</PanelHeading>
    <div class="q-gutter-y-sm">
      <div class="row no-wrap"><div class="col-5 text-caption">Build</div>
        <div class="col">{{ fmtTime(buildTime) }}</div></div>
      <div class="row no-wrap"><div class="col-5 text-caption">Committed</div>
        <div class="col">{{ fmtTime(committedAt) }}</div></div>
      <div class="row no-wrap"><div class="col-5 text-caption">First boot</div>
        <div class="col">{{ fmtTime(firstBootAt) }}</div></div>
      <div class="row no-wrap"><div class="col-5 text-caption">Rollback</div>
        <div class="col">{{ rollbackAvailable ? 'available (other slot)' : 'unavailable' }}</div></div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from '../stores/device'

const device = useDeviceStore()

const progName = computed(() => {
  const p = device.get('s.sys.progname')
  if (typeof p === 'string' && p.trim()) return p.trim()
  const proj = device.get('s.sys.project')
  if (typeof proj === 'string' && proj.length > 0) return proj.charAt(0).toUpperCase() + proj.slice(1)
  return 'Spangap'
})

const buildTime         = computed(() => num(device.get('sys.buildtime.app')))
const committedAt       = computed(() => num(device.get('s.sys.ota.committed_at')))
const firstBootAt       = computed(() => num(device.get('s.sys.ota.first_boot_at')))
const rollbackAvailable = computed(() => committedAt.value > 0)

function num(v: unknown): number {
  if (typeof v === 'number') return v
  if (typeof v === 'string') return parseInt(v, 10) || 0
  return 0
}

function fmtTime(epoch: number): string {
  if (!epoch) return '(none)'
  const d = new Date(epoch * 1000)
  return d.toLocaleString(undefined, {
    year: 'numeric', month: 'short', day: '2-digit',
    hour: '2-digit', minute: '2-digit', second: '2-digit',
  })
}
</script>
