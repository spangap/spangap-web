# webrtc — DataChannels for everything

Config sync, the log tail, the CLI, and any app-defined data path (live video,
audio, file playback) all travel over WebRTC DataChannels. A single
`RTCPeerConnection` per browser tab carries every path at once; the only
per-session WebSocket that touches the device is `/webrtc`, and only for SDP
signaling.

The `webrtc` task is **content-free**: it terminates DTLS/ICE-lite/SCTP and
routes each DataChannel to a packet-mode ITS connection, then stays out of the
way. It knows nothing about the bytes it carries. The maintainer reference (SCTP,
DCEP, the scheduler, the retransmit pool) is in
[webrtc-internals.md](webrtc-internals.md); the browser-side session singleton is
in [browser-shell.md](browser-shell.md).

## Why one PeerConnection

TLS on the device is single-threaded in the net task, so every WebSocket upgrade
serialises a full handshake. Once DTLS+SCTP is up over UDP, opening and closing
additional DataChannels costs only a few SCTP control messages. That buys:

- **Per-channel reliability** — each DC picks `ordered` / `maxRetransmits` /
  `maxPacketLifeTime`. Video can be unreliable-unordered, audio partial-reliable,
  control fully reliable, all on the same association.
- **Browser-side decoding** — payload content is opaque to the device; the
  browser does all decoding and rendering.
- **One of everything** — one DTLS handshake, one heartbeat story, one reconnect
  story, one auth gate (at the `/webrtc` signaling WS) per tab.
- **Seek = reset** — closing and reopening a DC on the same PC restarts a session
  with new parameters cheaply (no DTLS renegotiation).

The device runs **one** DTLS + one SCTP association at a time. The signaling WS
admits a single active session; a second connection is rejected or takes over
explicitly (below).

## The DC ↔ ITS router

A DataChannel label has the form `"<taskname>:<port>"`. The task splits on `:`,
parses the integer port, and `itsConnect(taskname, port, protocolBytes)`. From
then on it is a pure bridge:

- one SCTP DATA message on a stream → one `itsSend` on the paired ITS handle;
- one packet read from an ITS handle → one SCTP DATA message on the paired stream,
  with the DC's negotiated reliability;
- an SCTP stream reset → `itsDisconnect` on the paired handle, and vice versa.

Content-level metadata (timestamps, filenames, seek ranges) rides the DCEP
`protocol` field as JSON — never the label. The label is routing only; the SCTP
stream id is the channel identity.

## Adding a browser↔device channel

Opening a channel is opening an ITS port on the device and naming a DataChannel
on the browser. There is no signalling code and no payload knowledge in the
`webrtc` task. On the device:

```c
// In your task's init: a packet-mode ITS server port.
itsServerPortOpen(MY_PORT, /*packetBased=*/true, /*maxHandles=*/2,
                  /*toSize=*/2048, /*fromSize=*/256 * 1024);
// onConnect: produce packets and itsSend each one. onDisconnect: stop.
```

`fromSize` is the per-DC send buffer — size it for the largest packet you emit
(one frame, one audio block). On the browser, a DataChannel labelled
`"mystream:1"` auto-connects to `MY_PORT`. Per-packet framing inside the payload
is entirely the app's; `webrtc` never inspects it. Reliability is set by the
browser at `createDataChannel()` time.

## The built-in content channels

Three platform tasks expose packet-mode ITS ports under the same label
convention:

| DC label | Server task | Direction | Payload |
|---|---|---|---|
| `storage:1` | `storage` | bidirectional | JSON merge-patches + ping/save |
| `log:1` | `log` | device → browser | ANSI log lines (tail paste + live) |
| `cli:1` | `cli` | bidirectional | raw terminal bytes |

`log:1` accepts a `{"backlog":N}` DCEP protocol string to override the default
tail-paste size. The `log` and `cli` tasks also keep stream-mode TCP ports for
`nc`-style access, disabled by default. These ports and their tasks are owned by
[spangap-core](../../spangap-core); `webrtc` just routes to them.

## Single session, BUSY, and takeover

The `/webrtc` signaling WS evaluates auth first, then admits one session:

| Situation | WS close code |
|---|---|
| No / invalid session cookie | `4401` |
| Another session active, no `?force=1` | `4409` (BUSY) |
| Another session active, `?force=1` | victim gets `4008` (KICK); new client accepted |

There is no automatic takeover even with a valid cookie — the browser surfaces
`busy`/`kicked` and the user clicks "Take over" (`?force=1`) or "Resume". Normal
network blips use other close codes and auto-reconnect with backoff. If a holder
dies without a clean WS close, the slot is reclaimed within seconds.

## Signaling and ICE

Signaling is the offer/answer exchange over the `/webrtc` WebSocket (forwarded by
[web](web.md) to the `webrtc` task). The device is **ICE-lite**: the browser
offers, the device answers and ignores the peer's ICE credentials. The answer
carries one `host` candidate per active interface IP — the WiFi STA address, the
SoftAP address, and the WireGuard address (`s.wg.address`, only while `wg.up`) —
plus, when [upnp](../../upnp) has discovered an external IP, one `srflx`
(server-reflexive) candidate for it. The UDP port is `s.net.webrtc_port`
(net-owned, default 4433 — see [spangap-net](../../spangap-net)). DTLS is 1.2,
server role, with mbedTLS cookie-based DoS protection on the handshake.

## Storage surface

`webrtc` defines no settings of its own. It reads net-owned `s.net.webrtc_port`
(the UDP port — when it is `0` the UDP socket isn't opened and WebRTC is off),
reads `s.wg.address` / `wg.up` to add the WireGuard host candidate to the SDP
answer, and reacts to `wifi.{sta,ap}.up` to bring the socket and DTLS up/down. It
registers the `/webrtc` WS path with [web](web.md) for signaling, but opens its
own UDP socket directly (binding `s.net.webrtc_port` on `INADDR_ANY`) — `net` does
no UDP forwarding, so the media socket is `webrtc`'s alone.

## Failure mode

DTLS dies → SCTP dies → every DataChannel dies → the browser reconnects the WS
and reopens the channels it needs. The blast radius is the whole session rather
than one endpoint, but the recovery is a single, uniform path instead of
per-endpoint reconnect logic. The TCP `log` and `cli` ports remain as a backstop
when enabled.
