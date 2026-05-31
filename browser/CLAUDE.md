# spangap-browser (package scope)

This is the npm package published as `spangap-browser` (unscoped). It contains only platform-side browser code — settings UI, FloatingWindow, log/CLI windows, WebRTC session manager, auth flow, default config panels, login/setup pages.

For the full spangap platform context (architecture, conventions, recipes, gotchas), see [`../CLAUDE.md`](../CLAUDE.md). For the consumer-facing install/use guide, see [`README.md`](README.md).

## Layout

```
browser/
├── package.json            name: spangap-browser, exports map for subpath imports
├── tsconfig.json
├── src/
│   ├── index.ts            re-exports (lib + stores)
│   ├── lib/                auth, device-url, reconnect, webrtc-session
│   ├── components/         SettingToggle/Slider/Select/Text, SettingsPanel,
│   │                       FloatingWindow, LogWindow, MenuBar, PanelHeading,
│   │                       TerminalWindow, EditorWindow
│   ├── stores/             device, log, menu, index (pinia setup)
│   ├── modules/            advanced, editor, network, system (auto-register on import)
│   ├── panels/             AboutPanel, DeveloperPanel, NetworkPanel,
│   │                       SystemPanel, WifiScanDialog
│   └── pages/              LoginPage, SetupPage
└── docs/
```

## Working in browser/

- The package ships TS source; the consumer's Vite/bundler handles compilation. No tsc dist build.
- Subpath imports: components, panels, pages are `.vue`; modules and lib are `.ts`. Import them per the `exports` map in `package.json`.
- Camera / video player / RTSP / recording UI lives in the consuming app (e.g. seccam), not here.
- Modules in `modules/` self-register with the menu registry on import. Don't put side effects in `lib/` or `components/`.
- Components use Quasar primitives + scoped styles. Global app CSS stays in the consuming app.

## Pairing with spangap-core

The browser package shape mirrors what's exposed over the storage DataChannel and the various WS / HTTP endpoints from spangap-core. When core changes its public protocol (config keys, storage tree shape, DC port assignments), the browser must follow in lockstep. Versions move together until we settle on a real release cadence.
