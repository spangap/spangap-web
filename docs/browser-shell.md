# browser-shell — the SPA shell

The browser half of this straddle is the npm package **`spangap-browser`**: the
shared UI shell that consuming Quasar / Vue 3 apps assemble into their device SPA.
It provides the app launcher (the Dock), the path-based menu and settings system,
the declarative settings tree and its renderer, the config-bound `Setting*` controls,
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

## The settings tree

Settings are **one tree**, held by `useSettingsTreeStore` (`stores/settingsTree`)
and mirroring the LCD's `lcdSettingsContribute`. Every node holds key/value
**rows** *and* **children**: the pane renders its rows first, then its children
as navigation entries. There is no leaf/container distinction, and no node is
owned — a contribution names a path of segment ids, every intermediate node on
the way is conjured, and two straddles contributing at the same path concatenate
their row blocks.

Siblings sort by one rule: nodes carrying an `order` first ascending, everything
else after them in arrival order. The build emits contributions pre-sorted (it
knows straddle init order), so arrival order is already meaningful. Naming is
last-setter-wins per field, so the buildable has the final say over the tree its
image ships.

A node with no rows and no rendering descendant is **not shown** — not in the
rail, not as a chevron in a pane. Declaring a node is therefore not the same as
putting it on the screen: the top-level menus are named and ordered once each
(spangap-core names Apps and System, spangap-net names WiFi & Network, the
buildable names Reticulum Mesh) and each appears only once something contributes
to it.

`SettingsWindow.vue` renders the tree as a first-class app window (the gear Dock
icon): a nav rail plus the selected node on desktop, a drill-down on phones. The
root is an ordinary node, so opening Settings lands on something rather than on
an instruction to pick something.

The **menu store** (`stores/menu`) is a different thing and stays: it serves the
menu-bar groups (`advanced/…`, `app/…`), a window-manager mechanism with a
genuine leaf model. A `settings/…` path handed to it is forwarded into the tree,
which is the transitional adapter that lets a hand-written `*Panel.vue` occupy a
node until its straddle describes it declaratively.

### Declarative settings

Settings panes are not hand-written. The build lowers every straddle.yaml
`settings:` block into node-tree fragments and calls `registerSettingsNodes`
(`lib/settingsNodes`), which merges them into the tree; one renderer
(`NodePane.vue` over `SettingRows.vue`) interprets the rows, mapping each to the
matching `Setting*` component — so a declared pane is visually identical to a
hand-written one. This is the web parallel to the firmware's generated settings
nodes: one generic renderer, no SFC per pane.

Row kinds: `section`, `caption`, `switch`, `slider`, `text` (with a write-only
`secret` variant), `dropdown` (optionally `searchable`), `value` (read-only live
text, optionally `copyable`), `button`, `buttons` (several content-sized buttons
on one line, gathered left, centre or right, each optionally gated on its own
key), `info` (a run of value rows as one block: a shared label column sized to
the widest label and capped at a third, and no gap between the lines), and
`list` — a collection with an item editor, add forms, removal, reordering and
scan-and-adopt candidates. Any row may carry `whenKey`, which shows it while a
key is truthy. A row the build gated with `when_surface: lcd` never reaches here
at all — the backup and restore buttons are the reverse case, `when_surface: web`,
because the archive they move needs a browser on the other end.

Sections do not repeat. Two straddles writing `section: "Status"` at one node
get one heading with both sets of rows under it — merged by the build, since a
node's rows arrive here already assembled.

Two firmware conventions are what let a static descriptor describe a whole pane,
and both are worth knowing before writing UI code here:

- **The firmware publishes finished strings.** A value row, a subtitle, a status
  pill all render exactly what the key holds. Nothing on this side formats,
  composes or compares; a gate is tested for truthiness, never equality.
