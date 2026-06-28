# webrtc — internals

Maintainer reference for `webrtc_task.cpp` and `webrtc_sctp.cpp`. The
[operator guide](webrtc.md) covers the contract; this file is for changing the
DTLS/ICE/SCTP plumbing.

## 1. What this function provides

A from-scratch, minimal WebRTC data-plane for a single peer:

- **ICE-lite** — one `host` candidate per active interface (STA / SoftAP /
  WireGuard) plus a `srflx` candidate from upnp's external IP when known;
  generated ufrag/pwd, peer credentials ignored. No STUN/TURN, no trickle.
- **DTLS 1.2 server** over a non-blocking UDP socket, with mbedTLS
  `ssl_cookie` HelloVerify DoS protection.
- **Minimal SCTP** (`webrtc_sctp.cpp`) — INIT/INIT-ACK/COOKIE handshake (server
  role), DATA send/receive with fragmentation and reassembly, SACK generate +
  parse, RE-CONFIG (RFC 6525) stream reset in/out, FORWARD-TSN, HEARTBEAT-ACK,
  CRC32C.
- **DCEP** (RFC 8832) — DATA_CHANNEL_OPEN/ACK, channelType→reliability mapping,
  label + protocol-string extraction.
- **The DC↔ITS router** — a `streamId ↔ ITS handle` table (`dcMap`) and nothing
  else; an ITS client (`itsClientInit`) for outbound routing plus an ITS server
  for the signaling WS.

## 2. The task

One FreeRTOS task (`webrtc`). Its main loop: an **unconditional `vTaskDelay(1)`
per iteration** (under sustained load `itsPoll(1)` can return immediately and
starve IDLE0 past the watchdog — keep the yield), then drain the ITS inbox, drain
the UDP socket (`recvfrom` in a loop), run the send scheduler, then service the
signaling WS. When the UDP socket is closed it blocks in `itsPoll(portMAX_DELAY)`.
The signaling WS, the UDP socket, and DTLS are all torn down and rebuilt on
`wifi.{sta,ap}.up` transitions and on `s.net.webrtc_port` change.

Unlike `web` (which never touches a raw socket — `net` accepts every TCP/TLS
connection and forwards it up over ITS), `webrtc` opens and binds its **own**
non-blocking UDP socket (`openUdpSocket`: `socket`/`bind` on `INADDR_ANY:port`,
`port` from `s.net.webrtc_port`, skipped when `port <= 0`). `net` does no UDP
multiplexing, so only the signaling WS arrives over ITS; all media is on this
socket. Traffic counters are still fed back to `net` (`netTrafficIn`/`Out`).

## 3. The DC ↔ ITS router

The whole router is the `dcMap` table. Both directions:

- SCTP DATA on a streamId → `itsSend(handle, payload, len)` on the paired handle.
  One DC message = one ITS packet; `webrtc_sctp.cpp::processDataFragment`
  reassembles fragmented DCEP messages before `sctp.onData`, so a full message
  lands with its boundaries intact.
- A packet from an ITS handle → the scheduler emits an SCTP DATA on the paired
  streamId with the channel's negotiated reliability.
- SCTP RE-CONFIG (stream reset) ↔ `itsDisconnect`.

On `DCEP OPEN`, the label `"<task>:<port>"` is split and `itsConnect(task, port,
protocolBytes)` opens the outbound ITS handle; the DCEP `protocol` field's JSON
is passed as the ITS protocol bytes. `routerRecvBuf` is allocated lazily and
grown to the largest routed port's `fromSize`, so the device never reserves a
worst-case buffer it may not use.

## 4. SCTP details (`webrtc_sctp.cpp`)

- **Reliability per channel.** The DCEP channelType drives whether DATA carries an
  SSN (ordered), enters the rexmit pool (reliable / partial), or is
  fire-and-forget (unreliable + unordered). Types: `DC_RELIABLE`,
  `DC_RELIABLE_UNORDERED`, `DC_UNRELIABLE_REXMIT[_UNO]`, `DC_UNRELIABLE_TIMED[_UNO]`.
