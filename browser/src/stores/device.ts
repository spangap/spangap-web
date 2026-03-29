import { defineStore } from 'pinia'
import { reactive, ref } from 'vue'

export const useDeviceStore = defineStore('device', () => {
  const settings: Record<string, any> = reactive({})
  const connected = ref(false)

  let ws: WebSocket | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let reconnectDelay = 1000
  let knownFixedMtime: number | null = null
  let lastRx = 0
  let reloading = false

  /** Deep-merge src into dst (Vue 3 Proxy handles new property reactivity). */
  function deepMerge(dst: any, src: any) {
    for (const key of Object.keys(src)) {
      const val = src[key]
      if (val && typeof val === 'object' && !Array.isArray(val)) {
        if (!dst[key] || typeof dst[key] !== 'object') dst[key] = {}
        deepMerge(dst[key], val)
      } else {
        dst[key] = val
      }
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
    const loc = window.location
    const proto = loc.protocol === 'https:' ? 'wss:' : 'ws:'
    const params = new URLSearchParams(loc.search)
    const host = params.get('host') || loc.hostname
    const port = params.get('port') || loc.port || (loc.protocol === 'https:' ? '443' : '80')
    return `${proto}//${host}:${port}`
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

  /** Reload SPA only when LittleFS /fixed web payload changed (not on app-only OTA). */
  function checkBuildTime() {
    const fx = settings.sys?.buildtime?.fixed
    if (typeof fx !== 'number') return
    if (knownFixedMtime === null) {
      knownFixedMtime = fx
      console.log('[device] sys.buildtime.fixed', fx)
      return
    }
    if (fx !== knownFixedMtime) {
      console.log('[device] fixed image changed:', knownFixedMtime, '→', fx, '— reloading')
      knownFixedMtime = fx
      reloadForNewAssets()
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
      startHeartbeat()
    }

    ws.onmessage = (ev) => {
      if (typeof ev.data !== 'string') return
      lastRx = Date.now()
      try {
        const json = JSON.parse(ev.data)
        if (json.pong) return
        deepMerge(settings, json)
        checkBuildTime()
      } catch {
        /* ignore non-JSON */
      }
    }

    ws.onclose = () => {
      ws = null
      connected.value = false
      stopHeartbeat()
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

  function startHeartbeat() {
    stopHeartbeat()
    heartbeatTimer = setInterval(() => {
      if (!ws || ws.readyState !== WebSocket.OPEN) return
      if (Date.now() - lastRx > 5000) {
        console.log('[device] heartbeat timeout, reconnecting')
        ws.onclose = null
        ws.onerror = null
        try { ws.close() } catch {}
        ws = null
        connected.value = false
        stopHeartbeat()
        reconnectDelay = 1000
        scheduleReconnect()
        return
      }
      ws.send('{"ping":1}')
    }, 5000)
  }

  function stopHeartbeat() {
    if (heartbeatTimer) { clearInterval(heartbeatTimer); heartbeatTimer = null }
  }

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

    /* Send nested JSON to device */
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(buildNested(path, val)))
    }
  }

  /** Force immediate settings write on device. */
  function save() {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send('{"save":1}')
    }
  }

  connect()

  /* Clean close on page unload — lets server free ITS handle immediately */
  window.addEventListener('beforeunload', () => {
    reloading = true  /* suppress reconnect */
    if (ws) { ws.close(); ws = null }
  })

  return { settings, connected, get, set, save, connect }
})
