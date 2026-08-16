/**
 * safeMode — enter the device's recovery boot from the browser.
 *
 * Three device operations (back the state store up, restore one, factory reset)
 * all need a boot where nothing else is touching that store. Entering one is an
 * ordinary storage write plus the reboot wait that every rebooting write needs,
 * which lives in lib/reboot.ts — this file is only the three flag names and the
 * one case with something to say.
 */
import { setAndReboot } from './reboot'

export type SafeModeOp = 'backup' | 'restore' | 'factory'

const FLAG_KEY: Record<SafeModeOp, string> = {
  backup: 's.sys.backup',
  restore: 's.sys.restore',
  factory: 's.sys.factory_reset',
}

/** Factory-reset targets, matching SAFE_WIPE_* in spangap.h. */
export const WIPE_FLASH = 1
export const WIPE_SD = 2
export const WIPE_BOTH = 3

/** Write the flag and wait out the reboot. `value` is the factory-reset target
 *  (WIPE_*); ignored for the other two. */
export function enterSafeMode(op: SafeModeOp, value = 1) {
  setAndReboot(FLAG_KEY[op], value, {
    edge: true,
    /* The one reboot with something to say: the device is not coming back to
     * this address, so the page says so instead of spinning as if it were. */
    notice: op === 'factory'
      ? { title: 'Erasing this device',
          note: 'It reboots, overwrites its state, and comes back on its own ' +
                'access point — this browser will not find it here again.' }
      : undefined,
  })
}