- **Announced streams.** INIT-ACK announces `SCTP_ANNOUNCED_STREAMS` (65535):
  stream ids are **not reused** after a RE-CONFIG reset, and every seek/live-play
  toggle consumes a fresh id, so the total over the association's lifetime — not
  the concurrent count — must fit.
- **Retransmit pool.** One slot per in-flight *message* (not fragment),
  `SCTP_REXMIT_SLOTS` = 256, `SCTP_REXMIT_POOL_BYTES` = 1 MB PSRAM, keyed by
  first-TSN. SACK cumulative-ack frees covered entries; **Gap Ack Blocks** also
  free out-of-order-received ranges (critical when cumTsn is stuck behind an
  abandoned TSN so new data still drains). Retransmit resends only the fragments
  the latest gap blocks show missing; an **RTO path** (no SACK for 1500 ms,
  `forceAll=true`) resends everything past cumTsn for silent UDP loss the peer
  never reported. On stream reset the pool's entries for that stream are purged
  and a FORWARD-TSN is sent so the peer's cumTsn advances past the abandoned TSNs.
  The DCEP ACK emitted on a new DC's OPEN is itself inserted into the pool — if
  that single control packet were lost, the new DC would never progress past its
  first TSN.
- **Inbound reorder buffer.** `SCTP_REORDER_SLOTS` = 64 PSRAM slots hold DATA
  chunks received past a gap in the cumulative TSN, so delivery and SACKing stay
  in strict TSN order. When full, a chunk is dropped (and left un-acked) so the
  reliable peer retransmits.

## 5. The send scheduler

`schedulerPass` is strict-priority with round-robin among equal-priority channels
(DCEP `priority` from the browser is honoured), capped at a small number of
messages per pass so the main loop stays responsive and inbound SACKs drain near
Chrome's delayed-ACK cadence. It runs **after** the UDP drain so fresh SACKs have
freed rexmit slots first. On `sendto` returning `EAGAIN`, the BIO retries with
short yields rather than propagating WANT_WRITE on the first hiccup; a partial
SCTP record is never emitted — the whole fragment goes or it retries.

## 6. Signaling and session control

`/webrtc` is forwarded by [web](web.md) as a WS connection. The ITS server port
opens with `maxHandles=2` so a *second* connection can run through `onConnect`
far enough to do the WS upgrade and send a `4409`/`4008` close before being
dropped — at most one of the two slots is the active session. Close codes are
`WS_CLOSE_AUTH` (4401), `WS_CLOSE_BUSY` (4409), `WS_CLOSE_KICK` (4008). The auth
check (`authCheck` on the `session` cookie) is **always first**, so `?force=1`
can never be evaluated without a valid cookie. On a force takeover the current
holder is sent `4008`, its send buffer is drained so the frame actually reaches
the browser (otherwise the browser sees a raw 1006), then it is evicted.

## 7. Dependencies and pitfalls

- **`#include "upnp.h"` makes upnp a hard dep.** `generateSdpAnswer` calls
  `upnpExternalIp()` for the SDP candidate. Gating that call on
  `CONFIG_SPANGAP_UPNP` would let upnp become an `additional_installs` entry
  instead; until then it's a build requirement.
- **Keep the per-loop `vTaskDelay(1)`.** It is the IDLE0-watchdog safety valve,
  not an optimisation.
- **DTLS verify mode is `VERIFY_NONE`.** The browser authenticates the channel by
  the DTLS fingerprint carried in the (already auth-gated) SDP, not by a CA chain.
- **All routed ITS ports are internal and packet-mode** — both endpoints are
  device tasks, so each `itsSend`/`itsRecv` is one discrete packet with a 4-byte
  header, which is how DC message boundaries survive the router even though ITS
  streams are otherwise byte-oriented.
