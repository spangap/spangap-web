/**
 * v-fit-text — keep a one-line heading on one line, shrinking it if it must.
 *
 * The element's own stylesheet size is the size it wants. When the phrase is
 * wider than the box, the type size drops just far enough to fit, so a long
 * heading shrinks instead of wrapping or being clipped.
 *
 * For BLOCK elements only, and the element must carry `white-space: nowrap`
 * and `overflow: hidden` in its own rule:
 *   - nowrap makes `scrollWidth` the width of the unbroken phrase, so the
 *     natural width can be read straight off the element with no inner span
 *     and no offscreen measuring copy.
 *   - a block's width comes from its container, so writing a font size back
 *     cannot change the width being observed, and a fit never re-triggers
 *     itself. On an inline-block it would.
 *
 * Every measurement is taken at the stylesheet size — the inline size is
 * cleared first — so a fit never compounds on the result of the previous one
 * and repeated fits land on the same answer.
 */

interface FitState {
  observer: ResizeObserver
}

const states = new WeakMap<HTMLElement, FitState>()

function fit(el: HTMLElement): void {
  /* Measure at the stylesheet's size, whatever a previous fit applied. */
  el.style.fontSize = ''
  const base = parseFloat(getComputedStyle(el).fontSize)
  if (!base) return

  const avail = el.clientWidth
  const natural = el.scrollWidth
  if (avail <= 0 || natural <= 0) return   /* hidden, empty, or not yet laid out */
  if (natural <= avail) return             /* fits as written */

  /* Glyph advances scale linearly with type size, so the size that fits is one
   * division — no trial sizes and no convergence loop. Tenths of a pixel:
   * finer is invisible, and the rounding keeps sub-pixel noise in the
   * measurement from nudging the size to and fro. */
  el.style.fontSize = `${Math.floor(((base * avail) / natural) * 10) / 10}px`
}

export const vFitText = {
  mounted(el: HTMLElement) {
    const observer = new ResizeObserver(() => fit(el))
    observer.observe(el)
    states.set(el, { observer })
    fit(el)
    /* Fallback-font metrics differ from the real face, so a fit measured
     * before the face arrives would be measuring the wrong glyphs. */
    void document.fonts?.ready.then(() => fit(el))
  },

  /* The phrase changed — a heading re-rendered with different text. */
  updated(el: HTMLElement) {
    fit(el)
  },

  unmounted(el: HTMLElement) {
    states.get(el)?.observer.disconnect()
    states.delete(el)
  },
}
