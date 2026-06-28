# web — internals

Maintainer reference for the HTTP/HTTPS server. The [operator guide](web.md) is
the user-facing view; this file is for changing `web.cpp` and `auth_web.cpp`
without breaking them.

## 1. What this function provides

`web` is two cooperating tasks plus an inline auth face:

- **`web`** (core 1, 8 KB stack) — the server task: a non-blocking state machine
  over up to `s.web.max_connections` client slots, request parsing, routing,
  WebDAV verbs, directory listings, and the HTTP/WS helper API in `web.h`.
- **`web_file`** (core 1, 8 KB DRAM stack) — a single file-response worker that
  serialises file reads, gzip inflate, and extension transforms off the main task.
- **`auth_web.cpp`** — the JSON-over-HTTP face for core auth, registered as an
  in-web URL handler (`authWebInit`, called from `webInit`); no task of its own.

It owns no certificate, no socket, and no credential store — `net` supplies the
TLS connection, core auth supplies the credentials.

## 2. Task and connection model

The `web` task never blocks on the network. Each client slot (`web_handle_t`) is
a small state machine: `HS_READING` → parse → (`HS_RECV_BODY` for PUT) →
`HS_SEND_HDR` → `HS_SEND_BODY`. The main loop sweeps all slots, then sleeps in
`itsPoll` — 10 ms when any slot is active (so send retries make progress),
`portMAX_DELAY` when idle (woken by an ITS notification from `net`). HTTP
keep-alive is preserved: per-connection state (`itsHandle`, `tls`, `clientAddr`)
survives a `handleReset` across request boundaries; a slot times out after
`WEB_KEEPALIVE_MS` (5 s) of inactivity, and `webItsBusy` evicts the oldest slot
to admit a new connection when full.

**The file worker is the only place `web` reads a filesystem.** The main task
hands a `web_file_req_t` (heap pointer) to `web_file` over ITS aux and marks the
slot `sending`; it then defers all further parsing on that handle until the
worker posts a done-aux (`WEB_FILE_DONE_PORT`, carrying the slot index) back to
the `web` task. The worker pulls the whole request — discovery (`path.gz` →
`path` → SPA `index.html` fallback), headers, range, and body streaming — onto
its own DRAM stack. A single worker is deliberate: SD and network bandwidth are
both modest, concurrency bought little, and the per-request DRAM stacks were
exhausting internal heap under load. Backpressure is the worker's inbox depth
(`WEB_FILE_INBOX_DEPTH` = 8); a full inbox fails fast to a `503`.

Generated bodies (directory listings, PROPFIND/LOCK XML) and short responses are
built on the main task into a PSRAM `genBuf` and streamed by `trySendBody`
without the worker.

## 3. Request routing order

`tryParseRequest` runs once a full header block (`\r\n\r\n`) is buffered, in this
order:

1. **HTTPS redirect** — if the connection is plain HTTP, `httpsOnly` is set, the
   client is not loopback, and the path isn't in `httpAllowed`, reply `301` to
   `https://<Host><target>`.
2. **Registered path** (longest-prefix match on a `/` boundary): a WS upgrade or
   HTTP request to a `WEB_PATH_FORWARD` entry injects the whole buffer back and
   `itsServerForward`s to the owner; a `WEB_PATH_HANDLER` entry invokes the
   callback inline then drains the body and keeps the slot for keep-alive.
3. **File mapping** (`findMapping`, longest URL prefix): resolve auth, reject
   path traversal / hidden components, dispatch WebDAV verbs on `dav` mappings,
   then serve `GET`/`HEAD` via the file worker (SPA) or the synchronous path
   (directory-index mappings).

Auth is resolved by `resolveAuth` (cookie → `authCheck` → realm) and enforced in
step 3: a request to an auth-required mapping whose realm isn't in the list (and
isn't loopback) is sent to `serveRootIndex` (the login SPA), not `401`.

## 4. The loopback exemption (two places)

`ip_addr_isloopback(&wh.clientAddr)` is checked twice, and both are load-bearing
for the on-device LCD viewer fetching its own server-rendered pages over plain
HTTP from `127.0.0.1`:

- in the **HTTPS-redirect** guard — loopback is never redirected (it has no cert
  to validate against itself);
