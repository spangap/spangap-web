<template>
  <div class="q-gutter-y-md">
    <PanelHeading>This System</PanelHeading>
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
import { computed, ref, watch, onMounted } from 'vue'
import { useDeviceStore } from '../stores/device'

const device = useDeviceStore()

const currentTz = computed(() => String(device.get('s.ntp.tz') ?? ''))

/* The IANA→POSIX map no longer lives in config — it's a plain file on the
 * device at /state/timezones.json. Fetch it once to populate the dropdown.
 * onTzChange sends only s.ntp.tz; the device resolves POSIX from that file. */
const tzOptions = ref<{ label: string; value: string }[]>([])

function collectZones(node: any, prefix: string, out: { label: string; value: string }[]) {
  for (const [name, val] of Object.entries(node as Record<string, any>)) {
    if (name === 'updated') continue
    const iana = prefix ? `${prefix}/${name}` : name
    if (typeof val === 'string') out.push({ label: iana.replace(/_/g, ' '), value: iana })
    else if (val && typeof val === 'object') collectZones(val, iana, out)
  }
}

onMounted(async () => {
  try {
    const r = await fetch('/state/timezones.json', { credentials: 'same-origin', cache: 'no-cache' })
    if (!r.ok) return
    const zones = await r.json()
    if (!zones || typeof zones !== 'object') return
    const opts: { label: string; value: string }[] = []
    collectZones(zones, '', opts)
    opts.sort((a, b) => a.label.localeCompare(b.label))
    tzOptions.value = opts
    filteredTzOptions.value = opts
  } catch { /* offline — leave dropdown empty */ }
})

const filteredTzOptions = ref<{ label: string; value: string }[]>([])

watch(tzOptions, () => { filteredTzOptions.value = tzOptions.value })

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
}
</script>
