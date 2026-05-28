# spangap-browser — internals

## Layout

```
browser/
├── package.json
├── tsconfig.json
└── src/
    ├── index.ts            re-exports (lib + stores)
    ├── lib/                auth, device-url, reconnect, webrtc-session
    ├── components/         SettingToggle/Slider/Select/Text, SettingsPanel,
    │                       FloatingWindow, LogWindow, MenuBar, PanelHeading,
    │                       TerminalWindow, EditorWindow
    ├── stores/             device, log, menu, index
    ├── modules/            advanced, editor, network, system
    ├── panels/             AboutPanel, DeveloperPanel, NetworkPanel,
    │                       SystemPanel, WifiScanDialog
    └── pages/              LoginPage, SetupPage
```

## Working in this subdir

- The package ships TS source; the consumer's Vite/bundler handles
  compilation. No `tsc` dist build.
- Subpath imports: components, panels, pages are `.vue`; modules and
  lib are `.ts`. Import them per the `exports` map in `package.json`.
- Camera, video player, RTSP, recording UI live in the consuming app
  (e.g. seccam), not here.
- Modules in `modules/` self-register with the menu registry **on
  import**. Don't put side effects in `lib/` or `components/`.
- Components use Quasar primitives + scoped styles. Global app CSS
  stays in the consuming app.

## Pairing with `spangap-web` firmware

The browser package shape mirrors what's exposed over the storage
DataChannel and the various WS / HTTP endpoints from `spangap-web` and
the other firmware straddles. When firmware changes its public
protocol (config keys, storage tree shape, DC port assignments), the
browser must follow in lockstep. Versions move together until we settle
on a real release cadence.

## peerDependencies

Listed in `package.json` as peer deps so the consuming app provides
one copy:

- `vue` (3.x)
- `quasar` + `@quasar/extras`
- `pinia`
- `vue-router`
- `@xterm/xterm` (terminal in `TerminalWindow.vue`)

The consuming app builds them all into one bundle with its own Quasar/
Vite pipeline. The built SPA is served read-only from
`/fixed/webroot/` on the device.

## Activator integration

When `spangap-web` is in the firmware build graph (and `--no-web-ui` is
not set), the build CLI generates a browser dispatcher that:

1. Walks every consumed straddle's `browser/` subdir.
2. Imports each `modules/<prefix>.ts` so they self-register.
3. Wires their panels and pages into the menu and router.

This package's `modules/` are picked up the same way — there is no
hard-coded list anywhere.

## Conventions for adding panels

Place app-specific panels in the **consuming app**'s tree (e.g.
`reticulous-tdeck/web-interface/src/`) when they are app-only. Place
straddle-owned panels in *that straddle's* `browser/src/panels/` and
register them from `browser/src/modules/<prefix>.ts`.

A self-registering module looks like:

```typescript
// browser/src/modules/<prefix>.ts
import { menuRegistry } from 'spangap-browser';

menuRegistry.register({
  group: 'My Group',
  id: 'my-panel',
  label: 'My Panel',
  component: () => import('../panels/MyPanel.vue'),
});
```
