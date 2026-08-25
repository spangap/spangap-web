<template>
  <div v-if="ready">
    <SettingRow :label="label">
      <div class="slider-line">
        <q-slider
          class="slider"
          :model-value="displayVal"
          :min="min" :max="max" :step="1"
          dense color="primary"
          @update:model-value="onDrag"
          @change="onCommit"
        />
        <div class="slider-val">{{ displayVal }}</div>
      </div>
    </SettingRow>
    <!-- The hint is descriptive text: set as one wherever it appears (see the
         caption row in SettingRows), and hung under the control it describes. -->
    <div v-if="hint" class="slider-hint" :style="{ marginLeft: NAME_COL }">{{ hint }}</div>
  </div>
</template>

<script setup lang="ts">
import { computed, ref } from 'vue'
import { useDeviceStore } from '../stores/device'
import { NAME_COL, useSettingsReady } from '../lib/settingsRuntime'
import SettingRow from './SettingRow.vue'

const props = defineProps<{ label: string; k: string; min: number; max: number; hint?: string }>()
const device = useDeviceStore()
const ready = useSettingsReady()

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

<style scoped>
.slider-line {
  display: flex;
  align-items: center;
  gap: 10px;
  min-width: 0;
}
/* The track keeps the column; the knob at either end is a circle whose edge
 * would otherwise sit against the column boundary and against the readout. */
.slider {
  flex: 1 1 auto;
  min-width: 0;
  padding: 0 6px;
}
.slider-val {
  flex: 0 0 auto;
  min-width: 40px;
  font-size: 12px;
  text-align: right;
  /* The readout ends where a text field's box does, not against the pane's
   * edge: the control column runs to that edge, so without this the number
   * sits flush on the margin. */
  padding-right: 16px;
}
.slider-hint {
  font-size: 11px;
  font-style: italic;
  line-height: 1.4;
  opacity: 0.7;
  /* Starts on the column boundary the controls start on, so it reads as
   * belonging to the one above it. */
  margin-top: 4px;
  margin-right: 8px;
}
</style>
