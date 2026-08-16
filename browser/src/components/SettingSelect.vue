<template>
  <div class="row items-center no-wrap">
    <div class="col-4 text-caption">{{ label }}</div>
    <q-select
      class="col"
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
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useDeviceStore } from '../stores/device'

const props = defineProps<{
  label: string
  k: string
  options: { label: string; value: string }[]
  disable?: boolean
  /** Type-to-filter, for option lists too long to scan (timezones, countries). */
  searchable?: boolean
}>()
const device = useDeviceStore()

const currentVal = computed(() => String(device.get(props.k) ?? ''))

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
