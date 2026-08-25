<!--
  SettingsFormDialog — the one dialog that carries inputs, because it fronts a
  command sentinel.

  Values are collected LOCALLY and reach the device only on submit. That is what
  replaces per-keystroke validation everywhere: the owning task validates the
  submitted object and answers on two keys — a rejection is a sentence on the
  ERROR key (shown here, dialog stays open), an accepted submission bumps the
  ACK key (dialog closes). No timeouts, no read-back, and no watching the data
  itself: an edit that changes nothing still acks. A collection passes both keys
  down (its sentinels share `<collection-cmd>.error` / `.done`); a bare form
  defaults to `<form-cmd>.error` / `.done`.

  A string `default:` may be a template over sibling fields ("{host}:{port}").
  It tracks them until the operator edits that field, and then it is theirs.

  Every field leaves here as a STRING — a switch as "1"/"0", a slider as its
  number spelled out. The owning task reads the payload with one rule (a JSON
  string or the field is absent), and the on-device form submits the same shape,
  so one parser serves both surfaces.
-->
<template>
  <q-dialog :model-value="true" @update:model-value="close">
    <q-card class="form-card">
      <q-card-section v-if="form.title" class="text-subtitle1">{{ form.title }}</q-card-section>

      <q-card-section class="q-gutter-y-sm">
        <template v-for="(row, i) in displayRows" :key="i">
          <template v-if="visible(row)">
            <PanelHeading v-if="row.kind === 'section' || row.kind === 'heading'">{{ subst(row.text, model) }}</PanelHeading>

            <!-- The disclosure group's button; open, its rows are spliced into
                 displayRows after it, binding the same local model. -->
            <div v-else-if="row.kind === 'advanced'" class="adv-btn" @click="advOpen[row.label ?? ''] = !advOpen[row.label ?? '']">
              <span>{{ row.label ?? 'Advanced Settings' }}</span>
              <span class="adv-chev">{{ advOpen[row.label ?? ''] ? '▾' : '▸' }}</span>
            </div>

            <div
              v-else-if="row.kind === 'caption'"
              class="row-caption"
              :style="captionWide(i) ? undefined : { marginLeft: NAME_COL }"
            >
              <CaptionText :text="subst(row.text, model)" />
            </div>

            <!-- A dialog is fields and little else, so its controls take the
                 whole of the control column rather than the share a pane's do. -->
            <SettingRow v-else :label="row.label">
              <q-toggle
                v-if="row.kind === 'switch'"
                :model-value="truthy(model[row.field!])"
                dense color="primary"
                @update:model-value="(v) => edit(row, v ? '1' : '0')"
              />

              <q-slider
                v-else-if="row.kind === 'slider'"
                :model-value="Number(model[row.field!] ?? row.min ?? 0)"
                :min="bound(row.minKey, row.min ?? 0)"
                :max="bound(row.maxKey, row.max ?? 100)"
                :step="1" dense color="primary"
                @update:model-value="(v) => edit(row, String(v ?? 0))"
              />

              <!-- An integer field over the local buffer. The digits-only
                   filter and the -/+ pair are the pane's; the RANGE is left to
                   the sentinel handler, which is where a form's values are
                   judged — one rejected field and one rejected form should not
                   be answered two different ways. -->
              <div v-else-if="row.kind === 'integer'" class="num-line">
                <q-btn
                  v-if="row.buttons"
                  class="num-step" dense unelevated
                  :style="buttonStyle('grey')" label="−"
                  @click="edit(row, String(stepped(row, -1)))"
                />
                <q-input
                  class="num-field"
                  :model-value="String(model[row.field!] ?? '')"
                  dense outlined
                  inputmode="numeric"
                  autocomplete="off" autocorrect="off" spellcheck="false"
                  v-bind="NO_MANAGER"
                  @beforeinput="numberChars((row.min ?? 0) < 0)"
                  @update:model-value="(v) => edit(row, String(v ?? ''))"
                />
                <span v-if="row.unit" class="unit">{{ row.unit }}</span>
                <q-btn
                  v-if="row.buttons"
                  class="num-step" dense unelevated
                  :style="buttonStyle('grey')" label="+"
                  @click="edit(row, String(stepped(row, +1)))"
                />
              </div>

              <!-- A dotted quad: digits and dots while typing, four octets of
                   0-255 on commit. Empty is accepted and means unset — that is
                   how a fixed address is handed back to DHCP. -->
              <q-input
                v-else-if="row.kind === 'ipv4'"
                :model-value="String(model[row.field!] ?? '')"
                :placeholder="placeholderOf(row)"
                dense outlined
                inputmode="decimal"
                autocomplete="off" autocorrect="off" spellcheck="false"
                v-bind="NO_MANAGER"
                @beforeinput="quadChars"
                @update:model-value="(v) => edit(row, String(v ?? ''))"
                @blur="checkQuad(row)"
              />

              <q-select
                v-else-if="row.kind === 'dropdown'"
                :model-value="String(model[row.field!] ?? '')"
                :options="row.searchable ? (filtered[row.field!] ?? row.options ?? []) : (row.options ?? [])"
                dense outlined emit-value map-options options-dense
                :use-input="row.searchable"
                :input-debounce="0"
                @filter="(v: string, u: (fn: () => void) => void) => filter(row, v, u)"
                @update:model-value="(v) => edit(row, v)"
              />

              <q-select
                v-else-if="row.kind === 'timezone'"
                :model-value="String(model[row.field!] ?? '')"
                :options="filtered[row.field!] ?? tzOptions"
                dense outlined emit-value map-options options-dense
                use-input
                :input-debounce="0"
                @filter="(v: string, u: (fn: () => void) => void) => filter(row, v, u)"
                @update:model-value="(v) => edit(row, v)"
              />

              <q-input
                v-else
                :model-value="String(model[row.field!] ?? '')"
                :type="row.secret ? 'password' : 'text'"
                :placeholder="placeholderOf(row)"
                dense outlined
                :autocomplete="row.secret ? 'new-password' : 'off'"
                autocorrect="off" autocapitalize="off" spellcheck="false"
                v-bind="NO_MANAGER"
                @update:model-value="(v) => edit(row, String(v ?? ''))"
              />
            </SettingRow>
          </template>
        </template>

        <div v-if="error" class="text-negative text-caption">{{ error }}</div>
        <div v-if="quadWarning" class="text-negative text-caption">{{ QUAD_REASON }}</div>
      </q-card-section>

      <q-card-actions align="right">
        <q-btn dense no-caps unelevated :style="buttonStyle('grey')" label="Cancel" @click="close" />
        <q-btn dense no-caps unelevated :style="buttonStyle()" :label="form.submit ?? 'Save'" @click="submit" />
      </q-card-actions>
    </q-card>
  </q-dialog>
