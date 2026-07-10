<template>
  <!-- Trashcan glyph for delete buttons (Settings convention: delete is always
       a trashcan, not a text label). Inline SVG because the SPA ships no icon
       font; inherits currentColor from the host button. -->
  <svg :width="size" :height="size" viewBox="0 0 24 24" fill="currentColor">
    <path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/>
  </svg>
</template>

<script setup lang="ts">
withDefaults(defineProps<{ size?: number }>(), { size: 14 })
</script>
