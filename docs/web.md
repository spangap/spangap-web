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
| `/stream` | WS | RTSP stream proxy — web handles TLS + WS framing, proxies raw bytes to rtsp task via stream buffers |

## LittleFS

Static files are stored on the LittleFS partition (1.5MB at 0x670000 in default_8MB partition table). Files are gzipped at build time and served with `Content-Encoding: gzip`.

### Building and flashing

SPIFFS image is built automatically as part of the normal build. Source files go in `data/`.

```bash
idf.py build                              # builds spiffs.bin from data/
idf.py -p /dev/tty.usbmodem2101 flash    # flashes everything including spiffs
```

## Architecture

Single task on core 1 (stack 4096, queue depth 4). Polls for incoming HTTP connections with `select()`. Reacts to `MSG_NVS_CHANGED`/`MSG_NETWORK_IS_UP` for port updates and `MSG_NETWORK_DOWN` to close server socket. Blocks indefinitely on IPC queue when server socket is closed.