- **The firmware validates in sentinel handlers.** A form submits its fields as
  one JSON object to a command key, and the owning task answers on the sentinel
  family's error/ack pair — `<cmd>.error` / `<cmd>.done`, shared by all of one
  collection's sentinels and passed into the form dialog by the collection; a
  bare form defaults to `<form-cmd>.error` / `.done`. A reason on the error key
  keeps the dialog open showing it; the ack key moving closes it (an edit that
  changes nothing still acks). The dialog clears the error key just before each
  submit so an identical rejection still registers as a change. This side
  submits and displays — there is no client-side validation, and a collection
  never writes its own array.

A **button** runs an action: `set` (write a key, optionally `edge` to force a
change past the storage actor's dedup, or `reboots` to run the shared
reboot-wait behaviour in `lib/reboot.ts`), `dialog` (a confirmation or choice
with no input fields, whose buttons nest further actions), or `form` (the one
dialog with inputs, because it fronts a sentinel). Any button — a row's, one of
a `buttons:` line's, a dialog's, a collection item's — may state a `color` from
the palette a status pill uses (`red`, `green`, `amber`, `blue`, `grey`, or an
explicit `rrggbb`, all resolved by `paletteColor` in `lib/settingsRuntime`). A
coloured button is filled with it, the way a pill is; an uncoloured one keeps
the outline every other button has.

### Hand-written panels and the `Setting*` controls

A straddle that needs a bespoke pane places a `*Panel.vue` under its
`browser/src/panels/` and registers it from its `register*` module. The shipped
config-bound controls — `SettingToggle`, `SettingSlider`, `SettingSelect`,
`SettingText`, plus `PanelHeading` — take a config-key path and handle the storage
subscription, optimistic update, and rollback. The shell's own default panels are
About, System, and Developer.

Headings are one unbroken line. `PanelHeading` and the nav tree's group titles
carry `white-space: nowrap; overflow: hidden` and the `v-fit-text` directive
(`lib/fitText.ts`): when the phrase is wider than its box, the type size drops
by one division — glyph advances scale linearly with size, so no trial sizes and
no convergence loop — and a long heading shrinks instead of wrapping or being
clipped. The text itself is never rewritten, so find-in-page and copy still see
the phrase the settings tree and its descriptors carry. Every fit measures at the
stylesheet size (the inline size is cleared first), so repeated fits land on the
same answer; a `ResizeObserver` re-fits on layout changes and `document.fonts.
ready` re-fits once the real face has replaced the fallback metrics. Block
elements only — on an inline-block, writing the size back would change the width
being measured.

The System panel carries the **Backup & Recovery** actions — back up, restore,
factory reset. Each one is an ordinary storage write (`s.sys.backup`,
`s.sys.restore`, `s.sys.factory_reset`), which the firmware turns into a reboot
into [safe mode](../../spangap-core/docs/safe-mode.md); `lib/safeMode` then covers
the page while the device is away, and **reloads** once it answers again. That
reload is required, not cosmetic: without it the browser sits on the stale app
shell and never sees the safe-mode page the device is now serving. All three ask
first, because all three take the device away for a reboot; restore and factory
reset say additionally that there is no undo, and the factory-reset dialog offers
a target only when `sys.sd.present` says there is a card to choose between.

The cover `safeMode` puts up comes in two shapes, split on whether there is
anything left to say. A backup or restore reboot is a **wait the operator just
asked for** — black screen, spinner, no words, because a paragraph saying the
device is rebooting only repeats the dialog they pressed OK on. The card with
text is for the covers that carry something they do not already know: a factory
reset, which comes back on its own access point and will not be at this address,
and a write that never left because the config channel was down.

The Settings window opens on its first pane (System) rather than on an empty
"select a setting" pane — on the two-pane desktop layout there is room for both,
so an empty right half is only a step to take before anything is on screen. On a
phone the nav *is* the window until a leaf is picked, so nothing is pre-picked
there.

It also **closes the WebRTC session** a second after the write goes out (long
enough for the flag patch to leave). Safe mode does not bring webrtc up, so a
session left running spends the whole window reconnecting to something that will
never answer, and that backoff is what made the device seem slow to reappear —
the poll was competing with a reconnect storm for the same link.

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
natural "seek" / "switch source" mechanism.

