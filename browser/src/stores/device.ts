/**
 * device — reactive mirror of the device's config tree.
 *
 * Uses a `storage:1` DataChannel on the shared WebRTC session (see
 * `lib/webrtc-session.ts`). The DC carries JSON merge-patches in both
 * directions, one message per packet: on open the device sends a full
 * dump; subsequent device changes arrive as coalesced patches. Browser
 * writes are sent as nested JSON patches too.
 */
import { defineStore } from 'pinia'
import { reactive, ref } from 'vue'
import { getSession } from '../lib/webrtc-session'
import { logSystemNotice } from './log'

export const useDeviceStore = defineStore('device', () => {
  const settings: Record<string, any> = reactive({})
  const connected = ref(false)
  /** True once a full storage dump has been received (the first {__dump:'e'}).
   *  Unlike `connected` (DataChannel open), this guarantees s.* values are
   *  populated — so consumers can read settings instead of seeing undefined. */
  const synced = ref(false)
  /** True once the link is considered down (no pong for >LINK_DOWN_MS, or the channel
   *  dropped after we'd been connected). Stays true through the reconnect until
   *  a fresh full storage dump lands — i.e. "reconnected AND resynced". Drives
   *  the full-screen ConnectionOverlay. */
  const linkDown = ref(false)

  const session = getSession()
  let dc: RTCDataChannel | null = null
  let unregisterBuilder: (() => void) | null = null
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let knownAssetId: number | null = null
  /** Wall-clock of the last pong (or channel open). The ping loop declares
   *  the link down when both this AND session.lastDcRxAt (traffic on any
   *  DataChannel) are >LINK_DOWN_MS stale. */
  let lastPongAt = 0
  /** Set once the storage channel has opened at least once, so the liveness
   *  check and drop-detection only fire after a real connection existed. */
  let everConnected = false
  let reloading = false
  let clientInfoPushed = false
  /** Somebody has interacted with this tab (see pushHuman), and whether that has
   *  been told to the device over the channel we currently hold. */
  let humanSeen = false
  let humanPushed = false
  /** Keys set while DC was down; flushed on reconnect so record.* toggles reach the device. */
  const pendingSet = new Map<string, string | number>()

  /** True if every key on `obj` is an unsigned-integer string. Empty objects
   *  are not numeric-keyed for our purposes — they shouldn't trigger array
   *  merge semantics. */
  function isNumericKeyedObject(obj: any): boolean {
    if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return false
    const keys = Object.keys(obj)
    if (keys.length === 0) return false
    return keys.every(k => /^\d+$/.test(k))
  }

  /** Merge a numeric-keyed-object patch into an existing array, element-wise.
   *  Mirror of deepMergeIntoArray() in storage.cpp — without this, a patch
   *  like {3:{pass:"x"}} arriving for an existing nets[] would replace the
   *  whole array. */
  function mergeIntoArray(dstArr: any[], patchObj: Record<string, any>) {
    const deletions = Object.keys(patchObj)
      .filter(k => patchObj[k] === null)
      .map(k => parseInt(k, 10))
      .sort((a, b) => b - a)
    for (const idx of deletions)
      if (idx >= 0 && idx < dstArr.length) dstArr.splice(idx, 1)

    for (const k of Object.keys(patchObj)) {
      const val = patchObj[k]
      if (val === null) continue
      const idx = parseInt(k, 10)
      const dstElem = dstArr[idx]
      if (val && typeof val === 'object' && !Array.isArray(val) &&
          dstElem && typeof dstElem === 'object' && !Array.isArray(dstElem)) {
        deepMerge(dstElem, val)
      } else if (idx < dstArr.length) {
        dstArr[idx] = val
      } else {
        while (dstArr.length < idx) dstArr.push(null)
        dstArr.push(val)
      }
    }
  }

  /** Deep-merge src into dst. `null` means delete (key/subtree). Plain arrays
   *  on src replace. Numeric-keyed objects merge element-wise into existing
   *  arrays (so individual array element fields can be patched). */
  function deepMerge(dst: any, src: any) {
    for (const key of Object.keys(src)) {
      const val = src[key]

      if (val === null) {
        delete dst[key]
        continue
      }

      if (Array.isArray(val)) {
        dst[key] = val
        continue
      }

      if (val && typeof val === 'object') {
        if (Array.isArray(dst[key]) && isNumericKeyedObject(val)) {
          mergeIntoArray(dst[key], val)
          continue
        }
        if (!dst[key] || typeof dst[key] !== 'object' || Array.isArray(dst[key])) dst[key] = {}
        deepMerge(dst[key], val)
        continue
      }

      dst[key] = val
    }
  }

  /** Read a value by dot-notation path (e.g., "s.camera.img.quality"). */
  function get(path: string): any {
    const parts = path.split('.')
    let obj: any = settings
    for (const p of parts) {
      if (obj == null) return undefined
      obj = obj[p]
    }
    return obj
  }

  /** Build nested object from a dot-notation path and value.
   *  e.g., ("s.camera.img.quality", 15) → {s:{camera:{img:{quality:15}}}} */
  function buildNested(path: string, val: any): any {
    const parts = path.split('.')
    const root: any = {}
    let current = root
    for (let i = 0; i < parts.length - 1; i++) {
      current[parts[i]] = {}
      current = current[parts[i]]
    }
    current[parts[parts.length - 1]] = val
    return root
  }

  function reloadForNewAssets() {
    reloading = true
    if (dc) { try { dc.close() } catch { /* */ } dc = null }
    fetch('/', { cache: 'no-store' })
      .catch(() => {})
      .finally(() => {
        setTimeout(() => {
          window.location.href = window.location.pathname + window.location.search
        }, 500)
      })
  }

  /** Reload SPA when deployed webroot bytes change — uses CRC32 from build_times (not file mtimes). */
  function checkBuildTime() {
    const bt = settings.sys?.buildtime
    if (!bt || typeof bt.fixed !== 'number') return
    const useWebCrc =
      Object.prototype.hasOwnProperty.call(bt, 'web') && typeof bt.web === 'number'
    const id = useWebCrc ? bt.web : bt.fixed
    if (knownAssetId === null) {
      knownAssetId = id
      console.log(
        '[device] asset id',
        useWebCrc ? `web crc32=${id}` : `fixed mtime fallback=${id}`,
      )
      return
    }
    if (id !== knownAssetId) {
      console.log('[device] web assets changed:', knownAssetId, '→', id, '— reloading')
      knownAssetId = id
      reloadForNewAssets()
    }
  }

  /** After a full dump has landed, push client time + timezone if needed.
   *  Only call this from the {__dump:'e'} handler: the dump streams in
   *  chunks, and reading settings mid-dump sees whatever subtrees happen to
   *  have merged — an undefined s.ntp.tz here would overwrite the device's
   *  configured timezone with the browser's. */
  function pushClientInfo() {
    if (clientInfoPushed) return
    clientInfoPushed = true

    /* Push epoch time if device doesn't have valid time */
    const valid = settings.sys?.time?.valid
    if (valid !== undefined && Number(valid) === 0) {
      const epoch = Math.floor(Date.now() / 1000)
      set('sys.time.set', epoch)
    }

    /* Push browser timezone if not yet configured. We send only the IANA
     * name — the device resolves the POSIX string itself from its built-in
     * zone table. */
    const tz = settings.s?.ntp?.tz
    if (tz === undefined || tz === '') {
      try {
        const ianaName = Intl.DateTimeFormat().resolvedOptions().timeZone
        if (ianaName) set('s.ntp.tz', ianaName)
      } catch { /* Intl not available */ }
    }
  }

  /** Tell the device a person is at this UI. The device holds several
   *  timeouts that only exist to protect an unattended node — chief among them
   *  the startup quiet period before it may transmit — and drops them when
   *  `sys.human_detected` lands. Sent as a command, not a set: it is a signal
   *  about us, not config we mirror.
   *
   *  One message per channel, not per event: the flag is sticky for the
   *  device's boot, so the listeners below only have to catch the first real
   *  interaction. A click before the channel opened still counts — humanSeen
   *  outlives the connection and pushes on the next sync. */
  function pushHuman() {
    if (humanPushed || !dc || dc.readyState !== 'open') return
    humanPushed = true
    sendCommand({ sys: { human_detected: 1 } })
  }

  /* Real interaction only: a pointer going down, a key going down, a scroll, a
   * touch. Capture phase so a handler that stops propagation can't hide the
   * person from us, passive so none of this delays the gesture itself. */
  for (const ev of ['pointerdown', 'keydown', 'wheel', 'touchstart']) {
    window.addEventListener(ev, () => {
      humanSeen = true
      pushHuman()
    }, { capture: true, passive: true })
  }

  function flushPendingSets() {
    if (!dc || dc.readyState !== 'open') return
    const entries = [...pendingSet.entries()]
    for (const [path, val] of entries) {
      try {
        dc.send(JSON.stringify(buildNested(path, val)))
        pendingSet.delete(path)
      } catch {
        /* keep in map */
      }
    }
  }

  /** Declare the link down (idempotent on the false→true edge): show the
   *  overlay, log the notice to log + cli, and force the shared session to
   *  rebuild even if ICE/DTLS still report 'connected' (an app-level ping
   *  gap can outrun the transport noticing). */
  function enterLinkDown() {
    if (linkDown.value) return
    linkDown.value = true
    logSystemNotice('Disconnected, stand by for reconnect.')
    session.refresh()
  }

  /** Clear the down state once we're reconnected AND resynced (a fresh full
   *  dump just completed). Idempotent on the true→false edge. */
  function clearLinkDown() {
    if (!linkDown.value) return
    linkDown.value = false
    logSystemNotice('Reconnected.', 'I')
  }

  /** Channel builder: called by the shared session each time it builds a
   *  fresh PC, BEFORE createOffer. This guarantees `storage:1` is in the
   *  SDP so the offer has an m=application line. */
  function buildChannel(pc: RTCPeerConnection) {
    if (dc) { try { dc.onclose = null; dc.close() } catch { /* */ } }
    try {
      dc = pc.createDataChannel('storage:1', { ordered: true, protocol: '' })
    } catch (e) {
      console.error('[device] createDataChannel failed:', e)
      dc = null
      return
    }

    dc.onopen = () => {
      connected.value = true
      everConnected = true
      lastPongAt = Date.now()      /* fresh baseline so the liveness check doesn't trip */
      clientInfoPushed = false
      /* A fresh channel may be a rebooted device, whose flag went with it. */
      humanPushed = false
      if (humanSeen) pushHuman()
      startHeartbeat()
      flushPendingSets()
    }

    dc.onmessage = (ev) => {
      const text = typeof ev.data === 'string'
        ? ev.data
        : new TextDecoder().decode(ev.data instanceof ArrayBuffer ? ev.data : (ev.data as Uint8Array).buffer)
      session.noteDcActivity()
      try {
        const json = JSON.parse(text)
        if (json.pong) { lastPongAt = Date.now(); return }
        /* The full dump now STREAMS as several chunks bracketed by
           {__dump:'b'}/{__dump:'e'}; each chunk is a plain subtree we merge as
           it lands. On 'e' the dump is complete — re-flush any pending sets so
           local toggles (e.g. record.*) win over the just-merged stale state,
           replacing the old fixed 300ms timeout race. */
        if (json.__dump !== undefined) {
          if (json.__dump === 'b') clientInfoPushed = false
          else if (json.__dump === 'e') { synced.value = true; flushPendingSets(); pushClientInfo(); clearLinkDown() }
          return
        }
        deepMerge(settings, json)
        checkBuildTime()
      } catch { /* ignore non-JSON */ }
    }

    dc.onclose = () => {
      dc = null
      connected.value = false
      stopHeartbeat()
      /* A drop after we'd been connected is a disconnect — raise the overlay
         and kick a reconnect (unless we're intentionally reloading the SPA).
         buildChannel fires again on the next fresh PC; the overlay clears when
         that channel's full dump completes. */
      if (everConnected && !reloading) enterLinkDown()
    }

    dc.onerror = () => { /* onclose fires next */ }
  }

  /* Active-ping cadence and the no-pong window that declares the link down.
     The device can legitimately go heads-down for the better part of a second
     during an inbound-message burst (storage + lxmf + web all share one core),
     so we absorb ~1 pong's worth of silence as deliberate resistance —
     LINK_DOWN_MS = one ping cadence + ~1s grace. Past that a real vanish is
     surfaced promptly (~2s, not the old 4s); a hard transport close still trips
     enterLinkDown instantly via dc.onclose, so genuine disconnects stay
     visible right away. */
  const PING_MS = 1000
  /* Patience before a silent link is declared down. Real closes don't wait
   * for this (dc.onclose fires enterLinkDown directly); this only covers
   * stalls, and the device can legitimately go quiet for a few seconds
   * under load — a flash-commit storm or a scheduling spike. Declaring too
   * eagerly is worse than the stall: the forced reconnect costs a DTLS
   * handshake that freezes the device for seconds more. */
  const LINK_DOWN_MS = 6000
  function startHeartbeat() {
    stopHeartbeat()
    /* Active ping. The device echoes {"pong":1}; lastPongAt tracks the last
       reply. Only meaningful once a real link existed, so a slow first connect
       doesn't flash the overlay. Inbound traffic on ANY DataChannel of the
       session (session.lastDcRxAt) counts the same as a pong: during a bulk
       burst on a sibling channel (e.g. a huge `show` on cli) the pong can
       queue behind the flood, but the arriving flood itself proves the link. */
    heartbeatTimer = setInterval(() => {
      if (dc && dc.readyState === 'open') {
        try { dc.send('{"ping":1}') } catch { /* ignore */ }
      }
      const lastAlive = Math.max(lastPongAt, session.lastDcRxAt)
      if (everConnected && Date.now() - lastAlive > LINK_DOWN_MS) enterLinkDown()
    }, PING_MS)
  }

  function stopHeartbeat() {
    if (heartbeatTimer) { clearInterval(heartbeatTimer); heartbeatTimer = null }
  }

  function connect() {
    if (unregisterBuilder) return  /* already attached */
    unregisterBuilder = session.registerChannel(buildChannel)
    session.connect()
  }

  /* Phone sleep/wake: nudge the session when the tab becomes visible. */
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && !reloading) {
      setTimeout(() => {
        if (!dc || dc.readyState !== 'open') session.connect()
      }, 2000)
    }
  })

  /** Set a config value by dot-notation path. Sends nested JSON to device. */
  function set(path: string, val: string | number) {
    /* Update local nested object */
    const parts = path.split('.')
    let obj: any = settings
    for (let i = 0; i < parts.length - 1; i++) {
      if (!obj[parts[i]] || typeof obj[parts[i]] !== 'object') obj[parts[i]] = {}
      obj = obj[parts[i]]
    }
    obj[parts[parts.length - 1]] = val

    pendingSet.set(path, val)
    if (dc && dc.readyState === 'open') {
      try {
        dc.send(JSON.stringify(buildNested(path, val)))
        pendingSet.delete(path)
      } catch {
        /* leave in pendingSet */
      }
    }
  }

  /** Send a pre-built nested JSON object to the device and merge locally.
   *  Use for operations that can't be expressed as a single dot-path set
   *  (e.g., replacing an entire array). */
  function sendJson(obj: Record<string, any>) {
    deepMerge(settings, obj)
    if (dc && dc.readyState === 'open') {
      try { dc.send(JSON.stringify(obj)) } catch { /* */ }
    }
  }

  /** Send a raw command object to the device WITHOUT merging it into the local
   *  mirror (for control messages like {"fetch":...} that aren't config state). */
  function sendCommand(obj: Record<string, any>) {
    if (dc && dc.readyState === 'open') {
      try { dc.send(JSON.stringify(obj)) } catch { /* */ }
    }
  }

  /** Force immediate settings write on device. */
  function save() {
    if (dc && dc.readyState === 'open') dc.send('{"save":1}')
  }

  /* Flush pending settings + clean close on page unload */
  window.addEventListener('beforeunload', () => {
    reloading = true  /* suppress reconnect */
    if (dc && dc.readyState === 'open') {
      try { dc.send('{"save":1}') } catch { /* */ }
      try { dc.close() } catch { /* */ }
    }
    dc = null
  })

  return { settings, connected, synced, linkDown, get, set, sendJson, sendCommand, save, connect }
})
