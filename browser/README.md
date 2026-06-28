# spangap-browser

## What is this?

**spangap-browser** is the browser-side runtime and shared UI shell for
[spangap](../../spangap) device apps: the WebRTC session manager, config sync, the
config-bound `Setting*` controls, the path-based menu/settings system and the
`GeneratedPanel` renderer, the Dock app launcher, FloatingWindow / LogWindow /
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
    ├── lib/                apps, generatedPanels, auth, device-url, reconnect,
    │                       viewport, windows, webrtc-session
    ├── components/         SettingToggle/Slider/Select/Text, PanelHeading,
    │                       GeneratedPanel, GeneratedListRow, Dock, SettingsWindow,
    │                       SettingsNavTree, FloatingWindow, LogWindow,
    │                       TerminalWindow, EditorWindow, ConnectionOverlay, UsableArea
    ├── stores/             device, log, menu, index (pinia setup)
    ├── modules/            advanced, editor, system (self-register on import)
    ├── panels/             AboutPanel, SystemPanel, DeveloperPanel
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
import { registerGeneratedPanels } from 'spangap-browser/lib/generatedPanels';
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
```

Modules in `spangap-browser/modules/*` self-register with the menu store
(`useMenuStore`) on import — your app imports them once (typically from a Quasar
boot file). In practice the generated `straddles.gen.ts` does this for every
staged straddle automatically.

## Conventions

- **`Setting*` controls** (`SettingToggle`, `SettingSlider`, `SettingSelect`,
  `SettingText`) are config-bound — pass a config-key path and they handle the
  storage subscription, optimistic update, and rollback.
- **Register an app** for the Dock with `registerApp({ id, label, icon, open,
  isOpen?, placement? })` from `lib/apps`.
- **Register a settings pane** with `useMenuStore().register('settings/<group>/<leaf>',
  label, { type: 'panel', component })`; or let the build generate it from a
  straddle.yaml `settings:` block via `registerGeneratedPanels`.
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
  (Dock, menu store, generated panels, the WebRTC session).
- [../README.md](../README.md) — the straddle overview and the full function index.
