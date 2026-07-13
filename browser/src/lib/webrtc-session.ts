/**
 * webrtc-session — shared WebRTC peer connection to the device.
 *
 * Owns the signaling WebSocket (`/webrtc`) and a single RTCPeerConnection
 * that every data channel consumer (video, storage, log, cli) opens
 * channels on. One DTLS + one SCTP association for the whole browser tab.
 *
 * Single-session model: the device's /webrtc endpoint rejects a second
 * connect with close code 4409 (BUSY) unless the client opts in with
 * `?force=1`. An evicted client sees 4008. Both cases freeze the tab
 * until the user clicks a button — no auto-reconnect for either.
 */
import { deviceWssBase } from './device-url'
import { ReconnectTimer } from './reconnect'

export type SessionState =
  | 'idle'          // never connected, or explicitly closed
  | 'connecting'    // WS open, SDP exchange in progress
  | 'connected'     // DTLS + SCTP up, consumers can open channels
  | 'busy'          // rejected with 4409 — another session is active
  | 'kicked'        // evicted with 4008 — user took over from elsewhere
  | 'error'         // unexpected close; will auto-reconnect
  | 'auth'          // rejected with 4401 — needs login

/** WS close codes (mirror main/webrtc_task.cpp). */
const WS_CLOSE_AUTH = 4401
const WS_CLOSE_BUSY = 4409
const WS_CLOSE_KICK = 4008

/** Hook called when a new PC comes up — consumers open their DC(s) here.
 *  Invoked each reconnect with a fresh PC. Implementations should be
 *  idempotent (no shared state across calls) and can close the DC in
 *  their own teardown path; the session tears down the PC itself. */
export type ChannelBuilder = (pc: RTCPeerConnection) => void

class WebrtcSession {
  private _state: SessionState = 'idle'
  private _pc: RTCPeerConnection | null = null
  private ws: WebSocket | null = null
  private reconnect = new ReconnectTimer()
  private wantConnected = false
  private pendingForce = false
  private generation = 0
  private builders: Set<ChannelBuilder> = new Set()
  private listeners: Set<(s: SessionState) => void> = new Set()

  get state(): SessionState { return this._state }
  get pc(): RTCPeerConnection | null { return this._pc }

  /** Wall-clock ms of the last inbound message on ANY DataChannel of this
   *  session — consumers call noteDcActivity() from their onmessage. The
   *  app-level liveness check (device store) treats this as proof-of-life
   *  equivalent to a pong: a bulk burst on one channel (big CLI dump, log
   *  backlog, mirror frames) can delay the storage pong behind it, but any
   *  received byte proves the transport is alive. */
  lastDcRxAt = 0

  /** Record inbound DataChannel traffic for the app-level liveness check. */
  noteDcActivity() { this.lastDcRxAt = Date.now() }

  /** Subscribe to state changes. Returns an unsubscribe fn. */
  onStateChange(cb: (s: SessionState) => void): () => void {
    this.listeners.add(cb)
    return () => { this.listeners.delete(cb) }
  }

  /** Register a channel builder. Called with the current PC if one exists,
   *  and again on every reconnect. Returns an unregister function. */
  registerChannel(builder: ChannelBuilder): () => void {
    this.builders.add(builder)
    if (this._pc && this._state === 'connected') {
      try { builder(this._pc) } catch (e) { console.error('[session] builder threw:', e) }
    }
    return () => { this.builders.delete(builder) }
  }

  /** Start or resume the session. `force=true` adds `?force=1` so the
   *  device evicts whoever holds the single session slot.
   *
   *  Idempotent: if already connecting or connected (and not force), this
   *  is a no-op. Calling while in 'busy'/'kicked'/'auth' restarts. Force
   *  always restarts. */
  connect(opts?: { force?: boolean }) {
    const force = opts?.force ?? false
    this.wantConnected = true
    if (!force && (this._state === 'connecting' || this._state === 'connected')) return
    this.pendingForce = force
    this.reconnect.reset()
    this.openSignaling()
  }

  /** Force a full reconnect even when the PC still reports 'connected'. Used
   *  by an app-level liveness check (the storage-channel ping) that has decided
   *  the link is dead before ICE/DTLS/SCTP notices — tears the session down and
   *  reconnects with fast backoff. No-op if we don't want to be connected, or
   *  if we're parked in a terminal state (busy/kicked/auth) the user must clear.
   */
  refresh() {
    if (!this.wantConnected) return
    if (this._state === 'busy' || this._state === 'kicked' || this._state === 'auth') return
    this.teardown()
    this.reconnect.reset()        /* fast first retry */
    this.setState('connecting')
    this.scheduleReconnect()
  }

  /** Close everything, go to 'idle'. No auto-reconnect until connect()
   *  is called again. */
  close() {
    this.wantConnected = false
    this.reconnect.clear()
    this.teardown()
    this.setState('idle')
  }

