<!--
  NodePane — one settings node: its rows, then its children as navigation
  entries.

  There is no leaf/container distinction any more. A node that only has children
  renders as a list of them, a node that only has rows renders as a panel, and a
  node with both renders both — in that order, always. The root is an ordinary
  node, which is why Settings opens on something instead of on "select a
  setting". A child with nothing to render is not listed: a declared-but-empty
  menu is a name waiting for contributions, not a dead end to walk into.
-->
<template>
  <div class="q-gutter-y-md">
    <SettingRows :rows="node.rows" />

    <div v-if="kids.length" class="node-children">
      <div
        v-for="child in kids"
        :key="child.path"
        class="node-child"
        @click="tree.open(child.path)"
      >
        <span class="node-child-label">{{ child.label }}</span>
        <span class="node-child-chev">›</span>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useSettingsTreeStore, visibleChildren } from '../stores/settingsTree'
import type { SettingsNode } from '../stores/settingsTree'
import SettingRows from './SettingRows.vue'

const props = defineProps<{ node: SettingsNode }>()
const tree = useSettingsTreeStore()
const kids = computed(() => visibleChildren(props.node))
</script>

<style scoped>
.node-children {
  display: flex;
  flex-direction: column;
  gap: 2px;
}
.node-child {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 10px;
  border-radius: 6px;
  cursor: pointer;
  background: rgba(255, 255, 255, 0.04);
  font-size: 14px;
}
.node-child:hover { background: rgba(255, 255, 255, 0.09); }
.node-child-chev { opacity: 0.5; }
</style>
