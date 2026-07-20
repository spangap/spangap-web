import { shallowReactive, computed, markRaw, type Component } from 'vue'

/**
 * Top-bar icon registry — straddle-owned indicators, self-mounted to the right
 * of the app header (just left of the power/logout button).
 *
 * The exact sibling of lib/windowMounts: a straddle's register* module (already
 * called by straddles.gen.ts when the straddle is staged) surfaces a small
 * status indicator in the header without the buildable's MainLayout naming it.
 * The buildable renders <TopbarIcons/> once; every registered icon appears there
 * via <component :is>. The shell is agnostic to what an icon shows — the
 * component (and any storage keys it reads) lives entirely in the owning
 * straddle, so, like the window registry, no buildable source references a
 * package that might not be installed.
 *
 * Mount order = registration order = the generator's init order, so left-to-right
 * ordering is deterministic; the newest-registered icon sits nearest the power
 * button.
 */
export interface TopbarIcon {
  /** Registry key (also the :key); by convention the straddle-scoped indicator id. */
  id: string
  /** The indicator component. Reads whatever device-store keys it needs and
   *  renders itself; it should collapse (render nothing) when it has no value. */
  component: Component
}

/* shallowReactive: map mutations are tracked (so <TopbarIcons/> re-renders on
 * registration) but the component object is not proxied. */
const icons = shallowReactive(new Map<string, TopbarIcon>())

export function registerTopbarIcon(i: TopbarIcon): void {
  icons.set(i.id, { ...i, component: markRaw(i.component) })
}

/** Registered icons in registration (= init) order. */
export const topbarIcons = computed<TopbarIcon[]>(() => [...icons.values()])
