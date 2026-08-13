<!--
  PanelHeading — the section heading inside a settings panel ("Time & Date",
  "Access Point", "Known Networks"). Registered globally as <PanelHeading>, so
  the hand-written panels, every straddle's panel and GeneratedPanel's `section`
  rows all render through this one component.

  A heading is always a single unbroken line: it never breaks between words.
  When the phrase is wider than the panel is, the type size drops just far
  enough to fit, so a long heading shrinks instead of wrapping or being clipped.
-->
<template>
  <div v-fit-text class="panel-heading"><slot /></div>
</template>

<script setup lang="ts">
import { vFitText } from '../lib/fitText'
</script>

<style scoped>
.panel-heading {
  font-size: 10.8px;
  font-weight: 700;
  color: rgba(255, 255, 255, 0.75);
  /* The spaces in a heading hold their words together exactly as non-breaking
   * spaces would, and without rewriting the text: the phrase stays character
   * for character the one the nav tree and the panel descriptors carry, so
   * find-in-page and copy still see it. Both properties are also what lets
   * v-fit-text read the unbroken phrase width off scrollWidth. */
  white-space: nowrap;
  overflow: hidden;
}
</style>
