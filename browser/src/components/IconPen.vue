<template>
  <!-- Pen glyph for edit buttons (Settings convention: edit is always a pen,
       not a text label). Inline SVG because the SPA ships no icon font — a
       font-ligature icon name renders as the word itself; inherits currentColor
       from the host button. -->
  <svg :width="size" :height="size" viewBox="0 0 24 24" fill="currentColor">
    <path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04a1 1 0 0 0 0-1.41l-2.34-2.34a1 1 0 0 0-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/>
  </svg>
</template>

<script setup lang="ts">
withDefaults(defineProps<{ size?: number }>(), { size: 14 })
</script>
