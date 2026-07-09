<!-- StraddleWindows — renders every window in the window-mount registry
     (lib/windowMounts.ts). A buildable's MainLayout mounts this ONCE inside
     its UsableArea; from then on, staging a straddle whose register* module
     calls registerWindowMount() is enough to put its window on screen — no
     layout edit, no static import of a package that may not be staged.

     Binds the standard straddle-window contract for whichever fields the
     mount supplies: `title` + `visible` + `focus-token` in, `update:visible`
     out (written back to the straddle's own ref). A wrapper mount that
     supplies only `component:` gets no bindings — it owns its own windows. -->
<template>
  <component
    :is="w.component"
    v-for="w in windowMounts"
    :key="w.id"
    v-bind="bindings(w)"
  />
</template>

<script setup lang="ts">
import { windowMounts, type WindowMount } from '../lib/windowMounts'

/* Called from the render scope, so the .value reads register the refs as
 * render dependencies — visibility/focus changes re-render as usual. */
function bindings(w: WindowMount): Record<string, unknown> {
  const b: Record<string, unknown> = {}
  if (w.title !== undefined) b.title = w.title
  if (w.visible) {
    b.visible = w.visible.value
    b['onUpdate:visible'] = (v: boolean) => { w.visible!.value = v }
  }
  if (w.focusToken) b.focusToken = w.focusToken.value
  return b
}
</script>
