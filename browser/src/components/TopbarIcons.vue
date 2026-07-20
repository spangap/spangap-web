<!-- TopbarIcons — renders every indicator in the top-bar icon registry
     (lib/topbarIcons.ts). A buildable's MainLayout mounts this ONCE in its
     header, just left of the power/logout button; from then on, staging a
     straddle whose register* module calls registerTopbarIcon() is enough to
     surface its indicator — no layout edit, no static import of a package that
     may not be staged. The shell only positions the icons; each component owns
     its own content and collapses when it has nothing to show. -->
<template>
  <div class="topbar-icons">
    <component :is="i.component" v-for="i in topbarIcons" :key="i.id" />
  </div>
</template>

<script setup lang="ts">
import { topbarIcons } from '../lib/topbarIcons'
</script>

<style scoped>
.topbar-icons {
  display: flex;
  align-items: center;
  gap: 10px;
}
</style>
