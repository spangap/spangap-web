<!--
  PanelHeading — a heading inside a settings pane, at one of three levels.
  Registered globally as <PanelHeading>, so the hand-written panels, every
  straddle's panel and the declarative heading rows all render through this one
  component.

  Level 1 names the PAGE and appears once at its top ("Reticulum Interfaces").
  Level 2 names a GROUP within it ("TCP Interface") — the highest level a
  straddle that does not own the menu states. Level 3 names a sub-group
  ("Outbound Peers"). The sizes step down with the level so the three are
  told apart at a glance; the INDENT of what follows a heading is the caller's
  business (SettingRows walks the rows and carries the running level), because
  a heading knows its own level but not what comes after it.

  A heading is always a single unbroken line: it never breaks between words.
  When the phrase is wider than the panel is, the type size drops just far
  enough to fit, so a long heading shrinks instead of wrapping or being clipped.
-->
<template>
  <div v-fit-text class="panel-heading" :class="`heading-${level}`"><slot /></div>
</template>

<script setup lang="ts">
import { vFitText } from '../lib/fitText'

withDefaults(defineProps<{ level?: 1 | 2 | 3 }>(), { level: 2 })
</script>

<style scoped>
.panel-heading {
  letter-spacing: 0.2px;
  /* The spaces in a heading hold their words together exactly as non-breaking
   * spaces would, and without rewriting the text: the phrase stays character
   * for character the one the nav tree and the panel descriptors carry, so
   * find-in-page and copy still see it. Both properties are also what lets
   * v-fit-text read the unbroken phrase width off scrollWidth. */
  white-space: nowrap;
  overflow: hidden;
}
/* The page's own name: the largest type on it, and the only heading with a
 * rule under it — nothing else on the pane needs separating from the title. */
.heading-1 {
  font-size: 22px;
  font-weight: 700;
  color: #fff;
  margin: 2px 0 14px;
  padding-bottom: 6px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.14);
}
/* A group. Smaller than the title and set in the pane's own sentence case —
 * a heading names its subject, it is not a title of a work, and shouting it
 * in capitals says nothing the size and weight do not. */
.heading-2 {
  font-size: 15px;
  font-weight: 700;
  color: rgba(255, 255, 255, 0.95);
  margin: 22px 0 6px;
}
/* A sub-group: plain sentence case, one size above the field names it heads. */
.heading-3 {
  font-size: 13.5px;
  font-weight: 600;
  color: rgba(255, 255, 255, 0.88);
  margin: 14px 0 4px;
}
</style>
