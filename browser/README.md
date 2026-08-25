# spangap-browser

## What is this?

**spangap-browser** is the browser-side runtime and shared UI shell for
[spangap](../../spangap) device apps: the WebRTC session manager, config sync, the
config-bound `Setting*` controls, the path-based menu/settings system and the
settings-tree renderer, the Dock app launcher, FloatingWindow / LogWindow /
TerminalWindow / EditorWindow, the default System/About/Developer panels, the auth
+ session flow, and the login / setup pages. It is the npm half of the
[spangap-web](..) straddle and pairs in lockstep with its firmware half.

The straddle-level model guide is [../docs/browser-shell.md](../docs/browser-shell.md);
this file covers install and import.

## What this subdir owns

```
browser/
├── package.json            name: spangap-browser, subpath exports map
├── tsconfig.json
└── src/
    ├── index.ts            re-exports (lib/* + the three stores)
    ├── lib/                apps, settingsNodes, settingsRuntime, reboot, auth,
    │                       viewport, windows, webrtc-session, safeMode
    ├── components/         SettingToggle/Slider/Select/Text, PanelHeading,
    │                       NodePane, SettingRows, SettingsCollection, Dock, SettingsWindow,
    │                       SettingsNavTree, FloatingWindow, LogWindow,
    │                       TerminalWindow, EditorWindow, ConnectionOverlay, UsableArea
    ├── stores/             device, log, menu, settingsTree, index (pinia setup)
    ├── modules/            advanced, editor, system (self-register on import)
    ├── vite/               dev-server plugins (plain .mjs — they run in the
    │                       config loader, not in the app bundle)
    ├── panels/             AboutPanel, DeveloperPanel (menu-bar panes; settings
    │                       are described in each straddle.yaml, not written here)
    └── pages/              LoginPage, SetupPage
```

## Install

```bash
npm install spangap-browser
```

For local development against a sibling spangap checkout:

```jsonc
// package.json
"dependencies": {
  "spangap-browser": "file:../path/to/spangap-web/browser"
}
```

`spangap-browser` ships TypeScript / Vue source (no `dist`); your bundler (Vite
via Quasar) consumes `.vue` and `.ts` directly.

## How others use it

The package exposes its API via subpath exports — import only what you need.

```typescript
// stores + root re-exports
import { useDeviceStore, useLogStore, useMenuStore } from 'spangap-browser';

// lib
import { registerApp } from 'spangap-browser/lib/apps';
import { registerSettingsNodes } from 'spangap-browser/lib/settingsNodes';
import { webrtcSession } from 'spangap-browser/lib/webrtc-session';
import { authLogin, checkAuth } from 'spangap-browser/lib/auth';

// components
import SettingToggle from 'spangap-browser/components/SettingToggle.vue';
import FloatingWindow from 'spangap-browser/components/FloatingWindow.vue';

// default panel modules (import once to register them in the menu)
import 'spangap-browser/modules/system';
import 'spangap-browser/modules/advanced';
import 'spangap-browser/modules/editor';

// pages — wire into your app's router
import LoginPage from 'spangap-browser/pages/LoginPage.vue';
import SetupPage from 'spangap-browser/pages/SetupPage.vue';

// dev-server plugins — for quasar.config.ts, not for the app bundle
import { workspaceMounts } from 'spangap-browser/vite/workspace-mounts';
import { linkedDepsHmr } from 'spangap-browser/vite/linked-deps-hmr';
```

`workspaceMounts` serves directories of the spangap workspace as extra dev-server
paths, so a page that expects them beside itself in production finds them there
under `spangap dev` too. Vite has one static root, so extra trees can only be
middleware; the mounts register in `configureServer`, ahead of Vite's own static
and transform middlewares, or the request would resolve as an app asset first.

```typescript
build: {
  vitePlugins: [
    [workspaceMounts, { '/flashmon': 'flashmon/flashmon', '/builds': 'builds' }],
  ],
}
```

Paths are workspace-relative (`SPANGAP_WORKSPACE`, which `spangap dev` sets) or
absolute. A mount whose directory isn't there is skipped with a warning, so a
bare `quasar dev` outside a workspace still starts — minus those paths. Dev only:
the plugin declares no build hooks, and in production whatever serves the app
serves these too.

`linkedDepsHmr` is what makes an edit to a linked package — every straddle
browser half — reach the browser live. It takes no options:

