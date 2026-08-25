/**
 * The small shared pieces the settings renderer needs: `{field}` substitution,
 * gate truthiness, and the one action kind that has no UI of its own (`set`).
 *
 * Substitution is `{field}` replacement and nothing else — no expressions, no
 * fallback chains, no slicing. Anything fancier is a string the firmware
 * publishes ready-made, which is the same reason a value row renders its key
 * verbatim and a gate is only ever tested for truthiness.
 */
import { computed } from 'vue'
import { useDeviceStore } from '../stores/device'
import { setAndReboot } from './reboot'
import type { GenSet } from './settingsNodes'

/** What a template resolves against: one collection item, one form's fields, or
 *  nothing at all. */
export type Scope = Record<string, unknown> | null | undefined

export function subst(tmpl: string | undefined, scope: Scope): string {
  if (!tmpl) return ''
  return tmpl.replace(/\{(\w+)\}/g, (_m, field: string) => {
    const v = scope?.[field]
    return v === undefined || v === null ? '' : String(v)
  })
}

/** A collection's items, whichever shape storage holds them in.
 *
 *  A list seeded by a JSON default (`"nets": []`) is a real array, and stays
 *  one: an indexed patch merges into it element-wise. A list that was never
 *  seeded is an OBJECT keyed "0", "1", … — a write to `s.tcp.peers.0.host`
 *  builds a patch of nested objects, and with no array underneath to merge
 *  into, the object is what lands in the tree and what the dump ships us. The
 *  firmware reads both (`storageArrayCount` counts an array's items or an
 *  object's contiguous numeric keys), so this side must too, or a device's
 *  peers are on its display and nowhere in the browser. Contiguous from 0 and
 *  stop at the first gap, exactly as the firmware counts. */
export function asItems(v: unknown): Record<string, unknown>[] {
  if (Array.isArray(v)) return v as Record<string, unknown>[]
  if (!v || typeof v !== 'object') return []
  const obj = v as Record<string, unknown>
  const out: Record<string, unknown>[] = []
  for (let i = 0; obj[String(i)] !== undefined && obj[String(i)] !== null; i++) {
    out.push(obj[String(i)] as Record<string, unknown>)
  }
  return out
}

/** Keep the browser's password manager off a settings input. None of these
 *  fields is a credential for anything the manager knows: a secret row is
 *  write-only and always renders empty, and a host or a port filled from a
 *  vault is simply wrong. An extension that adopts one fills it and takes the
 *  caret with it, which on a long pane scrolls the operator somewhere they
 *  weren't. One attribute per vendor, because each reads only its own. */
export const NO_MANAGER = {
  'data-1p-ignore': 'true',
  'data-lpignore': 'true',
  'data-bwignore': 'true',
  'data-protonpass-ignore': 'true',
  'data-form-type': 'other',
} as const

/** Whether a control may be rendered: the storage dump has landed, so every
 *  key it binds holds the device's value.
 *
 *  A control mounted before that shows its zero state — a switch off, a field
 *  empty — and then animates to the truth as the dump merges, which reads as
 *  the act of opening the page having changed the setting. Held back until the
 *  dump is in, a control's FIRST frame is its real value: right without an
 *  animation to make it right, and nothing to see in the meantime. `synced`
 *  only ever goes up (a reconnect keeps the mirror it already has), so this
 *  gates the first paint and nothing after it. */
export function useSettingsReady() {
  const device = useDeviceStore()
  return computed(() => device.synced)
}

/* One palette, named the same way wherever a colour is stated — a status pill,
 * a button. The firmware's table (lcd_settings_desc.cpp) holds these same
 * hexes, so a red button is the red a red pill is on either surface. */
const NAMED: Record<string, string> = {
  green: '#2e7d43', red: '#8b2b2b', amber: '#8a6d1f',
  blue: '#2563a0', grey: '#3a4658', gray: '#3a4658',
}

/** A palette name or an explicit "rrggbb" as CSS. Empty falls back to grey,
 *  which is what an unstated pill colour has always been. */
