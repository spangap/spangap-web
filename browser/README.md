# spangap-browser

## What is this?

**spangap-browser** is the browser-side runtime and shared UI shell for
[spangap](../../spangap) device apps: a WebRTC session manager, the
config-bound settings controls, FloatingWindow / LogWindow / Terminal,
default config panels (network, system, advanced, ACME, DuckDNS, UPnP,
WireGuard, …), the auth + session flow, and login/setup pages. It is the
browser half of the [spangap-web](..) straddle.

It is the npm half of the dual-side platform; pairs with the firmware
half of `spangap-web`.

## What this subdir owns

```
browser/
├── package.json            name: spangap-browser, exports map for subpath imports
├── tsconfig.json
└── src/
    ├── index.ts            re-exports (lib + stores)
    ├── lib/                auth, device-url, reconnect, webrtc-session
    ├── components/         SettingToggle/Slider/Select/Text, SettingsPanel,
    │                       FloatingWindow, LogWindow, MenuBar, PanelHeading,
    │                       TerminalWindow, EditorWindow
    ├── stores/             device, log, menu, index (pinia setup)
    ├── modules/            advanced, editor, network, system (auto-register on import)
    ├── panels/             AboutPanel, DeveloperPanel, NetworkPanel,
    │                       SystemPanel, WifiScanDialog
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

`spangap-browser` ships TypeScript source (no `dist`); your bundler
(Vite via Quasar) consumes `.vue` and `.ts` directly.

## How others use it

The package exposes its API via subpath exports — import only what you
need.

```typescript
// stores
import { useDeviceStore, useLogStore, menuRegistry } from 'spangap-browser';

// lib
import { webrtcSession } from 'spangap-browser/lib/webrtc-session';
import { signIn } from 'spangap-browser/lib/auth';

// components
import SettingToggle from 'spangap-browser/components/SettingToggle';
import FloatingWindow from 'spangap-browser/components/FloatingWindow';

// default panels (register them in your boot file to get them in the menu)
import 'spangap-browser/modules/network';
import 'spangap-browser/modules/system';
import 'spangap-browser/modules/advanced';
import 'spangap-browser/modules/editor';

// pages — wire into your app's router
import LoginPage from 'spangap-browser/pages/LoginPage';
import SetupPage from 'spangap-browser/pages/SetupPage';
```

Modules in `spangap-browser/modules/*` self-register with `menuRegistry`
on import — your app just needs to import them once (typically from a
Quasar boot file).

## Conventions

- **Settings components** (`SettingToggle`, `SettingSlider`,
  `SettingSelect`, `SettingText`) are config-bound — pass a config-key
  path and they handle the storage subscription, optimistic update, and
  rollback.
- `useDeviceStore()` exposes the live device state (uptime, free heap,
  build id, etc.) — it's populated by the storage WebSocket.
- `webrtcSession` is a singleton; call
  `webrtcSession.registerChannel(builder)` from your modules to add
  app DataChannels.
- Stores use Pinia. The package re-exports the configured pinia setup
  from `stores/index.ts` (or your app can use its own).

## What it does NOT own

- Camera, video player, RTSP, recording UI — those live in the consuming
  app's browser tree, not here.
- App-specific panels (per-radio settings, chat UIs, …) — they live in
  the consuming straddle's `browser/` subdir.

## Read next

- [INTERNALS.md](INTERNALS.md) — package-scope developer notes.
- [../README.md](../README.md) — the firmware/browser straddle overview.
- [../INTERNALS.md](../INTERNALS.md) — the webrtc DC↔ITS pattern.

## See also

- [README-old.md](README-old.md) — pre-split consumer guide.
- [CLAUDE.md](CLAUDE.md) — pre-split package-scope notes.
