<template>
  <div class="row items-center no-wrap">
    <div class="col-4 text-caption">{{ label }}</div>
    <q-slider
      class="col"
      :model-value="displayVal"
      :min="min" :max="max" :step="1"
      dense color="primary"
      @update:model-value="onDrag"
      @change="onCommit"
    />
    <div class="text-caption q-ml-sm" style="min-width:44px;text-align:right">{{ displayVal }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useDeviceStore } from '../stores/device'

const props = defineProps<{ label: string; k: string; min: number; max: number }>()
const device = useDeviceStore()

const intVal = computed(() => Number(device.get(props.k) ?? 0))
/** Live value while dragging; null = use device store */
const dragVal = ref<number | null>(null)

const displayVal = computed(() =>
  dragVal.value !== null ? dragVal.value : intVal.value,
)

function onDrag(val: number | null) {
  if (val === null) return
  dragVal.value = val
}

function onCommit(val: number) {
  device.set(props.k, val)
  dragVal.value = null
}
</script>
