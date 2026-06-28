/**
 * Runtime registry for declarative settings panels.
 *
 * The build (spangap-inside) lowers each straddle.yaml `settings:` block into a
 * JSON descriptor inlined into the buildable's straddles.gen.ts, then calls
 * registerGeneratedPanels() with the list. Here we (1) name/sort the menu
 * containers the panels live under, (2) register each panel's leaf in the menu
 * tree pointing at the single shared GeneratedPanel.vue, and (3) stash the
 * descriptor by its leaf path so GeneratedPanel can look itself up at render
 * time (the menu store renders a panel component with no props, so the panel
 * finds its descriptor via the active-panel id).
 *
 * This is the web parallel to the firmware spangapSettingsGenRegister() — one
 * generic renderer interprets the data, rather than generating a Vue SFC per
 * pane.
 */
import { useMenuStore } from '../stores/menu'
import GeneratedPanel from '../components/GeneratedPanel.vue'

export interface GenOption {
  label: string
  value: string
}

export interface GenRow {
  kind: string // section | caption | switch | slider | text | dropdown | value | button | list
  text?: string // section/caption
  label?: string
  k?: string
  min?: number
  max?: number
  secret?: boolean
  options?: GenOption[] // dropdown
  cmd?: string // button / list add-remove
  payload?: string // button
  itemLabel?: string // list — template over item fields, e.g. "{host}:{port}"
  add?: string // list — command key the "+" writes
  remove?: string // list — command key a row delete writes (item id)
  fields?: GenRow[] // list — per-item editor rows
}

export interface GenContainer {
  path: string
  label: string
  placement?: number
}

export interface GenPanel {
  id: string // full menu leaf path, e.g. "settings/network/mdns"
  label: string
  placement?: number
  containers?: GenContainer[]
  rows: GenRow[]
}

const registry = new Map<string, GenPanel>()

/** The descriptor for the panel at menu leaf path `id` (undefined if none). */
export function getGeneratedPanel(id: string | null): GenPanel | undefined {
  return id ? registry.get(id) : undefined
}

export function registerGeneratedPanels(panels: GenPanel[]): void {
  const menu = useMenuStore()
  for (const panel of panels) {
    for (const c of panel.containers ?? []) {
      // Only override placement when the descriptor set one — otherwise leave a
      // container's placement (possibly established by another straddle) alone.
      const opts: { label: string; placement?: number } = { label: c.label }
      if (c.placement !== undefined) opts.placement = c.placement
      menu.setMenu(c.path, opts)
    }
    registry.set(panel.id, panel)
    const opts = panel.placement !== undefined ? { placement: panel.placement } : {}
    menu.register(panel.id, panel.label, { type: 'panel', component: GeneratedPanel }, opts)
  }
}
