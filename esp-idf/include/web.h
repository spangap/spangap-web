#ifndef SECCAM_WEB_H
#define SECCAM_WEB_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

void webInit();

/** Append a URL→filesystem mapping to s.web.map if no entry with this URL
 *  is already present. Used by modules to register their own paths during
 *  init without clobbering user customisations. webInit installs the base
 *  mappings ("/", "/state", "/fixed", and "/sdcard" if CONFIG_DIPTYCH_SDCARD)
 *  this way; ACME adds "/.well-known"; consumers can add app-specific
 *  prefixes from their config_defaults_cb or module init. */
void webMapAddIfAbsent(const char* url, const char* files,
                       int index_dirs, int dav, const char* auth);

/* ---- Web's ITS ports ---- */

/** Web's server ports — connections forwarded by net for plain HTTP / TLS. */
static constexpr uint16_t WEB_HTTP_PORT  = 80;
static constexpr uint16_t WEB_HTTPS_PORT = 443;

/** Web's aux port: tasks register URL prefixes here. */
static constexpr uint16_t WEB_PATH_REG_PORT = 0;

/* ---- ITS aux message: task → web URL registration ---- */

/** Tasks send this to "web" on WEB_PATH_REG_PORT via itsSendAux to register
 *  a URL prefix. When an HTTP request matches, web forwards the ITS
 *  connection (via itsServerForward) to the task. Task handles HTTP/WS
 *  itself. */
typedef struct {
    uint16_t itsPort;     /* ITS port number (passed to onConnect on forward) */
    char path[16];        /* URL prefix (e.g. "rtsp", "log", "cli") */
} web_path_msg_t;

/* Web forwards use net_connect_t (from net.h) with ws=1. */

/* ---- HTTP/WebSocket convenience functions for tasks ---- */

/** Read HTTP request headers from ITS handle (the injected request).
 *  Reads until \r\n\r\n or maxLen. Returns total bytes read (which may
 *  include body bytes that arrived in the same TCP segment as the headers
 *  — those are still in the buffer past the \r\n\r\n boundary; pass this
 *  return value to webReadBody as `total`). Returns -1 on error. */
int webGetHeader(int itsHandle, char* buf, int maxLen, int timeoutMs = 500);

/** Read a Content-Length-bounded HTTP request body into a freshly malloc'd
 *  PSRAM buffer (NUL-terminated). `hdr` is the buffer webGetHeader filled in;
 *  `total` is its return value. Body bytes that came along with the headers
 *  in the same TCP segment are copied out of `hdr`; remainder is read via
 *  itsRecv. Returns nullptr if Content-Length is missing, zero, exceeds
 *  `maxLen`, or alloc fails. Writes actual length to *outLen if non-null.
 *  Caller must heap_caps_free() the returned buffer. */
char* webReadBody(int itsHandle, const char* hdr, int total,
                  size_t maxLen, int* outLen = nullptr);

/** Extract a header field value from raw HTTP headers.
 *  Returns true if found, copies value (trimmed) to out. */
bool webHeaderField(const char* headers, int headersLen,
                    const char* field, char* out, int outLen);

/** Extract request method from HTTP request line. e.g. "GET" */
bool webGetMethod(const char* hdr, int hdrLen, char* out, int outLen);

/** Extract request path from HTTP request line (no leading /, no query string).
 *  "GET /rtsp/stream?q=1 HTTP/1.1" → "rtsp/stream" */
bool webGetPath(const char* hdr, int hdrLen, char* out, int outLen);

/** Extract a query parameter value from the request line.
 *  "GET /path?key=value&foo=bar" with key="foo" → "bar" */
bool webGetQuery(const char* hdr, int hdrLen,
                 const char* key, char* out, int outLen);

/** Send a complete HTTP response (status + content-type + body). */
bool webSendResponse(int itsHandle, int status, const char* contentType,
                     const void* body, size_t bodyLen);

/** Send a minimal HTTP response (status only, no body). */
bool webSendStatus(int itsHandle, int status);

/** Extract a named cookie value from HTTP headers' Cookie field.
 *  e.g., webExtractCookie(hdr, len, "session", out, outLen) extracts
 *  the value of "session=..." from "Cookie: ...; session=abc; ...".
 *  Returns true if found. */
bool webExtractCookie(const char* headers, int headersLen,
                      const char* name, char* out, int outLen);

/* ---- WebSocket ---- */

/** WS upgrade from pre-read headers. Task calls webGetHeader first,
 *  inspects URL/headers as needed, then calls this.
 *  Checks for "Upgrade: websocket", extracts key, sends 101. */
bool wsUpgrade(int itsHandle, const char* hdr, int hdrLen);

/** Convenience: reads headers internally, does WS upgrade.
 *  If must=true: returns false on non-WS (headers consumed).
 *  If must=false: returns false on non-WS, injects headers back for task. */
bool wsUpgrade(int itsHandle, bool must = true, int timeoutMs = 500);

/** Read one WS frame from ITS handle (non-blocking).
 *  Returns: 1=text, 2=binary, -1=close/error, 0=no data.
 *  Payload written to buf, length to *outLen. *binary set if binary frame.
 *  Handles unmasking and ping/pong. */
int wsReadFrame(int itsHandle, uint8_t* buf, size_t bufSize, size_t* outLen,
                bool* binary = nullptr);

/** Send a WS text frame via ITS handle. Returns true on success. */
bool wsSendText(int itsHandle, const char* data, size_t len);

/** Send a WS binary frame via ITS handle. Returns true on success. */
bool wsSendBinary(int itsHandle, const void* data, size_t len);

/** Send a WS close frame (no status code). */
void wsSendClose(int itsHandle);

/** Send a WS close frame with a status code (e.g. 4401 for auth failure). */
void wsSendClose(int itsHandle, uint16_t code);

#endif
