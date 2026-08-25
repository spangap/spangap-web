<template>
  <SettingRow v-if="ready" :label="label">
    <q-select
      class="set-field set-field--fit"
      :style="{ minWidth: fitWidth }"
      :model-value="currentVal"
      :options="shown"
      :disable="disable"
      dense outlined
      emit-value map-options
      options-dense
      :use-input="searchable"
      :input-debounce="0"
      @filter="onFilter"
      @update:model-value="onChange"
    />
  </SettingRow>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useDeviceStore } from '../stores/device'
import { useSettingsReady } from '../lib/settingsRuntime'
import SettingRow from './SettingRow.vue'

const props = defineProps<{
  label: string
  k: string
  options: { label: string; value: string }[]
  disable?: boolean
  /** Type-to-filter, for option lists too long to scan (timezones, countries). */
  searchable?: boolean
}>()
const device = useDeviceStore()
const ready = useSettingsReady()

const currentVal = computed(() => String(device.get(props.k) ?? ''))

/* Wide enough for the LONGEST option, not for whichever one is selected: a
 * picker that resized as you chose would move every control below it. `ch` is
 * the digit width of the face in use, which is close enough for a label and
 * costs no measurement. The chrome (arrow, padding) is the constant. */
const fitWidth = computed(() => {
  const longest = props.options.reduce((n, o) => Math.max(n, o.label.length), 0)
  return `${Math.min(longest, 40) + 5}ch`
})

const needle = ref('')
const shown = computed(() => {
  if (!props.searchable || !needle.value) return props.options
  const n = needle.value.toLowerCase()
  return props.options.filter(o => o.label.toLowerCase().includes(n))
})

function onFilter(v: string, update: (fn: () => void) => void) {
  update(() => { needle.value = v })
}

function onChange(val: string) {
  device.set(props.k, val)
}
</script>
