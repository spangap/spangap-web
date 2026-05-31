<template>
  <div class="q-gutter-y-md">
    <PanelHeading>This Camera</PanelHeading>
    <div class="q-gutter-y-sm">
      <SettingText label="Hostname" k="s.net.hostname" />
    </div>

    <q-separator dark />

    <PanelHeading>Time &amp; Date</PanelHeading>
    <div class="q-gutter-y-sm">
      <div class="row items-center no-wrap">
        <div class="col-4 text-caption">Timezone</div>
        <q-select
          class="col"
          :model-value="currentTz"
          :options="filteredTzOptions"
          dense outlined
          emit-value map-options
          options-dense
          use-input
          input-debounce="100"
          @filter="filterTz"
          @update:model-value="onTzChange"
        />
      </div>
      <SettingText label="NTP Server" k="s.ntp.server" />
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useDeviceStore } from '../stores/device'

const device = useDeviceStore()

const currentTz = computed(() => String(device.get('s.ntp.tz') ?? ''))

/** Build timezone options from s.time.zones tree on the device. */
const tzOptions = computed(() => {
  const zones = device.get('s.time.zones')
  if (!zones || typeof zones !== 'object') return []
  const opts: { label: string; value: string }[] = []
  for (const [continent, cities] of Object.entries(zones as Record<string, any>)) {
    if (continent === 'updated' || typeof cities !== 'object') continue
    for (const [city, posix] of Object.entries(cities as Record<string, any>)) {
      if (typeof posix !== 'string') {
        /* Sub-region like America/Argentina/Buenos_Aires */
        if (typeof posix === 'object') {
          for (const [sub, val] of Object.entries(posix as Record<string, string>)) {
            const iana = `${continent}/${city}/${sub}`
            opts.push({ label: iana.replace(/_/g, ' '), value: iana })
          }
        }
        continue
      }
      const iana = `${continent}/${city}`
      opts.push({ label: iana.replace(/_/g, ' '), value: iana })
    }
  }
  opts.sort((a, b) => a.label.localeCompare(b.label))
  return opts
})

const filteredTzOptions = ref<{ label: string; value: string }[]>([])

watch(tzOptions, () => { filteredTzOptions.value = tzOptions.value }, { immediate: true })

function filterTz(val: string, update: (fn: () => void) => void) {
  update(() => {
    const needle = val.toLowerCase()
    filteredTzOptions.value = needle
      ? tzOptions.value.filter(o => o.label.toLowerCase().includes(needle))
      : tzOptions.value
  })
}

function onTzChange(ianaName: string) {
  device.set('s.ntp.tz', ianaName)
  /* Look up POSIX string from the zones tree and send alongside */
  const parts = ianaName.split('/')
  let node: any = device.get('s.time.zones')
  for (const p of parts) {
    if (!node || typeof node !== 'object') break
    node = node[p]
  }
  if (typeof node === 'string') device.set('s.ntp.posix', node)
}
</script>
