# web — the HTTP/HTTPS server

`web` is the device's web server: it serves static files and SPA assets, hosts
WebDAV shares, forwards URL prefixes to other tasks, and enforces cookie-session
auth on protected paths. It runs as a single FreeRTOS task on core 1 plus a
helper file worker, and it never touches a raw socket — [spangap-net](../../spangap-net)
accepts the TCP/TLS connections and hands them up over ITS.

For the maintainer reference (state machine, file worker, WebDAV verb handling,
pitfalls), see [web-internals.md](web-internals.md).

## How it interacts with the other straddles

`net` opens listen ports 80 and 443, terminates TLS on 443, and forwards each
accepted connection to `web`'s ITS server ports `WEB_HTTP_PORT` (80) and
`WEB_HTTPS_PORT` (443). A per-connection `net_connect_t` carries the `tls` flag
and the client address. `web` parses the HTTP request, decides the route, and
either serves it itself or **forwards the live connection** to the owning task —
re-injecting the already-read bytes so the new owner re-parses a fresh-looking
request. After a forward, `web` is out of the data path.

Three routing outcomes per request:

- **Forward to a task** — a task registered a URL prefix (`web_path_msg_t` on
  `WEB_PATH_REG_PORT`). `web` forwards the connection; the task speaks HTTP/WS
  itself using the `web.h` helpers. This is how the WebRTC signaling WS (`/webrtc`),
  log, CLI, and app endpoints attach.
- **In-web callback** — a task registered a prefix with `webRegisterHandler()`;
  the callback runs synchronously on `web`'s task with the parsed buffer. Used for
  short request/response endpoints (the auth JSON API registers `auth` this way).
- **File serving** — the path matches a URL→filesystem mapping; `web` serves the
  file (or a directory listing, or a WebDAV verb) via its file worker.

## Registering a URL prefix

A task that wants to own a URL prefix sends a `web_path_msg_t` to `web` on
`WEB_PATH_REG_PORT`; on a match `web` forwards the ITS connection to it:

```c
web_path_msg_t reg = { .itsPort = MY_PORT };
safeStrncpy(reg.path, "mypath", sizeof(reg.path));
itsSendAux("web", WEB_PATH_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500));
```

The task's `onConnect` for `MY_PORT` then reads headers with `webGetHeader()`,
inspects them with `webGetMethod`/`webGetPath`/`webHeaderField`/`webExtractCookie`,
pulls a POST body with `webReadBody()`, and replies with `webSendResponse()` or
WebSocket frames (`wsUpgrade` then `wsReadFrame`/`wsSendText`/`wsSendBinary`).

For an endpoint too small to warrant its own task, `webRegisterHandler(path, cb)`
runs `cb(handle, hdr, hdrLen)` inline on `web`'s task; the handler must stay
bounded (no streaming, no long blocking) and must not disconnect — `web` drains
and recycles the slot afterward.

## File serving and URL mappings

Files are served through `s.web.map[]` — a list of URL→filesystem mappings. The
longest matching URL prefix wins, and the URL prefixes match the underlying mount
points so a browser address-bar path is the same string as the device path.
`webInit` seeds the base mappings; modules add more with `webMapAddIfAbsent()`
(idempotent — user customisations survive), and ACME adds `/.well-known`.

| URL prefix | Filesystem | Auth | `index` | `dav` | Notes |
|---|---|---|---|---|---|
| `/` | `/fixed/webroot` | — | SPA | — | The Quasar SPA. Tries `<file>.gz`, then `<file>`, falling back to `index.html` for extensionless paths. |
| `/state` | `/state` | admin | yes | yes | Live config and certs for inspection / hand-edit; WebDAV-mountable. |
| `/fixed` | `/fixed` | admin | yes | yes | Read-only LittleFS — webroot, factory/additional state. |
| `/sdcard` | `/sdcard` | admin | yes | yes | Recordings + logs (only when `CONFIG_SPANGAP_SDCARD`). |
| `/.well-known` | `/state/.well-known` | — | no | — | ACME HTTP-01 challenges (added by ACME). |

`index=1` mappings emit an HTML directory listing for directory URLs; `index=0`
("SPA mode") falls back to the mapping's `index.html` for extensionless paths.
`dav=1` mappings additionally accept WebDAV verbs. `auth="admin"` requires a
valid session cookie for the `admin` realm (comma-separated realm lists are
allowed, e.g. `"admin,view"`).

### Content negotiation, gzip, and transforms

Files are gzipped on disk at build time. `web` honours `Accept-Encoding`: a `.gz`
file is sent verbatim (`Content-Encoding: gzip`) only to clients that advertise
gzip; others get it inflated on the fly with the ESP32-S3 ROM `tinfl`
decompressor (zero added flash). `GET` supports `Range` requests (`206 Partial
Content`) for non-gzipped files.

A straddle can register a content transform by file extension, keeping `web`
format-agnostic. The served file's full bytes (decompressed if `.gz` on disk) are
handed to the callback, which returns a response body:

```c
webRegisterFileExt("md,markdown", myMdToHtml);   // → text/html
```

The viewer straddle registers Markdown this way so the device renders `*.md` to
HTML server-side; transformed bodies are always sent uncompressed. A later
registration for an already-claimed extension is ignored.

## WebDAV

