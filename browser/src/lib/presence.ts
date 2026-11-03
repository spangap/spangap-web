import { ref, type Ref } from 'vue'

/**
 * Whether a person is at this tab: the tab is visible, and how recently they
 * did something real in it (a pointer or key going down, a scroll, a touch).
 * Page load counts as input: somebody opened the page.
 */

/** document.visibilityState === 'visible', kept current. */
export const tabVisible = ref(document.visibilityState === 'visible')
document.addEventListener('visibilitychange', () => {
  tabVisible.value = document.visibilityState === 'visible'
})

let lastInput = Date.now()
const recents: { ms: number; flag: Ref<boolean>; timer: number }[] = []

function arm(r: (typeof recents)[number]) {
  clearTimeout(r.timer)
  r.flag.value = true
  r.timer = window.setTimeout(() => { r.flag.value = false }, Math.max(0, lastInput + r.ms - Date.now()))
}

for (const ev of ['pointerdown', 'keydown', 'wheel', 'touchstart']) {
  window.addEventListener(ev, () => {
    lastInput = Date.now()
    for (const r of recents) arm(r)
  }, { capture: true, passive: true })
}

/** True while there has been input within the last `ms`. One flag per `ms`,
 *  shared by every caller asking for the same window. */
export function useRecentInput(ms: number): Ref<boolean> {
  let r = recents.find(x => x.ms === ms)
  if (!r) {
    r = { ms, flag: ref(true), timer: 0 }
    recents.push(r)
    arm(r)
  }
  return r.flag
}
