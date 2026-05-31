<template>
  <FloatingWindow
    :id="id"
    :title="title"
    :visible="visible"
    :default-geom="defaultGeom"
    :can-dock="false"
    :min-size="{ w: 20, h: 12 }"
    @update:visible="onVisibleChange"
  >
    <template #titlebar-right>
      <span class="ed-zoom-btn" @click="zoomOut">-</span>
      <span class="ed-zoom-btn" @click="zoomIn">+</span>
    </template>

    <template #default>
      <div class="ed-body">
        <div class="ed-toolbar">
          <button class="ed-btn ed-save" :disabled="!loaded || saving"
            @click="askSave">Save</button>
          <button class="ed-btn ed-discard" :disabled="!loaded"
            @click="askDiscard">Discard Changes</button>
          <span class="ed-status">
            {{ statusText }}
          </span>
        </div>
        <textarea
          v-model="text"
          class="ed-textarea"
          :style="{ fontSize: fontSize + 'px' }"
          spellcheck="false"
          autocorrect="off"
          autocapitalize="off"
          :readonly="!loaded"
        />

        <!-- Confirm dialogs (q-dialog teleports to body, so nesting is purely
             organizational — keeps the component a single-root template). -->
        <q-dialog v-model="showSaveConfirm" persistent>
          <q-card dark class="q-pa-md" style="min-width:280px">
            <q-card-section>
              <div class="text-subtitle1 text-weight-medium">Save changes?</div>
              <div class="text-caption q-mt-sm" style="opacity:0.8">
                Write {{ path }} on the device. The window will close after saving.
              </div>
            </q-card-section>
            <q-card-actions align="right">
              <q-btn flat label="Cancel" @click="showSaveConfirm = false" />
              <q-btn flat label="Save" color="primary" @click="confirmSave" />
            </q-card-actions>
          </q-card>
        </q-dialog>

        <q-dialog v-model="showDiscardConfirm" persistent>
          <q-card dark class="q-pa-md" style="min-width:280px">
            <q-card-section>
              <div class="text-subtitle1 text-weight-medium">Discard changes?</div>
              <div class="text-caption q-mt-sm" style="opacity:0.8">
                Unsaved edits to {{ path }} will be lost. The window will close.
              </div>
            </q-card-section>
            <q-card-actions align="right">
              <q-btn flat label="Cancel" @click="showDiscardConfirm = false" />
              <q-btn flat label="Discard" color="negative" @click="confirmDiscard" />
            </q-card-actions>
          </q-card>
        </q-dialog>
      </div>
    </template>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { ref, computed, onMounted } from 'vue'
import FloatingWindow from './FloatingWindow.vue'
import { layout } from '../modules/advanced'

const props = defineProps<{
  id: string
  path: string
  title: string
  visible: boolean
}>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()
function close() { emit('update:visible', false) }

/* 2/3 × 2/3 of the free canvas (area not occupied by docked windows),
 * centered. Captured once at mount; FloatingWindow's clamp watcher will
 * keep us inside the floating area if docks change later. */
const defaultGeom = (() => {
  const a = layout.value.floatingArea
  const aw = a.right - a.left
  const ah = a.bottom - a.top
  const w = aw * 2 / 3
  const h = ah * 2 / 3
  return { x: a.left + (aw - w) / 2, y: a.top + (ah - h) / 2, w, h }
})()

/* Font size persisted globally for all editor windows. */
const BASE_FONT = 14
const ZOOM_KEY = 'spangap.win.editor.zoom'
const stored = localStorage.getItem(ZOOM_KEY)
const zoom = ref(stored !== null ? (Number(stored) || 0) : 0)
const fontSize = computed(() => Math.max(8, BASE_FONT + zoom.value * 2))
function persistZoom() {
  try { localStorage.setItem(ZOOM_KEY, String(zoom.value)) } catch { /* */ }
}
function zoomIn()  { zoom.value = Math.min(zoom.value + 1, 10); persistZoom() }
function zoomOut() { zoom.value = Math.max(zoom.value - 1, -5); persistZoom() }

const text = ref('')
const original = ref('')
const loaded = ref(false)
const loadError = ref('')
const saving = ref(false)
const saveError = ref('')

const dirty = computed(() => loaded.value && text.value !== original.value)

const statusText = computed(() => {
  if (saveError.value) return saveError.value
  if (saving.value) return 'Saving…'
  if (loadError.value) return loadError.value
  if (!loaded.value) return 'Loading…'
  if (dirty.value) return 'Modified'
  return ''
})