</template>

<script setup lang="ts">
import { computed, onUnmounted, reactive, ref, watch } from 'vue'
import { useDeviceStore } from '../stores/device'
import { subst, truthy, rowVisible, buttonStyle, NAME_COL, NO_MANAGER, numberChars, quadChars } from '../lib/settingsRuntime'
import type { GenForm, GenRow, GenOption } from '../lib/settingsNodes'
import PanelHeading from './PanelHeading.vue'
import CaptionText from './CaptionText.vue'
import SettingRow from './SettingRow.vue'

const props = defineProps<{
  form: GenForm
  /** Seed values: a candidate's fields, or the item an editor is editing. */
  prefill?: Record<string, unknown>
  /** The identity an item editor commits against — carried as `_id` so editing
   *  the id field itself is an ordinary edit and the task still knows which
   *  item to apply it to. */
  editId?: string
  /** Where the owning task answers. A collection passes its shared
   *  `<collection-cmd>.error` / `.done`; absent, `<form-cmd>.error` / `.done`. */
  errorKey?: string
  ackKey?: string
}>()
const emit = defineEmits<{ close: [] }>()

const device = useDeviceStore()

/* Where the caret came from, handed back when this dialog goes. The owner
 * closes a form by dropping it from the tree, not by hiding it, so the dialog's
 * own refocus never runs and the caret is left loose on the page — the browser
 * or an extension then picks the field IT likes, and the pane scrolls to
 * wherever that is. One frame later is after the dialog's teardown, whose
 * focus guard would otherwise pull the caret straight back into a dying
 * dialog; preventScroll keeps the handover itself from moving the pane. */
