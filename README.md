# spangap-web

## What is this?

**spangap-web** is the web-stack half of the [spangap](../spangap)
platform: an HTTPS server, cookie-session auth, the WebRTC plumbing
(DTLS/ICE-lite/SCTP) that carries every device↔browser data path, and
the shared browser UI shell (FloatingWindow, MenuBar, SettingX,
TerminalWindow, LogWindow, WebRTC session manager, auth flow, storage
sync, menu registry, Pinia stores).

It is **the browser-side UI activator**: when this straddle is in the
dep graph (and `--no-web-ui` is not set), the build picks up every
other straddle's `browser/` subdir and includes its `init()` in the
generated browser dispatcher.

## What this straddle owns

```
spangap-web/
├── esp-idf/         firmware half: HTTPS, auth, WebRTC plumbing
│   ├── include/
│   │   ├── web.h
│   │   ├── auth.h
│   │   ├── webrtc_task.h
│   │   └── webrtc_sctp.h
│   └── src/
│       ├── web.cpp           HTTP/HTTPS server, REST, WebDAV (WIP), WS helpers
│       ├── auth.cpp          cookie sessions, realms, rate limiting, force-takeover
│       ├── webrtc_task.cpp   DTLS/ICE-lite/SCTP signaling + DC↔ITS router
│       └── webrtc_sctp.cpp   SCTP association
└── browser/         npm package: spangap-browser — the shared UI shell
    └── src/
        ├── lib/             auth, device-url, reconnect, webrtc-session
        ├── components/      SettingToggle/Slider/Select/Text, SettingsPanel,
        │                    FloatingWindow, LogWindow, MenuBar, PanelHeading,
        │                    TerminalWindow, EditorWindow
        ├── stores/          device, log, menu, index (pinia setup)
        ├── modules/         advanced, editor, network, system (auto-register)
        ├── panels/          AboutPanel, DeveloperPanel, NetworkPanel,
        │                    SystemPanel, WifiScanDialog
        └── pages/           LoginPage, SetupPage
```

## How others use it

### Firmware side

```cpp
webInit();        // HTTPS + REST + WS helpers
authInit();       // cookie sessions, realms
webrtcInit();     // DTLS/ICE/SCTP, generic DC↔ITS router
```

Register an HTTP URL prefix that hands the connection off to your task:

```cpp
web_path_msg_t wreg = { .itsPort = MY_PORT };
safeStrncpy(wreg.path, "mypath", sizeof(wreg.path));
itsSendAux("web", WEB_PATH_REG_PORT, &wreg, sizeof(wreg), pdMS_TO_TICKS(500));
```

Add a browser DataChannel by opening an ITS server port and letting the
WebRTC task auto-route `<task>:<n>` labels to it. See
[spangap/INTERNALS.md](../spangap/INTERNALS.md) "Add a browser
DataChannel" for the full recipe.

### Browser side

See [browser/README.md](browser/README.md) for the npm install + import
patterns. The package is `spangap-browser` (unscoped). Subpath imports
pull only the pieces you need; modules in `modules/*` self-register
with `menuRegistry` on import.

## The WebRTC pattern

The webrtc task is **content-free**. It terminates DTLS/ICE/SCTP and
routes a DataChannel labelled `<task>:<n>` to ITS port `n` on
`<task>`, in both directions. Adding a browser↔device channel is
opening an ITS port on one side and naming a DataChannel on the other —
no signalling code, no payload knowledge in the WebRTC task,
per-channel reliability.

One `RTCPeerConnection` and one DTLS handshake per browser tab carry
every data path at once — config sync, log, CLI, and any app-defined
media or file stream.

## What it does NOT own

- WiFi / TCP / TLS — in [spangap-net](../spangap-net).
- The HTTPS *certificate* — `spangap-net` owns the mbedTLS server; this
  straddle slots into it.
- Camera / video / RTSP UI — those live in the consuming app's browser
  subdir.

## Lockstep with `spangap-core`

The browser package shape mirrors what's exposed over the storage
DataChannel and the various WS / HTTP endpoints from `spangap-core` and
the other firmware straddles. When firmware changes its public protocol
(config keys, storage tree shape, DC port assignments), the browser
package must follow in lockstep. Versions move together.

## Read next

- [INTERNALS.md](INTERNALS.md) — firmware-side implementation notes,
  generated browser dispatcher, activator details.
- [browser/README.md](browser/README.md) — consumer guide for the npm
  package half.
- Platform-wide [spangap/INTERNALS.md](../spangap/INTERNALS.md) for ITS
  patterns and the WebRTC recipe.
