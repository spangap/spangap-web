# spangap-web — internals

## `web.cpp` — HTTP/HTTPS server

ESP-IDF `httpd` server bound to TLS sockets handed up from
`spangap-net`'s `tls`. URL paths are registered with `WEB_PATH_REG_PORT`
and forwarded to the registering task as ITS connections — `web` parses
the headers, decides the route, then **forwards** the live connection
to the owner task with already-consumed bytes re-injected so the new
owner re-parses a fresh-looking request. `web` is out of the data path
after the handoff.

WebDAV is WIP; REST helpers and WebSocket helpers are stable.

## `auth.cpp` — cookie-session auth

Cookie sessions, realms (per-prefix policy), rate limiting on failed
auth, force-takeover. Secrets (passwords, session keys) live under
`secrets.*` and never cross to the browser.

## `webrtc_task.cpp` + `webrtc_sctp.cpp` — generic WebRTC

The webrtc task is content-free: it terminates DTLS/ICE-lite/SCTP and
routes DataChannel traffic.

- Signalling is over the HTTPS server (the offer/answer exchange happens
  in REST endpoints `web` registered for it).
- DC labels of the form `<task>:<n>` are parsed by the task; it then
  calls `itsConnect("<task>", n, …)` and pipes the SCTP stream to the
  resulting ITS handle in both directions.
- DataChannel mode (ordered / reliable) maps to the ITS port's
  byte-stream-vs-packet-mode choice.

**`vTaskDelay(1)` is mandatory in webrtc_task's main loop** — without
it the IDLE0 watchdog fires on core 0.

## The browser activator

When `spangap-web` is in the build graph and `--no-web-ui` is not set,
the build CLI walks every other straddle's `browser/` subdir and
generates a dispatcher that:

1. Imports each `modules/<prefix>.ts` so they self-register with
   `menuRegistry`.
2. Wires their panels into the menu tree.
3. Sets up the Pinia stores those straddles declare.

The consumer app's Quasar/Vite pipeline then compiles the whole tree
(this package's source plus every straddle's `browser/`) into one SPA
served read-only from `/fixed/webroot/` on the device.

## Why this is its own straddle

Phase-2 split from the old monolithic `spangap-core`. Two reasons:

1. **A LoRa-only / headless device doesn't need a web UI.** Excluding
   the activator removes the HTTPS server, the browser dispatcher
   generation, and the entire shipped SPA.
2. **Both halves change together.** Keeping the firmware web stack and
   the browser shell in the same straddle means a protocol change moves
   one version number, not two.

## Conventions

- All URL routes are forwarded to the owning task via ITS; do not put
  app payload-handling logic in `web.cpp` itself.
- DC port numbering is the same numbering as the ITS port — keep them
  consistent across firmware and browser code.
- The browser-side `webrtcSession` is a singleton; call
  `webrtcSession.registerChannel(builder)` from your modules to add
  app DataChannels.
- `peerDependencies` for the npm half: Vue 3, Quasar, Pinia,
  `@xterm/xterm`. The consuming app owns these.
