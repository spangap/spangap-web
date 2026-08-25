<!--
  SettingRow — one row of a settings pane: the name, then the control.

  A GRID of two columns, not a flex line, and one component so that every row
  in a pane is laid out by the same rule: a control that brought its own idea of
  width (a slider's track, an input's box) can no longer land anywhere but in
  the control column. Names are right-aligned against the boundary, so a column
  of them ends where the controls begin.
-->
<template>
  <div class="set-row" :style="{ gridTemplateColumns: `${NAME_COL} 1fr` }">
    <div class="set-name">{{ label }}</div>
    <div class="set-control"><slot /></div>
  </div>
</template>

<script setup lang="ts">
import { NAME_COL } from '../lib/settingsRuntime'

defineProps<{ label?: string }>()
</script>

<style scoped>
.set-row {
  display: grid;
  align-items: center;
}
.set-name {
  font-size: 12px;
  line-height: 1.35;
  opacity: 0.8;
  text-align: right;
  /* The gutter between the columns, carved out of the name's own track so the
   * controls still start exactly on the boundary. */
  padding-right: 16px;
  overflow-wrap: anywhere;
}
.set-control {
  min-width: 0;
}

/* A text box or a dropdown is a box, not a bar: it takes a share of the control
 * column and leaves the rest, and it is set to the height of the one line it
 * holds. `set-field` is the class a control asks for this with; the rules reach
 * into Quasar's own parts, which is what a height and a padding of its fields
 * take. */
.set-control :deep(.set-field) {
  width: 30%;
  /* Below this a field is a box too small to read what is in it, whatever the
   * pane's width says. */
  min-width: 120px;
  max-width: 100%;
}
/* A third of that, for an entry that is a handful of characters. A field the
 * pane has to guess the length of is better guessed short: a wide box invites
 * a long answer that will not fit anywhere else. */
.set-control :deep(.set-field--short) {
  width: 10%;
  min-width: 64px;
}
/* A field with a word after it right-aligns, so the entry and the word read as
 * one thing instead of drifting apart across the box. */
.set-control :deep(.set-field--right input) {
  text-align: right;
}
/* A picker is sized to its options, not stretched across the column: one as
 * wide as the pane looks like a text field, and a column of them of differing
 * widths says at a glance which choices are small ones. */
.set-control :deep(.set-field--fit) {
  width: auto;
  min-width: 0;
  display: inline-block;
}
.set-control :deep(.set-field .q-field__control),
.set-control :deep(.set-field .q-field__marginal) {
  height: 28px;
  min-height: 28px;
}
.set-control :deep(.set-field .q-field__native),
.set-control :deep(.set-field .q-field__input) {
  padding: 3px 0;
  line-height: 22px;
  min-height: 22px;
}
</style>
