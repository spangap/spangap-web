<!--
  SettingText — a string key as a text field.

  `secret` masks the characters and offers an eye to unmask them. It does NOT
  make the field write-only: the value is loaded, shown when you ask for it,
  and edited in place like any other. A passphrase you are expected to be able
  to check against the other end of the link is not a credential to be posted
  into a void — masking is there for the person standing behind you, and that
  is all it is there for.
-->
<template>
  <SettingRow v-if="ready" :label="label">
    <q-input
      class="set-field"
      :class="{ 'set-field--short': short, 'set-field--right': !!unit }"
      :model-value="String(device.get(k) ?? '')"
      :type="secret && !revealed ? 'password' : 'text'"
      dense outlined
      debounce="500"
      :autocomplete="secret ? 'new-password' : 'off'"
      autocorrect="off"
      autocapitalize="off"
      spellcheck="false"
      v-bind="NO_MANAGER"
      @update:model-value="onChange"
    >
      <template v-if="unit" #after><span class="unit">{{ unit }}</span></template>
      <template v-if="secret" #append>
        <span
          class="reveal"
          role="button"
          :title="revealed ? 'Hide the value' : 'Show the value'"
          @click="revealed = !revealed"
        ><IconEye :off="revealed" /></span>
      </template>
    </q-input>
  </SettingRow>
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { useDeviceStore } from '../stores/device'
import { NO_MANAGER, useSettingsReady } from '../lib/settingsRuntime'
import SettingRow from './SettingRow.vue'
import IconEye from './IconEye.vue'

const props = defineProps<{
  label: string
  k: string
  secret?: boolean
  /** A third of the usual width, for an entry that is a handful of characters. */
  short?: boolean
  /** A word printed after the field — a unit, or the fixed tail of what is
   *  being entered (".duckdns.org"). Never part of the value; a field carrying
   *  one is short and right-aligned, so the entry and its word read as one. */
  unit?: string
}>()
const ready = useSettingsReady()
const device = useDeviceStore()
/** Per-field and per-visit: leaving the pane masks it again. */
const revealed = ref(false)

function onChange(val: string | number | null) {
  if (val !== null) device.set(props.k, String(val))
}
</script>

<style scoped>
.reveal {
  display: inline-flex;
  align-items: center;
  cursor: pointer;
  opacity: 0.6;
}
.reveal:hover { opacity: 1; }
.unit {
  font-size: 12px;
  opacity: 0.7;
  white-space: nowrap;
  padding-left: 6px;
}
</style>
