# diptych-browser

Browser-side runtime + UI for [diptych](../) device apps: WebRTC session manager, settings UI components, FloatingWindow / log viewer / CLI terminal, default config panels (network, system, advanced, ACME, DuckDNS, UPnP, WireGuard, …), auth + session flow, login/setup pages.

Half of the dual-side platform; pairs with [diptych-core](../core).

## Install

```bash
npm install diptych-browser
```

Or for local development against a sibling diptych checkout:

```jsonc
// package.json
"dependencies": {
  "diptych-browser": "file:../path/to/diptych/browser"
}
```

`diptych-browser` ships TypeScript source (no `dist`); your bundler (Vite via Quasar) consumes `.vue` and `.ts` directly.

## Use

The package exposes its API via subpath exports — import only what you need.

```typescript
// stores
import { useDeviceStore, useLogStore, menuRegistry } from 'diptych-browser';

// lib
import { webrtcSession } from 'diptych-browser/lib/webrtc-session';
import { signIn } from 'diptych-browser/lib/auth';

// components
import SettingToggle from 'diptych-browser/components/SettingToggle';
import FloatingWindow from 'diptych-browser/components/FloatingWindow';

// default panels (register them in your boot file to get them in the menu)
import 'diptych-browser/modules/network';
import 'diptych-browser/modules/system';
import 'diptych-browser/modules/advanced';
import 'diptych-browser/modules/editor';

// pages — wire into your app's router
import LoginPage from 'diptych-browser/pages/LoginPage';
import SetupPage from 'diptych-browser/pages/SetupPage';
```

Modules in `diptych-browser/modules/*` self-register with `menuRegistry` on import — your app just needs to import them once (typically from a Quasar boot file).

## Conventions

- Settings components (`SettingToggle`, `SettingSlider`, `SettingSelect`, `SettingText`) are config-bound — pass a config-key path and they handle the storage subscription, optimistic update, and rollback.
- `useDeviceStore()` exposes the live device state (uptime, free heap, build id, etc.) — it's populated by the storage WebSocket.
- `webrtcSession` is a singleton; call `webrtcSession.registerChannel(builder)` from your modules to add app DataChannels.
- Stores use Pinia. The package re-exports the configured pinia setup from `stores/index.ts` (or your app can use its own).

## Development

This package is part of the diptych monorepo. See [`../CLAUDE.md`](../CLAUDE.md) for cross-cutting context, and [`CLAUDE.md`](CLAUDE.md) for browser-package scope.
