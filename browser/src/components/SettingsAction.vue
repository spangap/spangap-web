<!--
  SettingsAction — a button that runs a declarative action, and the dialogs an
  action can open.

  Three kinds and no more: write a key, ask a question, or collect fields for a
  sentinel. Dialog buttons nest actions, so a choice tree (a reset-scope picker,
  say) is dialogs of buttons of writes — which is why there is a recursion here
  and no special case for "confirm then do".
-->
<template>
  <!-- Filled with the palette colour, the way a pill is — see buttonStyle(). -->
  <q-btn
    dense no-caps unelevated
    :style="buttonStyle(color)"
    :label="label"
    @click="run(action)"
  />

  <q-dialog v-if="dialog" :model-value="true" @update:model-value="dialog = null">
    <q-card class="action-card">
      <q-card-section>{{ subst(dialog.text, scope) }}</q-card-section>
      <q-card-actions align="right" class="column items-stretch">
        <q-btn
          v-for="(b, i) in dialog.buttons"
          :key="i"
          dense no-caps unelevated
          class="action-choice"
          :style="buttonStyle(b.color)"
          :label="b.label"
          @click="pick(b)"
        />
      </q-card-actions>
    </q-card>
  </q-dialog>

  <SettingsFormDialog
    v-if="form"
    :form="form"
    :prefill="scope ?? undefined"
    @close="form = null"
  />
</template>

<script setup lang="ts">
import { ref } from 'vue'
import { subst, runSet, buttonStyle } from '../lib/settingsRuntime'
import type { GenAction, GenDialog, GenDialogButton, GenForm } from '../lib/settingsNodes'
import SettingsFormDialog from './SettingsFormDialog.vue'

const props = defineProps<{
  label: string
  action: GenAction
  /** A palette name ("red", "amber", …) or an explicit "rrggbb". */
  color?: string
  /** The collection item this action is scoped to, for `{field}` substitution. */
  scope?: Record<string, unknown> | null
}>()

const dialog = ref<GenDialog | null>(null)
const form = ref<GenForm | null>(null)

function run(a: GenAction | undefined) {
  if (!a) return
  if (a.set) runSet(a.set, props.scope)
  else if (a.dialog) dialog.value = a.dialog
  else if (a.form) form.value = a.form
}

/* Every button closes the dialog; a button with no action is a cancel. */
function pick(b: GenDialogButton) {
  dialog.value = null
  run(b.do)
}
</script>

<style scoped>
.action-card { min-width: 300px; max-width: 90vw; }
/* Stacked choices: each is a button in its own right, not a line of text. */
.action-choice { margin: 4px 0 0; padding: 4px 12px; }
</style>
