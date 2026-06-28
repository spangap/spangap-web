import { reactive, computed } from 'vue'

/**
 * App registry — the launchable "apps" surfaced by the bottom Dock.
 *
 * The dock replaced the menu bar / hamburger: every launchable thing (Settings,
 * CLI, System Log, Maps, Messages, Nomad, Viewer, …) registers one AppEntry and
 * the dock renders one icon per app, sorted by placement. Clicking an icon calls
 * `open()` (which raises/shows the app's FloatingWindow via the straddle's
 * existing *Visible / *Focus refs). `isOpen()` drives a running-app dot.
 *
 * App-bearing straddles register from their browser register* module (the same
 * place they already register menu items and export their window-visibility
 * refs), so staging a straddle is enough to surface its dock icon.
 */
export interface AppEntry {
  id: string
  label: string
  icon: string // basename → /app-icons/<icon>.svg (surfaced from each straddle's lcd-icons)
  open: () => void
  /** Sibling ordering, same buckets as the menu store: >0 left, 0 middle, <0 right. */
  placement?: number
  /** Optional running-state probe — the dock shows a dot when true. */
  isOpen?: () => boolean
}

const apps = reactive(new Map<string, AppEntry>())

export function registerApp(a: AppEntry): void {
  apps.set(a.id, a)
}

/* ── Icon registry ──
 * The launcher SVGs are bundled into app.js by the generated straddles.gen.ts
 * (import.meta.glob over src/app-icons/*.svg as raw strings) and registered
 * here, keyed by basename. The Dock renders them inline (v-html) — the device
 * webroot ships only app.js, so a separate /app-icons/*.svg asset never reaches
 * the device; bundling the SVG source is what makes the icons appear. */
const iconSvgs = reactive(new Map<string, string>())

export function registerAppIcons(glob: Record<string, string>): void {
  for (const [path, svg] of Object.entries(glob)) {
    const base = path.split('/').pop()?.replace(/\.svg$/, '') ?? ''
    // Strip any leading <?xml …?> prolog / comments so the string starts at the
    // <svg> element — cleaner when injected via v-html.
    if (base) iconSvgs.set(base, svg.replace(/^\s*<\?xml[^>]*\?>\s*/i, ''))
  }
}

/** The raw inline SVG for an app icon basename, or undefined if none bundled. */
export function appIconSvg(name: string): string | undefined {
  return iconSvgs.get(name)
}

function placeRank(p: number): number {
  return p > 0 ? 0 : p < 0 ? 2 : 1
}

/** Dock order: positive placements first (ascending), then 0 (alphabetic by
 *  label), then negative (ascending). Mirrors the menu store's comparator. */
export const sortedApps = computed<AppEntry[]>(() =>
  [...apps.values()].sort((a, b) => {
    const pa = a.placement ?? 0
    const pb = b.placement ?? 0
    const ra = placeRank(pa)
    const rb = placeRank(pb)
    if (ra !== rb) return ra - rb
    if (pa !== pb) return pa - pb
    return a.label.localeCompare(b.label)
  }),
)
