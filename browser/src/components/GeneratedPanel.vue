<!--
  GeneratedPanel — the single runtime renderer for declarative settings panels.

  The build inlines each straddle.yaml `settings:` block as a JSON descriptor
  (see lib/generatedPanels.ts) and registers it in the menu tree pointing here.
  The menu store renders a panel component with no props, so we find our own
  descriptor from the active-panel id. Each row kind maps to the matching
  Setting* component (the same ones a hand-written *Panel.vue uses), so a
  generated pane and a hand-written one are visually identical.
-->
<template>
  <div v-if="panel" class="q-gutter-y-md">
    <template v-for="(row, i) in panel.rows" :key="i">
      <PanelHeading v-if="row.kind === 'section'">{{ row.text }}</PanelHeading>

      <div v-else-if="row.kind === 'caption'" class="text-caption" style="opacity:0.7; line-height:1.35">
        {{ row.text }}
      </div>

      <SettingToggle v-else-if="row.kind === 'switch'" :label="row.label!" :k="row.k!" />

      <SettingSlider
        v-else-if="row.kind === 'slider'"
        :label="row.label!"
        :k="row.k!"
        :min="row.min ?? 0"
        :max="row.max ?? 100"
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
      />

      <div v-else-if="row.kind === 'value'" class="row items-center no-wrap">
        <div class="col-4 text-caption">{{ row.label }}</div>
        <div class="col text-caption">{{ liveValue(row.k!) }}</div>
      </div>

      <div v-else-if="row.kind === 'button'">
        <q-btn dense no-caps outline color="primary" :label="row.label" @click="fireCmd(row)" />
      </div>

      <GeneratedListRow v-else-if="row.kind === 'list'" :row="row" />
    </template>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useMenuStore } from '../stores/menu'
import { useDeviceStore } from '../stores/device'
import { getGeneratedPanel, type GenRow } from '../lib/generatedPanels'
import PanelHeading from './PanelHeading.vue'
import SettingToggle from './SettingToggle.vue'
import SettingSlider from './SettingSlider.vue'
import SettingText from './SettingText.vue'
import SettingSelect from './SettingSelect.vue'
import GeneratedListRow from './GeneratedListRow.vue'

const menu = useMenuStore()
const device = useDeviceStore()

const panel = computed(() => getGeneratedPanel(menu.activePanel))

function liveValue(k: string): string {
  const v = device.get(k)
  return v === undefined || v === null ? '' : String(v)
}

function setSecret(k: string, v: string | number | null) {
  device.set(k, String(v ?? ''))
  device.save()
}

function fireCmd(row: GenRow) {
  if (row.cmd) device.set(row.cmd, row.payload ?? '1')
}
</script>
