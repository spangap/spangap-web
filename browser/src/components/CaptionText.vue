<!--
  CaptionText — descriptive text with links in it.

  `[label](url)` is the one way a link is written in a settings description,
  and it is written once: here it becomes an anchor, and on the display
  (lcdSettingCaption) the label survives and the URL is dropped — a panel with
  no browser has nowhere to send you, and the URL was only ever addressed to a
  machine.

  This is a link syntax, not a markdown parser: nothing else is interpreted,
  and anything that does not close both brackets is shown exactly as written.
  The href is bound, never interpolated into markup, so a caption cannot carry
  anything but text and a destination.
-->
<template>
  <span>
    <template v-for="(part, i) in parts" :key="i">
      <a
        v-if="part.href"
        :href="part.href"
        target="_blank"
        rel="noopener noreferrer"
      >{{ part.text }}</a>
      <template v-else>{{ part.text }}</template>
    </template>
  </span>
</template>

<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{ text?: string }>()

/** Only http(s) is a destination. Anything else stays plain text rather than
 *  becoming an anchor — a caption is not a place to open a scheme from. */
const LINK = /\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g

const parts = computed<{ text: string; href?: string }[]>(() => {
  const src = props.text ?? ''
  const out: { text: string; href?: string }[] = []
  let at = 0
  for (const m of src.matchAll(LINK)) {
    if (m.index > at) out.push({ text: src.slice(at, m.index) })
    out.push({ text: m[1]!, href: m[2]! })
    at = m.index + m[0].length
  }
  if (at < src.length) out.push({ text: src.slice(at) })
  return out
})
</script>

<style scoped>
a {
  color: #6cc0ff;
  text-decoration: underline;
}
</style>
