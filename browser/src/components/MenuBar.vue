<template>
  <q-toolbar class="menu-bar">
    <!-- Mobile: hamburger button -->
    <q-btn v-if="compact" flat dense aria-label="Menu" class="hamburger-btn">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor"
        stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
        <line x1="3" y1="6"  x2="21" y2="6" />
        <line x1="3" y1="12" x2="21" y2="12" />
        <line x1="3" y1="18" x2="21" y2="18" />
      </svg>
      <q-menu class="menu-dropdown" :max-height="maxMenuHeight + 'px'">
        <q-list dense style="min-width: 220px">
          <!-- App group: About + Log out under progName -->
          <q-expansion-item :label="progName" dense dense-toggle>
            <q-item class="hb-sub" clickable v-close-popup @click="menuStore.openPanel('app/about')">
              <q-item-section>About</q-item-section>
            </q-item>
            <q-item v-if="authActive" class="hb-sub" clickable v-close-popup @click="onLogout">
              <q-item-section>Log out</q-item-section>
            </q-item>
          </q-expansion-item>
          <q-separator />
          <template v-for="menu in visibleMenus">
            <!-- Single-panel menu group: direct item -->
            <q-item v-if="menu.items.length === 1 && menu.items[0].type === 'panel'"
              :key="menu.id"
              clickable v-close-popup @click="onSinglePanelClick(menu)">
              <q-item-section>{{ menu.label }}</q-item-section>
            </q-item>
            <!-- Multi-item menu group: expansion item -->
            <q-expansion-item v-else :key="menu.id" :label="menu.label" dense dense-toggle>
              <template v-for="item in menu.items">
                <q-item v-if="item.type === 'panel'"
                  :key="item.id"
                  class="hb-sub" clickable v-close-popup
                  @click="menuStore.openPanel(item.id)">
                  <q-item-section>{{ item.label }}</q-item-section>
                </q-item>
                <q-expansion-item v-else-if="item.type === 'submenu'"
                  :key="item.id"
                  class="hb-sub" :label="item.label" dense dense-toggle>
                  <template v-for="child in (item.children || [])">
                    <q-expansion-item v-if="child.type === 'submenu'"
                      :key="child.id"
                      class="hb-sub2" :label="child.label" dense dense-toggle>
                      <q-item v-for="gc in (child.children || [])" :key="gc.id"
                        class="hb-sub3" clickable v-close-popup
                        :disable="!isClickableChild(gc)"
                        @click="onSubmenuChildClick(gc)">
                        <q-item-section>{{ gc.label }}</q-item-section>
                      </q-item>
                    </q-expansion-item>
                    <q-item v-else
                      :key="child.id"
                      class="hb-sub2" clickable v-close-popup
                      :disable="!isClickableChild(child)"
                      @click="onSubmenuChildClick(child)">
                      <q-item-section>{{ child.label }}</q-item-section>
                    </q-item>
                  </template>
                </q-expansion-item>
                <q-item v-else-if="item.type === 'toggle'"
                  :key="item.id"
                  class="hb-sub" @click.stop>
                  <q-item-section>{{ item.label }}</q-item-section>
                  <q-item-section side>
                    <q-toggle
                      :model-value="device.get(item.key) == 1"
                      dense color="primary"
                      @update:model-value="v => device.set(item.key, v ? 1 : 0)"
                    />
                  </q-item-section>
                </q-item>
                <q-item v-else-if="item.type === 'action'"
                  :key="item.id"
                  class="hb-sub" clickable v-close-popup
                  @click="item.action?.()">
                  <q-item-section>
                    <div class="action-row">
                      <span v-if="menuHasChecks(menu)" class="check-slot">{{ item.checked && item.checked() ? '✓' : '' }}</span>
                      <span>{{ item.dynamicLabel ? item.dynamicLabel() : item.label }}</span>
                    </div>
                  </q-item-section>
                </q-item>
              </template>
            </q-expansion-item>
          </template>
        </q-list>
      </q-menu>
    </q-btn>

    <!-- Desktop: app name + full menu bar -->
    <template v-else>
      <q-btn flat dense no-caps :label="progName"
        :style="{ fontWeight: 900, fontSize: '20px', padding: '2px 21px', marginRight: '0px', letterSpacing: 0, fontFamily: 'system-ui' }">
        <q-menu class="menu-dropdown">
          <q-list dense>
            <q-item clickable v-close-popup @click="menuStore.openPanel('app/about')">
              <q-item-section>About</q-item-section>
            </q-item>
            <q-separator v-if="authActive" />
            <q-item v-if="authActive" clickable v-close-popup @click="onLogout">
              <q-item-section>Log out</q-item-section>
            </q-item>
          </q-list>
        </q-menu>
      </q-btn>
      <template v-for="menu in visibleMenus">
        <!-- Single-panel menu group: direct button, no dropdown -->
        <q-btn v-if="menu.items.length === 1 && menu.items[0].type === 'panel'"
          :key="menu.id"
          flat dense no-caps
          :label="menu.label"
          :style="{ fontWeight: 500, fontSize: '20px', padding: '2px 21px', letterSpacing: 0 }"
          @click="onSinglePanelClick(menu)" />
        <!-- Multi-item menu group: dropdown -->
        <q-btn v-else :key="menu.id" flat dense no-caps :label="menu.label"
          :style="{ fontWeight: 500, fontSize: '20px', padding: '2px 21px', letterSpacing: 0 }">
          <q-menu
            :ref="el => setMenuRef(menu.id, el)"
            class="menu-dropdown" :max-height="maxMenuHeight + 'px'"
            @mouseenter="onMenuEnter(menu.id)"
            @mouseleave="onMenuLeave(menu.id)"
          >
            <div class="menu-scroll-wrap" :style="{ maxHeight: maxMenuHeight + 'px' }">
              <q-list dense>
                <template v-for="item in menu.items">
                  <!-- Submenu -->
                  <q-item v-if="item.type === 'submenu'" :key="item.id" clickable
                    @mouseenter="onSubmenuEnter(menu.id, item.id)"
                    @mouseleave="onSubmenuLeave(menu.id, item.id)">
                    <q-item-section>{{ item.label }}</q-item-section>
                    <q-item-section side>
                      <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 6 15 12 9 18" /></svg>
                    </q-item-section>
                    <q-menu
                      :ref="el => setSubmenuRef(menu.id, item.id, el)"
                      anchor="top end" self="top start"
                      class="menu-dropdown"
                      no-parent-event
                      @mouseenter="onSubmenuEnter(menu.id, item.id)"
                      @mouseleave="onSubmenuLeave(menu.id, item.id)"
                    >
                      <q-list dense>
                        <template v-for="child in item.children">
                          <q-item v-if="child.type === 'submenu'" :key="child.id" clickable
                            @mouseenter="onSubmenuEnter(menu.id, child.id)"
                            @mouseleave="onSubmenuLeave(menu.id, child.id)">
                            <q-item-section>{{ child.label }}</q-item-section>
                            <q-item-section side>
                              <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 6 15 12 9 18" /></svg>
                            </q-item-section>
                            <q-menu
                              :ref="el => setSubmenuRef(menu.id, child.id, el)"
                              anchor="top end" self="top start"
                              class="menu-dropdown"
                              no-parent-event
                              @mouseenter="onSubmenuEnter(menu.id, child.id)"
                              @mouseleave="onSubmenuLeave(menu.id, child.id)"
                            >
                              <q-list dense>
                                <q-item
                                  v-for="gc in (child.children || [])" :key="gc.id"
                                  clickable v-close-popup
                                  :disable="!isClickableChild(gc)"
                                  @click="onSubmenuChildClick(gc)"
                                >
                                  <q-item-section>{{ gc.label }}</q-item-section>
                                </q-item>
                              </q-list>
                            </q-menu>
                          </q-item>
                          <q-item v-else
                            :key="child.id"
                            clickable v-close-popup
                            :disable="!isClickableChild(child)"
                            @click="onSubmenuChildClick(child)"
                          >
                            <q-item-section>{{ child.label }}</q-item-section>
                          </q-item>
                        </template>
                      </q-list>
                    </q-menu>
                  </q-item>
                  <!-- Panel item -->
                  <q-item
                    v-else-if="item.type === 'panel'"
                    :key="item.id"
                    clickable v-close-popup
                    @mouseenter="onItemEnter"
                    @click="menuStore.openPanel(item.id)"
                  >
                    <q-item-section>{{ item.label }}</q-item-section>
                  </q-item>
                  <!-- Toggle item -->
                  <q-item v-else-if="item.type === 'toggle'" :key="item.id" @click.stop @mouseenter="onItemEnter">
                    <q-item-section>{{ item.label }}</q-item-section>
                    <q-item-section side>
                      <q-toggle
                        :model-value="device.get(item.key) == 1"
                        dense color="primary"
                        @update:model-value="v => device.set(item.key, v ? 1 : 0)"
                      />
                    </q-item-section>
                  </q-item>
                  <!-- Action item -->
                  <q-item
                    v-else-if="item.type === 'action'"
                    :key="item.id"
                    clickable v-close-popup
                    @mouseenter="onItemEnter"
                    @click="item.action?.()"
                  >
                    <q-item-section>
                      <div class="action-row">
                        <span v-if="menuHasChecks(menu)" class="check-slot">{{ item.checked && item.checked() ? '✓' : '' }}</span>
                        <span>{{ item.dynamicLabel ? item.dynamicLabel() : item.label }}</span>
                      </div>
                    </q-item-section>
                  </q-item>
                </template>
              </q-list>
              <div class="menu-scroll-fade"></div>
            </div>
          </q-menu>
        </q-btn>
      </template>
    </template>

    <q-space />
    <span v-if="motionDetected" class="status-pill status-motion">MOTION</span>
    <span v-if="audioDetected" class="status-pill status-sound">SOUND</span>
    <span v-if="recordingActive" class="rec-blink">REC</span>

    <!-- Full-screen disconnected overlay (teleports to body). Replaces the old
         menu-bar 'Disconnected' caption. Mounted here because MenuBar is in
         every spangap-web app, so they all get the overlay. -->
    <ConnectionOverlay />
  </q-toolbar>
