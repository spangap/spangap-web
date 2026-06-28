<!--
  SettingsWindow — Settings as a first-class app window (the gear dock icon),
  replacing the old left drawer.

  Desktop: two panes — a nav rail (the settings tree) on the left, the selected
  panel on the right. Phone: the FloatingWindow is full-screen and we drill down
  — the nav fills the window until a leaf is picked, then the panel replaces it
  with a "‹ Settings" back affordance.

  Generated panels (GeneratedPanel.vue) and hand-written panes both register
  under the 'settings/…' menu path and render here unchanged.
-->
<template>
  <FloatingWindow
    id="settings"
    title="Settings"
    :visible="visible"
    :focus-token="focusToken"
    :default-geom="{ x: 18, y: 10, w: 64, h: 78 }"
    @update:visible="(v) => emit('update:visible', v)"
  >
    <div class="settings-window">
      <div v-if="!compact || !activePanel" class="settings-nav">
        <SettingsNavTree :items="rootItems" :active="activePanel" @select="select" />
      </div>

      <div v-if="!compact || activePanel" class="settings-pane">
        <div v-if="compact && activePanel" class="settings-back" @click="back">‹ Settings</div>
        <q-scroll-area class="settings-scroll">
          <div class="settings-scroll-inner">
            <component :is="activeComponent" v-if="activeComponent" />
            <div v-else class="settings-empty">Select a setting from the list.</div>
          </div>
        </q-scroll-area>
      </div>
    </div>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useMenuStore } from '../stores/menu'
import { useCompact } from '../lib/viewport'
import FloatingWindow from './FloatingWindow.vue'
import SettingsNavTree from './SettingsNavTree.vue'

defineProps<{ visible: boolean; focusToken?: number }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const menu = useMenuStore()
const compact = useCompact()

/* The 'settings' top-level group's children (System / Internet / Mesh Network…). */
const rootItems = computed(() => {
  const g = menu.menus.get('settings')
  return g ? g.items : []
})

const activePanel = computed(() => menu.activePanel)
const activeComponent = computed(() => menu.activePanelComponent)

function select(id: string) { menu.openPanel(id) }
function back() { menu.closePanel() }
</script>

<style scoped>
.settings-window {
  display: flex;
  height: 100%;
  width: 100%;
  color: #fff;
}
.settings-nav {
  flex: 0 0 38%;
  max-width: 260px;
  min-width: 140px;
  overflow-y: auto;
  padding: 8px;
  border-right: 1px solid rgba(255, 255, 255, 0.12);
}
.settings-pane {
  flex: 1;
  min-width: 0;
  display: flex;
  flex-direction: column;
}
.settings-back {
  padding: 8px 12px;
  color: #4aa3ff;
  cursor: pointer;
  font-size: 14px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.12);
}
.settings-scroll { flex: 1; }
.settings-scroll-inner { padding: 14px; }
.settings-empty {
  padding: 24px;
  color: rgba(255, 255, 255, 0.5);
  font-size: 14px;
}
/* Phone: nav fills the full-screen window. */
.fw--compact .settings-nav { flex: 1; max-width: none; border-right: none; }
</style>