- in the **map auth** guard — loopback bypasses realm checking (it carries no
  session cookie).

The client address comes from `net_connect_t` on the forwarded connection. Only
the device can originate a loopback connection, so neither exemption widens the
network attack surface. Don't gate either on a build flag — the viewer relies on
both unconditionally.

## 5. WebDAV

WebDAV verbs are handled synchronously on the main task (rare traffic, and they
touch `fs_*` directory APIs anyway). `OPTIONS` advertises `DAV: 1, 2`. `PROPFIND`
builds a `D:multistatus` from `fs_listdir` (capped at 1024 entries, Depth clamped
to 1). `PUT` streams the body to a file via `HS_RECV_BODY`, handling both
`Content-Length` and chunked transfer-encoding. `MOVE`/`COPY` parse the
`Destination` header through `urlToFsPath` (re-checking traversal). `LOCK`
returns a synthetic exclusive lock token derived from a path hash — locks are
**advisory only**, not tracked, which is enough for Finder/Explorer mounts.
macOS marker files are special-cased (faked `201`/`207`) so Spotlight/QuickLook
probes don't spew 404s.

## 6. The `web.h` helper API

Tasks that receive forwarded connections include `web.h` and call these directly
(not via the `web` task) on their own context:

- `webGetHeader` reads until `\r\n\r\n`; its return value includes body bytes that
  arrived in the same TCP segment — pass it to `webReadBody` as `total`.
- `webReadBody` reassembles a `Content-Length`-bounded body into a PSRAM buffer
  (caller frees), copying same-segment body bytes first then `itsRecv`ing the rest.
- `webGetMethod` / `webGetPath` / `webGetQuery` / `webHeaderField` /
  `webExtractCookie` parse the request (field-name matching is case-insensitive —
  a reverse proxy may lowercase header names).
- `webSendResponse` / `webSendStatus` write a complete response.
- `wsUpgrade` (two overloads) does the RFC 6455 `Sec-WebSocket-Accept` handshake;
  `wsReadFrame` (handles unmasking, ping/pong, close), `wsSendText`/`wsSendBinary`/
  `wsSendClose` carry frames. `wsReadFrame` rejects 64-bit length frames.

## 7. The auth HTTP face (`auth_web.cpp`)

`authWebInit` registers the `auth` prefix as an in-web handler. `authUrlHandler`
dispatches `auth/login`, `auth/passwd`, `auth/logout` to `authLogin`/`authPasswd`/
`authLogout` in core auth and writes JSON results. `auth/passwd("admin","","")`
doubles as the "is admin password unset?" onboarding probe (returns `AUTH_OK`
when unset). Logout deletes the cookie server-side; cross-connection eviction
from the original design is intentionally dropped — the deleted cookie fails
`authCheck` on any later request, which is sufficient.

## 8. Pitfalls

- **`itsPoll` sleep must be ≥ 10 ms when active.** `pdMS_TO_TICKS(3)` rounds to 0
  at the 100 Hz tick rate and busy-spins the task. Keep the active-poll at 10 ms.
- **Large buffers go in PSRAM.** Directory-index and PROPFIND XML, gzip output,
  and whole-file reads use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`; the
  `web_map_t`/`web_mime_t` tables are `PSRAM_BSS`. Internal DRAM is scarce.
- **Don't reuse a slot index while the file worker holds it.** `webItsDisconnect`
  on a slot with `sending=true` only sets `wantsCleanup`; the real release waits
  for the worker's done-aux, so the slot index isn't recycled mid-stream
  (otherwise the worker would write into a new client's connection).
- **Inject the whole receive buffer on forward, not just the consumed headers.**
  A POST body that arrived in the same TCP segment as the headers sits past the
  `\r\n\r\n` boundary; forwarding only `consumed` bytes silently drops it.
- **`web_file` stack is 8 KB, not 5.** The worker runs extension transforms (e.g.
  the viewer's MD4C Markdown→HTML) and gzip inflate inline; MD4C wants the
  headroom even though the decompressor state and large buffers are on the heap.
- **The config-version gate (`s.web.version`) is legacy.** It guards the one-time
  `storageDefaultTree` seed of MIME types; it is not a feature and there is no
  migration policy behind it.