/* Files are served + PUT-able directly via the /state web wiring (WebDAV).
 * props.path is e.g. "/state/boot" → fetch as-is (GET reads, PUT writes). */
const apiUrl = computed(() => props.path)

async function loadFile() {
  loaded.value = false
  loadError.value = ''
  try {
    const resp = await fetch(apiUrl.value, { credentials: 'same-origin' })
    if (resp.status === 404) {
      /* New / missing file — open as empty so the user can create it. */
      text.value = ''
      original.value = ''
    } else if (!resp.ok) {
      loadError.value = `Error ${resp.status}`
      return
    } else {
      const body = await resp.text()
      text.value = body
      original.value = body
    }
    loaded.value = true
  } catch (e: unknown) {
    loadError.value = `Load failed: ${(e as Error)?.message ?? 'network error'}`
  }
}

const showSaveConfirm = ref(false)
const showDiscardConfirm = ref(false)

function askSave() {
  if (!loaded.value || saving.value) return
  showSaveConfirm.value = true
}
function askDiscard() {
  if (!loaded.value) return
  showDiscardConfirm.value = true
}

async function confirmSave() {
  showSaveConfirm.value = false
  if (saving.value) return
  saving.value = true
  saveError.value = ''
  try {
    const resp = await fetch(apiUrl.value, {
      method: 'PUT',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'text/plain' },
      body: text.value,
    })
    if (!resp.ok) {
      saveError.value = `Save failed: HTTP ${resp.status}`
      saving.value = false
      return
    }
  } catch (e: unknown) {
    saveError.value = `Save failed: ${(e as Error)?.message ?? 'network error'}`
    saving.value = false
    return
  }
  saving.value = false
  close()
}

function confirmDiscard() {
  showDiscardConfirm.value = false
  close()
}

/* Title-bar X. If clean, just close. If dirty, route through the discard
 * confirm so changes aren't lost on a stray click. */
function onVisibleChange(v: boolean) {
  if (v) return
  if (dirty.value) {
    showDiscardConfirm.value = true
    return
  }
  close()
}

onMounted(() => { void loadFile() })
</script>

<style scoped>
.ed-zoom-btn {
  width: 18px; height: 18px; display: flex; align-items: center; justify-content: center;
  border-radius: 4px; font-size: 14px; font-weight: 700;
  color: rgba(255,255,255,0.5); cursor: pointer; font-family: system-ui; line-height: 1;
}
.ed-zoom-btn:hover { color: rgba(255,255,255,0.9); background: rgba(255,255,255,0.1); }

/* Uniform 5px black frame around the editor body — FloatingWindow's default
 * .fw-body padding is `0 5px` (no top/bottom). Add the missing top + bottom
 * so the white editor sits inside a consistent black bumper. */
:deep(.fw-body) { padding: 5px; overflow: hidden; }

.ed-body {
  width: 100%; height: 100%;
  display: flex; flex-direction: column;
  background: #ffffff; color: #000000;
  overflow: hidden;
}

.ed-toolbar {
  display: flex; align-items: center; gap: 8px;
  padding: 6px 10px;
  background: #e8e8e8;
  border-bottom: 1px solid #c8c8c8;
  flex: 0 0 auto;
}

.ed-btn {
  font-family: system-ui;
  font-size: 13px;
  font-weight: 500;
  padding: 4px 12px;
  border-radius: 4px;
  border: 1px solid #b0b0b0;
  background: #f5f5f5;
  color: #1a1a1a;
  cursor: pointer;
}
.ed-btn:hover:not(:disabled) { background: #ffffff; border-color: #888; }
.ed-btn:disabled { opacity: 0.45; cursor: not-allowed; }
.ed-save:not(:disabled)    { background: #2a6fc4; color: #fff; border-color: #2658a0; }
.ed-save:not(:disabled):hover { background: #3b82d9; }

.ed-status {
  margin-left: auto;
  font-family: system-ui;
  font-size: 12px;
  color: #555;
}

.ed-textarea {
  flex: 1 1 auto;
  width: 100%;
  border: none;
  outline: none;
  resize: none;
  padding: 8px 10px;
  background: #ffffff;
  color: #000000;
  font-family: 'SF Mono', 'Menlo', 'Monaco', 'Consolas', 'Liberation Mono', 'Courier New', monospace;
  line-height: 1.35;
  white-space: pre;
  overflow: auto;
  tab-size: 4;
}
</style>
