<template>
  <div class="usable-area" :class="{ 'usable-area--stacked': stacked }">
    <slot />
    <!-- Click-outside dismiss: a transparent scrim over the usable area that
         closes the open settings pane when the user clicks anywhere outside
         it. The pane itself lives in the side drawer (or the stacked panel),
         which paints above this scrim, so clicks on the pane never reach it.
         Shown only while a pane is open and the host opts in (see
         `dismissOverlay`). Owned here so every spangap-web app gets the
         behaviour for free, rather than each app reimplementing it. -->
    <div
      v-if="dismissOverlay && menuStore.activePanel !== null"
      class="panel-dismiss-overlay"
      @click="menuStore.closePanel()"
    />
  </div>
</template>

<script setup lang="ts">
import { useMenuStore } from '../stores/menu'

withDefaults(defineProps<{
  /* When true, show the click-outside scrim that dismisses the open settings
   * pane. Hosts that render the pane inline (e.g. stacked below the content on
   * narrow screens) pass false so a tap on the content area isn't swallowed. */
  dismissOverlay?: boolean
  /* Lay the slotted content out as a vertical stack (content above, an inline
   * panel below) instead of the default overlay/drawer arrangement. */
  stacked?: boolean
}>(), {
  dismissOverlay: true,
  stacked: false,
})

const menuStore = useMenuStore()
</script>

<style scoped>
/* Positioning context for FloatingWindow children. Fills the column
 * remaining below the q-header inside q-page-container, so absolute
 * `top: 0%` lands at the top of the page area, not under the menu bar.
 * The parent q-page-container must be `display: flex; flex-direction: column`. */
.usable-area {
  flex: 1;
  min-height: 0;
  position: relative;
  overflow: hidden;
}
.usable-area--stacked {
  display: flex;
  flex-direction: column;
}
.panel-dismiss-overlay {
  position: absolute;
  inset: 0;
  z-index: 100;
  cursor: default;
}
</style>
