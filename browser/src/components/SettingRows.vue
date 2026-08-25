<!--
  SettingRows — the runtime renderer for a node's row block.

  Each row kind maps to the matching Setting* component (the same ones a
  hand-written panel uses), so a declared row and a hand-written one are
  visually identical. Rows are storage-bound here; the form dialog renders the
  same descriptors against a local buffer instead.

  The whole block waits for the storage dump (`ready`): a pane appears complete
  and correct in one paint rather than filling in — no control showing its zero
  state first, no row arriving late as its gate key lands.
-->
<template>
  <!-- Keyed by the storage key, never by position: two nodes both opening with
       a switch would otherwise have Vue patch one pane's switch into the
       other's, and the control would ANIMATE from the value it held to the
       value this pane's key holds — which reads as arriving on the page having
       just changed the setting. A key of its own makes it a different control,
       so it mounts already showing the truth. -->
  <template v-for="({ row, indent, wide }, i) in shown" :key="rowKey(row, i)">
    <!-- The three heading levels. Each renders at its own indent (below), and
         what follows it is indented by the walk that produced these. -->
    <PanelHeading v-if="row.kind === 'title'" :level="1" :style="pad(indent)">{{ row.text }}</PanelHeading>

    <PanelHeading v-else-if="row.kind === 'heading'" :level="2" :style="pad(indent)">{{ row.text }}</PanelHeading>

    <PanelHeading v-else-if="row.kind === 'section'" :level="3" :style="pad(indent)">{{ row.text }}</PanelHeading>

    <!-- Everything that is not a heading carries the running indent the walk
         computed for it, in one wrapper: a row's own component then never has
         to know where in the hierarchy it landed. -->
    <div v-else :style="pad(indent)">

    <!-- A disclosure group: one full-width button; open, its rows render in
         place through this same component, storage-bound like any pane row. -->
    <div v-if="row.kind === 'advanced'" class="adv-group">
      <div class="adv-btn" @click="toggleAdv(rowKey(row, i))">
        <span>{{ row.label ?? 'Advanced Settings' }}</span>
        <span class="adv-chev">{{ advOpen[rowKey(row, i)] ? '▾' : '▸' }}</span>
      </div>
      <div v-if="advOpen[rowKey(row, i)]" class="adv-body q-gutter-y-sm">
        <SettingRows :rows="row.rows ?? []" />
      </div>
    </div>

    <!-- A description is an aside about the rows it sits among, so it is set
         one step deeper than they are. -->
    <div
      v-else-if="row.kind === 'caption'"
      class="row-caption"
      :style="wide ? undefined : { marginLeft: NAME_COL }"
    ><CaptionText :text="row.text" /></div>

    <SettingToggle v-else-if="row.kind === 'switch'" :label="row.label!" :k="row.k!" />

    <SettingSlider
      v-else-if="row.kind === 'slider'"
      :label="row.label!"
      :k="row.k!"
      :min="bound(row.minKey, row.min ?? 0)"
      :max="bound(row.maxKey, row.max ?? 100)"
    />

    <!-- A number typed in. `min`/`max` stay undefined where the row states no
         bound — the control enforces what was stated and nothing more. -->
    <SettingInteger
      v-else-if="row.kind === 'integer'"
      :label="row.label!"
      :k="row.k!"
      :min="row.minKey ? bound(row.minKey, row.min ?? 0) : row.min"
      :max="row.maxKey ? bound(row.maxKey, row.max ?? 0) : row.max"
      :step="row.step"
      :buttons="row.buttons"
      :unit="row.unit"
    />

    <!-- `secret` masks the field and offers an eye; it does not make it
         write-only. The value loads and edits in place like any other. -->
    <SettingText
      v-else-if="row.kind === 'text'"
      :label="row.label!"
      :k="row.k!"
      :secret="row.secret"
      :short="row.short"
      :unit="row.unit"
    />

    <!-- A dotted quad. Empty is accepted and means unset — that is how a fixed
         address is handed back to DHCP. -->
    <SettingIpv4 v-else-if="row.kind === 'ipv4'" :label="row.label!" :k="row.k!" :unit="row.unit" />

    <SettingSelect
      v-else-if="row.kind === 'dropdown'"
      :label="row.label!"
      :k="row.k!"
      :options="row.options ?? []"
      :searchable="row.searchable"
    />

    <SettingRow v-else-if="row.kind === 'value'" :label="row.label">
      <div
        class="row-value"
        :class="{ 'value-copyable': row.copyable }"
        :title="row.copyable ? 'Click to copy' : undefined"
        @click="row.copyable && copy(liveValue(row.k!))"
      >{{ liveValue(row.k!) }}</div>
    </SettingRow>

    <!-- A button is a control, so it starts where the controls start rather
         than at the pane's left edge. -->
    <SettingRow v-else-if="row.kind === 'button'">
      <SettingsAction :label="row.label!" :action="row.do!" :color="row.color" />
    </SettingRow>

    <!-- Several buttons on one line, gathered left, centre or right. Each is
         content-sized (a lone button: row is the full-width one), and a button
         may carry its own gate — a hidden one leaves the line entirely. -->
    <SettingRow v-else-if="row.kind === 'buttons'">
      <div class="btn-row" :style="{ justifyContent: JUSTIFY[row.align ?? 'left'] }">
        <template v-for="(b, j) in row.items ?? []" :key="j">
          <SettingsAction
            v-if="rowVisible(b.whenKey, null)"
            :label="b.label"
            :action="b.do"
            :color="b.color"
          />
        </template>
      </div>
    </SettingRow>

    <!-- An info group: one shared label column sized to the widest label and
         capped at a third, and no gap between the lines. The grid track does
         the sizing that the LCD has to measure for itself. -->
    <div v-else-if="row.kind === 'info'" class="info-grid" :style="{ gridTemplateColumns: `${NAME_COL} 1fr` }">
      <template v-for="(r, j) in row.rows ?? []" :key="j">
        <template v-if="visible(r)">
          <div class="info-label">{{ r.label }}</div>
          <div
            class="info-value"
            :class="{ 'value-copyable': r.copyable }"
            :title="r.copyable ? 'Click to copy' : undefined"
            @click="r.copyable && copy(liveValue(r.k!))"
          >{{ liveValue(r.k!) }}</div>
        </template>
      </template>
    </div>

    <SettingsCollection v-else-if="row.kind === 'list'" :row="row" />

    <!-- A hand-written panel still occupying this node. Transitional. -->
    <component :is="row.component" v-else-if="row.kind === 'component' && row.component" />

    </div>
  </template>