```typescript
build: {
  vitePlugins: [
    [linkedDepsHmr, {}],
  ],
}
```

The app resolves those packages with `resolve.preserveSymlinks` (their peers —
vue, quasar, pinia, vue-router — exist only in the consumer's `node_modules`), so
their module ids sit under `<root>/node_modules/<name>/…`. Vite prepends
`**/node_modules/**` to its watcher's ignore list and a config can only add to
that list, never subtract; the symlink target is outside the root, so it isn't
watched either. With no event the transform cache is never invalidated, and even
a page reload serves the edit from before it — only a dev-server restart would.
The plugin watches each `file:` dependency's real directory and re-emits the
event on Vite's own watcher under the `node_modules` path the module graph is
keyed by, where it takes the ordinary HMR path. Dev only, like the mounts.

Modules in `spangap-browser/modules/*` self-register with the menu store
(`useMenuStore`) on import — your app imports them once (typically from a Quasar
boot file). In practice the generated `straddles.gen.ts` does this for every
staged straddle automatically.

## Conventions

- **`Setting*` controls** (`SettingToggle`, `SettingSlider`, `SettingSelect`,
  `SettingText`) are config-bound — pass a config-key path and they handle the
  storage subscription, optimistic update, and rollback. They render only once
  the storage dump has landed (`useSettingsReady()`, i.e. `device.synced`), and
  so does a whole `SettingRows` block: a control mounted against a key that has
  no value yet shows its zero state and then animates to the truth, which reads
  as opening the pane having changed the setting. Anything else binding device
  state into a control's initial value takes the same gate. For the same reason
  rows are keyed by their storage key and a pane by its node path: a control
  patched over the one another pane left in its place would animate from that
  pane's value to this one's.
- **No icon font ships** with the SPA — a font-ligature icon name (`icon="edit"`)
  renders as the word itself. Icons are inline-SVG components (`IconPen`,
  `IconTrash`) that inherit `currentColor`.
- **A settings row is `<SettingRow label="…">` with the control in its slot** — a
  two-column grid, not a flex line, so a control that brings its own idea of
  width (a slider's track, a field's box) can only land in the control column.
  The name column's width lives in `NAME_COL` (`settingsRuntime.ts`) and
  everything that must line up with it reads it from there: the readout grid, a
  slider's hint, a description hung under a row. A text box or dropdown asks for
  `class="set-field"` and takes a share of the column rather than all of it.
- **`PanelHeading`** is the largest type in a pane; descriptive text (`caption`
  rows, a slider's `hint`) is the smallest, italic and inset. A description
  following a heading runs full width — it is about everything under it; one
  following a row is indented to the control column, where the row it describes
  has its value.
- **Buttons** are filled and white-lettered via `buttonStyle(color?)` — a colour
  a button states is its background, never its ink, and a button that states no
  colour is the palette's blue. Same table as the pills, so a red button is the
  red a red pill is.
- **Banding** — two dark greys alternating, edge to edge, matching the device's
  own lists (`listRow()` in `lcd_settings_desc.cpp`) — is for the rows of a
  COLLECTION: things the operator configures, like known networks or TCP peers.
  Settings rows and status readouts are not a list of things and are not banded.
- **Register an app** for the Dock with `registerApp({ id, label, icon, open,
  isOpen?, placement? })` from `lib/apps`.
- **Register a settings pane** with `useMenuStore().register('settings/<group>/<leaf>',
  label, { type: 'panel', component })`; or let the build generate it from a
  straddle.yaml `settings:` block via `registerSettingsNodes`.
- **`useDeviceStore()`** exposes the live device state, populated over the
  `storage:1` DataChannel.
- **`webrtcSession`** is a singleton; call `webrtcSession.registerChannel(builder)`
  to add app DataChannels.
- Stores use Pinia; the package re-exports the configured setup from
  `stores/index.ts` (or use your own).

## What it does NOT own

- Camera, video player, RTSP, recording UI — the consuming app's browser tree.
- Network / WiFi-scan panels — these live in
  [spangap-net](../../spangap-net)'s `browser/`.
- App-specific panels — the owning straddle's `browser/src/panels/`.

## Read next

- [INTERNALS.md](INTERNALS.md) — package-scope developer notes.
- [../docs/browser-shell.md](../docs/browser-shell.md) — the shell model
  (Dock, menu store, the settings tree, the WebRTC session).
- [../README.md](../README.md) — the straddle overview and the full function index.
