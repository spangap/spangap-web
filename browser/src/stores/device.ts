import { defineStore } from 'pinia'
import { reactive, ref } from 'vue'
import { deviceWssBase } from '../lib/epl'

export const useDeviceStore = defineStore('device', () => {
  const settings: Record<string, any> = reactive({})
  const connected = ref(false)

  let ws: WebSocket | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let reconnectDelay = 1000
  let knownAssetId: number | null = null
  let lastRx = 0
  let reloading = false
  let clientInfoPushed = false
  /** Keys set while WS was down; flushed on reconnect so record.* toggles reach the device. */
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

  function wsUrl() {
    return deviceWssBase() + '/epl'
  }

  function reloadForNewAssets() {
    reloading = true
    if (ws) {
      ws.close()
      ws = null
    }
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
            if (ws && ws.readyState === WebSocket.OPEN) {
              ws.send(JSON.stringify({ s: { time: { zones: nested } } }))
              console.log(`[device] timezone zones updated (${Object.keys(flat).length} zones)`)
            }
          })
      })
      .catch(() => { /* offline — use existing zones */ })
  }

  function flushPendingSets() {
    if (!ws || ws.readyState !== WebSocket.OPEN) return
    const entries = [...pendingSet.entries()]
    for (const [path, val] of entries) {
      try {
        ws.send(JSON.stringify(buildNested(path, val)))
        pendingSet.delete(path)
      } catch {
        /* keep in map */
      }
    }
  }

  function connect() {
    if (ws) return
    const url = wsUrl()

    try {
      ws = new WebSocket(url)
    } catch {
      scheduleReconnect()
      return
    }

    ws.onopen = () => {
      connected.value = true
      reconnectDelay = 1000
      lastRx = Date.now()
      clientInfoPushed = false
      startHeartbeat()
      flushPendingSets()
      /* Full dump may arrive after open; re-flush so toggles like record.* win over stale merge. */
      setTimeout(() => flushPendingSets(), 300)
    }

    ws.onmessage = (ev) => {
      if (typeof ev.data !== 'string') return
      lastRx = Date.now()
      try {
        const json = JSON.parse(ev.data)
        if (json.pong) return
        deepMerge(settings, json)
        checkBuildTime()
        pushClientInfo()
      } catch {
        /* ignore non-JSON */
      }
    }

    ws.onclose = (ev) => {
      ws = null
      connected.value = false
      stopHeartbeat()
      if (ev.code === 4401) return  /* auth failure — don't reconnect */
      scheduleReconnect()
    }

    ws.onerror = () => {}
  }

  function scheduleReconnect() {
    if (reconnectTimer || reloading) return
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null
      connect()
    }, reconnectDelay)
    reconnectDelay = Math.min(reconnectDelay * 2, 15000)
  }

  function forceReconnect() {
    if (ws) {
      ws.onclose = null
      ws.onerror = null
      try { ws.close() } catch {}
      ws = null
    }
    connected.value = false
    stopHeartbeat()
    reconnectDelay = 1000
    scheduleReconnect()
  }

  function startHeartbeat() {
    stopHeartbeat()
    heartbeatTimer = setInterval(() => {
      if (!ws || ws.readyState !== WebSocket.OPEN) { forceReconnect(); return }
      try { ws.send('{"ping":1}') } catch { forceReconnect(); return }
      if (Date.now() - lastRx > 30000) {
        console.log('[epl] heartbeat timeout, reconnecting')
        forceReconnect()
      }
    }, 10000)
  }

  function stopHeartbeat() {
    if (heartbeatTimer) { clearInterval(heartbeatTimer); heartbeatTimer = null }
  }

  /* Phone sleep/wake: check WS health when page becomes visible.
   * Brief delay lets the browser resume the socket before we judge it dead. */
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && !reloading) {
      setTimeout(() => {
        if (!ws || ws.readyState !== WebSocket.OPEN) forceReconnect()
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
    if (ws && ws.readyState === WebSocket.OPEN) {
      try {
        ws.send(JSON.stringify(buildNested(path, val)))
        pendingSet.delete(path)
      } catch {
        /* leave in pendingSet */
      }
    }
  }

  /** Force immediate settings write on device. */
  function save() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send('{"save":1}')
    }
  }

  /* Do not auto-connect — MainLayout calls connect() after auth check */

  /* Flush pending settings + clean close on page unload */
  window.addEventListener('beforeunload', () => {
    reloading = true  /* suppress reconnect */
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send('{"save":1}')
      ws.close()
    }
    ws = null
  })

  return { settings, connected, get, set, save, connect }
})
