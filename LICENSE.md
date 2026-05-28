# License

This repository, **spangap-web** (HTTPS + auth + WebRTC + shared browser UI
shell for spangap device apps), is released under the **Apache License,
Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by spangap project contributors.

## Third-party software

### Vendored in this repository

None.

### Firmware build-time dependencies

Declared in `esp-idf/idf_component.yml`:

| Component | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |

### Browser build-time dependencies

Declared in `browser/package.json`. The consumer app-straddle's Quasar build
bundles these into the served SPA:

| Package | License |
|---|---|
| `vue` (peer)                     | MIT |
| `quasar` (peer)                  | MIT |
| `pinia` (peer)                   | MIT |
| `vue-router` (peer)              | MIT |
| `@xterm/xterm`, `@xterm/addon-fit` | MIT |
| `register-service-worker`        | MIT |
| `workbox-precaching`, `workbox-routing`, `workbox-strategies` | MIT |

Other transitive npm dependencies retain their upstream licenses; see the
consumer's `package-lock.json` for the full resolved tree.
