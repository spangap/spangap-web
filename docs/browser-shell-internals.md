# browser-shell — internals

Maintainer reference for the `spangap-browser` shell model — how the Dock, menu
store, generated panels, and the WebRTC session fit together. The
[operator guide](browser-shell.md) is the consumer view; the package's own
[README](../browser/README.md) and [INTERNALS](../browser/INTERNALS.md) cover npm
mechanics (subpath exports, peer deps, no-`dist` build).

## 1. What this function provides

The npm package `spangap-browser`, exported via subpath imports
(`./lib/*`, `./stores/*`, `./modules/*`, `./components/*.vue`, `./panels/*.vue`,
`./pages/*.vue`). The package root (`index.ts`) re-exports only a subset — `lib/`
`auth`, `device-url`, `reconnect`, `viewport`, `windows`, `webrtc-session` and the
`device` / `log` / `menu` stores; `lib/apps` and `lib/generatedPanels` are reached
by subpath, not the root. It ships TypeScript/Vue source — the consumer's Vite/Quasar pipeline
compiles it. The shell pieces:

- **`lib/apps.ts`** — the Dock app registry (`registerApp`, `sortedApps`) and the
  bundled icon registry (`registerAppIcons`, `appIconSvg`).
- **`stores/menu.ts`** — `useMenuStore`: the path-based menu/settings tree.
- **`lib/generatedPanels.ts`** + **`components/GeneratedPanel.vue`** — the
  declarative-panel registry and its single runtime renderer.
- **`lib/webrtc-session.ts`** — the shared `RTCPeerConnection` singleton.
- **`stores/device.ts`** / **`stores/log.ts`** — config sync and the log ring.
- **`lib/auth.ts`** + `pages/{LoginPage,SetupPage}.vue` — the auth flow.
- **`components/`** — `Dock`, `SettingsWindow`, `SettingsNavTree`, `FloatingWindow`,
  `LogWindow`, `TerminalWindow`, `EditorWindow`, `ConnectionOverlay`, `UsableArea`,
  the `Setting*` controls, `PanelHeading`, `GeneratedListRow`.
- **`panels/`** — the default `About`, `System`, `Developer` panes.
- **`modules/`** — `advanced` (exports `registerAdvanced()`: the Settings / CLI /
  System-Log dock apps, the backlog-size menu, the open-editor menu, the Developer
  pane) and `system` (`registerSystem()`: the System pane + the hidden About pane).
  These are explicit register helpers the host app calls at boot, not auto-imports.
  `editor` is a pure reactive registry (`editors`, `openEditor`/`closeEditor`/
  `isPathOpen`) consumed by `EditorWindow`, with no registration of its own.

## 2. The generated dispatcher

The build (spangap-inside) emits `straddles.gen.ts` into the buildable. It:

1. imports each consumed straddle's `browser/src/register*` module (where the
   straddle registers its apps, menu items, and window refs);
2. inlines each straddle.yaml `settings:` block as a `GenPanel[]` and calls
   `registerGeneratedPanels`;
3. globs each straddle's `src/app-icons/*.svg` as raw strings and calls
   `registerAppIcons`.

There is no hard-coded straddle list anywhere — the generator is the single source.

## 3. The menu store (`stores/menu.ts`)

`register(path, label, leaf, opts)` splits the slash-path: segment 0 is a
top-level group, the last segment is the leaf, the middle are submenus created on
demand (title-cased from their id) and merged by id across concurrent
registrations. Re-registering the same leaf path updates in place. A leaf is
`{type:'panel', component}`, `{type:'toggle', key}`, or `{type:'action', action}`.

`placement` (default 0) buckets siblings: `>0` first ascending, `0` middle
alphabetic, `<0` last ascending — `placeRank`/`byPlacement` implement it, and the
Dock's `sortedApps` mirrors the same comparator. `setMenu(path, opts)` overrides a
container's label/placement/hidden. `unregister` removes a leaf or subtree and
prunes containers (and the group) left empty. `activePanel` + `activePanelComponent`
drive the rendered pane; `hidden` leaves are openable by id but not listed (e.g.
the About pane).

## 4. Generated panels (`lib/generatedPanels.ts`)

`registerGeneratedPanels(panels)` walks each `GenPanel`: it `setMenu`s the panel's
containers (only overriding `placement` when the descriptor set one, so a container
another straddle already placed isn't clobbered), stashes the descriptor by its
leaf path, and `menu.register`s the leaf pointing at the shared `GeneratedPanel`.
`GeneratedPanel.vue` takes no props — it finds its own descriptor via
`getGeneratedPanel(activePanel)` and maps each `GenRow.kind` to a `Setting*`
component. A `secret` text row is **write-only**: it renders a password field that
is never read back (the value lives in `secrets.*`, which is not synced to the
browser) and writes via a setter.

## 5. The WebRTC session singleton

`lib/webrtc-session.ts` owns one `RTCPeerConnection` and the `/webrtc` signaling
WS. Channel builders run synchronously before `createOffer()` on every fresh PC so
the offer always carries the `m=application` line. `connect()` is idempotent.
Close-code handling: `4401`→`auth`, `4409`→`busy`, `4008`→`kicked`; `busy`/`kicked`
disable auto-reconnect and a takeover requires the user to opt in (`?force=1`).
Reconnect resilience: heartbeat ping, a staleness check, and a visibility-change
nudge for phone wake. The staleness check compares against
`max(lastPongAt, session.lastDcRxAt)` — every channel's `onmessage` calls
`noteDcActivity()`, so any received byte counts as a pong and a flood on one
channel that delays the storage pong doesn't trip a false link-down.

## 6. Pitfalls

- **The shell's own apps/panes register on an explicit call, not on import.** The
  host app calls `registerAdvanced()` / `registerSystem()` at boot; nothing in
  `lib/`, `modules/`, or `components/` self-registers when merely imported, so
  import order can't silently change what the Dock or settings tree contains. The
  generated dispatcher auto-imports only each *consumed straddle's* `register*`
  module — the package's own shell is wired up by the consuming SPA.
- **Bundle icons as raw SVG, don't ship asset files.** The device webroot ships
  only `app.js`; a `/app-icons/*.svg` URL never reaches the device, so the Dock
  renders inline SVG bundled at build time via `registerAppIcons`. The registry
  strips any leading `<?xml?>` prolog before `v-html` injection.
- **`registerApp` placement and `menu` placement share one comparator** — keep
  them in sync if you change the bucket rule.
- **Never read a secret back.** Secret config keys live under `secrets.*` and are
  not in the synced storage tree; the generated secret row is write-only by design.
- **Settings panes register under `settings/…`** and render in `SettingsWindow`
  unchanged whether generated or hand-written — don't special-case one path.