</template>

<script setup lang="ts">
import { computed, reactive } from 'vue'
import { useDeviceStore } from '../stores/device'
import { rowVisible, NAME_COL, useSettingsReady } from '../lib/settingsRuntime'
import type { GenRow } from '../lib/settingsNodes'
import PanelHeading from './PanelHeading.vue'
import CaptionText from './CaptionText.vue'
import SettingRow from './SettingRow.vue'
import SettingToggle from './SettingToggle.vue'
import SettingSlider from './SettingSlider.vue'
import SettingInteger from './SettingInteger.vue'
import SettingText from './SettingText.vue'
import SettingIpv4 from './SettingIpv4.vue'
import SettingSelect from './SettingSelect.vue'
import SettingsAction from './SettingsAction.vue'
import SettingsCollection from './SettingsCollection.vue'

const props = defineProps<{ rows: GenRow[] }>()
const device = useDeviceStore()

/** One indent step, in px. Every level of the hierarchy is worth exactly one. */
const STEP = 16

function pad(level: number) {
  return level > 0 ? { marginLeft: `${level * STEP}px` } : undefined
}

/* Nothing until the dump is in — see useSettingsReady().
 *
 * Indentation is a WALK, not a property of a row: a row's depth is set by the
 * headings above it, which only the sequence knows. `base` is where a level-3
 * section sits (0 on a pane with no group heading, 1 under one) and `content`
 * is where ordinary rows sit. A description goes one step deeper than the rows
 * it sits among, so it reads as an aside about them. */