export function paletteColor(name: string | undefined): string {
  if (!name) return NAMED.grey
  return NAMED[name] ?? (name.startsWith('#') ? name : `#${name}`)
}

/** The width of the name column, and so where the control column starts. Every
 *  part of a pane that has to line up with it — a readout's grid, a description
 *  hung under a row, a hint under a slider — states it from here, so the pane
 *  keeps one boundary and it moves in one edit. */
export const NAME_COL = '25%'

/** A settings button: FILLED with its palette colour and lettered in white,
 *  exactly as the device's are — a colour the button states is a background,
 *  never ink. An outline in that same colour on a near-black pane leaves thin
 *  coloured text on black, which reads as a link rather than a control. A
 *  button that states no colour is the palette's blue, the shade a blue pill
 *  is. */
export function buttonStyle(color?: string): Record<string, string> {
  return {
    backgroundColor: paletteColor(color || 'blue'),
    color: '#fff',
    /* Wide and shallow: the label wants room either side of it, and a settings
     * button is one line of a pane rather than a call to action. minHeight
     * clears Quasar's own floor, which would otherwise decide the height. */
    padding: '2px 16px',
    minHeight: '0',
  }
}

/** A gate key is TRUTHY or it is not, never compared against a value — the
 *  firmware publishes gate keys as truthy/empty exactly so this stays a
 *  one-liner on both surfaces. */
export function truthy(v: unknown): boolean {
  return v !== undefined && v !== null && v !== '' && v !== 0 && v !== '0' && v !== false
}

/** Whether a row survives its `whenKey`. Inside a form or an item editor the
 *  gate may name a SIBLING FIELD as a template ("{dhcp}"), which is answered
 *  from the local scope; a bare key names storage. */
export function rowVisible(whenKey: string | undefined, scope: Scope): boolean {
  if (!whenKey) return true
  if (whenKey.includes('{')) return truthy(subst(whenKey, scope))
  return truthy(useDeviceStore().get(whenKey))
}

/** Whether a collection's per-item action survives its `whenKey`. The template
 *  here builds a KEY out of the item ("wifi.netjoinable.{id}") rather than
 *  reading a sibling field's value the way a row's gate does — the gate belongs
 *  to the item, and only the device knows its state. */
export function itemActionVisible(whenKey: string | undefined, item: Scope): boolean {
  if (!whenKey) return true
  return truthy(useDeviceStore().get(subst(whenKey, item)))
}

/** The `set` action: write a key, with the two annotations that change what the
 *  write means rather than what it writes. */
export function runSet(spec: GenSet, scope?: Scope) {
  const device = useDeviceStore()
  const key = subst(spec.key, scope)
  const value = subst(spec.value, scope)
  if (spec.reboots) {
    setAndReboot(key, value, { edge: spec.edge })
    return
  }
  /* An edge write forces a change past the storage actor's dedup: a command
   * flag left set by an attempt that did not complete would otherwise swallow
   * every later press, permanently. */
  if (spec.edge) device.set(key, 0)
  device.set(key, value)
  device.save()
}

/* ── restricting what a field will take ──
 *
 * `beforeinput` rather than filtering after the fact: a filter applied in the
 * update handler cannot change what is on screen when the filtered result
 * equals the value already bound — Vue sees no change and never re-renders, so
 * the rejected character sits in the box. Refusing the insertion is both
 * simpler and the only version that actually holds, and it covers paste and
 * drop as well as typing.
 */

/** Refuse an insertion that would put a character outside `allowed` into the
 *  field. Bind as `@beforeinput`; the listener reaches the native input. */
export function onlyChars(allowed: RegExp) {
  return (e: InputEvent) => {
    /* Deletions and IME composition carry no data — nothing to vet. */
    if (e.data == null) return
    for (const ch of e.data) if (!allowed.test(ch)) { e.preventDefault(); return }
  }
}

/** Digits, and a minus only where the range goes below zero. */
export const numberChars = (signed: boolean) => onlyChars(signed ? /[0-9-]/ : /[0-9]/)
/** Digits and dots. */
export const quadChars = onlyChars(/[0-9.]/)