A channel close means one of two very different things, and a consumer that acts
on it must tell them apart: the far end ended **that channel** (its app quit), or
the channel went down **with its session** (reconnect, the device store's
link-down `refresh()`, transport failure). `session.epoch` is the generation of
the current PC, bumped by every teardown *before* the PC is closed; a consumer
records it in its `onopen` and asks `session.sessionHealthy(epoch)` in its
`onclose`. True means the far-end app quit. False means the transport went and
the builder will run again on the next PC — the consumer keeps its UI and
reattaches, because a link blip is not a user action and must not be treated as
one. Session states: `idle`, `connecting`,
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
owns its own `cli:1` DC. Both windows close themselves when the device ends
their channel under a healthy session — the CLI's `exit`, the log stream
stopping — mirroring the on-device CLI and Log apps, which stop on their own ITS
disconnect. The store publishes that as `logStreamEnded`, a counter raised only
for a stream that ended while the session was up; `logConnected` stays the plain
"channel is open" flag. A drop that takes the whole session leaves both windows
on screen, and their channels reattach on the next PC.

## Auth and the login flow

`lib/auth` wraps the device's JSON auth API (served by [web](web.md)):
`authLogin(password, realm?)`, `authPasswd(realm, old, new)`, `authLogout()`, and
`checkAuth()` (a `HEAD /` probe reading the `X-Authenticated` header to learn
whether auth is enabled and which realm the cookie holds). `isAdminUnset()` probes
`authPasswd('admin','','')` to drive onboarding. The session cookie is set with a
60-day `max-age`, `SameSite=Strict`. `LoginPage.vue` and `SetupPage.vue` are the
shipped login and first-run onboarding pages; wire them into the app router.

## Floating windows

`FloatingWindow.vue` is the generic draggable / resizable shell behind the log,
terminal, and editor windows. Per-window geometry and visibility persist in
`localStorage` under `spangap.win.<id>`, and that record is the whole of what a
page load restores. It is pointer-event driven (`touch-action: none` so phones
don't hijack the gesture as scroll).

**The persisted `visible` flag is user intent.** It moves only for something the
user would recognise as opening or closing the window — the close dot, a dock
launch, an app dismissing its own window. A window whose content rides a live
link therefore rides out a link drop with `visible` untouched and reattaches when
the session returns; lowering it on a transport event would quietly rewrite the
layout that comes back on the next page load, and the window would be missing
from every load after that.

**Click-to-focus is click-to-focus only.** A click anywhere in a background
(non-front-most) window raises and focuses it, and that click is swallowed — it
does not also actuate whatever sits under the pointer, so a stray click on a
buried window can't fire a button, follow a link, or trigger a row action. The
`mousedown` is left to propagate, so an input or terminal under the pointer still
takes keyboard focus and accepts typing immediately. Two cases pass through
untouched: a pointer that moved more than a few pixels between press and release,
and a release with a non-collapsed text selection — both are drags, not clicks.
Once a window is front-most, its content behaves normally; only the raising click
is ever swallowed. Window content therefore needs no guard of its own.

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

### Top-bar icons (`lib/topbarIcons`)

`lib/topbarIcons` is a small reactive registry (the sibling of the window-mount
registry): `registerTopbarIcon({ id, component })`, read by the `TopbarIcons.vue`
component. A buildable's `MainLayout` mounts `<TopbarIcons/>` once in its header,
just left of the power/logout button; from then on, staging a straddle whose
`register*` module calls `registerTopbarIcon()` surfaces its indicator there — no
layout edit and no static import of a package that may not be staged. The shell
only positions the icons; each registered component owns its own content (reads
whatever device-store keys it needs) and collapses when it has nothing to show.
Ordering is registration (= init) order, newest nearest the power button.

## What it does NOT own

- Camera, video player, RTSP, recording UI — the consuming app's `browser/` tree.
- Network / WiFi-scan panels — [spangap-net](../../spangap-net).
- App-specific panels (per-radio settings, chat UIs) — the owning straddle's
  `browser/`.
