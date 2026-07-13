# browser-shell — the SPA shell

The browser half of this straddle is the npm package **`spangap-browser`**: the
shared UI shell that consuming Quasar / Vue 3 apps assemble into their device SPA.
It provides the app launcher (the Dock), the path-based menu and settings system,
the declarative `GeneratedPanel` renderer, the config-bound `Setting*` controls,
the floating windows (log, terminal, editor), the WebRTC session singleton, and
the auth / login flow.

This page is the model-level guide. For npm install and subpath-import mechanics
see [../browser/README.md](../browser/README.md); for package-author notes see
[../browser/INTERNALS.md](../browser/INTERNALS.md) and
[browser-shell-internals.md](browser-shell-internals.md).

## The activator

When `spangap-web` is in the firmware build graph (and `--no-web-ui` is not set),
the build walks every consumed straddle's `browser/` subdir and generates a
dispatcher (`straddles.gen.ts`) that imports each straddle's `browser/src/register*`
module, registers its declarative settings panels, and bundles its app icons. The
consumer's Quasar/Vite pipeline compiles the whole tree — this package plus every
straddle's `browser/` — into one SPA, gzips it, and the device serves it read-only
from `/fixed/webroot/`. Staging a straddle is all it takes to surface its UI;
there is no hand-maintained list.

## The Dock and the app model

