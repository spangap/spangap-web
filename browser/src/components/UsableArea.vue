<template>
  <div class="usable-area"><slot /></div>
</template>

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
</style>