const opener = document.activeElement as HTMLElement | null
onUnmounted(() => {
  requestAnimationFrame(() => {
    if (opener && opener !== document.body && opener.isConnected) {
      opener.focus({ preventScroll: true })
    }
  })
})

const model = reactive<Record<string, unknown>>({})
/** Fields the operator has touched: their template defaults stop tracking. */
const dirty = reactive<Record<string, boolean>>({})

/* Every field row, disclosure groups flattened: the model, the defaults and
 * the submit payload cover an advanced group's fields whether or not it is
 * open — a closed group's untouched fields still submit their defaults. */
function flatFields(rows: GenRow[]): GenRow[] {
  const out: GenRow[] = []
  for (const r of rows) {
    if (r.kind === 'advanced') out.push(...flatFields(r.rows ?? []))
    else out.push(r)
  }
  return out
}
const fieldsFlat = flatFields(props.form.fields)

/* Which disclosure groups are open, and the rows as rendered: a closed
 * group is just its button, an open one has its rows spliced in after it. */
const advOpen = reactive<Record<string, boolean>>({})
const displayRows = computed<GenRow[]>(() => {
  const out: GenRow[] = []
  for (const r of props.form.fields) {
    out.push(r)
    if (r.kind === 'advanced' && advOpen[r.label ?? '']) out.push(...(r.rows ?? []))
  }
  return out
})
const submitted = ref(false)
const filtered = reactive<Record<string, GenOption[]>>({})

/* The timezone picker's list is this client's own Intl database — the yaml
 * declares no options and nothing is fetched from the device. The device
 * still validates the submitted name against its own zone DB, so a zone the
 * device doesn't know comes back as the form's error sentence. */
const intlWithZones = Intl as unknown as { supportedValuesOf?: (k: string) => string[] }
const tzOptions: GenOption[] =
  typeof intlWithZones.supportedValuesOf === 'function'
    ? intlWithZones.supportedValuesOf('timeZone').map(z => ({ value: z, label: z }))
    : []

for (const row of fieldsFlat) {
  if (!row.field) continue
  const seed = props.prefill?.[row.field]
  if (seed !== undefined && seed !== null && seed !== '') {
    model[row.field] = seed
    dirty[row.field] = true
  } else if (row.kind === 'timezone' && row.placeholderKey) {
    /* A picker's "value if left alone" is its current selection: seed it with
     * the applied zone so opening and pressing Set is a no-op edit. */
    model[row.field] = String(device.get(row.placeholderKey) ?? '')
  } else {
    model[row.field] = ''
  }
}
applyDefaults()

/** Untouched template defaults track their siblings, live. */
function applyDefaults() {
  for (const row of fieldsFlat) {
    if (!row.field || !row.dflt || dirty[row.field]) continue
    model[row.field] = subst(row.dflt, model)
  }
}

function edit(row: GenRow, value: unknown) {
  if (!row.field) return
  model[row.field] = value
  dirty[row.field] = true
  applyDefaults()
}

function visible(row: GenRow): boolean {
  return rowVisible(row.whenKey, model)
}

/** A placeholder the device publishes outranks the compiled-in one: it is what
 *  this field would resolve to if left empty, which only the firmware knows. */
function placeholderOf(row: GenRow): string | undefined {
  if (row.placeholderKey) return String(device.get(row.placeholderKey) ?? '')
  return row.placeholder
}

function bound(k: string | undefined, fallback: number): number {
  if (!k) return fallback
  const v = Number(device.get(k))
  return Number.isFinite(v) ? v : fallback
}

/** A caption directly under a heading is about the group and spans both
 *  columns; one under a field starts on the control column, where the field
 *  it describes starts. Only the row order knows which. */
function captionWide(i: number): boolean {
  for (let j = i - 1; j >= 0; j--) {
    const k = displayRows.value[j]?.kind
    if (k === 'caption') continue
    return k === 'section' || k === 'heading' || k === 'title'
  }
  return true
}

