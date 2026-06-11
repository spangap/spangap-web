import { computed, type ComputedRef } from 'vue'
import { useQuasar } from 'quasar'

/**
 * Single source of truth for "are we on a phone-class viewport".
 *
 * Below this breakpoint the whole shell changes character: the desktop
 * menu bar becomes a hamburger, the many overlapping FloatingWindows
 * collapse to one full-screen window switched from the menu, the settings
 * drawer takes the entire screen, and app internals that use a desktop
 * multi-column layout (e.g. LXMF's master/detail) fold to a single column.
 *
 * Every one of those decisions reads this same flag so they flip together
 * and the breakpoint lives in exactly one place. `< md` (≈ <1024px) matches
 * the breakpoint MenuBar already used for its hamburger.
 */
export function useCompact(): ComputedRef<boolean> {
  const $q = useQuasar()
  return computed(() => $q.screen.lt.md)
}
