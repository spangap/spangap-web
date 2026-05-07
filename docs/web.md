# Web Server

HTTP/HTTPS server for static files, WebSocket config sync, and RTSP stream proxy. Include `web.h`.

## Usage

```cpp
webInit();  // creates task on core 1, opens http_port (80) + https_port (443)
```

Open `https://seccam.local/` in a browser (or `http://` by IP).

## Endpoints

| Path | Method | Description |
|------|--------|-------------|
| `/` | GET | Serves `index.html` from LittleFS (gzipped) |
| `/<file>` | GET | Serve any file from LittleFS (tries `<file>.gz` first) |
| `/` | WS | Config sync WebSocket (Pinia store ↔ cfg store) |
| `/rtsp` | WS | RTSP stream proxy — web handles TLS + WS framing, proxies raw bytes to rtsp task via stream buffers |

## LittleFS

Static files are stored on the LittleFS partition (1.5MB at 0x670000 in default_8MB partition table). Files are gzipped at build time and served with `Content-Encoding: gzip`.

### Building and flashing

The **LittleFS** `fixed` partition image is built automatically as part of `idf.py build` (see root `CMakeLists.txt`). Web assets are deployed to `data/webroot/` via `web-interface/deploy.sh`. Source tree includes `data/factory_state/` and `data/webroot/`.

```bash
idf.py build                              # builds fixed.bin from data/
idf.py -p /dev/tty.usbmodem2101 flash    # flashes app + partition table + fixed
```

Settings UI behavior for camera/exposure is summarized in [camera-web-ui.md](camera-web-ui.md).

## Architecture

Single task on core 1. Polls for incoming HTTP connections with `select()`. Subscribes to `net.up` ephemeral var via `storageSubscribeChanges` for network state changes — opens server sockets when network comes up, closes them on network down. Blocks on `ulTaskNotifyTake` + `itsPoll` when server socket is closed.

### Endpoint registration via ITS aux

Endpoints (RTSP, log, CLI, storage/config) register via ITS aux messages (`web_path_msg_t` for WS paths, `net_port_msg_t` for TCP ports). Tasks send these during their init to register URL prefixes and TCP listen ports. On a matching HTTP request, the web task injects the HTTP headers back (`itsServerInject`) and forwards the connection to the target task (`itsServerForward`). This centralizes port management and TLS handling in the web/net tasks — consumer tasks never see raw sockets.

## Web Utility API (`web.h`)

HTTP/WS convenience functions for tasks receiving forwarded connections. Tasks include `web.h` and call these directly (not via the web task).

- **`webGetHeader(itsHandle, buf, maxLen, timeoutMs)`** — read HTTP request headers from ITS handle (injected data). Returns header length.
- **`webHeaderField(hdr, len, "Field", out, outLen)`** — extract a header field value.
- **`webGetMethod(hdr, len, out, outLen)`** — extract HTTP method (`GET`, `POST`, etc.).
- **`webGetPath(hdr, len, out, outLen)`** — extract URL path (no leading `/`, no query string).
- **`webGetQuery(hdr, len, "key", out, outLen)`** — extract query parameter value.
- **`webSendResponse(h, status, contentType, body, bodyLen)`** — send full HTTP response.
- **`webSendStatus(h, status)`** — send status-only response (204, 404, etc.).
- **`wsUpgrade(h, hdr, hdrLen)`** — WS upgrade from pre-read headers. Extracts key, sends 101.
- **`wsUpgrade(h, must, timeoutMs)`** — convenience: reads headers internally, does upgrade. `must=false` injects headers back on non-WS.
- **`wsReadFrame(h, buf, size, &len, &binary)`** — read one WS frame (handles unmasking, ping/pong).
- **`wsSendText(h, data, len)`** / **`wsSendBinary(h, data, len)`** — send WS frames.
- **`wsSendClose(h)`** — send WS close frame.

Typical task flow for a forwarded WS connection:
```c
// In onConnect (runs on task's own context via ITS_MSG_FORWARD):
if (connectData->ws) {
    wsUpgrade(handle);  // reads HTTP, sends 101
    // From here: wsReadFrame() for input, wsSendText() for output
}
```
