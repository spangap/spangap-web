<template>
  <!-- Eye glyph for the reveal toggle on a masked field; `off` is the struck
       version, shown while the value is visible. Inline SVG because the SPA
       ships no icon font — a font-ligature icon name renders as the word
       itself; inherits currentColor from the host. -->
  <svg :width="size" :height="size" viewBox="0 0 24 24" fill="currentColor">
    <path v-if="!off" d="M12 5c-5 0-9.27 3.11-11 7.5C2.73 16.89 7 20 12 20s9.27-3.11 11-7.5C21.27 8.11 17 5 12 5zm0 12.5a5 5 0 1 1 0-10 5 5 0 0 1 0 10zm0-8a3 3 0 1 0 0 6 3 3 0 0 0 0-6z"/>
    <path v-else d="M12 7a5 5 0 0 1 5 5c0 .65-.13 1.26-.36 1.83l2.92 2.92A11.8 11.8 0 0 0 23 12.5C21.27 8.11 17 5 12 5c-1.27 0-2.49.2-3.64.57l2.16 2.16C11.09 7.26 11.54 7 12 7zM2.71 3.16 1.29 4.57l2.48 2.48A11.8 11.8 0 0 0 1 12.5C2.73 16.89 7 20 12 20c1.52 0 2.97-.29 4.3-.82l3.13 3.13 1.41-1.41L2.71 3.16zM12 17a5 5 0 0 1-4.55-7.09l1.55 1.55a3 3 0 0 0 3.54 3.54l1.55 1.55c-.63.29-1.34.45-2.09.45z"/>
  </svg>
</template>

<script setup lang="ts">
withDefaults(defineProps<{ size?: number; off?: boolean }>(), { size: 16, off: false })
</script>
