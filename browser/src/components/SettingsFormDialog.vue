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
        <template v-for="(row, i) in form.fields" :key="i">
          <template v-if="visible(row)">
            <PanelHeading v-if="row.kind === 'section'">{{ subst(row.text, model) }}</PanelHeading>

            <div v-else-if="row.kind === 'caption'" class="text-caption" style="opacity:0.7">
              {{ subst(row.text, model) }}
            </div>

            <div v-else class="row items-center no-wrap">
              <div class="col-4 text-caption">{{ row.label }}</div>

              <q-toggle
                v-if="row.kind === 'switch'"
                class="col"
                :model-value="truthy(model[row.field!])"
                dense color="primary"
                @update:model-value="(v) => edit(row, v ? '1' : '0')"
              />

              <q-slider
                v-else-if="row.kind === 'slider'"
                class="col"
                :model-value="Number(model[row.field!] ?? row.min ?? 0)"
                :min="bound(row.minKey, row.min ?? 0)"
                :max="bound(row.maxKey, row.max ?? 100)"
                :step="1" dense color="primary"
                @update:model-value="(v) => edit(row, String(v ?? 0))"
              />

              <q-select
                v-else-if="row.kind === 'dropdown'"
                class="col"
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
                class="col"
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
                class="col"
                :model-value="String(model[row.field!] ?? '')"
                :type="row.secret ? 'password' : 'text'"
                :placeholder="placeholderOf(row)"
                dense outlined
                :autocomplete="row.secret ? 'new-password' : 'off'"
                autocorrect="off" autocapitalize="off" spellcheck="false"
                v-bind="NO_MANAGER"
                @update:model-value="(v) => edit(row, String(v ?? ''))"
              />
            </div>
          </template>
        </template>

        <div v-if="error" class="text-negative text-caption">{{ error }}</div>
      </q-card-section>

      <q-card-actions align="right">
        <q-btn flat no-caps label="Cancel" @click="close" />
        <q-btn unelevated no-caps color="primary" :label="form.submit ?? 'Save'" @click="submit" />
      </q-card-actions>
    </q-card>
  </q-dialog>
</template>

<script setup lang="ts">
import { computed, onUnmounted, reactive, ref, watch } from 'vue'
import { useDeviceStore } from '../stores/device'
import { subst, truthy, rowVisible, NO_MANAGER } from '../lib/settingsRuntime'
import type { GenForm, GenRow, GenOption } from '../lib/settingsNodes'
import PanelHeading from './PanelHeading.vue'

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

for (const row of props.form.fields) {
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
  for (const row of props.form.fields) {
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
  for (const row of props.form.fields) {
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
</style>
