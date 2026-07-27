import { ref, watch, onUnmounted, nextTick, type Ref } from 'vue'
import { useCompact } from './viewport'

/**
 * Auto-chromeless: drive a FloatingWindow in and out of chromeless mode as its
 * own content stops or starts fitting.
 *
 * Chromeless mode (FloatingWindow's `chromeless` prop) drops the titlebar, moves
 * the close control to a hover-only dot over the body, and pins the window on
 * top. This composable decides *when* a window should be in that mode: it watches
 * the body's height and asks the caller's `fits()` predicate. The window binds
 * the returned `collapsed` flag to `:chromeless` and supplies only what "fits"
 * means for its content — the observe/hysteresis machinery is shared.
 *
 * Hysteresis: collapsing removes the titlebar and hands its height back to the
 * body, which could make the content fit again and flip straight back — a
 * flicker loop. So expansion requires the content to fit even once that height
 * is *restored*: we test `fits(currentHeight - reclaimedChrome)`. The reclaimed
 * height is the titlebar's, so the collapse/expand thresholds sit a full
 * titlebar apart.
 */

/** Titlebar heights FloatingWindow reclaims when it drops its chrome (px). Kept
 *  in sync with FloatingWindow.vue's `.fw-titlebar` / `.fw--compact` heights. */
const CHROME_PX = 28
const CHROME_PX_COMPACT = 44

export interface ChromelessFitOptions {
  /** The FloatingWindow body element whose height drives the fit test. */
  el: Ref<HTMLElement | undefined>
  /** True iff the window's content fits in `availH` px of body height. Called
   *  with the live height to decide collapse, and with the height minus the
   *  reclaimed titlebar to decide expansion (the hysteresis). A predicate that
   *  ignores `availH` (e.g. a purely horizontal test) simply gets no vertical
   *  hysteresis, which is correct — chrome only frees vertical space. */
  fits: (availH: number) => boolean
}

export interface ChromelessFit {
  /** Bind to FloatingWindow's `chromeless` prop. */
  collapsed: Ref<boolean>
  /** Re-run the fit test now — for callers whose `fits()` result can change
   *  without the body resizing (e.g. its text content changed). */
  evaluate: () => void
}

export function useChromelessFit(opts: ChromelessFitOptions): ChromelessFit {
  const collapsed = ref(false)
  const compact = useCompact()
  const observer = new ResizeObserver(() => evaluate())

  function evaluate(): void {
    const body = opts.el.value
    if (!body) return
    const h = body.clientHeight
    if (h <= 0) return                          /* hidden / not yet laid out */
    if (!collapsed.value) {
      if (!opts.fits(h)) collapsed.value = true
    } else {
      const reclaim = compact.value ? CHROME_PX_COMPACT : CHROME_PX
      if (opts.fits(h - reclaim)) collapsed.value = false
    }
  }

  /* Follow the element across mount/unmount: observe it once it exists, and
   * re-check on the next tick when it first gains a size. */
  watch(opts.el, (el) => {
    observer.disconnect()
    if (el) { observer.observe(el); nextTick(evaluate) }
  }, { immediate: true })

  onUnmounted(() => observer.disconnect())

  return { collapsed, evaluate }
}
