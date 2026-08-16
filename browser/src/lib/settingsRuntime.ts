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
