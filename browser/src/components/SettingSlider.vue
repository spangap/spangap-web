<template>
  <div class="row items-center no-wrap">
    <div class="col-4 text-caption">{{ label }}</div>
    <q-slider
      class="col"
      :model-value="intVal"
      :min="min" :max="max" :step="1"
      dense color="primary"
      @change="onChange"
    />
    <div class="text-caption q-ml-sm" style="width:40px;text-align:right">{{ intVal }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from '../stores/device'

const props = defineProps<{ label: string; k: string; min: number; max: number }>()
const device = useDeviceStore()

const intVal = computed(() => Number(device.get(props.k) ?? 0))

function onChange(val: number) {
  device.set(props.k, val)
}
</script>
