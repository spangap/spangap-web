/**
 * safeMode — enter the device's recovery boot from the browser.
 *
 * Three device operations (back the state store up, restore one, factory reset)
 * all need a boot where nothing else is touching that store. Entering one is an
 * ORDINARY STORAGE WRITE: the firmware watches the three keys and, when one is
 * set on a running system, saves and reboots into the operation. There is no
 * endpoint here, no token and no new transport — the same write from the CLI or
 * over rnsh does exactly the same thing.
 *
 * What the browser owns is only the wait. The device goes away mid-request, and
 * the page must RELOAD afterwards: without a real reload the browser sits on the
 * stale app shell and never sees the safe-mode page the device is now serving.
 */
import { useDeviceStore } from '../stores/device'
import { getSession } from './webrtc-session'

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

/* Two covers, and the difference is whether there is anything left to say.
 *
 * A reboot the operator just asked for is a WAIT, not an outage: black screen,
 * spinner, no words. They pressed OK on a dialog that said the device would
 * reboot, so a paragraph explaining that it is rebooting only tells them what
 * they already know. The card is for the cases that carry information the
 * operator does not have — a device that will not come back to this address, or
 * a request that never left. */
const SPIN_HTML = `
<div id="safemode-wait" style="position:fixed;inset:0;z-index:100000;background:#000;
  display:flex;align-items:center;justify-content:center">
  <div style="width:44px;height:44px;border:3px solid rgba(255,255,255,.18);
    border-top-color:#fff;border-radius:50%;animation:safemode-spin 1s linear infinite"></div>
</div>
<style>@keyframes safemode-spin{to{transform:rotate(360deg)}}</style>`

const CARD_HTML = `
<div id="safemode-wait" style="position:fixed;inset:0;z-index:100000;
  background:rgba(0,0,0,.48);color:#fff;font:15px/1.5 system-ui,sans-serif;
  display:flex;align-items:center;justify-content:center;padding:1rem">
  <div style="background:#1d1d1d;border-radius:6px;width:100%;max-width:26rem;
    padding:1.1rem 1.3rem 1.3rem;box-shadow:0 10px 30px rgba(0,0,0,.6)">
    <h2 style="font-size:1.15rem;font-weight:600;margin:0 0 .6rem" id="safemode-title"></h2>
    <p style="color:#c9ced4;margin:.6rem 0 0" id="safemode-note"></p>
  </div>
</div>`

/** Put up a full-screen cover. `html` is injected into a host appended to
 *  <body>, so it outlives whatever the SPA does to its own root. */
function show(html: string) {
  const host = document.createElement('div')
  host.innerHTML = html
  document.body.appendChild(host)
}

/** The spinner: a wait with nothing to report. */
function spin() { show(SPIN_HTML) }

/** The card: a wait the operator needs told something about. */
function cover(title: string, note: string) {
  show(CARD_HTML)
  const th = document.getElementById('safemode-title')
  const nh = document.getElementById('safemode-note')
  if (th) th.textContent = title
  if (nh) nh.textContent = note
}

/** Reload onto whatever the device is serving once it is back. The caller has
 *  already put a cover up; this is only the timer under it.
 *
 *  A fixed wait, not a probe. Probing here was wrong twice over: a `fetch` that
 *  succeeds proves nothing (the device serves normally for the second or two
 *  before it restarts, and the flag write can take longer than that to persist),
 *  and a `fetch` that fails cannot be told apart from a certificate the browser
 *  will not accept. Both readings led the page to announce something untrue —
 *  "the device did not restart" over a device that was mid-reboot. Waiting a
 *  fixed period and navigating hands the outcome to the browser, which is the
 *  only component that can actually show it. */
const RESTART_WAIT_MS = 12000

function waitForReboot() {
  setTimeout(() => location.reload(), RESTART_WAIT_MS)
}

/** Write the flag, flush it, and wait out the reboot. The device does the rest.
 *  `value` is the factory-reset target (WIPE_*); ignored for the other two. */
export function enterSafeMode(op: SafeModeOp, value = 1) {
  const device = useDeviceStore()

  /* device.set() is silent when the config channel is down: it queues the write
   * and returns, so the button would appear to do nothing at all — and the
   * session close below would then guarantee the queued write never left.
   * Refuse up front and say why, rather than covering the screen with a
   * "rebooting" message for a device that was never asked to. */
  if (!device.connected) {
    cover('Not connected to the device',
          'The request was not sent. Reload the page, wait for it to connect, ' +
          'and try again.')
    return
  }

  /* Clear first, then set. These flags are edge-triggered COMMANDS carried in a
   * value, and the storage actor dedups a write whose value already equals the
   * committed one — no change, so no notification, so no reboot. A flag left at
   * 1 by an attempt that did not reboot would therefore swallow every later
   * press, permanently: the one state from which the button can never work
   * again. Writing 0 first guarantees an edge whatever the flag was. The 0 is
   * itself a change the watcher sees and ignores (it acts on non-zero only). */
  device.set(FLAG_KEY[op], 0)
  device.set(FLAG_KEY[op], value)
  device.save()

  /* Drop the WebRTC session once the write is on the wire. Safe mode does not
   * bring webrtc up, so leaving the session running means the SPA spends the
   * whole window reconnecting to something that will never answer. One second is
   * enough for the flag patch and the save to leave; the device does the rest on
   * its own. */
  setTimeout(() => { try { getSession().close() } catch { /* already gone */ } }, 1000)

  if (op === 'factory') {
    /* The one reboot with something to say: the device is not coming back to
     * this address, so the page says so instead of spinning as if it were. */
    cover('Erasing this device',
          'It reboots, overwrites its state, and comes back on its own access ' +
          'point — this browser will not find it here again.')
  } else {
    spin()
  }
  waitForReboot()
}
