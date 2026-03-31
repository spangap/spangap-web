<template>
  <div class="row items-center no-wrap">
    <div class="col-4 text-caption">{{ label }}</div>
    <div class="col">
      <q-toggle
        :model-value="boolVal"
        dense color="primary"
        @update:model-value="onToggle"
      />
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from '../stores/device'

const props = defineProps<{ label: string; k: string }>()
const device = useDeviceStore()

const boolVal = computed(() => device.get(props.k) == 1)

function onToggle(val: boolean) {
  device.set(props.k, val ? 1 : 0)
}
</script>