The bottom **Dock** (`components/Dock.vue`) is the app launcher — it replaces the
former menu bar. Every launchable thing (Settings, CLI, System Log, and each
app-bearing straddle's window) registers one `AppEntry` via `registerApp` from
`lib/apps`:

```ts
import { registerApp } from 'spangap-browser/lib/apps'

registerApp({
  id: 'messages',
  label: 'Messages',
  icon: 'messages',          // → bundled src/app-icons/messages.svg
  open: () => { messagesVisible.value = true; messagesFocus.value++ },
  isOpen: () => messagesVisible.value,   // optional — drives a running-app dot
  placement: 0,              // sibling order: >0 left, 0 middle (alphabetic), <0 right
})
```

The Dock renders one icon per app, sorted by `placement`. Clicking calls the
app's `open()` (which raises/shows its `FloatingWindow` via the straddle's own
visibility/focus refs). Icons are inline SVGs bundled into `app.js` by the
generator (`registerAppIcons` over each straddle's `src/app-icons/*.svg`), so no
separate icon asset ships to the device. On desktop the Dock is a centered
floating bar; on a phone it is a fixed bottom nav bar that shows the first four
apps plus a "More" sheet when there are more than five.

## The menu and settings system

Settings live in a path-based menu store, `useMenuStore` (`stores/menu`), mirroring
the LCD's `lcdRegisterSettings`. A caller registers one leaf at a slash-path and
the store auto-creates the intermediate containers:

```ts
import { useMenuStore } from 'spangap-browser'
const menu = useMenuStore()
menu.register('settings/system/general', 'General', { type: 'panel', component })
```

The first segment is the top-level group, the last is the leaf, the middle are
submenus merged by id (minimum path `group/leaf`). A leaf is a `panel` (a settings
pane), a `toggle` (a device-store dotpath rendered as a switch), or an `action`.
`placement` orders siblings the same way the Dock does. `SettingsWindow.vue`
renders the tree as a first-class app window (the gear Dock icon): a nav rail plus
the selected pane on desktop, a drill-down on phones.

### Declarative panels (`GeneratedPanel`)

Most settings panes are not hand-written. The build lowers each straddle.yaml
`settings:` block into a JSON descriptor and calls `registerGeneratedPanels`
(`lib/generatedPanels`), which registers each panel's menu leaf pointing at one
shared `GeneratedPanel.vue`. At render time the panel finds its descriptor by the
active-panel id and maps each row to the matching `Setting*` component, so a
generated pane is visually identical to a hand-written one. Row kinds: `section`,
`caption`, `switch`, `slider`, `text` (with a write-only `secret` variant),
`dropdown`, `value` (read-only live value), `button`, and `list` (an
add/remove editor over an array, e.g. host:port entries). This is the web parallel
to the firmware's generated settings panes — one generic renderer, no SFC per pane.

### Hand-written panels and the `Setting*` controls

A straddle that needs a bespoke pane places a `*Panel.vue` under its
`browser/src/panels/` and registers it from its `register*` module. The shipped
config-bound controls — `SettingToggle`, `SettingSlider`, `SettingSelect`,
`SettingText`, plus `PanelHeading` — take a config-key path and handle the storage
subscription, optimistic update, and rollback. The shell's own default panels are
About, System, and Developer.

> Network and WiFi-scan panels are **not** here — they live in
> [spangap-net](../../spangap-net)'s `browser/`.

## The WebRTC session

`webrtcSession` (`lib/webrtc-session`) is the singleton every consumer shares —
one `RTCPeerConnection` per tab, owning the `/webrtc` signaling WebSocket. A
consumer adds a channel by registering a builder:

```ts
import { webrtcSession } from 'spangap-browser/lib/webrtc-session'
webrtcSession.registerChannel((pc) => pc.createDataChannel('mystream:1', { ordered: false }))
```

Builders fire **before** `createOffer()` on every fresh PC, so the initial SDP
always carries an `m=application` line (without this Chrome rejects the answer with
"order of m-lines doesn't match"). `connect()` is idempotent, so the device store,
terminal, and an app's player can't race to tear down the same PC. Closing and
reopening a DC on the same PC is cheap (DCEP reset + open, no DTLS handshake) — the
natural "seek" / "switch source" mechanism. Session states: `idle`, `connecting`,
`connected`, `busy` (4409), `kicked` (4008), `auth` (4401), `error`. `busy` and
`kicked` disable auto-reconnect and surface a "Take over" / "Resume" overlay
(`ConnectionOverlay.vue`); other close codes auto-reconnect with backoff.

### Config sync

The `device` store (`stores/device`) binds bidirectionally to the device storage
tree over the `storage:1` DataChannel. On open the device sends a full dump, then
coalesced nested-JSON merge-patches; the browser sends patches back. `{"save":1}`
forces a flush, `{"ping":1}`/`{"pong":1}` is the heartbeat, and a `beforeunload`
handler saves and closes on page unload. The link-down check accepts inbound
traffic on *any* DataChannel of the session as proof-of-life, not just the pong:
channel `onmessage` handlers call `session.noteDcActivity()`, so a bulk burst on
a sibling channel (a huge CLI dump, a log backlog, mirror frames) that queues
the pong behind it can't flag a live link as down. Components read with `device.get("s.…")`
and write with `device.set("s.…", value)`. The store also auto-pushes the IANA
timezone on first connect (if unset) and the client epoch time when the device
clock is invalid, so the device gets time even without NTP.

### Log stream

The `log` store (`stores/log`) pre-connects the `log:1` DataChannel at startup so
the device streams immediately. It keeps a 256 KB line-aligned ring, queues lines
emitted before the DC opens, and hooks `console.*` + `window.error`/
`unhandledrejection` so browser console output is mirrored to the device log
(pre-coloured with ANSI so xterm and other consumers render it identically).
`LogWindow.vue` is a pure xterm display over this buffer; `TerminalWindow.vue`
owns its own `cli:1` DC.

## Auth and the login flow

`lib/auth` wraps the device's JSON auth API (served by [web](web.md)):
`authLogin(password, realm?)`, `authPasswd(realm, old, new)`, `authLogout()`, and
`checkAuth()` (a `HEAD /` probe reading the `X-Authenticated` header to learn
whether auth is enabled and which realm the cookie holds). `isAdminUnset()` probes
`authPasswd('admin','','')` to drive onboarding. The session cookie is set with a
60-day `max-age`, `SameSite=Strict`. `LoginPage.vue` and `SetupPage.vue` are the
shipped login and first-run onboarding pages; wire them into the app router.

## Floating windows

`FloatingWindow.vue` is the generic draggable / resizable / dockable shell behind
the log, terminal, and editor windows. Per-window geometry, dock side/size, and
visibility persist in `localStorage` under `spangap.win.<id>`. It is pointer-event
driven (`touch-action: none` so phones don't hijack the gesture as scroll) and
gives phones a sensible first-run layout via an optional `defaultDock` prop.

### The window manager (`lib/windows`)

`lib/windows` is a small reactive registry — not a Pinia store — that mirrors the
live set of `FloatingWindow`s: `registerWindow` / `unregisterWindow`,
`setWindowTitle` / `setWindowVisible` / `setWindowZ`, plus `focusWindow(id)` (and
the nonce `windowFocusReq` that `FloatingWindow` watches to raise itself). The
computed `focusedWindowId` (front-most visible window) and `openWindows`
(visible, front-most first) back the mobile window switcher. On desktop the
registry only mirrors z-order; on a phone it is load-bearing — the floating
paradigm collapses to a single full-screen window, and `focusedWindowId` is the
one painted.

### Compact mode (`lib/viewport`)

`useCompact()` (`lib/viewport`) is the single source of truth for "phone-class
viewport" (`$q.screen.lt.md`, ≈ <1024px). Every responsive decision reads this
one flag so the Dock's bottom-nav form, the single-window collapse, the
full-screen settings drawer, and an app's master/detail→single-column fold all
flip together at one breakpoint.

## What it does NOT own

- Camera, video player, RTSP, recording UI — the consuming app's `browser/` tree.
- Network / WiFi-scan panels — [spangap-net](../../spangap-net).
- App-specific panels (per-radio settings, chat UIs) — the owning straddle's
  `browser/`.
