<template>
  <div class="row items-center no-wrap">
    <div class="col-4 text-caption">{{ label }}</div>
    <q-input
      class="col"
      :model-value="device.settings[k] ?? ''"
      dense outlined
      debounce="500"
      @update:model-value="onChange"
    />
  </div>
</template>

<script setup lang="ts">
import { useDeviceStore } from '../stores/device'

const props = defineProps<{ label: string; k: string }>()
const device = useDeviceStore()

function onChange(val: string | number | null) {
  if (val !== null) device.set(props.k, String(val))
}
</script>