</template>

<script setup lang="ts">
import { computed } from 'vue'
import { storeToRefs } from 'pinia'
import { useQuasar } from 'quasar'
import { useRouter } from 'vue-router'
import { useDeviceStore } from '../stores/device'
import { useMenuStore, type MenuGroup, type MenuItem } from '../stores/menu'
import { authLogout } from '../lib/auth'
import ConnectionOverlay from './ConnectionOverlay.vue'

const $q = useQuasar()
const router = useRouter()
const device = useDeviceStore()
const menuStore = useMenuStore()
const { settings } = storeToRefs(device)

const authActive = computed(() => document.cookie.includes('session='))

const recordingActive = computed(() => settings.value.record?.active == 1)
const motionDetected  = computed(() => settings.value.detect?.motion == 1)
const audioDetected   = computed(() => settings.value.detect?.audio == 1)

/* Collapse menu bar to hamburger below the md breakpoint (< 1024px). */
const compact = computed(() => $q.screen.lt.md)

/* A group renders only when it isn't hidden and has at least one non-hidden
 * leaf — so the About-only 'app' group (all leaves hidden) and the Recording
 * group (group hidden while no SD card) both stay openable but unshown.
 * `hidden` may be a constant or a predicate evaluated each render. */
function isHidden(h?: boolean | (() => boolean)): boolean {
  return typeof h === 'function' ? h() : !!h
}
function leafVisible(it: MenuItem): boolean {
  if (isHidden(it.hidden)) return false
  return it.children ? it.children.some(leafVisible) : true
}
const visibleMenus = computed<MenuGroup[]>(() =>
  (menuStore.sortedMenus as MenuGroup[]).filter(m => !isHidden(m.hidden) && m.items.some(leafVisible)),
)

