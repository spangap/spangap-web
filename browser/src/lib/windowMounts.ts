import { shallowReactive, computed, markRaw, type Component, type Ref } from 'vue'

/**
 * Window-mount registry — straddle-owned FloatingWindows, self-mounted.
 *
 * The dock/menu/settings registries carry callbacks and descriptors; this one
 * carries the window *component itself*, so a straddle's register* module (the
 * one straddles.gen.ts already calls when the straddle is staged) can surface
 * its window without the buildable's MainLayout naming it. The buildable
 * renders <StraddleWindows/> once; every registered mount appears there via
 * <component :is>, bound to the straddle's own visible/focus refs — the same
 * bindings MainLayout used to hand-write per window.
 *
 * This is also what makes a soft-staged straddle's window possible at all:
 * its component import lives inside its own module, which the generated
 * straddles.gen.ts imports exactly when the straddle is staged — so the
 * component bundles iff staged, and no buildable source ever references a
 * package that might not be installed.
 *
 * Mount order = registration order = the generator's init order, so initial
 * stacking is deterministic. A straddle needing several windows (lxmf's
 * one-Messages-window-per-identity) registers a single wrapper component that
 * owns its own v-for and refs, passing only `component:` here.
 */
export interface WindowMount {
  /** Registry key; by convention the window id the component passes to
   *  FloatingWindow / lib/windows. */
  id: string
  /** The FloatingWindow-wrapping panel — or a self-contained wrapper that
   *  manages its own windows, in which case omit the bindings below. */
  component: Component
  /** Window title, passed through as the component's `title` prop. */
  title?: string
  /** The straddle's *Visible ref — bound as `visible`, written on update:visible. */
  visible?: Ref<boolean>
  /** The straddle's *Focus nonce ref — bound as `focus-token`. */
  focusToken?: Ref<number>
}

/* shallowReactive: map mutations are tracked (so <StraddleWindows/> re-renders
 * on registration) but the mount objects are NOT proxied — the component stays
 * a plain object and the refs stay refs, not reactive-unwrapped properties. */
const mounts = shallowReactive(new Map<string, WindowMount>())

export function registerWindowMount(m: WindowMount): void {
  mounts.set(m.id, { ...m, component: markRaw(m.component) })
}

/** Registered mounts in registration (= init) order. */
export const windowMounts = computed<WindowMount[]>(() => [...mounts.values()])
