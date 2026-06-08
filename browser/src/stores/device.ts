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
  /** True once the link is considered down (no pong for >4s, or the channel
   *  dropped after we'd been connected). Stays true through the reconnect until
   *  a fresh full storage dump lands — i.e. "reconnected AND resynced". Drives
   *  the full-screen ConnectionOverlay. */
  const linkDown = ref(false)

  const session = getSession()
  let dc: RTCDataChannel | null = null
  let unregisterBuilder: (() => void) | null = null
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let knownAssetId: number | null = null
  let lastRx = 0
  /** Wall-clock of the last pong (or channel open). The 1s ping loop declares
   *  the link down when this is >4s stale. */
  let lastPongAt = 0
  /** Set once the storage channel has opened at least once, so the liveness
   *  check and drop-detection only fire after a real connection existed. */
  let everConnected = false
  let reloading = false
  let clientInfoPushed = false
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

  const ZONES_URL = 'https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.json'

  /** After first full dump, push client time + timezone + zones update if needed. */
  function pushClientInfo() {
    if (clientInfoPushed) return
    /* Wait until we have sys.time in the dump (indicates full dump received) */
    if (!settings.sys?.time) return
    clientInfoPushed = true

    /* Push epoch time if device doesn't have valid time */
    const valid = settings.sys?.time?.valid
    if (valid !== undefined && Number(valid) === 0) {
      const epoch = Math.floor(Date.now() / 1000)
      set('sys.time.set', epoch)
    }

    /* Push browser timezone if not yet configured. We send only the IANA
     * name — the device resolves the POSIX string itself from its on-disk
     * timezones.json (the zones map no longer lives in config). */
    const tz = settings.s?.ntp?.tz
    if (tz === undefined || tz === '') {
      try {
        const ianaName = Intl.DateTimeFormat().resolvedOptions().timeZone
        if (ianaName) set('s.ntp.tz', ianaName)
      } catch { /* Intl not available */ }
    }

    /* Update timezone zones from GitHub if stale (>3 months) */
    updateZonesIfStale()
  }

  function updateZonesIfStale() {
    /* The device's IANA→POSIX map is a plain file at /state/timezones.json,
     * no longer a config subtree. We track the version we last uploaded in
     * the tiny s.ntp.zones_etag config key (so the big map stays out of the
     * config dump) and, when GitHub's copy is newer, PUT a fresh file. */
    const deviceEtag = String(settings.s?.ntp?.zones_etag ?? '')

    /* HEAD request to check ETag without downloading full file */
    fetch(ZONES_URL, { method: 'HEAD', cache: 'no-cache' })
      .then(r => {
        if (!r.ok) return
        const ghEtag = r.headers.get('ETag') ?? ''
        if (!ghEtag) return
        if (deviceEtag === ghEtag) return  /* already current */
        /* ETag changed — download, reshape to the nested tree, PUT to device */
        return fetch(ZONES_URL, { cache: 'no-cache' })
          .then(r2 => r2.ok ? r2.json() : null)
          .then(flat => {
            if (!flat || typeof flat !== 'object') return
            const nested: Record<string, any> = {}
            for (const [iana, posix] of Object.entries(flat)) {
              const parts = iana.split('/')
              let node = nested
              for (let i = 0; i < parts.length - 1; i++)
                node = node[parts[i]] ??= {}
              node[parts[parts.length - 1]] = posix
            }
            nested.updated = ghEtag
            return fetch('/state/timezones.json', {
              method: 'PUT',
              credentials: 'same-origin',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify(nested),
            }).then(put => {
              if (!put.ok) {
                console.warn(`[device] timezone PUT failed (${put.status})`)
                return
              }
              set('s.ntp.zones_etag', ghEtag)
              console.log(`[device] timezone zones updated (${Object.keys(flat).length} zones)`)
            })
          })
      })
      .catch(() => { /* offline — use existing zones */ })
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
      lastRx = Date.now()
      lastPongAt = Date.now()      /* fresh baseline so the 4s check doesn't trip */
      clientInfoPushed = false
      startHeartbeat()
      flushPendingSets()
    }

    dc.onmessage = (ev) => {
      const text = typeof ev.data === 'string'
        ? ev.data
        : new TextDecoder().decode(ev.data instanceof ArrayBuffer ? ev.data : (ev.data as Uint8Array).buffer)
      lastRx = Date.now()
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
          else if (json.__dump === 'e') { flushPendingSets(); pushClientInfo(); clearLinkDown() }
          return
        }
        deepMerge(settings, json)
        checkBuildTime()
        pushClientInfo()
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

  function startHeartbeat() {
    stopHeartbeat()
    /* Active 1s ping. The device echoes {"pong":1}; lastPongAt tracks the last
       reply. 4s without a pong → the link is down: overlay + log/cli notice +
       forced reconnect (see enterLinkDown). Only meaningful once a real link
       existed, so a slow first connect doesn't flash the overlay. */
    heartbeatTimer = setInterval(() => {
      if (dc && dc.readyState === 'open') {
        try { dc.send('{"ping":1}') } catch { /* ignore */ }
      }
      if (everConnected && Date.now() - lastPongAt > 4000) enterLinkDown()
    }, 1000)
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

  return { settings, connected, linkDown, get, set, sendJson, save, connect }
})
