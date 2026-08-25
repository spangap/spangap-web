<!--
  SettingInteger — a number typed in, not dragged to.

  For the quantity an operator already knows — a timeout, a port, a count — as
  against one that is felt, which is what a slider is still for. The field takes
  digits only, so what reaches the commit is always a number; a number outside
  the stated bounds is REFUSED there, with a warning naming the range, and the
  field goes back to what the device holds. Refused rather than clamped:
  clamping stores a value nobody typed, and does it silently.

  `buttons` puts a - and a + beside the field. They SNAP to multiples of `step`
  rather than adding to whatever is there — at step 5, down from 23 is 20 and
  then 15 — so a hand-typed number joins the grid on the first press instead of
  carrying its offset for ever.
-->
<template>
  <div v-if="ready">
    <SettingRow :label="label">
      <!-- Field, then its unit, then the two steppers. The unit belongs to the
           number, so it stays against it; the buttons are the control's own
           furniture and gather at the end where a thumb finds them together. -->
      <div class="num-line">
        <q-input
          class="set-field set-field--short set-field--right"
          :model-value="draft"
          dense outlined
          inputmode="numeric"
          autocomplete="off" autocorrect="off" spellcheck="false"
          v-bind="NO_MANAGER"
          @beforeinput="digits"
          @update:model-value="(v) => (draft = String(v ?? ''))"
          @blur="commit"
          @keyup.enter="commit"
        />
        <span v-if="unit" class="unit">{{ unit }}</span>
        <q-btn
          v-if="buttons"
          class="num-step" dense unelevated
          :style="buttonStyle('grey')" label="−"
          @click="step(-1)"
        />
        <q-btn
          v-if="buttons"
          class="num-step" dense unelevated
          :style="buttonStyle('grey')" label="+"
          @click="step(+1)"
        />
      </div>
    </SettingRow>

    <!-- The refusal. One line and an OK: there is no choice to make here, only
         the range to be told. -->
    <q-dialog v-model="warning">
      <q-card class="num-card">
        <q-card-section>{{ rangeMessage }}</q-card-section>
        <q-card-actions align="right">
          <q-btn dense no-caps unelevated :style="buttonStyle()" label="OK" @click="warning = false" />
        </q-card-actions>
      </q-card>
    </q-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useDeviceStore } from '../stores/device'
import { buttonStyle, NO_MANAGER, numberChars, useSettingsReady } from '../lib/settingsRuntime'
import SettingRow from './SettingRow.vue'

const props = defineProps<{
  label: string
  k: string
  /** Absent where the quantity has no bound — not defaulted to a number the
   *  control would then enforce. */
  min?: number
  max?: number
  step?: number
  buttons?: boolean
  unit?: string
}>()

/** Digits, and a minus only where the range actually goes below zero. */
const digits = numberChars(props.min === undefined || props.min < 0)

const device = useDeviceStore()
const ready = useSettingsReady()
const warning = ref(false)

const stored = computed(() => String(device.get(props.k) ?? ''))
/** What is in the field. Follows the device except while it is being typed in. */
const draft = ref(stored.value)
watch(stored, (v) => { draft.value = v })

const rangeMessage = computed(() => {
  if (props.min !== undefined && props.max !== undefined)
    return `Enter a number between ${props.min} and ${props.max}.`
  if (props.min !== undefined) return `Enter a number of at least ${props.min}.`
  if (props.max !== undefined) return `Enter a number no greater than ${props.max}.`
  return 'Enter a number.'
})

function inRange(n: number): boolean {
  if (props.min !== undefined && n < props.min) return false
  if (props.max !== undefined && n > props.max) return false
  return true
}

function commit() {
  const n = Number(draft.value)
  if (draft.value.trim() === '' || !Number.isInteger(n) || !inRange(n)) {
    warning.value = true
    draft.value = stored.value
    return
  }
  device.set(props.k, n)
}

/** The next multiple of the step past the current value, clamped. */
function step(dir: number) {
  const size = props.step && props.step > 0 ? props.step : 1
  const cur = Number.isInteger(Number(draft.value)) && draft.value.trim() !== ''
    ? Number(draft.value)
    : (props.min ?? 0)
  let next = dir > 0
    ? (Math.floor(cur / size) + 1) * size
    : Math.floor((cur - 1) / size) * size
  if (props.min !== undefined && next < props.min) next = props.min
  if (props.max !== undefined && next > props.max) next = props.max
  draft.value = String(next)
  device.set(props.k, next)
}
</script>

<style scoped>
.num-line {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}
.num-step {
  flex: 0 0 auto;
  min-width: 28px;
  padding: 0 6px;
}
.num-card {
  min-width: 260px;
}
.unit { font-size: 12px; opacity: 0.7; white-space: nowrap; }
</style>
