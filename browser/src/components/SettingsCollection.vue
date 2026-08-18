<!--
  SettingsCollection — an array-of-objects in storage, as an editable list.

  It NEVER writes the array. Every mutation is a write to a command sentinel
  under `cmd` — `.add`, `.remove`, `.set`, `.order` — and the owning task is the
  array's only writer, which is what keeps this surface and the device's own
  identical and what makes rejection a message rather than a race.

  Reordering writes the complete id order to `<cmd>.order` as a comma-joined
  list, and the firmware treats it as a PREFERENCE PERMUTATION: recognized ids
  are moved into that relative order, unknown ids ignored, unmentioned ids left
  in place. That makes a drag idempotent and benign against a concurrent edit,
  so the list can hold an optimistic order until the re-published array lands.
-->
<template>
  <div class="q-gutter-y-xs">
    <PanelHeading v-if="row.label">{{ row.label }}</PanelHeading>

    <div
      v-for="(item, idx) in ordered"
      :key="String(item[row.id!] ?? idx)"
      class="coll-item"
      :draggable="row.orderable"
      @dragstart="dragFrom = idx"
      @dragover.prevent
      @drop="drop(idx)"
    >
      <div class="coll-text">
        <div class="coll-title">{{ subst(row.item, item) }}</div>
        <div v-if="row.subtitle" class="coll-sub">{{ subst(row.subtitle, item) }}</div>
      </div>

      <span v-if="pill(item).text" class="coll-pill" :style="{ background: pill(item).color }">
        {{ pill(item).text }}
      </span>

      <SettingsAction
        v-for="(a, i) in (row.actions ?? []).filter((x) => itemActionVisible(x.whenKey, withId(item)))"
        :key="i"
        :label="a.label"
        :action="a.do"
        :color="a.color"
        :scope="withId(item)"
      />

      <q-btn v-if="row.edit?.length" dense flat round size="sm" icon="edit" @click="editing = item" />
      <q-btn v-if="row.remove" dense flat round size="sm" @click="askRemove(item)"><IconTrash /></q-btn>
    </div>

    <div v-if="!ordered.length && row.empty" class="text-caption" style="opacity:0.6">
      {{ row.empty }}
    </div>

    <!-- The collection's own buttons on one line, gathered right: scanning
         first, because finding a thing is what you reach for before describing
         one by hand. -->
    <div v-if="row.candidates?.refresh || row.add?.length" class="coll-actions">
      <q-btn
        v-if="row.candidates?.refresh"
        dense no-caps outline color="primary"
        :label="row.candidates.refresh.label"
        @click="openScan()"
      />
      <q-btn
        v-for="(a, i) in row.add ?? []"
        :key="i"
        dense no-caps outline color="primary"
        :label="a.label"
        @click="adding = a.form"
      />
    </div>

    <!-- Candidates: what the device can SEE, which is a different question from
         what it is configured for, and a transient answer to it. So they are a
         popup rather than part of the pane — opening it starts the scan and
         closing it stops the scan. -->
    <template v-if="row.candidates">
      <q-dialog :model-value="scanOpen" @update:model-value="(v) => { if (!v) closeScan() }">
        <q-card class="coll-card">
          <q-card-section class="scan-head">
            <div class="scan-title">
              {{ row.candidates.found ?? row.candidates.refresh?.label }}
            </div>
            <q-btn dense flat no-caps size="sm" label="Close" @click="closeScan()" />
          </q-card-section>
          <q-card-section class="scan-list">
            <div v-if="!candidates.length" class="text-caption" style="opacity:0.6">
              Scanning…
            </div>
            <div
              v-for="(c, i) in candidates"
              :key="'c' + i"
              class="coll-item coll-candidate"
              @click="adopt(c)"
            >
              <div class="coll-text">
                <div class="coll-title">{{ subst(row.candidates.item, c) }}</div>
                <div v-if="row.candidates.subtitle" class="coll-sub">
                  {{ subst(row.candidates.subtitle, c) }}
                </div>
              </div>
            </div>
          </q-card-section>
        </q-card>
      </q-dialog>
    </template>

    <q-dialog v-if="removing" :model-value="true" @update:model-value="removing = null">
      <q-card class="coll-card">
        <q-card-section>{{ subst(row.remove?.confirm, removing) }}</q-card-section>
        <q-card-actions align="right">
          <q-btn flat no-caps label="Cancel" @click="removing = null" />
          <q-btn flat no-caps color="negative" label="Remove" @click="doRemove()" />
        </q-card-actions>
      </q-card>
    </q-dialog>

    <!-- Both dialogs answer on the collection's shared keys: every sentinel of
         one collection reports on `<cmd>.error` / `<cmd>.done`. -->
    <SettingsFormDialog
      v-if="adding"
      :form="adding"
      :prefill="prefill"
      :error-key="`${row.cmd}.error`"
      :ack-key="`${row.cmd}.done`"
      @close="adding = null; prefill = undefined"
    />

    <SettingsFormDialog
      v-if="editing"
      :form="editForm"
      :prefill="editing"
      :edit-id="String(editing[row.id!] ?? '')"
      :error-key="`${row.cmd}.error`"
      :ack-key="`${row.cmd}.done`"
      @close="editing = null"
    />
  </div>
</template>

<script setup lang="ts">
import { computed, ref, watch, onUnmounted } from 'vue'
import { useDeviceStore } from '../stores/device'
import { useSettingsTreeStore } from '../stores/settingsTree'
import { subst, paletteColor, itemActionVisible, runSet, asItems } from '../lib/settingsRuntime'
import type { GenRow, GenForm } from '../lib/settingsNodes'
import PanelHeading from './PanelHeading.vue'
import SettingsAction from './SettingsAction.vue'
import SettingsFormDialog from './SettingsFormDialog.vue'
import IconTrash from './IconTrash.vue'