  /** Send JSON over the signaling WS. Returns true if the socket was open. */
  sendSignaling(obj: unknown): boolean {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false
    try { this.ws.send(JSON.stringify(obj)); return true }
    catch { return false }
  }

  private setState(s: SessionState) {
    if (s === this._state) return
    this._state = s
    for (const cb of this.listeners) {
      try { cb(s) } catch (e) { console.error('[session] state listener threw:', e) }
    }
  }

  private teardown() {
    this.generation++
    if (this._pc) {
      try { this._pc.close() } catch { /* ignore */ }
      this._pc = null
    }
    if (this.ws) {
      const w = this.ws
      this.ws = null
      w.onclose = null
      w.onerror = null
      w.onmessage = null
      try { w.close() } catch { /* ignore */ }
    }
  }

  private scheduleReconnect() {
    if (!this.wantConnected) return
    if (this._state === 'busy' || this._state === 'kicked' || this._state === 'auth') return
    this.reconnect.schedule(() => { if (this.wantConnected) this.openSignaling() })
  }

  private openSignaling() {
    this.teardown()
    this.setState('connecting')
    const gen = ++this.generation

    let url = `${deviceWssBase()}/webrtc`
    if (this.pendingForce) url += '?force=1'
    this.pendingForce = false

    try {
      this.ws = new WebSocket(url)
    } catch {
      this.setState('error')
      this.scheduleReconnect()
      return
    }

    this.ws.onopen = () => {
      if (gen !== this.generation) return
      this.reconnect.clear()
      void this.startWebRTC(gen)
    }

    this.ws.onmessage = (ev) => {
      try {
        const msg = JSON.parse(ev.data)
        if (msg.type === 'answer' && this._pc) {
          this._pc.setRemoteDescription({ type: 'answer', sdp: msg.sdp })
            .catch(e => console.error('[session] setRemoteDescription failed:', e))
        }
      } catch { /* ignore non-JSON */ }
    }

    this.ws.onclose = (ev) => {
      if (gen !== this.generation) return
      console.log('[session] signaling WS closed', ev.code)
      if (ev.code === WS_CLOSE_AUTH) {
        this.wantConnected = false
        this.setState('auth')
        return
      }
      if (ev.code === WS_CLOSE_BUSY) {
        this.wantConnected = false
        this.setState('busy')
        return
      }
      if (ev.code === WS_CLOSE_KICK) {
        this.wantConnected = false
        this.setState('kicked')
        return
      }
      this.setState('error')
      this.scheduleReconnect()
    }

    this.ws.onerror = () => { /* onclose follows */ }
  }

  private async startWebRTC(gen: number) {
    if (gen !== this.generation) return
    const pc = new RTCPeerConnection({
      iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
    })
    this._pc = pc

    pc.onconnectionstatechange = () => {
      if (gen !== this.generation) return
      const st = pc.connectionState
      console.log('[session] pc state:', st)
      if (st === 'connected') this.setState('connected')
      else if (st === 'failed') {
        this.teardown()
        this.setState('error')
        this.scheduleReconnect()
      }
    }

    /* Run registered builders before building the offer so their DCs
       show up in the initial SDP. The offer must contain at least one
       m=application line — if all builders are empty the createOffer
       below produces no m-lines and the device's answer (which always
       has m=application) fails Chrome's m-line order check. */
    for (const b of this.builders) {
      try { b(pc) } catch (e) { console.error('[session] builder threw:', e) }
    }

    const offer = await pc.createOffer()
    if (gen !== this.generation) return
    await pc.setLocalDescription(offer)
    if (gen !== this.generation) return

    /* Wait briefly for ICE gathering (local candidates on LAN). */
    await new Promise<void>((resolve) => {
      if (pc.iceGatheringState === 'complete') { resolve(); return }
      const check = () => {
        if (pc.iceGatheringState === 'complete') {
          pc.removeEventListener('icegatheringstatechange', check)
          resolve()
        }
      }
      pc.addEventListener('icegatheringstatechange', check)
      setTimeout(resolve, 2000)
    })

    if (gen !== this.generation) return
    const sdp = pc.localDescription?.sdp
    if (!sdp) return
    this.sendSignaling({ type: 'offer', sdp })
  }
}

/* Singleton — every consumer imports the same session. Stored on globalThis
 * via a registered Symbol so two bundle-side module instances (e.g. when
 * spangap-browser imports relative and the consumer imports via package
 * subpath under preserveSymlinks) still share one WebrtcSession. */
const SESSION_KEY = Symbol.for('spangap.webrtcSession')
type SessionHolder = { [SESSION_KEY]?: WebrtcSession }
const sessionHolder = globalThis as unknown as SessionHolder

export function getSession(): WebrtcSession {
  if (!sessionHolder[SESSION_KEY]) sessionHolder[SESSION_KEY] = new WebrtcSession()
  return sessionHolder[SESSION_KEY]!
}

export { WebrtcSession }
