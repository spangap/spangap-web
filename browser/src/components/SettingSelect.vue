<template>
  <div class="row items-center no-wrap">
    <div class="col-4 text-caption">{{ label }}</div>
    <q-select
      class="col"
      :model-value="currentVal"
      :options="options"
      dense outlined
      emit-value map-options
      options-dense
      @update:model-value="onChange"
    />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from '../stores/device'

const props = defineProps<{
  label: string
  k: string
  options: { label: string; value: string }[]
}>()
const device = useDeviceStore()

const currentVal = computed(() => device.settings[props.k] ?? '')

function onChange(val: string) {
  device.set(props.k, val)
}
</script>
