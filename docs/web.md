# Web Server

HTTP server for static file serving from SPIFFS. Include `web.h`.

## Usage

```cpp
webInit();  // creates task on core 1, opens HTTP port from NVS web_port (default 80)
```

Open `http://seccam.local/` in a browser (or by IP).

## Endpoints

| Path | Method | Description |
|------|--------|-------------|
| `/` | GET | Serves `index.html` from SPIFFS (gzipped) |
| `/<file>` | GET | Serve any file from SPIFFS (tries `<file>.gz` first) |

## SPIFFS

Static files are stored on the SPIFFS partition (1.5MB at 0x670000 in default_8MB partition table). Files are gzipped at build time and served with `Content-Encoding: gzip`.

### Building and flashing

SPIFFS image is built automatically as part of the normal build. Source files go in `data/`.

```bash
idf.py build                              # builds spiffs.bin from data/
idf.py -p /dev/tty.usbmodem2101 flash    # flashes everything including spiffs
```

## Architecture

Single task on core 1 (stack 4096, queue depth 4). Polls for incoming HTTP connections with `select()`. Reacts to `MSG_NVS_CHANGED`/`MSG_NETWORK_IS_UP` for port updates and `MSG_NETWORK_DOWN` to close server socket. Blocks indefinitely on IPC queue when server socket is closed.
