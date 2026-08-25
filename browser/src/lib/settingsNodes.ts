/**
 * The settings descriptor types, and the entry point the build calls.
 *
 * spangap-inside lowers every straddle's `settings:` block into node-tree
 * FRAGMENTS — a path of segments plus one row block — inlined into the
 * buildable's straddles.gen.ts and handed to registerSettingsNodes(). The
 * settings-tree store merges them by segment id; one renderer (NodePane.vue)
 * interprets the rows. There is no generated component per pane and no YAML
 * parser in the SPA.
 *
 * Two firmware conventions are what let a static descriptor describe a whole
 * pane. The firmware publishes FINISHED STRINGS: a status pill, a subtitle, a
 * value row all render exactly what the key holds, so nothing here formats,
 * composes or compares — a `whenKey` gate is a truthiness test, never an
 * equality one. And the firmware VALIDATES IN SENTINEL HANDLERS: a form
 * submits to a command key and the owning task answers on `<cmd>.error`, so
 * this side submits and displays rather than checking as you type.
 */
import type { Component } from 'vue'
import { useSettingsTreeStore } from '../stores/settingsTree'

export interface GenOption {
  label: string
  value: string
}

/** Write a key. `edge` writes 0 first, forcing a change past the storage
 *  actor's dedup; `reboots` runs the shared reboot-wait behaviour after. */
export interface GenSet {
  key: string
  value: string
  edge?: boolean
  reboots?: boolean
}

export interface GenDialogButton {
  label: string
  /** A palette name ("red", "green", "amber", "blue", "grey") or an explicit
   *  "rrggbb" — the same vocabulary and the same table a status pill uses. */
  color?: string
  do?: GenAction
}

/** A confirmation or choice. No input fields, ever — that is what a form is
 *  for. Every button closes the dialog; a button with no action is a cancel. */
export interface GenDialog {
  text: string
  buttons: GenDialogButton[]
}

/** The one dialog that carries inputs, because it fronts a sentinel. */
export interface GenForm {
  fields: GenRow[]
  cmd: string
  submit?: string
  title?: string
}

export interface GenAction {
  set?: GenSet
  dialog?: GenDialog
  form?: GenForm
}

export interface GenItemAction {
  label: string
  color?: string
  // Show this action only while the key is truthy. `{field}` templates against
  // the ITEM to build the key — unlike a row's whenKey, where a template names a
  // sibling field's value. The firmware publishes the gate.
  whenKey?: string
  do: GenAction
}

/** One button of a `buttons:` row. A lone `button:` row spans its line; these
 *  share one, so each sizes to its label and the row states where they sit. */
export interface GenButton {
  label: string
  color?: string
  whenKey?: string
  do: GenAction
}

/** Scan-and-adopt: an ephemeral array the owning task publishes. Picking a row
 *  opens the collection's first add form, prefilled.
 *
 *  The results are a POPUP on both surfaces, opened by the
 *  refresh button and headed by `found`: what the device can see is a different
 *  question from what it is configured for, and a transient answer to it —
 *  arriving over seconds, changing, and gone once you stop asking. Opening the
 *  popup starts the scan and closing it stops the scan. */
export interface GenCandidates {
  k: string
  item: string
  subtitle?: string
  found?: string // heading over the results; defaults to the refresh label
  refresh?: { label: string; do: GenAction }
  map?: Record<string, string>
}

export interface GenRow {
  // title|heading|section|caption|advanced|switch|slider|integer|text|dropdown|
  // timezone|value|button|buttons|info|list|component
  // heading: a level-1 heading above `section`; advanced: a disclosure group
  // whose `rows` render in place when opened (pane rows bind keys, editor rows
  // bind fields).
  // timezone: an IANA zone picker, form fields only. Optionless — this client
  // builds the list from its own Intl database; placeholderKey seeds the
  // initial selection with the currently applied zone.
  kind: string
  text?: string // section / caption
  label?: string
  k?: string // storage key (pane rows)
  field?: string // form / item-editor field name
  dflt?: string // form prefill; may be a "{field}" template. Never seeded
  whenKey?: string // show only while truthy; "{field}" allowed inside a form
  min?: number
  max?: number
  // Bounds the device publishes (slider and integer alike). Where set, the
  // key's value replaces the number above — a limit the firmware measured on
  // its own hardware, which the build could not know. The number stays as the
  // fallback until the key exists.
  minKey?: string
  maxKey?: string
  // integer — a number typed in rather than dragged to. `min`/`max` are absent
  // where the quantity has no bound; a value outside them is refused on commit
  // with a warning and never reaches storage. `buttons` adds a -/+ pair
  // stepping by `step` (default 1), which SNAPS to its own multiples: at step
  // 5, down from 23 is 20 and then 15.
  step?: number
  buttons?: boolean
  // A word printed after the field — a unit ("min", "dBm") or the fixed tail of
  // what is being entered (".duckdns.org"). Never part of the value; a field
  // carrying one is short and right-aligned.
  unit?: string
  // A third of a field's usual width, for an entry that is a handful of
  // characters. Numbers are short already.
  short?: boolean
  secret?: boolean
  placeholder?: string
  // A key holding the placeholder text, for a hint only the device knows (the
  // MAC it would use, the port it would pick). Wins over `placeholder`.
  placeholderKey?: string
  options?: GenOption[]
  searchable?: boolean // type-to-filter picker, for lists too long to scan
  copyable?: boolean // value rows: click-to-copy
  color?: string // button: a palette name or "rrggbb"
  do?: GenAction // button
  // buttons — several content-sized buttons on one line, gathered at `align`
  align?: 'left' | 'center' | 'right'
  items?: GenButton[]
  // info — a run of read-only values as one compact block: a shared label column
  // sized to the widest label but never wider than a third, and no gap between
  // the lines. Value rows only; a control needs room a narrow column cannot give.
  rows?: GenRow[]
  // list (a collection). `caption` sits between the heading and the rows: what
  // the list is, and what its order means — the rows are the device's own data
  // and carry no room for prose.
  caption?: string
  id?: string
  item?: string
  subtitle?: string
  status?: string
  empty?: string
  // Drag the rows into the order you want. The display starts the drag on a
  // grip so a drag elsewhere still scrolls the pane; here the row is the handle.
  reorder?: boolean
  cmd?: string
  add?: { label: string; form: GenForm }[]
  remove?: { confirm?: string }
  actions?: GenItemAction[]
  edit?: GenRow[]
  candidates?: GenCandidates
  // A hand-written panel occupying this node, contributed through the menu
  // store's settings adapter. Transitional: it goes with the last *Panel.vue.
  component?: Component
}

/** One path segment, carrying the naming this contributor proposes. */
export interface GenSegment {
  id: string
  label: string
  short?: string
  order?: number
}

/** One contribution: a path, and the rows to add at its last segment. */
export interface GenNode {
  segments: GenSegment[]
  rows: GenRow[]
}

export function registerSettingsNodes(nodes: GenNode[]): void {
  const tree = useSettingsTreeStore()
  for (const node of nodes) tree.contribute(node.segments, node.rows)
}
