<!--
  SettingsNavTree — recursive in-window navigation for the Settings app.

  Renders the menu store's 'settings' group tree (groups → submenus → leaves).
  Submenus are headings with indented children; leaf panels are selectable rows
  that emit `select` with the leaf's full path id (the SettingsWindow then opens
  that panel). Replaces the navigation the menu-bar dropdowns used to provide.
-->
<template>
  <div class="nav-tree">
    <template v-for="item in visibleItems" :key="item.id">
      <div v-if="item.type === 'submenu'" class="nav-group">
        <div v-fit-text class="nav-group-title">{{ item.label }}</div>
        <SettingsNavTree
          class="nav-children"
          :items="item.children ?? []"
          :active="active"
          @select="(id) => emit('select', id)"
        />
      </div>
      <div
        v-else
        class="nav-leaf"
        :class="{ 'nav-leaf--active': item.id === active }"
        @click="emit('select', item.id)"
      >
        {{ item.dynamicLabel ? item.dynamicLabel() : item.label }}
      </div>
    </template>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import type { MenuItem } from '../stores/menu'
import { vFitText } from '../lib/fitText'

const props = defineProps<{ items: MenuItem[]; active: string | null }>()
const emit = defineEmits<{ select: [id: string] }>()

function isHidden(it: MenuItem): boolean {
  return typeof it.hidden === 'function' ? it.hidden() : !!it.hidden
}

const visibleItems = computed(() =>
  props.items.filter(it => !isHidden(it) && (it.type === 'submenu' || it.type === 'panel')),
)
</script>

<style scoped>
.nav-children { margin-left: 10px; }
.nav-group-title {
  font-size: 18px;
  font-weight: 700;
  text-transform: uppercase;
  letter-spacing: 0.06em;
  color: #fff;
  margin: 26px 0 2px;
  /* A group title is one unbroken line — the spaces hold their words together
   * exactly as non-breaking spaces would, without rewriting the label the menu
   * store carries. With overflow hidden, v-fit-text reads the unbroken phrase
   * width off scrollWidth and shrinks the type to fit the rail. */
  white-space: nowrap;
  overflow: hidden;
}
.nav-leaf {
  padding: 6px 10px;
  border-radius: 6px;
  cursor: pointer;
  color: rgba(255, 255, 255, 0.85);
  font-size: 14px;
}
.nav-leaf:hover { background: rgba(255, 255, 255, 0.06); }
.nav-leaf--active { background: rgba(255, 255, 255, 0.14); color: #fff; }
</style>
