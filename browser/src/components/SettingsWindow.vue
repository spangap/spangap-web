<!--
  SettingsWindow — Settings as a first-class app window (the gear dock icon).

  Desktop: two panes — a nav rail (the settings tree) on the left, the selected
  node on the right. Phone: the FloatingWindow is full-screen and we drill down
  — the nav fills the window until a node is picked, then the pane replaces it
  with a "‹ Settings" back affordance.

  The right half always has something in it: the root is an ordinary node, so
  opening Settings lands on it rather than on an instruction to pick something.
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
      <div v-if="!compact || atRoot" class="settings-nav">
        <SettingsNavTree :nodes="tree.root.children" :active="tree.activePath" />
      </div>

      <div v-if="!compact || !atRoot" class="settings-pane">
        <div v-if="compact && !atRoot" class="settings-back" @click="tree.close()">‹ Settings</div>
        <q-scroll-area class="settings-scroll">
          <div class="settings-scroll-inner">
            <NodePane :node="tree.activeNode" />
          </div>
        </q-scroll-area>
      </div>
    </div>
  </FloatingWindow>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useSettingsTreeStore, ROOT_PATH } from '../stores/settingsTree'
import { useCompact } from '../lib/viewport'
import FloatingWindow from './FloatingWindow.vue'
import SettingsNavTree from './SettingsNavTree.vue'
import NodePane from './NodePane.vue'

defineProps<{ visible: boolean; focusToken?: number }>()
const emit = defineEmits<{ 'update:visible': [value: boolean] }>()

const tree = useSettingsTreeStore()
const compact = useCompact()

const atRoot = computed(() => tree.activePath === ROOT_PATH)
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
/* Phone: nav fills the full-screen window. */
.fw--compact .settings-nav { flex: 1; max-width: none; border-right: none; }
</style>
