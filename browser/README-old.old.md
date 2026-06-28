# spangap-browser

Browser-side runtime + UI for [spangap](../) device apps: WebRTC session manager, settings UI components, FloatingWindow / log viewer / CLI terminal, default config panels (network, system, advanced, ACME, DuckDNS, UPnP, WireGuard, …), auth + session flow, login/setup pages.

Half of the dual-side platform; pairs with [spangap-core](../core).

## Install

```bash
npm install spangap-browser
```

Or for local development against a sibling spangap checkout:

```jsonc
// package.json
"dependencies": {
  "spangap-browser": "file:../path/to/spangap/browser"
}
```

`spangap-browser` ships TypeScript source (no `dist`); your bundler (Vite via Quasar) consumes `.vue` and `.ts` directly.

## Use

The package exposes its API via subpath exports — import only what you need.

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

Modules in `spangap-browser/modules/*` self-register with `menuRegistry` on import — your app just needs to import them once (typically from a Quasar boot file).

## Conventions

- Settings components (`SettingToggle`, `SettingSlider`, `SettingSelect`, `SettingText`) are config-bound — pass a config-key path and they handle the storage subscription, optimistic update, and rollback.
- `useDeviceStore()` exposes the live device state (uptime, free heap, build id, etc.) — it's populated by the storage WebSocket.
- `webrtcSession` is a singleton; call `webrtcSession.registerChannel(builder)` from your modules to add app DataChannels.
- Stores use Pinia. The package re-exports the configured pinia setup from `stores/index.ts` (or your app can use its own).

## Development

This package is part of the spangap monorepo. See [`../CLAUDE.md`](../CLAUDE.md) for cross-cutting context, and [`CLAUDE.md`](CLAUDE.md) for browser-package scope.
