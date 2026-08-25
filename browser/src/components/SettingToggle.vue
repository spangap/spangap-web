<template>
  <SettingRow v-if="ready" :label="label">
    <q-toggle
      :model-value="boolVal"
      dense color="primary"
      @update:model-value="onToggle"
    />
  </SettingRow>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from '../stores/device'
import { useSettingsReady } from '../lib/settingsRuntime'
import SettingRow from './SettingRow.vue'

const props = defineProps<{ label: string; k: string }>()
const device = useDeviceStore()
const ready = useSettingsReady()

const boolVal = computed(() => device.get(props.k) == 1)

function onToggle(val: boolean) {
  device.set(props.k, val ? 1 : 0)
}
</script>
