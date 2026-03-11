# Web Server

Browser-accessible MJPEG video + WebSocket audio. Include `web.h`.

## Usage

```cpp
webCreate();  // creates task on core 1, starts HTTP server on port 80
```

Open `http://<ip>/` in a browser. Click anywhere to enable audio (browser autoplay policy).

## Endpoints

| Path | Method | Description |
|------|--------|-------------|
| `/` | GET | Serves `index.html` from SPIFFS (gzipped) |
| `/stream` | GET | Multipart MJPEG video stream |
| `/ws` | GET | WebSocket upgrade → binary u-law 8kHz audio |
| `/<file>` | GET | Serve any file from SPIFFS (tries `<file>.gz` first) |

## SPIFFS

Static files are stored on the SPIFFS partition (1.5MB at 0x670000 in default_8MB partition table). Files are gzipped at build time and served with `Content-Encoding: gzip`.

### Building and flashing

```bash
./build_spiffs.sh    # gzips seccam/spiffs/* → build_spiffs/spiffs.bin
./flash_spiffs.sh    # writes spiffs.bin to flash at 0x670000
```

Source files go in `seccam/spiffs/`. Currently just `index.html`.

## Video (MJPEG)

The `/stream` endpoint sends `multipart/x-mixed-replace` with JPEG frames from the camera subscriber. Each frame is a multipart part with `Content-Type: image/jpeg` and `Content-Length`.

Subscribes to camera on first `/stream` request, unsubscribes when the client disconnects.

## Audio (WebSocket)

The `/ws` endpoint upgrades to WebSocket (RFC 6455) and sends u-law 8kHz audio as binary frames.

- Each WebSocket message is one 20ms audio chunk (~160 bytes at 8kHz u-law)
- Audio processed via audio module's built-in pipeline: HPF enabled, gain=3, codec=AUDIO_ULAW_8K
- Browser decodes u-law in JS using a 256-entry lookup table

Subscribes to audio on WebSocket connect, unsubscribes on disconnect.

### Browser playback

The `index.html` page uses `AudioContext` at 8kHz sample rate. Incoming u-law binary messages are decoded to Float32 PCM and scheduled via `AudioBufferSourceNode` with seamless timing.

## Architecture

Single task manages two client slots:
- `mjpegClient` — long-lived HTTP response for video
- `wsClient` — WebSocket connection for audio

Both use the same task's camera and audio subscriber slots. The task subscribes when a client connects and unsubscribes when both clients are gone.

Task loop: 1ms IPC timeout, dispatch camera/audio messages, poll for new connections and send data to active clients.

## Simultaneous use

The web server runs alongside RTSP on separate ports (80 vs 554). Each has its own camera and audio subscriber slots. Both can stream simultaneously if subscriber limits allow (CAM_MAX_SUBSCRIBERS=2, AUDIO_MAX_SUBSCRIBERS=3).