const progName = computed(() => {
  const p = settings.value?.s?.sys?.progname
  if (typeof p === 'string' && p.trim().length > 0) return p.trim()
  const proj = settings.value?.s?.sys?.project
  if (typeof proj === 'string' && proj.length > 0) return proj.charAt(0).toUpperCase() + proj.slice(1)
  return 'Spangap'
})

const maxMenuHeight = computed(() => Math.floor($q.screen.height * 0.7))

function menuHasChecks(menu: MenuGroup): boolean {
  return menu.items.some(i => (i as any).checked)
}

function isClickableChild(child: MenuItem): boolean {
  if (child.disabled?.()) return false
  if (child.type === 'panel') return !!child.component
  if (child.type === 'action') return !!child.action
  return false
}

function onSubmenuChildClick(child: MenuItem) {
  if (child.disabled?.()) return
  if (child.type === 'panel' && child.component) menuStore.openPanel(child.id)
  else if (child.type === 'action') child.action?.()
}

function onSinglePanelClick(menu: MenuGroup) {
  const item = menu.items[0]
  if (menuStore.activePanel === item.id) menuStore.closePanel()
  else menuStore.openPanel(item.id)
}

async function onLogout() {
  try { await authLogout() } catch { /* ignore */ }
  window.location.href = '/'
}