/* -- address fields --
 * Four octets of 0-255, or empty. The complaint is a line in the dialog rather
 * than a modal on top of it: the field is right there and still holds what was
 * typed, so there is nothing to explain that the dialog cannot say in place. */
const QUAD_REASON = 'Enter an address like 192.168.1.10, or leave it empty.'
const quadWarning = ref(false)

function checkQuad(row: GenRow) {
  const v = String(model[row.field ?? ''] ?? '')
  const parts = v.split('.')
  quadWarning.value = v !== ''
    && (parts.length !== 4 || !parts.every(p => /^\d{1,3}$/.test(p) && Number(p) <= 255))
}

/* -- integer fields --
 * The effective bounds of one: the number the row states, or the one the
 * device publishes where it names a key for it. Undefined where the row states
 * no bound at all. */
function numBound(row: GenRow, which: 'min' | 'max'): number | undefined {
  const k = which === 'min' ? row.minKey : row.maxKey
  const n = which === 'min' ? row.min : row.max
  if (k) return bound(k, n ?? 0)
  return n
}

/** The next multiple of the field's step past its current value, clamped. It
 *  SNAPS rather than adding: at step 5, down from 23 is 20 and then 15. */
function stepped(row: GenRow, dir: number): number {
  const size = row.step && row.step > 0 ? row.step : 1
  const lo = numBound(row, 'min')
  const hi = numBound(row, 'max')
  const raw = String(model[row.field ?? ''] ?? '')
  const cur = raw.trim() !== '' && Number.isInteger(Number(raw)) ? Number(raw) : (lo ?? 0)
  let next = dir > 0
    ? (Math.floor(cur / size) + 1) * size
    : Math.floor((cur - 1) / size) * size
  if (lo !== undefined && next < lo) next = lo
  if (hi !== undefined && next > hi) next = hi
  return next
}

function filter(row: GenRow, needle: string, update: (fn: () => void) => void) {
  update(() => {
    const all = row.kind === 'timezone' ? tzOptions : (row.options ?? [])
    const n = needle.toLowerCase()
    filtered[row.field ?? ''] = n ? all.filter(o => o.label.toLowerCase().includes(n)) : all
  })
}

const errorKey = computed(() => props.errorKey ?? `${props.form.cmd}.error`)
const ackKey = computed(() => props.ackKey ?? `${props.form.cmd}.done`)
const error = ref('')

/* The owning task's verdict. A reason arriving means the submission was
 * rejected: show it and stay open. The ack key moving after a submit is what
 * acceptance looks like. */
watch(() => device.get(errorKey.value), (v) => {
  const reason = v === undefined || v === null ? '' : String(v)
  if (reason) { error.value = reason; submitted.value = false }
})

watch(() => device.get(ackKey.value), () => {
  if (submitted.value) close()
})

function submit() {
  const payload: Record<string, string> = {}
  for (const row of fieldsFlat) {
    if (!row.field) continue
    const v = model[row.field]
    payload[row.field] = v === undefined || v === null ? '' : String(v)
  }
  if (props.editId) payload._id = props.editId
  error.value = ''
  submitted.value = true
  /* Clear the device-side reason first, in order, so a rejection identical to
   * the last one is still a CHANGE the watch above can see — the storage actor
   * dedups same-value writes. */
  device.set(errorKey.value, '')
  device.set(props.form.cmd, JSON.stringify(payload))
  device.save()
}

function close() { emit('close') }
</script>

<style scoped>
.form-card { min-width: 320px; max-width: 90vw; }
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
/* Descriptive text, set the same way it is in a pane: smaller than a field
 * name, italic, and inset from the column the fields keep. */
.row-caption {
  font-size: 11px;
  font-style: italic;
  line-height: 1.4;
  opacity: 0.7;
  margin: 0 16px;
}
/* An integer field is as wide as the numbers it holds, with its steppers
 * against it — the same shape the pane's own integer row has. */
.num-line {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}
.num-field { flex: 0 0 auto; width: 90px; min-width: 90px; }
.unit { font-size: 12px; opacity: 0.7; white-space: nowrap; }
.num-step  { flex: 0 0 auto; min-width: 28px; padding: 0 6px; }
</style>
