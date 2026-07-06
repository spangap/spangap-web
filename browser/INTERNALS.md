# spangap-browser — internals

Package-scope notes for working in the npm half. The shell model (Dock, menu
store, generated panels, the WebRTC session) is documented at
[../docs/browser-shell.md](../docs/browser-shell.md) and
[../docs/browser-shell-internals.md](../docs/browser-shell-internals.md); this
file is about the package itself.

## Layout

```
browser/
├── package.json
├── tsconfig.json
└── src/
    ├── index.ts            re-exports lib/* + stores/{device,log,menu}
    ├── lib/                apps, generatedPanels, auth, device-url, reconnect,
    │                       viewport, windows, webrtc-session
    ├── components/         SettingToggle/Slider/Select/Text, PanelHeading,
    │                       GeneratedPanel, GeneratedListRow, Dock, SettingsWindow,
    │                       SettingsNavTree, FloatingWindow, LogWindow,
    │                       TerminalWindow, EditorWindow, ConnectionOverlay, UsableArea
    ├── stores/             device, log, menu, index
    ├── modules/            advanced, editor, system
    └── pages/              LoginPage, SetupPage
```

## Working in this subdir

- The package ships TS/Vue source; the consumer's Vite/Quasar bundler compiles it.
  No `tsc` dist build.
- Subpath imports follow the `exports` map in `package.json`: `lib/*` and
  `stores/*` are `.ts`; `components/*`, `panels/*`, `pages/*` are `.vue`. The
  package root (`index.ts`) re-exports the `lib/*` modules and the three stores.
- **Side effects live only in `modules/`.** A `modules/*.ts` self-registers with
  the menu store on import; keep `lib/` and `components/` side-effect free so the
  activator's import order stays predictable.
- Components use Quasar primitives + scoped styles; global app CSS stays in the
  consuming app.
- Camera / video player / RTSP / recording UI live in the consuming app, not here.

## Shell shape (Dock, Settings-as-window, no docking)

The full shell narrative lives in the `../docs/browser-shell*.md` files; the
package-structural invariants that matter when editing components here:

- **No menu-bar / hamburger / drawer chrome.** `MenuBar.vue` and the settings
  q-drawer (`SettingsPanel.vue`) were deleted. The shell is the bottom
  `Dock.vue` plus a set of windows. The Dock is a centered floating bar on
  desktop and an iOS-style bottom nav on a phone (first four apps + a "More"
  overflow, gated on `useCompact()` from `lib/viewport.ts`); it renders one icon
  per entry in the app registry (`lib/apps.ts` — `registerApp` / `sortedApps`),
  each straddle registering its window from the same `register*` module that
  wires its menu items. Clicking an icon calls the entry's `open()`; `isOpen()`
  drives the running-app indicator.
- **Settings is an app window**, not a drawer: `SettingsWindow.vue` +
  recursive `SettingsNavTree.vue` (the gear Dock icon) host the same
  `settings/…` menu tree (nav rail + panel pane on desktop, drill-down on
  phone), and generated settings panes mount inside it. The menu store stays for
  that settings hierarchy; only the old `window/*` menu actions went away.
- **No window docking.** `FloatingWindow.vue` is pure floating on desktop and
  full-screen on a phone. The dock store (`docks` / `dockOrder` / `dockWindow` /
  `layout`, and per-window `canDock` / `defaultDock`) is gone from
  `modules/advanced.ts`; legacy dock/dockSize fields left in persisted window
  state are ignored, with no migration.
- **App icons are inlined, not served.** The launcher SVGs are the single icon
  source shared with the LCD tiles: the activator globs each straddle's
  `src/app-icons/*.svg` as raw strings and calls `registerAppIcons`, and the
  Dock injects the matching SVG inline via `v-html` (`appIconSvg(icon)`),
  falling back to a letter badge (`label.charAt(0)`) when none is bundled
  (nomad / announces have no SVG). The device webroot ships only `app.js`, so no
  `/app-icons/*.svg` asset ever reaches the device — bundling the SVG source is
  what makes an icon appear.

## peerDependencies

Listed as peer deps so the consuming app provides one copy: `vue` (3.x),
`quasar` + `@quasar/extras`, `pinia`, `vue-router`. The package depends directly
on `@xterm/xterm` + `@xterm/addon-fit` (the terminal in `TerminalWindow.vue`) and
the workbox / service-worker runtime helpers. The consuming app builds them all
into one bundle with its own Quasar/Vite pipeline; the built SPA is served
read-only from `/fixed/webroot/` on the device.

## Pairing with the firmware half

The package shape mirrors what the firmware exposes over the storage DataChannel
and the WS / HTTP endpoints. When firmware changes its public protocol (config
keys, storage tree shape, DC port assignments), the browser follows in lockstep;
the two versions move together.

## Activator integration

When `spangap-web` is in the firmware build graph (and `--no-web-ui` is not set),
the build generates a dispatcher (`straddles.gen.ts`) that walks every consumed
straddle's `browser/` subdir, imports its `register*` module (self-registering its
apps and menu items), registers its declarative settings panels via
`registerGeneratedPanels`, and bundles its app icons via `registerAppIcons`. This
package's `modules/` are picked up the same way — there is no hard-coded list.

## Adding panels

- A **generated** pane: add a `settings:` block to the owning straddle.yaml; the
  build lowers it to a `GenPanel` descriptor and `GeneratedPanel.vue` renders it.
- A **hand-written** pane: place `MyPanel.vue` in the owning straddle's
  `browser/src/panels/` and register it from that straddle's `register*` module:

```typescript
import { useMenuStore } from 'spangap-browser'
useMenuStore().register('settings/my-group/my-panel', 'My Panel',
  { type: 'panel', component: () => import('../panels/MyPanel.vue') })
```

App-only panes that belong to the consuming app (not a straddle) live in that
app's tree, registered the same way.
