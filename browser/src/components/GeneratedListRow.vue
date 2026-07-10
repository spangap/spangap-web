<!--
  GeneratedListRow — the web half of a declarative `list` row.

  Display + intent only: it renders the array at `row.k` (one entry per item,
  titled by the `itemLabel` template over the item's fields) and, on a button,
  writes a command sentinel (`row.add` / `row.remove`). It NEVER mutates the
  array itself — the owning straddle subscribes to those command keys and
  performs the real add/delete (validating, popping an add dialog, rewriting the
  array). This keeps LCD and web symmetric: both just storageSet the cmd key.
-->
<template>
  <div class="q-gutter-y-xs">
    <div v-for="(item, idx) in items" :key="idx" class="row items-center no-wrap">
      <div class="col text-body2">{{ itemTitle(item) }}</div>
      <q-btn
        v-if="row.remove"
        dense
        flat
        round
        size="sm"
        @click="remove(item, idx)"
      ><IconTrash /></q-btn>
    </div>
    <div v-if="!items.length" class="text-caption" style="opacity:0.6">(none)</div>
    <q-btn
      v-if="row.add"
      dense
      no-caps
      outline
      color="primary"
      label="+ Add"
      @click="add"
    />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useDeviceStore } from '../stores/device'
import IconTrash from './IconTrash.vue'
import type { GenRow } from '../lib/generatedPanels'

const props = defineProps<{ row: GenRow }>()
const device = useDeviceStore()

const items = computed<Record<string, unknown>[]>(() => {
  const v = device.get(props.row.k!)
  return Array.isArray(v) ? v : []
})

function itemTitle(item: Record<string, unknown>): string {
  const tmpl = props.row.itemLabel ?? ''
  if (!tmpl) return JSON.stringify(item)
  return tmpl.replace(/\{(\w+)\}/g, (_m, f: string) => {
    const val = item?.[f]
    return val === undefined || val === null ? '' : String(val)
  })
}

function add() {
  if (props.row.add) device.set(props.row.add, '1')
}

function remove(item: Record<string, unknown>, idx: number) {
  // Write the item's identity (its rendered title) so a subscriber can find it;
  // falls back to the index when there's no template.
  if (props.row.remove) device.set(props.row.remove, props.row.itemLabel ? itemTitle(item) : String(idx))
}
</script>