const ready = useSettingsReady()
const shown = computed<{ row: GenRow; indent: number; wide: boolean }[]>(() => {
  if (!ready.value) return []
  let base = 0
  let content = 0
  /* A caption directly under a HEADING is about the group, so it spans both
   * columns at the heading's own indent, exactly as the heading does. One under
   * a field is about that field, so it starts on the control column. Which of
   * the two it is depends on what came before it — a walk, like the indent. */
  let afterHeading = false
  return props.rows.filter(visible).map((row) => {
    let indent = content
    const wide = row.kind === 'caption' && afterHeading
    afterHeading = row.kind === 'title' || row.kind === 'heading'
                || row.kind === 'section' || (row.kind === 'caption' && afterHeading)
    if (row.kind === 'title') {
      indent = 0
      base = 0
      content = 0
    } else if (row.kind === 'heading') {
      indent = 0
      base = 1
      content = 1
    } else if (row.kind === 'section') {
      indent = base
      content = base + 1
    }
    return { row, indent, wide }
  })
})

const JUSTIFY = { left: 'flex-start', center: 'center', right: 'flex-end' } as const

/* Which disclosure groups are open, keyed the same way rows are. Local state:
 * closing a pane forgets it, which is the right default for "advanced". */
const advOpen = reactive<Record<string, boolean>>({})
function toggleAdv(k: string) { advOpen[k] = !advOpen[k] }

function visible(row: GenRow): boolean {
  return rowVisible(row.whenKey, null)
}

/** What makes this row itself: its storage key where it binds one, its text
 *  where it is furniture, and the position only as a last resort. */
function rowKey(row: GenRow, i: number): string {
  return `${row.kind}:${row.k ?? row.label ?? row.text ?? i}`
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

function copy(text: string) {
  navigator.clipboard?.writeText(text).catch(() => { /* denied or insecure origin */ })
}
</script>

<style scoped>
/* Descriptive text is not a field: smaller than a field name, and italic. It
 * starts on the CONTROL column (marginLeft: NAME_COL, applied inline) because
 * it belongs to the control above it — set against the labels instead, it
 * reads as another label with no control beside it. */
.row-caption {
  font-size: 11px;
  font-style: italic;
  line-height: 1.4;
  opacity: 0.7;
  /* Off the row above it, and clear of the pane's right edge. */
  margin: 6px 16px 0 0;
}
.row-value {
  font-size: 12px;
  overflow-wrap: anywhere;
}
.adv-btn {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 7px 10px;
  border-radius: 6px;
  cursor: pointer;
  background: rgba(255, 255, 255, 0.05);
  font-size: 13px;
}
.adv-btn:hover { background: rgba(255, 255, 255, 0.09); }
.adv-chev { opacity: 0.55; }
.adv-body {
  margin: 6px 0 4px 10px;
  padding-left: 10px;
  border-left: 2px solid rgba(255, 255, 255, 0.12);
}
.value-copyable {
  cursor: pointer;
  user-select: all;
  text-decoration: underline dotted rgba(255, 255, 255, 0.3);
}
.btn-row {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
}
/* The same two columns an ordinary row has (NAME_COL, bound inline) and the
 * same 16px gutter carved out of the name track, so a readout's names end on
 * the pane's boundary and its values start where the controls do. The lines sit
 * against each other — a readout is one block, not a list of rows. */
.info-grid {
  column-gap: 0;
  row-gap: 0;
  display: grid;
  align-items: baseline;
}
.info-label {
  font-size: 12px;
  opacity: 0.7;
  text-align: right;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  padding-right: 16px;
}
.info-value {
  font-size: 12px;
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