/* Single close timer — any mouse activity in any menu/submenu cancels it.
 * `openSubmenuStack` is parent-first so we can close descendants without
 * disturbing ancestors. Submenu ids must use dot-hierarchy (parent.child)
 * so we can recognize ancestor relationships for nested menus. */
const menuRefs = new Map<string, any>()
const submenuRefs = new Map<string, any>()
let closeTimer: ReturnType<typeof setTimeout> | null = null
let activeMenuId: string | null = null
const openSubmenuStack: string[] = []

function setMenuRef(id: string, el: any) {
  if (el) menuRefs.set(id, el)
  else menuRefs.delete(id)
}
function setSubmenuRef(_menuId: string, itemId: string, el: any) {
  if (el) submenuRefs.set(itemId, el)
  else submenuRefs.delete(itemId)
}

/** Close open submenus from the top until reaching `itemId` or one of its
 * ancestors. Pass `null` to close all. */
function closeSubmenusNotAncestorOf(itemId: string | null) {
  for (let i = openSubmenuStack.length - 1; i >= 0; i--) {
    const openId = openSubmenuStack[i]
    if (itemId !== null && (itemId === openId || itemId.startsWith(openId + '.'))) break
    const ref = submenuRefs.get(openId)
    if (ref?.hide) ref.hide()
    openSubmenuStack.splice(i, 1)
  }
}

function cancelClose() {
  if (closeTimer) { clearTimeout(closeTimer); closeTimer = null }
}

function startClose() {
  cancelClose()
  closeTimer = setTimeout(() => {
    closeTimer = null
    if (activeMenuId) {
      const ref = menuRefs.get(activeMenuId)
      if (ref?.hide) ref.hide()
    }
  }, 1500)
}

function onMenuEnter(id: string) {
  activeMenuId = id
  cancelClose()
}
function onMenuLeave(_id: string) {
  startClose()
}

/** Called when mouse enters any non-submenu item — close any open submenu */
function onItemEnter() {
  closeSubmenusNotAncestorOf(null)
}

function onSubmenuEnter(_menuId: string, itemId: string) {
  cancelClose()
  closeSubmenusNotAncestorOf(itemId)
  if (!openSubmenuStack.includes(itemId)) {
    openSubmenuStack.push(itemId)
    const ref = submenuRefs.get(itemId)
    if (ref?.show) ref.show()
  }
}
function onSubmenuLeave(_menuId: string, _itemId: string) {
  startClose()
}
</script>

<style scoped>
.action-row {
  display: flex;
  align-items: center;
  width: 100%;
}
.check-slot {
  flex: 0 0 18px;
  width: 18px;
  text-align: left;
  margin-right: 4px;
}

.hamburger-btn {
  margin-right: 4px;
  padding: 6px 10px;
}

/* Indent for hamburger expansion contents */
.hb-sub :deep(.q-item__section--main) { padding-left: 18px; }
.hb-sub2 :deep(.q-item__section--main) { padding-left: 36px; }

.status-pill {
  font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
  font-weight: 800;
  font-size: 13px;
  letter-spacing: 0.5px;
  padding: 2px 7px;
  border-radius: 4px;
  margin-right: 6px;
  line-height: 1;
  text-shadow: 0 1px 1px rgba(0,0,0,0.5);
}
.status-motion {
  color: #fff;
  background: rgba(229, 57, 53, 0.85);
}
.status-sound {
  color: #fff;
  background: rgba(255, 152, 0, 0.85);
}

.rec-blink {
  color: #e53935;
  font-weight: 700;
  font-size: 28px;
  letter-spacing: 0.5px;
  margin-right: 6px;
  animation: rec-pulse 1s step-end infinite;
}
@keyframes rec-pulse {
  0%, 50% { opacity: 1; }
  50.01%, 100% { opacity: 0; }
}
.menubar-rec-btn {
  background: none;
  border: none;
  cursor: pointer;
  padding: 2px 4px;
  display: flex;
  align-items: center;
  opacity: 0.85;
  transition: opacity 0.15s;
}
.menubar-rec-btn:hover {
  opacity: 1;
}
</style>
