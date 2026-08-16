# browser-shell — internals

Maintainer reference for the `spangap-browser` shell model — how the Dock, the
menu store, the settings tree, and the WebRTC session fit together. The
[operator guide](browser-shell.md) is the consumer view; the package's own
[README](../browser/README.md) and [INTERNALS](../browser/INTERNALS.md) cover npm
mechanics (subpath exports, peer deps, no-`dist` build).

## 1. What this function provides

The npm package `spangap-browser`, exported via subpath imports
(`./lib/*`, `./stores/*`, `./modules/*`, `./components/*.vue`, `./panels/*.vue`,
`./pages/*.vue`). The package root (`index.ts`) re-exports only a subset — `lib/`
`auth`, `device-url`, `reconnect`, `viewport`, `windows`, `webrtc-session` and the
`device` / `log` / `menu` / `settingsTree` stores; `lib/apps` and
`lib/settingsNodes` are reached by subpath, not the root. It ships TypeScript/Vue source — the consumer's Vite/Quasar pipeline
compiles it. The shell pieces:

- **`lib/apps.ts`** — the Dock app registry (`registerApp`, `sortedApps`) and the
  bundled icon registry (`registerAppIcons`, `appIconSvg`).
- **`stores/menu.ts`** — `useMenuStore`: the path-based menu-bar registry
  (`advanced/…`, `app/…`). A `settings/…` path is forwarded to the tree store.
- **`stores/settingsTree.ts`** — `useSettingsTreeStore`: the settings tree, whose
  nodes hold both rows and children.
- **`lib/settingsNodes.ts`** + **`components/NodePane.vue`** — the descriptor
  types with the build's entry point, and the single runtime renderer.
- **`lib/settingsRuntime.ts`** + **`lib/reboot.ts`** — `{field}` substitution,
  gate truthiness, the `set` action and `paletteColor` (the one colour table,
  shared by status pills and coloured buttons and holding the same hexes the
  firmware's `pillColor` does); and the reboot-wait choreography a
  `reboots: true` write names instead of re-implementing.
- **`lib/webrtc-session.ts`** — the shared `RTCPeerConnection` singleton.
- **`lib/fitText.ts`** — the `v-fit-text` directive: keeps a one-line heading on
  one line, shrinking the type size to fit rather than wrapping or clipping.
- **`stores/device.ts`** / **`stores/log.ts`** — config sync and the log ring.
- **`lib/auth.ts`** + `pages/{LoginPage,SetupPage}.vue` — the auth flow.
- **`components/`** — `Dock`, `SettingsWindow`, `SettingsNavTree`, `FloatingWindow`,
  `LogWindow`, `TerminalWindow`, `EditorWindow`, `ConnectionOverlay`, `UsableArea`,
  the `Setting*` controls, `PanelHeading`, and the settings renderer
  (`NodePane`, `SettingRows`, `SettingsAction`, `SettingsCollection`,
  `SettingsFormDialog`).
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
2. inlines each straddle.yaml `settings:` block as `GenNode[]` tree fragments and
   calls `registerSettingsNodes`;
3. globs each straddle's `src/app-icons/*.svg` as raw strings and calls
   `registerAppIcons`.

There is no hard-coded straddle list anywhere — the generator is the single source.

## 3. The menu store (`stores/menu.ts`)

This serves the MENU-BAR groups. Settings is a different model entirely — see §4.


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

## 4. The settings tree (`stores/settingsTree.ts`)

`contribute(segments, rows)` walks the path of segment ids from the root,
conjuring what is missing, and appends the row block at the last segment. Naming
is last-setter-wins per field; a node nobody names keeps its title-cased id.
Siblings sort by `bySiblingOrder`: nodes carrying an `order` first ascending, the
rest after them in arrival order — the same rule the LCD registry and the
generator use.

There is **no leaf model**. A node with only children renders as a list of them,
a node with only rows renders as a panel, and one with both renders both, rows
first. `activePath` defaults to the root, which is why there is no
first-pane-default-landing workaround any more.

`nodeRenders(node)` / `visibleChildren(node)` are the emptiness rule: a node
shows only if it has rows or a descendant that does. `SettingsNavTree` filters
the rail through them and `NodePane` the chevron list, so a menu declared for its
name and order alone (spangap-core's `apps` / `system`, reticulous's
`reticulum`) stays out of both until something contributes to it.

`registerSettingsNodes` (`lib/settingsNodes.ts`) is the build's entry point; the
same file carries the descriptor types. `NodePane.vue` renders a node, delegating
its rows to `SettingRows.vue`, which maps each `GenRow.kind` to a `Setting*`
component. A `secret` text row is **write-only**: it renders a password field
that is never read back (the value lives in `secrets.*`, which is not synced to
the browser) and writes via a setter.

Two kinds are laid out in `SettingRows` itself rather than by a `Setting*`
control, because they are arrangements rather than controls. A `buttons` row is
a flex line whose `justify-content` comes from `align`, holding one
`SettingsAction` per item, each with its own `whenKey`. An `info` row is a
two-track grid — `minmax(0, min(33%, max-content)) 1fr` — which is the whole of
what the LCD has to measure for itself: content-sized, capped at a third.

The build hands over a node's rows already assembled, sections merged, one
fragment per node. Nothing here concatenates blocks or de-duplicates a heading;
`contribute()` appending is only for the hand-written contributions that arrive
after the generated ones.

`SettingsAction.vue` fronts the three action kinds — a `set` write, a dialog
whose buttons nest further actions, or a form. `SettingsFormDialog.vue` holds its
field values locally and reaches the device only on submit, which is what makes
submit-and-error possible in place of per-keystroke validation: `<cmd>.error`
going non-empty shows the reason and keeps the dialog open, and the watched
target changing after a submit closes it. No timeouts.

`SettingsCollection.vue` never writes its array. It writes `<cmd>.add` /
`.remove` / `.set` / `.order`; the owning task is the array's only writer. A drag
sends the whole id order comma-joined and holds an **optimistic order** until the
re-published array arrives — which is safe precisely because the firmware applies
the payload as a preference permutation rather than as an authoritative list.

A `settings/…` path handed to the menu store is forwarded here as a
component-typed row. That adapter is transitional: it lets a hand-written
`*Panel.vue` occupy a node until its straddle's `settings:` block describes it,
and it goes with the last one.

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
  them in sync if you change the bucket rule. The settings tree does NOT use it:
  its rule is `order`-then-arrival, with no buckets and no alphabetic tier.
- **Never read a secret back.** Secret config keys live under `secrets.*` and are
  not in the synced storage tree; the generated secret row is write-only by design.
- **A settings node is a node, not a leaf.** Code that assumes a settings path
  ends at something rendered, or that a node with children has no rows of its
  own, is wrong on both counts.
- **The firmware publishes finished strings, and validates in sentinel
  handlers.** Neither convention is decoration: they are what let a static
  descriptor describe a whole pane. Adding formatting, comparison or client-side
  validation here puts the same logic on two surfaces again, which is the
  duplication this replaced.
