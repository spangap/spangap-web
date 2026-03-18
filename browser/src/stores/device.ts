import { defineStore } from 'pinia'
import { reactive, ref } from 'vue'

export const useDeviceStore = defineStore('device', () => {
  const settings = reactive<Record<string, string>>({})
  const types = reactive<Record<string, string>>({})  // 'I' or 'S' per key
  const connected = ref(false)

  let ws: WebSocket | null = null
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null
  let reconnectDelay = 1000
  let knownBuildTime = ''
  let lastRx = 0
  let reloading = false

  function wsUrl() {
    const loc = window.location
    const proto = loc.protocol === 'https:' ? 'wss:' : 'ws:'
    const params = new URLSearchParams(loc.search)
    const host = params.get('host') || loc.hostname
    const port = params.get('port') || loc.port || (loc.protocol === 'https:' ? '443' : '80')
    return `${proto}//${host}:${port}`
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
      if (ev.data === 'pong') return
      // Parse lines: "T:key=value" (initial dump) or "T:key=value" (update)
      const lines = ev.data.split('\n')
      for (const line of lines) {
        if (line.length < 4) continue
        const type = line[0]  // 'I' or 'S'
        if (line[1] !== ':') continue
        const eq = line.indexOf('=', 2)
        if (eq < 0) continue
        const key = line.substring(2, eq)
        const val = line.substring(eq + 1)
        settings[key] = val
        types[key] = type

        if (key === 'build_time') {
          if (!knownBuildTime) {
            knownBuildTime = val
            console.log('[device] build:', val)
          } else if (val !== knownBuildTime) {
            console.log('[device] build changed:', knownBuildTime, '→', val, '— reloading')
            knownBuildTime = val
            reloading = true
            if (ws) { ws.close(); ws = null }
            // Probe flushes stale keep-alive connections (gets RST), then navigate fresh
            fetch('/', { cache: 'no-store' }).catch(() => {}).finally(() => {
              setTimeout(() => { window.location.href = window.location.pathname + window.location.search }, 500)
            })
          }
        }
      }
    }

    ws.onclose = () => {
      ws = null
      connected.value = false
      stopHeartbeat()
      scheduleReconnect()
    }

    ws.onerror = () => {
      // onclose will fire after this
    }
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
      if (Date.now() - lastRx > 10000) {
        console.log('[device] heartbeat timeout, reconnecting')
        // Don't wait for ws.close() TCP timeout — detach and reconnect now
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
      ws.send('ping')
    }, 5000)
  }

  function stopHeartbeat() {
    if (heartbeatTimer) { clearInterval(heartbeatTimer); heartbeatTimer = null }
  }

  function set(key: string, val: string | number) {
    const v = String(val)
    settings[key] = v
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(`${key}=${v}`)
    }
  }

  function save(key: string) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(`save ${key}`)
    }
  }

  function saveWith(key: string, val: string | number) {
    const v = String(val)
    settings[key] = v
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(`save ${key}=${v}`)
    }
  }

  // Auto-connect on store creation
  connect()

  return { settings, types, connected, set, save, saveWith, connect }
})
