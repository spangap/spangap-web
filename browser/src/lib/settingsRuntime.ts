/**
 * The small shared pieces the settings renderer needs: `{field}` substitution,
 * gate truthiness, and the one action kind that has no UI of its own (`set`).
 *
 * Substitution is `{field}` replacement and nothing else — no expressions, no
 * fallback chains, no slicing. Anything fancier is a string the firmware
 * publishes ready-made, which is the same reason a value row renders its key
 * verbatim and a gate is only ever tested for truthiness.
 */
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
