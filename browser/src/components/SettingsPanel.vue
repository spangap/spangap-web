<template>
  <div class="settings-panel-wrap">
    <!-- Compact (phone): the drawer is full-screen, so nothing is left outside
         to tap for dismissal. A header bar with a back arrow + the active
         pane's label gives the way out, mobile-app style. -->
    <div v-if="compact" class="settings-mobile-head">
      <button class="smh-back" aria-label="Back" @click="menuStore.closePanel()">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor"
          stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
          <polyline points="15 18 9 12 15 6" />
        </svg>
      </button>
      <span class="smh-title">{{ menuStore.activePanelLabel }}</span>
    </div>
    <div class="settings-panel" ref="panelRef" @scroll="onScroll">
      <div class="q-pa-md text-white" ref="contentRef">
        <component :is="menuStore.activePanelComponent" v-if="menuStore.activePanelComponent" />
      </div>
    </div>
    <!-- Scroll indicators -->
    <div class="scroll-arrow scroll-arrow--top" v-show="canScrollUp">
      <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="18 15 12 9 6 15" /></svg>
    </div>
    <div class="scroll-arrow scroll-arrow--bottom" v-show="canScrollDown">
      <svg width="21" height="21" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9" /></svg>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, watch, nextTick, onMounted, onBeforeUnmount } from 'vue'
import { useMenuStore } from '../stores/menu'
import { useCompact } from '../lib/viewport'

const menuStore = useMenuStore()
const compact = useCompact()
const panelRef = ref<HTMLElement | null>(null)
const contentRef = ref<HTMLElement | null>(null)
const canScrollUp = ref(false)
const canScrollDown = ref(false)

function onScroll() {
  const el = panelRef.value
  if (!el) return
  canScrollUp.value = el.scrollTop > 8
  canScrollDown.value = el.scrollTop + el.clientHeight < el.scrollHeight - 8
}

let resizeObserver: ResizeObserver | null = null

onMounted(() => {
  resizeObserver = new ResizeObserver(() => onScroll())
  if (panelRef.value) resizeObserver.observe(panelRef.value)
  if (contentRef.value) resizeObserver.observe(contentRef.value)
})

onBeforeUnmount(() => {
  resizeObserver?.disconnect()
})

watch(() => menuStore.activePanel, () => {
  nextTick(() => {
    const el = panelRef.value
    if (el) { el.scrollTop = 0 }
    onScroll()
  })
})
</script>

<style>
.settings-panel-wrap {
  height: 100%;
  position: relative;
  display: flex;
  flex-direction: column;
}
.settings-mobile-head {
  display: flex;
  align-items: center;
  gap: 6px;
  height: 44px;
  min-height: 44px;
  padding: 0 8px;
  background: #282828;
  border-bottom: 1px solid rgba(255, 255, 255, 0.12);
}
.smh-back {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 40px;
  height: 40px;
  background: none;
  border: none;
  color: #e8e8e8;
  cursor: pointer;
  border-radius: 8px;
}
.smh-back:active { background: rgba(255, 255, 255, 0.1); }
.smh-title {
  font-size: 16px;
  font-weight: 600;
  color: #e8e8e8;
  font-family: system-ui;
}
.settings-panel {
  flex: 1;
  overflow-y: auto;
  min-height: 0;
}
.settings-panel::-webkit-scrollbar {
  width: 4px;
}
.settings-panel::-webkit-scrollbar-track {
  background: transparent;
}
.settings-panel::-webkit-scrollbar-thumb {
  background: rgba(255,255,255,0.5);
  border-radius: 4px;
}
.settings-panel::-webkit-scrollbar-thumb:hover {
  background: rgba(255,255,255,0.65);
}

.scroll-arrow {
  position: absolute;
  left: 50%;
  transform: translateX(-50%);
  z-index: 10;
  pointer-events: none;
  color: #222;
  background: rgba(255,255,255,0.85);
  border-radius: 50%;
  width: 36px;
  height: 36px;
  display: flex;
  align-items: center;
  justify-content: center;
  box-shadow: 0 1px 4px rgba(0,0,0,0.3);
}
.scroll-arrow--top {
  top: 6px;
}
.scroll-arrow--bottom {
  bottom: 6px;
}
</style>
