<!--
  SettingRows — the runtime renderer for a node's row block.

  Each row kind maps to the matching Setting* component (the same ones a
  hand-written panel uses), so a declared row and a hand-written one are
  visually identical. Rows are storage-bound here; the form dialog renders the
  same descriptors against a local buffer instead.
-->
<template>
  <template v-for="(row, i) in rows" :key="i">
    <template v-if="visible(row)">
      <PanelHeading v-if="row.kind === 'section'">{{ row.text }}</PanelHeading>

      <div v-else-if="row.kind === 'caption'" class="text-caption" style="opacity:0.7; line-height:1.35">
        {{ row.text }}
      </div>

      <SettingToggle v-else-if="row.kind === 'switch'" :label="row.label!" :k="row.k!" />

      <SettingSlider
        v-else-if="row.kind === 'slider'"
        :label="row.label!"
        :k="row.k!"
        :min="bound(row.minKey, row.min ?? 0)"
        :max="bound(row.maxKey, row.max ?? 100)"
      />

      <SettingText v-else-if="row.kind === 'text' && !row.secret" :label="row.label!" :k="row.k!" />

      <!-- Secret: write-only. Never read back (lives in secrets.*, not synced). -->
      <div v-else-if="row.kind === 'text' && row.secret" class="row items-center no-wrap">
        <div class="col-4 text-caption">{{ row.label }}</div>
        <q-input
          class="col"
          :model-value="''"
          type="password"
          dense
          outlined
          debounce="600"
          placeholder="(write-only — set to change)"
          autocomplete="new-password"
          autocorrect="off"
          autocapitalize="off"
          spellcheck="false"
          @update:model-value="(v) => setSecret(row.k!, v)"
        />
      </div>

      <SettingSelect
        v-else-if="row.kind === 'dropdown'"
        :label="row.label!"
        :k="row.k!"
        :options="row.options ?? []"
        :searchable="row.searchable"
      />

      <div v-else-if="row.kind === 'value'" class="row items-center no-wrap">
        <div class="col-4 text-caption">{{ row.label }}</div>
        <div
          class="col text-caption"
          :class="{ 'value-copyable': row.copyable }"
          :title="row.copyable ? 'Click to copy' : undefined"
          @click="row.copyable && copy(liveValue(row.k!))"
        >{{ liveValue(row.k!) }}</div>
      </div>

      <div v-else-if="row.kind === 'button'">
        <SettingsAction :label="row.label!" :action="row.do!" :danger="row.danger" />
      </div>

      <SettingsCollection v-else-if="row.kind === 'list'" :row="row" />

      <!-- A hand-written panel still occupying this node. Transitional. -->
      <component :is="row.component" v-else-if="row.kind === 'component' && row.component" />
    </template>
  </template>
</template>

<script setup lang="ts">
import { useDeviceStore } from '../stores/device'
import { rowVisible } from '../lib/settingsRuntime'
import type { GenRow } from '../lib/settingsNodes'
import PanelHeading from './PanelHeading.vue'
import SettingToggle from './SettingToggle.vue'
import SettingSlider from './SettingSlider.vue'
import SettingText from './SettingText.vue'
import SettingSelect from './SettingSelect.vue'
import SettingsAction from './SettingsAction.vue'
import SettingsCollection from './SettingsCollection.vue'

defineProps<{ rows: GenRow[] }>()
const device = useDeviceStore()

function visible(row: GenRow): boolean {
  return rowVisible(row.whenKey, null)
}

/* A slider bound the device publishes, falling back to the compiled one until
 * the key exists. Reactive through the store, so a limit the firmware revises
 * moves the control with it. */
function bound(k: string | undefined, fallback: number): number {
  if (!k) return fallback
  const v = Number(device.get(k))
  return Number.isFinite(v) ? v : fallback
}

/* Whatever the key holds, verbatim — the firmware publishes the finished text. */
function liveValue(k: string): string {
  const v = device.get(k)
  return v === undefined || v === null ? '' : String(v)
}

function setSecret(k: string, v: string | number | null) {
  device.set(k, String(v ?? ''))
  device.save()
}

function copy(text: string) {
  navigator.clipboard?.writeText(text).catch(() => { /* denied or insecure origin */ })
}
</script>

<style scoped>
.value-copyable {
  cursor: pointer;
  user-select: all;
  text-decoration: underline dotted rgba(255, 255, 255, 0.3);
}
</style>