type Item = Record<string, unknown>

const props = defineProps<{ row: GenRow }>()
const device = useDeviceStore()

const adding = ref<GenForm | null>(null)
const editing = ref<Item | null>(null)
const removing = ref<Item | null>(null)
const prefill = ref<Item | undefined>(undefined)
const dragFrom = ref<number | null>(null)
const scanOpen = ref(false)
/** The order a drag put on screen, held until the array is re-published. */
const optimistic = ref<string[] | null>(null)

const items = computed<Item[]>(() => asItems(device.get(props.row.k!)))

const ordered = computed<Item[]>(() => {
  const list = items.value
  const want = optimistic.value
  if (!want) return list
  const byId = new Map(list.map(i => [String(i[props.row.id!] ?? ''), i]))
  const out: Item[] = []
  for (const id of want) { const it = byId.get(id); if (it) { out.push(it); byId.delete(id) } }
  return [...out, ...byId.values()]
})

/* The device published a new array: it has the final say on order. */
watch(items, () => { optimistic.value = null })

const candidates = computed<Item[]>(() => {
  const k = props.row.candidates?.k
  return k ? asItems(device.get(k)) : []
})

/* Leaving the pane stops the scan. The refresh target is a plain key, so
 * clearing it here is the whole "stop scanning on leave" contract — no straddle
 * carries a visibility timer for it. Unmounting covers closing the window or
 * navigating away; the active-path watch covers a switch between two nodes that
 * Vue may resolve by patching rather than remounting. */
function stopScanning() {
  const key = props.row.candidates?.refresh?.do.set?.key
  if (key) device.set(key, 0)
}
onUnmounted(stopScanning)
const tree = useSettingsTreeStore()
watch(() => tree.activePath, () => { scanOpen.value = false; stopScanning() })

/** The scan runs for exactly as long as its popup is up. */
function openScan() {
  const refresh = props.row.candidates?.refresh
  if (!refresh) return
  scanOpen.value = true
  if (refresh.do.set) runSet(refresh.do.set)
}

function closeScan() {
  if (!scanOpen.value) return
  scanOpen.value = false
  stopScanning()
}

/** `{id}` resolves to the value of whichever field identifies an item. */
function withId(item: Item): Item {
  return { ...item, id: item[props.row.id!] }
}

/** The status pill: the key holds packed "text|color", both halves finished by
 *  the firmware — this neither maps states to words nor picks their colour. */
function pill(item: Item): { text: string; color: string } {
  if (!props.row.status) return { text: '', color: '' }
  const raw = device.get(subst(props.row.status, withId(item)))
  const [text = '', color = ''] = String(raw ?? '').split('|')
  return { text, color: paletteColor(color) }
}

/** The item editor is the collection's `edit:` rows over `<cmd>.set`, so the
 *  sentinel family stays derived from the one `cmd` name. */
const editForm = computed<GenForm>(() => ({
  fields: props.row.edit ?? [],
  cmd: `${props.row.cmd}.set`,
  title: props.row.label,
}))

function askRemove(item: Item) {
  if (props.row.remove?.confirm) { removing.value = item; return }
  send('remove', String(item[props.row.id!] ?? ''))
}

function doRemove() {
  const item = removing.value
  removing.value = null
  if (item) send('remove', String(item[props.row.id!] ?? ''))
}

function send(sentinel: string, value: string) {
  device.set(`${props.row.cmd}.${sentinel}`, value)
  device.save()
}

function drop(to: number) {
  const from = dragFrom.value
  dragFrom.value = null
  if (from === null || from === to) return
  const ids = ordered.value.map(i => String(i[props.row.id!] ?? ''))
  const [moved] = ids.splice(from, 1)
  ids.splice(to, 0, moved)
  optimistic.value = ids
  send('order', ids.join(','))
}

/** Adopting a candidate opens the first add form, prefilled: same-name fields
 *  carry over implicitly, `map:` covers only the renames. */
function adopt(c: Item) {
  const cand = props.row.candidates
  const first = props.row.add?.[0]
  if (!cand || !first) return
  const out: Item = {}
  for (const field of first.form.fields) {
    if (!field.field) continue
    let from = field.field
    for (const [k, v] of Object.entries(cand.map ?? {})) if (v === field.field) from = k
    if (c[from] !== undefined) out[field.field] = c[from]
  }
  /* Picking one answers the question the scan asked, so the popup goes and the
   * scan with it, and the add form opens over a clear screen. */
  closeScan()
  prefill.value = out
  adding.value = first.form
}
</script>

<style scoped>
.coll-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 4px 0;
  min-width: 0;
}
.coll-actions { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 8px; }
.scan-head { display: flex; align-items: center; gap: 8px; }
.scan-title {
  flex: 1;
  min-width: 0;
  font-weight: 600;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.scan-list { max-height: 50vh; overflow-y: auto; }
.coll-candidate { cursor: pointer; opacity: 0.85; }
.coll-candidate:hover { opacity: 1; }
.coll-text { flex: 1; min-width: 0; }
.coll-title { font-size: 13px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.coll-sub {
  font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
  font-size: 11px;
  opacity: 0.6;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.coll-pill {
  border-radius: 6px;
  padding: 1px 6px;
  font-size: 11px;
  color: #fff;
  white-space: nowrap;
}
</style>
