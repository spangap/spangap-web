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

export const useDeviceStore = defineStore('device', () => {
  const settings: Record<string, any> = reactive({})
  const connected = ref(false)

  const session = getSession()
  let dc: RTCDataChannel | null = null
  let unregisterBuilder: (() => void) | null = null
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let knownAssetId: number | null = null
  let lastRx = 0
  let reloading = false
  let clientInfoPushed = false
  /** Keys set while DC was down; flushed on reconnect so record.* toggles reach the device. */
  const pendingSet = new Map<string, string | number>()

  /** Deep-merge src into dst. `null` means delete (key/subtree). Arrays replace. */
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

    /* Push browser timezone if not yet configured (IANA name + POSIX lookup) */
    const tz = settings.s?.ntp?.tz
    if (tz === undefined || tz === '') {
      try {
        const ianaName = Intl.DateTimeFormat().resolvedOptions().timeZone
        if (ianaName) {
          set('s.ntp.tz', ianaName)
          /* Look up POSIX string from zones tree */
          const parts = ianaName.split('/')
          let node: any = settings.s?.time?.zones
          for (const p of parts) {
            if (!node || typeof node !== 'object') break
            node = node[p]
          }
          if (typeof node === 'string') set('s.ntp.posix', node)
        }
      } catch { /* Intl not available */ }
    }

    /* Update timezone zones from GitHub if stale (>3 months) */
    updateZonesIfStale()
  }

  function updateZonesIfStale() {
    const deviceEtag = String(settings.s?.time?.zones?.updated ?? '')

    /* HEAD request to check ETag without downloading full file */
    fetch(ZONES_URL, { method: 'HEAD', cache: 'no-cache' })
      .then(r => {
        if (!r.ok) return
        const ghEtag = r.headers.get('ETag') ?? ''
        if (!ghEtag) return
        if (deviceEtag === ghEtag) return  /* already current */
        /* ETag changed — download and push */
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
            sendJson({ s: { time: { zones: nested } } })
            console.log(`[device] timezone zones updated (${Object.keys(flat).length} zones)`)
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
      lastRx = Date.now()
      clientInfoPushed = false
      startHeartbeat()
      flushPendingSets()
      /* Full dump may arrive after open; re-flush so toggles like record.* win over stale merge. */
      setTimeout(() => flushPendingSets(), 300)
    }

    dc.onmessage = (ev) => {
      const text = typeof ev.data === 'string'
        ? ev.data
        : new TextDecoder().decode(ev.data instanceof ArrayBuffer ? ev.data : (ev.data as Uint8Array).buffer)
      lastRx = Date.now()
      try {
        const json = JSON.parse(text)
        if (json.pong) return
        deepMerge(settings, json)
        checkBuildTime()
        pushClientInfo()
      } catch { /* ignore non-JSON */ }
    }

    dc.onclose = () => {
      dc = null
      connected.value = false
      stopHeartbeat()
      /* Session handles reconnect + BUSY/KICK state. buildChannel will
         fire again on the next fresh PC. */
    }

    dc.onerror = () => { /* onclose fires next */ }
  }

  function startHeartbeat() {
    stopHeartbeat()
    heartbeatTimer = setInterval(() => {
      if (!dc || dc.readyState !== 'open') return
      try { dc.send('{"ping":1}') } catch { /* ignore */ }
      if (Date.now() - lastRx > 30000) {
        console.log('[epl] heartbeat timeout, nudging session reconnect')
        session.connect()
      }
    }, 10000)
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

  return { settings, connected, get, set, sendJson, save, connect }
})
