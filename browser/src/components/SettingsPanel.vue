<template>
  <div class="settings-panel-wrap">
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

const menuStore = useMenuStore()
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
