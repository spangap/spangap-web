/**
 * settingsTree — the Settings app's one tree.
 *
 * Every node holds rows AND children: the pane renders its own rows first, then
 * its children as navigation entries. No node is owned — contribute() names a
 * path of segment ids, conjures whatever is missing along the way, and appends
 * a row block, so several straddles contributing at the same path simply
 * concatenate. A node's path is its stable id (deep links, the nav tree's
 * selection), exactly as the menu paths were. A node with no rows and no
 * rendering descendant is not shown (nodeRenders below): naming a menu is not
 * the same as showing one.
 *
 * This is deliberately NOT the menu store: that one keeps serving the menu-bar
 * groups (advanced/…, app/…), which are a window-manager mechanism with a
 * genuine leaf model, and nothing about settings should have to fit it.
 *
 * Ordering is the one global rule, shared with the LCD registry and the
 * generator: siblings carrying an `order` come first, ascending; everything
 * else follows in arrival order. The build emits contributions pre-sorted (it
 * knows straddle init order), so arrival order is already meaningful by the
 * time anything reaches here.
 */
import { defineStore } from 'pinia'
import { ref, computed, reactive } from 'vue'
import type { GenRow, GenSegment } from '../lib/settingsNodes'

export interface SettingsNode {
  id: string
  path: string // full slash-path from the root, e.g. "settings/mesh/lora"
  label: string // long name, shown in the parent's navigation entry
  short: string // compact name; defaults to the label
  order?: number
  arrival: number
  named: boolean // somebody supplied a label — last setter wins
  shortNamed: boolean
  rows: GenRow[]
  children: SettingsNode[]
}

export const ROOT_PATH = 'settings'

function titleCase(s: string): string {
  return s ? s[0].toUpperCase() + s.slice(1) : s
}

/** A node renders if it has anything to show: its own rows, or a descendant
 *  that has. Declaring a node is therefore not the same as putting it on the
 *  screen — a straddle may name a menu and give it an order while it is still
 *  empty, and it appears the moment somebody contributes to it. */
export function nodeRenders(node: SettingsNode): boolean {
  return node.rows.length > 0 || node.children.some(nodeRenders)
}

/** A node's children, minus the ones with nothing to render. */
export function visibleChildren(node: SettingsNode): SettingsNode[] {
  return node.children.filter(nodeRenders)
}

/** Siblings: ordered first ascending, unordered after them in arrival order. */
function bySiblingOrder(a: SettingsNode, b: SettingsNode): number {
  const ao = a.order !== undefined, bo = b.order !== undefined
  if (ao !== bo) return ao ? -1 : 1
  if (ao && a.order !== b.order) return a.order! - b.order!
  return a.arrival - b.arrival
}

export const useSettingsTreeStore = defineStore('settingsTree', () => {
  let arrival = 0

  const root = reactive<SettingsNode>({
    id: ROOT_PATH,
    path: ROOT_PATH,
    label: 'Settings',
    short: 'Settings',
    arrival: arrival++,
    named: true,
    shortNamed: true,
    rows: [],
    children: [],
  })

  /** The active pane's path. The root is a renderable node, so Settings opens
   *  on something rather than on an empty half-window. */
  const activePath = ref<string>(ROOT_PATH)

  function childOf(parent: SettingsNode, id: string): SettingsNode | undefined {
    return parent.children.find(c => c.id === id)
  }

  /** Merge a contribution: walk/conjure the path, apply last-setter-wins
   *  naming, then append the row block at the last segment. */
  function contribute(segments: GenSegment[], rows: GenRow[]): SettingsNode {
    let node = root
    for (const seg of segments) {
      let next = childOf(node, seg.id)
      if (!next) {
        next = reactive<SettingsNode>({
          id: seg.id,
          path: `${node.path}/${seg.id}`,
          label: titleCase(seg.id), // until somebody names it
          short: titleCase(seg.id),
          arrival: arrival++,
          named: false,
          shortNamed: false,
          rows: [],
          children: [],
        })
        node.children.push(next)
      }
      if (seg.label) { next.label = seg.label; next.named = true }
      if (seg.short) { next.short = seg.short; next.shortNamed = true }
      if (seg.order !== undefined) next.order = seg.order
      if (!next.shortNamed) next.short = next.label
      node.children.sort(bySiblingOrder)
      node = next
    }
    if (rows.length) node.rows.push(...rows)
    return node
  }

  function nodeAt(path: string | null): SettingsNode | null {
    if (!path) return null
    const segs = path.split('/').filter(Boolean)
    if (!segs.length || segs[0] !== ROOT_PATH) return null
    let node: SettingsNode = root
    for (const id of segs.slice(1)) {
      const next = childOf(node, id)
      if (!next) return null
      node = next
    }
    return node
  }

  const activeNode = computed(() => nodeAt(activePath.value) ?? root)

  function open(path: string) { activePath.value = path }
  function close() { activePath.value = ROOT_PATH }

  return { root, activePath, activeNode, contribute, nodeAt, open, close }
})
