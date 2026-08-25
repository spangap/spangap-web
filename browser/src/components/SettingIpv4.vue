<!--
  SettingIpv4 — a dotted-quad address or mask.

  The field takes digits and dots only, so what reaches the commit is always
  close to an address; a non-empty value that is not four octets of 0-255 is
  refused there, with a warning, and the field goes back to what the device
  holds. Refused rather than corrected: half an address silently rounded into a
  whole one is worse than being told.

  EMPTY IS ALWAYS ACCEPTED and means unset. That is not laxity — it is how a
  fixed address is handed back to DHCP, and how a mask or gateway is left for
  the network to supply.
-->
<template>
  <div v-if="ready">
    <SettingRow :label="label">
      <div class="ip-line">
      <q-input
        class="set-field"
        :class="{ 'set-field--right': !!unit }"
        :model-value="draft"
        dense outlined
        inputmode="decimal"
        :placeholder="placeholder"
        autocomplete="off" autocorrect="off" spellcheck="false"
        v-bind="NO_MANAGER"
        @beforeinput="quadChars"
        @update:model-value="(v) => (draft = String(v ?? ''))"
        @blur="commit"
        @keyup.enter="commit"
      />
      <span v-if="unit" class="unit">{{ unit }}</span>
      </div>
    </SettingRow>

    <q-dialog v-model="warning">
      <q-card class="ip-card">
        <q-card-section>{{ REASON }}</q-card-section>
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
import { buttonStyle, NO_MANAGER, quadChars, useSettingsReady } from '../lib/settingsRuntime'
import SettingRow from './SettingRow.vue'

const props = defineProps<{ label: string; k: string; placeholder?: string; unit?: string }>()

const REASON = 'Enter an address like 192.168.1.10, or leave it empty.'

const device = useDeviceStore()
const ready = useSettingsReady()
const warning = ref(false)

const stored = computed(() => String(device.get(props.k) ?? ''))
const draft = ref(stored.value)
watch(stored, (v) => { draft.value = v })

/** Four octets of 0-255. Empty is true: unset is a legal answer. */
function isQuad(v: string): boolean {
  if (v === '') return true
  const parts = v.split('.')
  if (parts.length !== 4) return false
  return parts.every(p => /^\d{1,3}$/.test(p) && Number(p) <= 255)
}

function commit() {
  if (!isQuad(draft.value)) {
    warning.value = true
    draft.value = stored.value
    return
  }
  device.set(props.k, draft.value)
}
</script>

<style scoped>
.ip-line { display: flex; align-items: center; gap: 8px; min-width: 0; }
.ip-card { min-width: 260px; }
.unit { font-size: 12px; opacity: 0.7; white-space: nowrap; }
</style>
