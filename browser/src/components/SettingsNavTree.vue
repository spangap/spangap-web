<!--
  SettingsNavTree — recursive in-window navigation for the Settings app.

  Every node is selectable — a node with children is not a mere heading, it is a
  pane in its own right that happens to also lead somewhere — so each row both
  opens its node and, when it has children, renders them indented beneath.
-->
<template>
  <div class="nav-tree">
    <template v-for="node in shown" :key="node.path">
      <div
        class="nav-node"
        :class="{
          'nav-node--active': node.path === active,
          'nav-node--group': kidsOf(node).length > 0,
        }"
        @click="tree.open(node.path)"
      >
        <span v-fit-text class="nav-node-label">{{ node.label }}</span>
      </div>
      <SettingsNavTree
        v-if="kidsOf(node).length"
        class="nav-children"
        :nodes="kidsOf(node)"
        :active="active"
      />
    </template>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useSettingsTreeStore, nodeRenders, visibleChildren } from '../stores/settingsTree'
import type { SettingsNode } from '../stores/settingsTree'
import { vFitText } from '../lib/fitText'

const props = defineProps<{ nodes: SettingsNode[]; active: string | null }>()
const tree = useSettingsTreeStore()

/* A menu nobody has contributed to is a name, not a destination — it stays out
 * of the rail until it holds something, here and in the pane alike. */
const shown = computed(() => props.nodes.filter(nodeRenders))
const kidsOf = visibleChildren
</script>

<style scoped>
.nav-children { margin-left: 10px; }
.nav-node {
  padding: 6px 10px;
  border-radius: 6px;
  cursor: pointer;
  color: rgba(255, 255, 255, 0.85);
  font-size: 14px;
  /* A label is one unbroken line: the spaces hold their words together exactly
   * as non-breaking spaces would, without rewriting the text the tree carries.
   * With overflow hidden, v-fit-text reads the unbroken phrase width off
   * scrollWidth and shrinks the type to fit the rail. */
  white-space: nowrap;
  overflow: hidden;
}
.nav-node:hover { background: rgba(255, 255, 255, 0.06); }
.nav-node--active { background: rgba(255, 255, 255, 0.14); color: #fff; }
/* A node that leads somewhere reads as the heading of what follows it. */
.nav-node--group {
  font-size: 18px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: #fff;
  margin: 26px 0 2px;
}
</style>
