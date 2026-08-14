# spangap-web

The web-stack half of the [spangap](../spangap) platform: the device's HTTPS
server and file host, cookie-session auth enforcement, the WebRTC plumbing that
carries every device↔browser data path, and the shared browser UI shell. It is
also the **browser-side UI activator** — when this straddle is in the build, the
build picks up every other straddle's `browser/` subdir and folds it into one SPA.

This is a multi-function straddle. Each function has its own operator guide and
maintainer reference under [`docs/`](docs/):

| Function | Operator guide | Maintainer reference |
|---|---|---|
| **web** — HTTP/HTTPS server, file serving, WebDAV, the URL-forwarding model, auth enforcement, the loopback exemption | [docs/web.md](docs/web.md) | [docs/web-internals.md](docs/web-internals.md) |
| **webrtc** — DTLS/ICE-lite/SCTP plumbing and the content-free DataChannel↔ITS router | [docs/webrtc.md](docs/webrtc.md) | [docs/webrtc-internals.md](docs/webrtc-internals.md) |
| **browser-shell** — the SPA shell: the Dock app launcher, `registerApp`, the menu store, `GeneratedPanel`, the WebRTC session, config sync, and auth flow | [docs/browser-shell.md](docs/browser-shell.md) | [docs/browser-shell-internals.md](docs/browser-shell-internals.md) |

The firmware half lives in [`esp-idf/`](esp-idf/) (`web.cpp`, `auth_web.cpp`,
`webrtc_task.cpp`, `webrtc_sctp.cpp`, `safe_mode.cpp` — the recovery boot's page
and backup/restore endpoint, described in
[spangap-core docs/safe-mode.md](../spangap-core/docs/safe-mode.md)). The browser half is the npm package
`spangap-browser` in [`browser/`](browser/), which keeps its own
[README](browser/README.md) and [INTERNALS](browser/INTERNALS.md) as the
package-author's guide.

## How it fits with the other straddles

- **[spangap-net](../spangap-net)** owns WiFi/TCP and the mbedTLS server. It
  hands `web` already-accepted TCP/TLS connections; `web` never touches a raw
  socket. The HTTPS certificate, the listen ports (`s.net.http_port`,
  `s.net.https_port`), and the WebRTC UDP port (`s.net.webrtc_port`) are all
  net-owned — this straddle references them.
- **[spangap-core](../spangap-core)** provides the runtime `web` and `webrtc`
  build on: ITS (inter-task IPC), storage, logging, CLI, the filesystem layer,
  and the credential store. The login/password/cookie primitive itself lives in
  core's [auth](../spangap-core/docs/auth.md); `web` only enforces it on HTTP
  requests and exposes the JSON login face.
- **[upnp](../upnp)** is a hard dependency: `webrtc_task.cpp`
  asks it for the external IP when building an SDP answer.

## Starting

`web` and `webrtc` start automatically when this straddle is in the build — the
generated init dispatcher brings them up in the platform band, after core and
net. A `--no-web` build simply stages neither, and the browser activator emits
nothing. There is no hand-written init call for a consumer to make.

## What the browser tells the device

The device store (`browser/src/stores/device.ts`) pushes a few things the device
cannot know on its own, over the config channel:

- **Clock and timezone**, once per fresh dump, and only where the device lacks
  them: epoch seconds to `sys.time.set` when `sys.time.valid` is `0`, the IANA
  zone name to `s.ntp.tz` when unset.
- **Human presence**: `sys.human_detected = 1` on the first pointer, key, wheel
  or touch event in the tab. Device-side holds that only exist to protect an
  unattended node end when it lands (see core's
  [init](../spangap-core/docs/init.md)). One message per channel, not per event
  — the flag is sticky for the device's boot, and a reconnect re-sends it
  because a fresh channel may be a rebooted device.

## What it does NOT own

- WiFi / TCP / TLS / the HTTPS certificate / the WebRTC UDP port — [spangap-net](../spangap-net).
- The credential / session / realm primitive — [spangap-core auth](../spangap-core/docs/auth.md).
- Network and WiFi-scan settings panels (`NetworkPanel`, `WifiScanDialog`) — those
  live in [spangap-net](../spangap-net)'s `browser/`.
- Camera / video / RTSP UI and app-specific panels — the consuming app's `browser/`.
