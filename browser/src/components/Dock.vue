<!--
  Dock — the bottom app launcher that replaces the menu bar / hamburger.

  Renders one icon per registered app (lib/apps.ts), sorted by placement.

  Desktop (!compact): a centered macOS-style dock floating above the bottom
  edge; the full set of icons, label shown on hover.

  Phone (compact): a fixed iOS-style bottom nav bar (icon + small label). If
  there are more than 5 apps, the first 4 (by placement) show as tabs and a 5th
  "More" triple-dot opens a sheet listing the rest — the standard 4+1 tab budget.

  Clicking an icon calls the app's open() (raise/show its window). A running-app
  dot marks apps whose isOpen() is true.
-->
<template>
  <div class="dock-wrap" :class="compact ? 'dock-wrap--phone' : 'dock-wrap--desktop'">
    <div class="dock" :class="compact ? 'dock--phone' : 'dock--desktop'">
      <button
        v-for="app in primaryApps"
        :key="app.id"
        class="dock-item"
        :class="{ 'dock-item--open': app.isOpen && app.isOpen() }"
        :title="app.label"
        @click="app.open()"
      >
        <span
          v-if="appIconSvg(app.icon)"
          class="dock-icon"
          v-html="appIconSvg(app.icon)"
        />
        <div v-else class="dock-icon dock-icon-fallback">{{ app.label.charAt(0) }}</div>
      </button>

      <!-- Phone overflow: a "More" tab opening the remaining apps in a sheet. -->
      <button v-if="overflowApps.length" class="dock-item" title="More">
        <div class="dock-more-icon">⋯</div>
        <q-menu anchor="top middle" self="bottom middle" class="dock-more-menu">
          <q-list dark style="min-width: 160px">
            <q-item
              v-for="app in overflowApps"
              :key="app.id"
              v-close-popup
              clickable
              @click="app.open()"
            >
              <q-item-section avatar>
                <span v-if="appIconSvg(app.icon)" class="dock-icon dock-icon-sm" v-html="appIconSvg(app.icon)" />
                <div v-else class="dock-icon dock-icon-sm dock-icon-fallback">{{ app.label.charAt(0) }}</div>
              </q-item-section>
              <q-item-section>{{ app.label }}</q-item-section>
            </q-item>
          </q-list>
        </q-menu>
      </button>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { useCompact } from '../lib/viewport'
import { sortedApps, appIconSvg } from '../lib/apps'

const compact = useCompact()

/* Phone overflow rule: >5 apps → first 4 as tabs + a "More" tab for the rest.
 * ≤5 (or desktop) → show everything inline. */
const PHONE_PRIMARY = 4
const primaryApps = computed(() => {
  if (compact.value && sortedApps.value.length > 5) return sortedApps.value.slice(0, PHONE_PRIMARY)
  return sortedApps.value
})
const overflowApps = computed(() => {
  if (compact.value && sortedApps.value.length > 5) return sortedApps.value.slice(PHONE_PRIMARY)
  return []
})
</script>

<style scoped>
/* ── desktop: centered floating dock above the bottom edge ── */
.dock-wrap--desktop {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 10px;
  display: flex;
  justify-content: center;
  pointer-events: none;
  z-index: 6000;
}
.dock--desktop {
  pointer-events: auto;
  display: flex;
  gap: 8px;
  padding: 8px 12px;
  background: rgba(30, 30, 32, 0.72);
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 18px;
  backdrop-filter: blur(12px);
  box-shadow: 0 8px 28px rgba(0, 0, 0, 0.45);
}
.dock--desktop .dock-item { width: 44px; height: 44px; }
.dock--desktop .dock-icon { width: 32px; height: 32px; }
.dock--desktop .dock-item:hover { transform: translateY(-4px); }

/* ── phone: fixed bottom nav bar ── */
.dock-wrap--phone {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  z-index: 6000;
}
.dock--phone {
  display: flex;
  justify-content: space-around;
  align-items: stretch;
  background: rgba(20, 20, 22, 0.96);
  border-top: 1px solid rgba(255, 255, 255, 0.12);
  padding: 4px 0 max(4px, env(safe-area-inset-bottom));
}
/* Taller bar (~20%) + bigger icons; no labels (the icon is the affordance). */
.dock--phone .dock-item { flex: 1; height: 62px; }
.dock--phone .dock-icon { width: 34px; height: 34px; }

/* ── shared item chrome ── */
.dock-item {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  background: transparent;
  border: none;
  cursor: pointer;
  padding: 0;
  transition: transform 0.12s ease;
}
.dock-icon { display: block; }
.dock-icon :deep(svg) { width: 100%; height: 100%; display: block; }
.dock-icon-sm { width: 24px; height: 24px; }
.dock-icon-fallback {
  display: flex;
  align-items: center;
  justify-content: center;
  border-radius: 9px;
  background: rgba(255, 255, 255, 0.14);
  color: #fff;
  font-weight: 600;
  font-size: 16px;
  text-transform: uppercase;
}
.dock-more-icon {
  width: 32px;
  height: 32px;
  display: flex;
  align-items: center;
  justify-content: center;
  font-size: 24px;
  color: rgba(255, 255, 255, 0.8);
  line-height: 1;
}
/* Running-app indicator: a faint glow behind the icon (replaces the blue dot).
 * ::before paints under the icon (above the bar background), so it reads as a
 * soft halo around a running app's icon — on both phone and desktop. */
.dock-item--open::before {
  content: '';
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  width: 46px;
  height: 46px;
  border-radius: 50%;
  background: radial-gradient(circle, rgba(74, 163, 255, 0.40) 0%, rgba(74, 163, 255, 0) 68%);
  pointer-events: none;
}
</style>
