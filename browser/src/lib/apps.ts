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

/* ── The image's app order ──
 * The straddle being built states one order for the whole device (straddle.yaml
 * `app_order:`); the generated straddles.gen.ts hands it over here, and the
 * firmware gets the same list as CONFIG_LCD_LAUNCHER_ORDER — so the Dock and the
 * panel's launcher grid offer the apps in the same order.
 *
 * An entry names an app by ANY name this surface knows it by — id, label or icon
 * basename, case-insensitively — because the two surfaces don't always label an
 * app identically, and one token has to reach both. A name for an app this image
 * doesn't carry is ignored, which is what lets one list span both surfaces'
 * apps: the panel's Maps and the browser's NetGraph sit in the same list, each
 * ignored where it doesn't exist. An app the list doesn't name keeps its
 * `placement` order, after the named ones. */
const appOrder = reactive<string[]>([])

export function registerAppOrder(names: string[]): void {
  appOrder.splice(0, appOrder.length, ...names.map((n) => n.toLowerCase()))
}

/** Position in the image's order, or Infinity for an app it doesn't name. */
function orderRank(a: AppEntry): number {
  if (!appOrder.length) return Infinity
  const names = [a.id, a.label, a.icon].map((n) => n.toLowerCase())
  const i = appOrder.findIndex((n) => names.includes(n))
  return i < 0 ? Infinity : i
}

function placeRank(p: number): number {
  return p > 0 ? 0 : p < 0 ? 2 : 1
}

/** Dock order: the image's app order first, in its order; then everything it
 *  doesn't name — positive placements (ascending), then 0 (alphabetic by label),
 *  then negative (ascending), which mirrors the menu store's comparator and is
 *  the whole order on an image that states none. */
export const sortedApps = computed<AppEntry[]>(() =>
  [...apps.values()].sort((a, b) => {
    const oa = orderRank(a)
    const ob = orderRank(b)
    if (oa !== ob) return oa < ob ? -1 : 1
    const pa = a.placement ?? 0
    const pb = b.placement ?? 0
    const ra = placeRank(pa)
    const rb = placeRank(pb)
    if (ra !== rb) return ra - rb
    if (pa !== pb) return pa - pb
    return a.label.localeCompare(b.label)
  }),
)