`dav=1` mappings implement WebDAV (RFC 4918, class 1 and 2): `OPTIONS`, `PROPFIND`
(Depth 0/1, with a `multistatus` listing), `PUT` (including chunked transfer
encoding), `DELETE`, `MKCOL`, `MOVE`, `COPY`, and `LOCK`/`UNLOCK` (advisory —
locks are acknowledged with a synthetic token, not tracked). macOS Finder marker
files (`.DS_Store`, `._*`, Spotlight/QuickLook markers) are accepted and faked so
a mounted share stays quiet. This makes `/state`, `/fixed`, and `/sdcard`
mountable as network drives for inspection and hand-editing.

## Auth enforcement

The credential primitive — realms, password hashes, session cookies, rate
limiting — lives in [spangap-core auth](../../spangap-core/docs/auth.md), brought
up by `spangapInit()`. `web` is the HTTP face of it:

- **Enforcement on mappings.** When auth is enabled and a mapping carries a
  non-empty `auth` realm list, `web` reads the `session` cookie, resolves the
  realm with `authCheck()`, and requires the cookie's realm to be in the
  mapping's list. A request without a valid realm is redirected to the SPA root
  (the login page) rather than served. Authenticated responses carry an
  `X-Authenticated: <realm>` header, which the browser reads to learn its login
  state.
- **The JSON login API.** `web` registers the `auth` URL prefix (inline handler):
  `POST /auth/login` (`{password, realm?}` → `{result, realm, cookie}`),
  `POST /auth/passwd` (`{realm, old, new}` → `{result}`), and `POST /auth/logout`
  (deletes the session cookie). Result codes match `auth_err_t`
  (`0` OK, `1` no such realm, `2` wrong password, `3` same as other realm,
  `4` rate-limited). Secrets never cross to the browser — passwords are write-only.

### HTTPS redirect and the loopback exemption

With `s.web.https_only=1` (the default), a plain-HTTP request is answered with a
`301` to the `https://` URL (preserving path + query), **except** for two cases:

- a request path matching an `s.web.http_allowed[]` prefix (default
  `/.well-known`, so ACME HTTP-01 challenges work before a cert exists);
- a request from a **loopback** address (127.0.0.1 / ::1).

The loopback exemption is what lets the device fetch its own pages: the LCD
viewer pulls server-rendered Markdown from `127.0.0.1` over plain HTTP, where
there is no self-signed certificate to validate against itself. Loopback requests
also bypass the realm check entirely (they carry no session cookie), so the
on-device viewer can read protected pages. Only the device itself can originate a
loopback request, so this grants nothing to the network.

## Storage surface

Settings live under `s.web.*`. The web server also seeds two net-owned mDNS
entries and reads several net-owned keys (which it does not define — see
[spangap-net](../../spangap-net)).

### Settings (`s.web.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.web.max_connections` | `8` | Concurrent client slots (clamped 2–16). Read once at task start. |
| `s.web.https_only` | `1` | Redirect plain HTTP to HTTPS (subject to the exemptions above). |
| `s.web.http_allowed[]` | `["/.well-known"]` | Path prefixes exempt from the HTTPS redirect. |
| `s.web.map[]` | base mappings (above) | URL→filesystem mappings; each entry has `.url`, `.files`, `.index`, `.dav`, `.auth`. Up to 8. |
| `s.web.mime[]` | built-in table | Extension→MIME-type map; each entry has `.ext` (comma list) and `.type`. Up to 16. |

The default `s.web.mime[]` covers `html`/`htm`, `css`, `js`, `json`, `png`,
`jpg`/`jpeg`, `ico`, `svg`, `txt`/`log`/`csv`, `avi`, and `wav`; unknown
extensions serve as `application/octet-stream`. Changes to any `s.web.*` key are
picked up live (the maps and MIME table reload on change).

### Referenced (net-owned — defined in [spangap-net](../../spangap-net))

| Key | Role here |
|---|---|
| `s.net.http_port` / `s.net.https_port` | The listen ports `web` registers with `net` (defaults 80 / 443). |
| `s.net.webrtc_port` | The WebRTC UDP port; default 4433. See [webrtc.md](webrtc.md). |
| `s.net.mdns.http` / `s.net.mdns.https` | mDNS service advertisements; `web` seeds these defaults (pointing at the port config keys) but `net` owns the mDNS mechanism. |

### Secrets

Passwords and session keys live under `secrets.*`, owned by core auth, and never
cross to the browser. `web` reads the `session` cookie but holds no secret of its
own.

## ITS ports

| Port | Constant | Purpose |
|---|---|---|
| 80 | `WEB_HTTP_PORT` | Plain-HTTP connections forwarded by `net`. |
| 443 | `WEB_HTTPS_PORT` | TLS connections forwarded by `net`. |
| 0 | `WEB_PATH_REG_PORT` | Aux: a task registers a URL prefix to forward (`web_path_msg_t`). |
| 2 | `WEB_PATH_HANDLER_PORT` | Aux: a task registers an in-web callback (`web_handler_msg_t`). |

(`web` also runs an internal `web_file` worker task on its own ITS ports for
serialised file responses — an implementation detail, see
[web-internals.md](web-internals.md).)

## CLI

```
web        list URL→filesystem mappings and registered path routes
```

Run it on-device with `spangap cli "web"`.
