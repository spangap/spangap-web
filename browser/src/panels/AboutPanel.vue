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

    <q-separator dark />

    <PanelHeading>Update</PanelHeading>
    <div class="q-gutter-y-sm">
      <SettingText label="Manifest URL" k="s.sys.ota.url" />
      <div class="row no-wrap"><div class="col-5 text-caption">Latest</div>
        <div class="col">{{ latestText }}</div></div>
      <div class="row q-gutter-x-sm">
        <q-btn dense flat outline label="Check for update" :disable="busy"
          @click="onCheck" />
        <q-btn dense unelevated color="primary" :disable="!updateAvailable || busy"
          @click="onUpgrade">
          {{ busy ? 'Updating…' : 'Install update' }}
        </q-btn>
      </div>
      <div v-if="busy" class="text-caption text-grey-5">
        Please wait — download, flash, and reboot can take a minute. Watch the
        Log window for progress.
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from '../../stores/device'

const device = useDeviceStore()

const progName = computed(() => {
  const p = device.get('s.sys.progname')
  return (typeof p === 'string' && p.trim()) || 'Seccam'
})

const buildTime    = computed(() => num(device.get('sys.buildtime.app')))
const committedAt  = computed(() => num(device.get('s.sys.ota.committed_at')))
const firstBootAt  = computed(() => num(device.get('s.sys.ota.first_boot_at')))
const latestTime   = computed(() => num(device.get('ota.latest_build_time')))
const updateAvailable = computed(() => num(device.get('ota.update_available')) === 1)
const busy         = computed(() => num(device.get('ota.busy')) === 1)
const rollbackAvailable = computed(() => committedAt.value > 0)

const latestText = computed(() => {
  if (!latestTime.value) return '(not checked)'
  const diff = latestTime.value - buildTime.value
  const stamp = fmtTime(latestTime.value)
  if (diff > 0) return `${stamp} — UPDATE AVAILABLE`
  if (diff < 0) return `${stamp} — running ahead of manifest`
  return `${stamp} — up to date`
})

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

function onCheck() { device.set('ota.check', 1) }
function onUpgrade() { device.set('ota.upgrade', 1) }
</script>
