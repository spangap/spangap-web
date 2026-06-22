/**
 * web — HTTP server for static file serving + URL routing.
 * ITS server: receives connections from network task (TCP call center).
 * Routes registered URL paths to target tasks via itsServerForward.
 * Serves files from configurable URL→filesystem mappings (s.web.map.*).
 * Runs on core 1.
 */
#include "web.h"
#include "mem.h"
#include "auth.h"
#include "storage.h"
#include "log.h"
#include "cli.h"
#include "its.h"
#include "net.h"
#include "compat.h"
#include "fs.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include "miniz.h"        /* ESP32-S3 ROM DEFLATE (tinfl_*) — zero added flash */
#include "lwip/ip_addr.h" /* ip_addr_isloopback — loopback request exemptions */

#define WEB_KEEPALIVE_MS   5000
#define WEB_ITS_TO_SIZE    4096   /* client→web (HTTP requests from browser) */
#define WEB_ITS_FROM_SIZE  16384  /* web→client (responses, forwarded streams) */

static TaskHandle_t webHandle = NULL;

/* ---- Configurable URL→filesystem mappings (s.web.map.*) ---- */

#define WEB_MAX_MAPS  8
#define WEB_MAX_MIMES 16

struct web_map_t {
    std::string url;      /* URL prefix, e.g. "/" or "/sdcard" */
    std::string files;    /* filesystem root, e.g. "/fixed/webroot" or "/sdcard" */
    bool dirIndex;        /* true = directory listings; false = SPA mode */
    bool dav;             /* true = enable WebDAV methods */
    std::string auth;     /* comma-separated realm list, empty = no auth required */
};

struct web_mime_t {
    std::string exts;     /* comma-separated extensions, e.g. "html,htm" */
    std::string type;     /* MIME type, e.g. "text/html" */
};

static web_map_t webMaps[WEB_MAX_MAPS];
static int webMapCount = 0;
static web_mime_t webMimes[WEB_MAX_MIMES];
static int webMimeCount = 0;

/* File-extension transforms (e.g. .md → text/html). Registered once at init by
 * the owning straddle (viewer registers markdown), read-only afterwards on the
 * file worker — no locking. */
#define WEB_MAX_EXT_HANDLERS 8
struct web_ext_handler_t {
    std::string          exts;   /* comma-separated, no dots, lower-case */
    web_file_transform_t cb;
};
static web_ext_handler_t webExtHandlers[WEB_MAX_EXT_HANDLERS];
static int webExtHandlerCount = 0;

/* HTTPS redirect config */
static bool httpsOnly = false;
#define WEB_MAX_HTTP_ALLOWED 8
static std::string httpAllowed[WEB_MAX_HTTP_ALLOWED];
static int httpAllowedCount = 0;

static void loadMappings() {
    webMapCount = 0;
    int n = storageArrayCount("s.web.map");
    for (int i = 0; i < n && i < WEB_MAX_MAPS; i++) {
        char key[48], url[32], files[64];
        snprintf(key, sizeof(key), "s.web.map.%d.url", i);
        storageGetStr(key, url, sizeof(url));
        snprintf(key, sizeof(key), "s.web.map.%d.files", i);
        storageGetStr(key, files, sizeof(files));
        snprintf(key, sizeof(key), "s.web.map.%d.index", i);
        int idx = storageGetInt(key, 0);
        snprintf(key, sizeof(key), "s.web.map.%d.dav", i);
        int dav = storageGetInt(key, 0);
        char auth[64] = {};
        snprintf(key, sizeof(key), "s.web.map.%d.auth", i);
        storageGetStr(key, auth, sizeof(auth));
        if (!url[0] || !files[0]) continue;
        webMaps[webMapCount++] = { url, files, idx != 0, dav != 0, auth };
    }
    httpsOnly = storageGetInt("s.web.https_only", 1) != 0;
    httpAllowedCount = 0;
    int ha = storageArrayCount("s.web.http_allowed");
    for (int i = 0; i < ha && i < WEB_MAX_HTTP_ALLOWED; i++) {
        char key[48], val[64];
        snprintf(key, sizeof(key), "s.web.http_allowed.%d", i);
        storageGetStr(key, val, sizeof(val));
        if (!val[0]) break;
        httpAllowed[httpAllowedCount++] = val;
    }
}

static void loadMimeTypes() {
    webMimeCount = 0;
    int n = storageArrayCount("s.web.mime");
    for (int i = 0; i < n && i < WEB_MAX_MIMES; i++) {
        char key[48], exts[64], type[64];
        snprintf(key, sizeof(key), "s.web.mime.%d.ext", i);
        storageGetStr(key, exts, sizeof(exts));
        snprintf(key, sizeof(key), "s.web.mime.%d.type", i);
        storageGetStr(key, type, sizeof(type));
        if (!exts[0] || !type[0]) continue;
        webMimes[webMimeCount++] = { exts, type };
    }
}

/* ---- MIME type lookup (config-driven, s.web.mime.*) ---- */

static const char* mimeType(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    const char* ext = dot + 1;
    size_t extLen = strlen(ext);
    for (int i = 0; i < webMimeCount; i++) {
        const char* p = webMimes[i].exts.c_str();
        while (*p) {
            const char* comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            if (extLen == len && strncasecmp(p, ext, len) == 0)
                return webMimes[i].type.c_str();
            if (!comma) break;
            p = comma + 1;
        }
    }
    return "application/octet-stream";
}

/* ---- File-extension transforms ---- */

/* True if `ext` (length extLen, no dot) appears in a comma-list "a,b,c". */
static bool extInList(const char* ext, size_t extLen, const char* list) {
    const char* p = list;
    while (*p) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (extLen == len && strncasecmp(p, ext, len) == 0) return true;
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

/* Resolve a path's extension to a registered transform, or nullptr. */
static web_file_transform_t findExtTransform(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return nullptr;
    const char* ext = dot + 1;
    size_t extLen = strlen(ext);
    for (int i = 0; i < webExtHandlerCount; i++)
        if (extInList(ext, extLen, webExtHandlers[i].exts.c_str()))
            return webExtHandlers[i].cb;
    return nullptr;
}

void webRegisterFileExt(const char* exts, web_file_transform_t cb) {
    if (!exts || !*exts || !cb) return;
    if (webExtHandlerCount >= WEB_MAX_EXT_HANDLERS) {
        warn("ext-handler table full, dropping '%s'\n", exts);
        return;
    }
    /* Lower-case the list once so lookups are a plain length+strncasecmp. */
    std::string lc(exts);
    for (char& c : lc) c = (char)tolower((unsigned char)c);
    webExtHandlers[webExtHandlerCount++] = { lc, cb };
    info("ext-handler registered for '%s'\n", lc.c_str());
}

/* ---- gzip inflate (ROM tinfl) ----
 * Inflate a .gz blob into a freshly heap_caps(SPIRAM) buffer. The ~11 KB tinfl
 * decompressor state lives on the heap (not the stack) and we feed a single
 * non-wrapping output buffer sized from the gzip ISIZE trailer — so this runs
 * with a tiny stack and no 32 KB stack dictionary. Returns the buffer (caller
 * heap_caps_free()s) and sets *outLen, or nullptr on malformed input / cap. */
static uint8_t* gzInflate(const uint8_t* gz, size_t gzLen, size_t cap, size_t* outLen) {
    if (gzLen < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8) return nullptr;
    uint8_t flg = gz[3];
    size_t pos = 10;                                   /* fixed gzip header */
    if (flg & 0x04) {                                  /* FEXTRA */
        if (pos + 2 > gzLen) return nullptr;
        size_t xlen = gz[pos] | (gz[pos + 1] << 8);
        pos += 2 + xlen;
    }
    if (flg & 0x08) while (pos < gzLen && gz[pos++]) {} /* FNAME (NUL-term) */
    if (flg & 0x10) while (pos < gzLen && gz[pos++]) {} /* FCOMMENT */
    if (flg & 0x02) pos += 2;                           /* FHCRC */
    if (pos + 8 > gzLen) return nullptr;

    /* ISIZE: original size mod 2^32, little-endian, in the last 4 bytes. */
    size_t isize = (size_t)gz[gzLen - 4] | ((size_t)gz[gzLen - 3] << 8) |
                   ((size_t)gz[gzLen - 2] << 16) | ((size_t)gz[gzLen - 1] << 24);
    if (isize == 0 || isize > cap) return nullptr;

    uint8_t* out = (uint8_t*)heap_caps_malloc(isize, MALLOC_CAP_SPIRAM);
    if (!out) return nullptr;
    auto* d = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    if (!d) { heap_caps_free(out); return nullptr; }
    tinfl_init(d);

    size_t inN = gzLen - 8 - pos;                       /* raw DEFLATE length */
    size_t outN = isize;
    tinfl_status st = tinfl_decompress(d, gz + pos, &inN, out, out, &outN,
                                       TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    heap_caps_free(d);
    if (st != TINFL_STATUS_DONE) { heap_caps_free(out); return nullptr; }
    *outLen = outN;
    return out;
}

/* ---- URL mapping: find best (most specific) match ---- */

static web_map_t* findMapping(const std::string& path) {
    web_map_t* best = nullptr;
    size_t bestLen = 0;
    for (int i = 0; i < webMapCount; i++) {
        auto& m = webMaps[i];
        if (m.url == "/") {
            if (!best) { best = &m; bestLen = 0; }
        } else {
            /* m.url is like "/sdcard"; path is like "sdcard/sub/f.txt" */
            const char* prefix = m.url.c_str() + 1;
            size_t plen = strlen(prefix);
            if (path.length() >= plen &&
                strncmp(path.c_str(), prefix, plen) == 0 &&
                (path.length() == plen || path[plen] == '/')) {
                if (!best || plen > bestLen) { best = &m; bestLen = plen; }
            }
        }
    }
    return best;
}

static std::string getRelPath(const std::string& urlPath, const web_map_t& map) {
    if (map.url == "/") return urlPath;
    size_t skip = map.url.length() - 1;
    if (skip >= urlPath.length()) return "";
    std::string rel = urlPath.substr(skip);
    if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
    return rel;
}

/* ---- Path traversal protection ---- */

static bool hasPathTraversal(const std::string& path) {
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            size_t len = i - start;
            if (len == 2 && path[start] == '.' && path[start + 1] == '.') return true;
            start = i + 1;
        }
    }
    return false;
}

/** Check if any path component starts with '.' (hidden file/dir). */
static bool hasHiddenComponent(const std::string& path) {
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            if (i > start && path[start] == '.') return true;
            start = i + 1;
        }
    }
    return false;
}

/* ---- SPA fallback: only for extensionless paths ---- */

static bool hasExtension(const std::string& path) {
    size_t slash = path.rfind('/');
    size_t dot = path.rfind('.');
    return dot != std::string::npos && (slash == std::string::npos || dot > slash);
}

/* ---- Range header parsing ---- */

static bool parseRange(const char* val, size_t fileSize, size_t* start, size_t* end) {
    if (strncmp(val, "bytes=", 6) != 0) return false;
    const char* p = val + 6;
    if (*p == '-') {
        size_t suffix = strtoul(p + 1, nullptr, 10);
        if (suffix == 0 || suffix > fileSize) return false;
        *start = fileSize - suffix;
        *end = fileSize - 1;
    } else {
        *start = strtoul(p, nullptr, 10);
        const char* dash = strchr(p, '-');
        if (!dash) return false;
        if (dash[1] && dash[1] != '\r')
            *end = strtoul(dash + 1, nullptr, 10);
        else
            *end = fileSize - 1;
    }
    return *start <= *end && *end < fileSize;
}

/* ---- XML helpers for PROPFIND ---- */

static int xmlEscape(char* dst, size_t dstSz, const char* src) {
    int pos = 0;
    for (; *src && pos < (int)dstSz - 6; src++) {
        switch (*src) {
            case '&': pos += snprintf(dst+pos, dstSz-pos, "&amp;"); break;
            case '<': pos += snprintf(dst+pos, dstSz-pos, "&lt;"); break;
            case '>': pos += snprintf(dst+pos, dstSz-pos, "&gt;"); break;
            case '"': pos += snprintf(dst+pos, dstSz-pos, "&quot;"); break;
            default:  dst[pos++] = *src;
        }
    }
    dst[pos] = '\0';
    return pos;
}

static char* generatePropfind(const std::string& urlPath, const char* fsPath,
                               int depth, size_t* outLen) {
    struct stat st;
    if (fs_stat(fsPath, &st) != 0) return nullptr;
    bool isDir = S_ISDIR(st.st_mode);

    struct entry_t { char name[80]; time_t mtime; uint32_t size; bool isDir; };
    constexpr int MAX = 1024;
    auto* entries = (entry_t*)heap_caps_malloc(MAX * sizeof(entry_t), MALLOC_CAP_SPIRAM);
    int childCount = 0;

    if (isDir && depth > 0 && entries) {
        auto* listing = (fs_listing_t*)heap_caps_malloc(MAX * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
        if (listing) {
            int n = fs_listdir(fsPath, listing, MAX);
            for (int i = 0; i < n && childCount < MAX; i++) {
                if (listing[i].name[0] == '.') continue;
                auto& e = entries[childCount++];
                safeStrncpy(e.name, listing[i].name, sizeof(e.name));
                e.mtime = listing[i].mtime;
                e.size  = listing[i].size;
                e.isDir = listing[i].isDir;
            }
            heap_caps_free(listing);
        }
    }

    size_t bufSz = 512 + (1 + childCount) * 500;
    char* buf = (char*)heap_caps_malloc(bufSz, MALLOC_CAP_SPIRAM);
    if (!buf) { free(entries); return nullptr; }

    int pos = 0;
    pos += snprintf(buf+pos, bufSz-pos,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<D:multistatus xmlns:D=\"DAV:\">\n");

    auto addResponse = [&](const char* href, time_t mtime, uint32_t size, bool dir) {
        char esc[256];
        xmlEscape(esc, sizeof(esc), href);
        struct tm tm;
        gmtime_r(&mtime, &tm);
        char date[48];
        strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        pos += snprintf(buf+pos, bufSz-pos,
            "<D:response><D:href>%s</D:href><D:propstat><D:prop>\n", esc);
        if (dir)
            pos += snprintf(buf+pos, bufSz-pos,
                "<D:resourcetype><D:collection/></D:resourcetype>\n");
        else
            pos += snprintf(buf+pos, bufSz-pos,
                "<D:resourcetype/>\n<D:getcontentlength>%u</D:getcontentlength>\n"
                "<D:getcontenttype>%s</D:getcontenttype>\n",
                (unsigned)size, mimeType(href));
        pos += snprintf(buf+pos, bufSz-pos,
            "<D:getlastmodified>%s</D:getlastmodified>\n"
            "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n",
            date);
    };

    std::string selfHref = "/" + urlPath;
    if (isDir && selfHref.back() != '/') selfHref += '/';
    addResponse(selfHref.c_str(), st.st_mtime, (uint32_t)st.st_size, isDir);

    for (int i = 0; i < childCount && pos < (int)bufSz - 512; i++) {
        auto& e = entries[i];
        std::string href = selfHref + e.name;
        if (e.isDir) href += '/';
        addResponse(href.c_str(), e.mtime, e.size, e.isDir);
    }

    pos += snprintf(buf+pos, bufSz-pos, "</D:multistatus>\n");
    free(entries);
    *outLen = pos;
    return buf;
}

/* ---- WebDAV: Destination header → filesystem path ---- */

static std::string urlToFsPath(const char* destUrl, const web_map_t* map) {
    /* destUrl is like "https://host/sdcard/sub/name.txt" — extract path after host */
    const char* p = strstr(destUrl, "://");
    if (p) { p += 3; p = strchr(p, '/'); }  /* skip scheme + host */
    else p = destUrl;
    if (!p || *p != '/') return {};
    p++;  /* skip leading / */
    std::string path(p);
    if (hasPathTraversal(path)) return {};
    std::string rel = getRelPath(path, *map);
    return map->files + "/" + rel;
}

/* ---- Directory index generation ---- */

static char* generateDirIndex(const std::string& urlDir, const char* fsDir, size_t* outLen) {
    struct entry_t { char name[80]; time_t mtime; uint32_t size; bool isDir; };
    constexpr int MAX_ENTRIES = 1024;
    auto* entries = (entry_t*)heap_caps_malloc(MAX_ENTRIES * sizeof(entry_t), MALLOC_CAP_SPIRAM);
    if (!entries) return nullptr;

    auto* listing = (fs_listing_t*)heap_caps_malloc(MAX_ENTRIES * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
    if (!listing) { free(entries); return nullptr; }
    int n = fs_listdir(fsDir, listing, MAX_ENTRIES);
    if (n < 0) { heap_caps_free(listing); free(entries); return nullptr; }
    int count = 0;
    for (int i = 0; i < n && count < MAX_ENTRIES; i++) {
        if (listing[i].name[0] == '.') continue;
        auto& e = entries[count++];
        safeStrncpy(e.name, listing[i].name, sizeof(e.name));
        e.mtime = listing[i].mtime;
        e.size  = listing[i].size;
        e.isDir = listing[i].isDir;
    }
    heap_caps_free(listing);

    std::sort(entries, entries + count, [](const entry_t& a, const entry_t& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return strcasecmp(a.name, b.name) < 0;
    });

    size_t bufSz = 512 + count * 300;
    char* buf = (char*)heap_caps_malloc(bufSz, MALLOC_CAP_SPIRAM);
    if (!buf) { free(entries); return nullptr; }

    int pos = 0;
    pos += snprintf(buf + pos, bufSz - pos,
        "<html><head><title>%s</title></head>\n"
        "<body style=\"font-family:monospace\">\n<h2>%s</h2><hr><pre>\n",
        urlDir.c_str(), urlDir.c_str());

    /* Parent directory link */
    if (urlDir != "/") {
        std::string parent = urlDir;
        if (parent.back() == '/') parent.pop_back();
        size_t ls = parent.rfind('/');
        parent = (ls != std::string::npos) ? parent.substr(0, ls + 1) : "/";
        pos += snprintf(buf + pos, bufSz - pos, "<a href=\"%s\">..</a>\n", parent.c_str());
    }

    for (int i = 0; i < count && pos < (int)bufSz - 256; i++) {
        auto& e = entries[i];
        struct tm tm;
        localtime_r(&e.mtime, &tm);
        char sz[16];
        if (e.isDir) snprintf(sz, sizeof(sz), "[dir]");
        else fmtSize(e.size, sz, sizeof(sz));
        pos += snprintf(buf + pos, bufSz - pos,
            "%04d-%02d-%02d %02d:%02d  %8s  <a href=\"%s%s%s\">%s%s</a>\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
            sz, urlDir.c_str(), e.name, e.isDir ? "/" : "",
            e.name, e.isDir ? "/" : "");
    }

    pos += snprintf(buf + pos, bufSz - pos, "</pre><hr></body></html>\n");
    free(entries);
    *outLen = pos;
    return buf;
}

/* ---- Per-handle state ---- */

enum handle_state_t { HS_IDLE, HS_READING, HS_RECV_BODY, HS_SEND_HDR, HS_SEND_BODY };

struct web_handle_t {
    int itsHandle = -1;
    handle_state_t state = HS_IDLE;
    bool tls = false;
    ip_addr_t clientAddr = {};
    uint32_t deadline = 0;

    /* Request buffer */
    char rbuf[1024];
    int rLen = 0;

    /* Auth */
    char authRealm[16] = {};  /* resolved realm for this request, or "" */
    bool acceptGzip = false;  /* request advertised Accept-Encoding: gzip */

    /* Response header */
    char hdr[320];
    int hdrLen = 0;
    int hdrSent = 0;

    /* PUT body reception */
    int putFile = -1;
    size_t putRemain = 0;
    bool putChunked = false;   /* Transfer-Encoding: chunked */

    /* True while a webFileTask is pumping a response body into the ITS handle.
     * Web defers all further parsing on this handle until the task's done-aux
     * clears the flag (see WEB_FILE_DONE_PORT handler). */
    bool sending = false;
    /* If the connection drops while sending, defer slot release until the
     * file task signals completion (so the slot index isn't reused). */
    bool wantsCleanup = false;

    /* Undrained body bytes from a rejected request (skip before next parse) */
    size_t skipRemain = 0;

    /* Response body (file streaming or generated content) */
    int file = -1;
    uint8_t* genBuf = nullptr;   /* heap-allocated generated content (dir listing) */
    size_t genLen = 0;
    size_t bodyRemain = 0;
    uint8_t wbuf[2048];
    int wLen = 0;
    int wSent = 0;
};

static web_handle_t* handles = nullptr;
static int webMaxHandles = 8;

static int webAllocSlot(int itsH) {
    for (int i = 0; i < webMaxHandles; i++)
        if (handles[i].itsHandle < 0) { handles[i].itsHandle = itsH; return i; }
    return -1;
}

static void handleReset(int slot) {
    auto& wh = handles[slot];
    if (wh.putFile >= 0) { fs_close(wh.putFile); wh.putFile = -1; }
    if (wh.file >= 0) { fs_close(wh.file); wh.file = -1; }
    if (wh.genBuf) { heap_caps_free(wh.genBuf); wh.genBuf = nullptr; }
    /* Preserve per-connection state across keep-alive request boundaries. */
    int       savedH    = wh.itsHandle;
    bool      savedTls  = wh.tls;
    ip_addr_t savedAddr = wh.clientAddr;
    wh = {};
    wh.itsHandle = savedH;
    wh.tls = savedTls;
    wh.clientAddr = savedAddr;
    wh.state = HS_IDLE;
    wh.file = -1;
    wh.putFile = -1;
}

static void handleTouch(int h) {
    handles[h].deadline = millis() + WEB_KEEPALIVE_MS;
}

/* ---- URL path table (registered by tasks via aux messages) ---- */

#define WEB_MAX_PATHS 8

enum web_path_kind_t : uint8_t {
    WEB_PATH_FORWARD = 0,   /* itsServerForward to (task, itsPort) */
    WEB_PATH_HANDLER = 1,   /* invoke cb on web's task */
};

struct web_path_t {
    char path[16];
    web_path_kind_t kind;
    union {
        struct { TaskHandle_t task; uint16_t itsPort; } fwd;
        web_url_handler_t cb;
    };
};

static web_path_t webPaths[WEB_MAX_PATHS];
static int webPathCount = 0;

/* Longest registered prefix that matches `path` on a `/` boundary or exactly.
 *  Examples:
 *    registered "auth"                     matches "auth", "auth/login"
 *    registered "api/recordings"           matches "api/recordings", "api/recordings/dates"
 *    registered ".well-known/acme-challenge" matches ".well-known/acme-challenge/<token>"
 *  None of those would claim "auth-something" or "api/other". */
static web_path_t* pathFind(const char* path) {
    web_path_t* best = nullptr;
    size_t bestLen = 0;
    size_t plen = strlen(path);
    for (int i = 0; i < webPathCount; i++) {
        size_t rlen = strlen(webPaths[i].path);
        if (rlen > plen || rlen <= bestLen) continue;
        if (strncmp(path, webPaths[i].path, rlen) != 0) continue;
        if (rlen < plen && path[rlen] != '/') continue;
        best = &webPaths[i];
        bestLen = rlen;
    }
    return best;
}

/* Exact-match lookup, used when registering to update an existing entry. */
static web_path_t* pathFindExact(const char* path) {
    for (int i = 0; i < webPathCount; i++)
        if (strcmp(webPaths[i].path, path) == 0) return &webPaths[i];
    return nullptr;
}

static web_path_t* pathAllocOrFind(const char* path) {
    web_path_t* p = pathFindExact(path);
    if (!p) {
        if (webPathCount >= WEB_MAX_PATHS) return nullptr;
        p = &webPaths[webPathCount++];
        safeStrncpy(p->path, path, sizeof(p->path));
    }
    return p;
}

/* ITS aux: forward-target registration (existing) */
static void webOnAux(TaskHandle_t sender, const void* data, size_t len) {
    if (len < sizeof(web_path_msg_t)) return;
    auto* msg = (const web_path_msg_t*)data;
    web_path_t* p = pathAllocOrFind(msg->path);
    if (!p) return;
    p->kind = WEB_PATH_FORWARD;
    p->fwd.task = sender;
    p->fwd.itsPort = msg->itsPort;
}

/* ITS aux: in-web callback registration */
static void webOnHandlerAux(TaskHandle_t /*sender*/, const void* data, size_t len) {
    if (len < sizeof(web_handler_msg_t)) return;
    auto* msg = (const web_handler_msg_t*)data;
    web_path_t* p = pathAllocOrFind(msg->path);
    if (!p) return;
    p->kind = WEB_PATH_HANDLER;
    p->cb = msg->cb;
}

void webRegisterHandler(const char* path, web_url_handler_t cb) {
    web_handler_msg_t msg = {};
    msg.cb = cb;
    safeStrncpy(msg.path, path, sizeof(msg.path));
    while (!itsSendAux("web", WEB_PATH_HANDLER_PORT, &msg, sizeof(msg), pdMS_TO_TICKS(500)))
        vTaskDelay(pdMS_TO_TICKS(100));
}

/* ---- ITS server callbacks ---- */

static int webItsConnect(int handle, const void* data, size_t len) {
    int s = webAllocSlot(handle);
    if (s < 0) return -1;
    handleReset(s);
    handles[s].state = HS_READING;
    handles[s].deadline = millis() + WEB_KEEPALIVE_MS;
    if (len >= sizeof(net_connect_t)) {
        auto* cd = (const net_connect_t*)data;
        handles[s].tls = cd->tls;
        handles[s].clientAddr = cd->clientAddr;
    }
    return s;
}

static bool webItsBusy(const void* data, size_t len) {
    /* Kick oldest handle */
    uint32_t earliest = UINT32_MAX;
    int victim = -1;
    for (int i = 0; i < webMaxHandles; i++) {
        if (handles[i].state != HS_IDLE && handles[i].deadline < earliest) {
            earliest = handles[i].deadline;
            victim = i;
        }
    }
    if (victim >= 0) {
        int itsH = handles[victim].itsHandle;
        handleReset(victim);
        handles[victim].itsHandle = -1;
        itsDisconnect(itsH);
        return false;  /* freed a slot */
    }
    return true;
}

static void webItsDisconnect(int ref) {
    if (ref < 0 || ref >= webMaxHandles) return;
    if (handles[ref].sending) {
        /* Streamer still writing — keep slot reserved so the index isn't
         * reused. Final cleanup happens when file task's done-aux fires. */
        handles[ref].wantsCleanup = true;
        return;
    }
    handleReset(ref);
    handles[ref].itsHandle = -1;
}

static void webOnFileDone(TaskHandle_t, const void* data, size_t len) {
    if (len < sizeof(int)) return;
    int slot;
    memcpy(&slot, data, sizeof(slot));
    if (slot < 0 || slot >= webMaxHandles) return;
    auto& wh = handles[slot];
    wh.sending = false;
    if (wh.wantsCleanup) {
        int itsH = wh.itsHandle;
        handleReset(slot);
        wh.itsHandle = -1;
        if (itsH >= 0) itsDisconnect(itsH);
    } else {
        handleReset(slot);
        wh.state = HS_READING;   /* ready for next request on keep-alive */
        handleTouch(slot);
    }
}

/* ---- Send helpers (via ITS) ---- */

static bool itsSendAll(int handle, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t sent = 0;
    while (sent < len) {
        size_t n = itsSend(handle, p + sent, len - sent, pdMS_TO_TICKS(100));
        if (n == 0) return false;
        sent += n;
    }
    return true;
}

/* ---- WebSocket helpers ---- */

static bool wsComputeAccept(const char* key, char* accept, size_t acceptLen) {
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[128];
    snprintf(concat, sizeof(concat), "%s%s", key, magic);
    unsigned char sha[20];
    mbedtls_sha1((const unsigned char*)concat, strlen(concat), sha);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char*)accept, acceptLen, &olen, sha, 20);
    accept[olen] = '\0';
    return true;
}

static bool isWsUpgrade(const char* buf, int len) {
    /* RFC 6455: the header field name and the "websocket" token are both
     * case-insensitive. Browsers hitting the device directly send
     * "Upgrade: websocket", but a reverse proxy (e.g. `spangap dev`'s Vite
     * proxy, which runs on Node) lowercases header names to "upgrade:", so a
     * literal strstr would miss it. webHeaderField matches the field name
     * case-insensitively; compare the value the same way. */
    char val[32];
    return webHeaderField(buf, len, "Upgrade", val, sizeof(val))
        && strcasecmp(val, "websocket") == 0;
}

/* ---- Request path extraction ---- */

static std::string extractPath(const char* buf, int len) {
    const char* p = (const char*)memchr(buf, '/', len);
    if (!p) return "";
    p++;
    const char* end = (const char*)memchr(p, ' ', len - (p - buf));
    if (!end) return "";
    std::string path(p, end - p);
    size_t qm = path.find('?');
    if (qm != std::string::npos) path = path.substr(0, qm);
    return path;
}

/* ---- Auth header injection ---- */

/** Inject X-Authenticated header into the response header buffer.
 *  Must be called AFTER snprintf fills hdr but BEFORE state = HS_SEND_HDR.
 *  Inserts before the final \r\n\r\n. No-op when auth is disabled. */
static void injectAuthHeader(web_handle_t& wh) {
    if (!authEnabled()) return;
    const char* extra = wh.authRealm;
    char insert[48];
    int insertLen = snprintf(insert, sizeof(insert), "X-Authenticated: %s\r\n", extra);
    if (wh.hdrLen + insertLen >= (int)sizeof(wh.hdr)) return;
    if (wh.hdrLen >= 4) {
        memmove(wh.hdr + wh.hdrLen - 2 + insertLen, wh.hdr + wh.hdrLen - 2, 2);
        memcpy(wh.hdr + wh.hdrLen - 2, insert, insertLen);
        wh.hdrLen += insertLen;
    }
}

/* ---- Per-request file task task ---- */

#define WEB_FILE_DONE_PORT 8080

struct web_file_req_t {
    char   path[176];
    char   fallbackPath[176]; /* SPA index fallback if path doesn't exist; empty = none */
    char   name[80];          /* for MIME type lookup */
    char   rangeHdr[48];      /* raw Range header value, empty = none */
    char   authRealm[16];     /* X-Authenticated value */
    bool   tryGz;
    bool   headOnly;
    bool   authEnabled;       /* whether to inject X-Authenticated at all */
    bool   acceptGzip;        /* client sent Accept-Encoding: gzip */
    int    itsHandle;
    int    slot;
};

static const char* mimeType(const char* path);

static bool sendAll(int h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t sent = 0;
    while (sent < len) {
        size_t n = itsInject(h, true, p + sent, len - sent, pdMS_TO_TICKS(60000));
        if (n == 0) return false;
        sent += n;
    }
    return true;
}

/** Try fopen path.gz then path. Returns fp (or nullptr) and sets *gz. */
static FILE* openWithGzFallback(const char* path, bool tryGz, bool* gz) {
    FILE* fp = nullptr;
    *gz = false;
    if (tryGz) {
        char g[200]; snprintf(g, sizeof(g), "%.190s.gz", path);
        fp = fopen(g, "rb");
        if (fp) { *gz = true; return fp; }
    }
    return fopen(path, "rb");
}

/* Single persistent worker serialises file responses — SD and network bandwidth
 * are both modest, so concurrency wasn't buying much and the per-request DRAM
 * stack was exhausting internal heap under load. Web posts requests to the
 * worker via ITS aux (pointer payload); worker's inbox depth provides
 * backpressure, caller gets a 503 if the inbox is full. */
#define WEB_FILE_REQ_PORT  8081
#define WEB_FILE_INBOX_DEPTH 8

/* Whole-file response (no Range, no streaming): read the open file fully into
 * PSRAM, inflate it if it's gzip-on-disk, optionally run an extension transform
 * (e.g. .md → HTML), and send the result uncompressed. Used for two cases:
 *   - a registered transform matches the extension, or
 *   - the file is .gz on disk but the client didn't send Accept-Encoding: gzip.
 * Closes fp. Returns true if a response (200, or a best-effort 500) was
 * delivered; false only if the socket write itself failed (caller disconnects). */
#define WEB_WHOLE_MAX_IN   (512 * 1024)        /* raw read cap (PSRAM)        */
#define WEB_WHOLE_MAX_OUT  (4 * 1024 * 1024)   /* inflate ISIZE cap (PSRAM)   */

static bool serveWholeFile(web_file_req_t* req, FILE* fp, size_t fileSize,
                           bool gzipped, web_file_transform_t xform) {
    uint8_t *raw = nullptr, *plain = nullptr, *body = nullptr;
    size_t plainLen = 0, bodyLen = 0;
    const char* ctype = nullptr;
    bool freeBody = false, built = false;

    if (fileSize > 0 && fileSize <= WEB_WHOLE_MAX_IN &&
        (raw = (uint8_t*)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM)) &&
        fread(raw, 1, fileSize, fp) == fileSize) {
        if (gzipped) {
            plain = gzInflate(raw, fileSize, WEB_WHOLE_MAX_OUT, &plainLen);
            heap_caps_free(raw); raw = nullptr;
        } else {
            plain = raw; plainLen = fileSize; raw = nullptr;
        }
        if (plain) {
            if (xform && xform(req->name, plain, plainLen, &body, &bodyLen, &ctype) && body) {
                freeBody = true;                 /* transform owns `body`        */
                heap_caps_free(plain); plain = nullptr;
            } else {                             /* serve file as-is (inflated)  */
                body = plain; bodyLen = plainLen; plain = nullptr;
                ctype = mimeType(req->name); freeBody = true;
            }
            built = true;
        }
    }
    if (raw)   heap_caps_free(raw);
    if (plain) heap_caps_free(plain);
    fclose(fp);

    bool ok;
    if (!built) {
        char hdr[128];
        int n = snprintf(hdr, sizeof(hdr), "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\nConnection: keep-alive\r\n\r\n");
        ok = sendAll(req->itsHandle, hdr, n);    /* delivered → keep connection */
    } else {
        char hdr[512];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
            "Connection: keep-alive\r\n", ctype, (unsigned)bodyLen);
        if (req->authEnabled && hl < (int)sizeof(hdr) - 48)
            hl += snprintf(hdr + hl, sizeof(hdr) - hl,
                           "X-Authenticated: %s\r\n", req->authRealm);
        if (hl < (int)sizeof(hdr) - 2) { hdr[hl++] = '\r'; hdr[hl++] = '\n'; }
        ok = sendAll(req->itsHandle, hdr, hl) &&
             (req->headOnly || bodyLen == 0 || sendAll(req->itsHandle, body, bodyLen));
    }
    if (body && freeBody) heap_caps_free(body);
    return ok;
}

static void processFileRequest(web_file_req_t* req) {
    /* Try primary path (.gz first if tryGz). If that fails, try fallback
     * (SPA index.html for extensionless paths / directory roots). */
    bool gzipped = false;
    FILE* fp = openWithGzFallback(req->path, req->tryGz, &gzipped);
    if (!fp && req->fallbackPath[0]) {
        fp = openWithGzFallback(req->fallbackPath, true, &gzipped);
    }

    bool ok = false;
    if (!fp) {
        /* 404 response */
        char hdr[256];
        int hdrLen = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
        sendAll(req->itsHandle, hdr, hdrLen);
        ok = true;  /* response delivered (even if 404) — don't kill the connection */
    } else {
        /* Get size via fseek+ftell. */
        fseek(fp, 0, SEEK_END);
        long fileSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        /* Whole-file paths (bypass Range/streaming): a registered extension
         * transform (e.g. .md → HTML), or a gzip-on-disk file that a client
         * which didn't advertise Accept-Encoding: gzip can't decode itself. */
        web_file_transform_t xform = findExtTransform(req->name);
        if (xform || (gzipped && !req->acceptGzip)) {
            ok = serveWholeFile(req, fp, (size_t)fileSize, gzipped, xform);
            goto done;
        }

        /* Parse Range header now that we have the file size. */
        size_t rangeStart = 0, rangeEnd = 0;
        if (!gzipped && req->rangeHdr[0])
            parseRange(req->rangeHdr, (size_t)fileSize, &rangeStart, &rangeEnd);
        bool hasRange = !gzipped && rangeEnd > 0;
        size_t bodyLen    = hasRange ? (rangeEnd - rangeStart + 1) : (size_t)fileSize;
        size_t bodyOffset = hasRange ? rangeStart : 0;

        /* Build headers */
        char hdr[512];
        int hdrLen;
        if (hasRange) {
            hdrLen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 206 Partial Content\r\nContent-Type: %s\r\n"
                "Content-Range: bytes %u-%u/%u\r\nContent-Length: %u\r\n"
                "Accept-Ranges: bytes\r\nConnection: keep-alive\r\n",
                mimeType(req->name), (unsigned)rangeStart, (unsigned)rangeEnd,
                (unsigned)fileSize, (unsigned)bodyLen);
        } else {
            hdrLen = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s%s"
                "Connection: keep-alive\r\n",
                mimeType(req->name), (unsigned)fileSize,
                gzipped ? "Content-Encoding: gzip\r\n" : "",
                !gzipped ? "Accept-Ranges: bytes\r\n" : "");
        }
        if (req->authEnabled && hdrLen < (int)sizeof(hdr) - 48) {
            hdrLen += snprintf(hdr + hdrLen, sizeof(hdr) - hdrLen,
                               "X-Authenticated: %s\r\n", req->authRealm);
        }
        if (hdrLen < (int)sizeof(hdr) - 2) {
            hdr[hdrLen++] = '\r'; hdr[hdrLen++] = '\n';
        }

        if (!sendAll(req->itsHandle, hdr, hdrLen)) {
            fclose(fp);
            goto done;
        }

        if (req->headOnly || bodyLen == 0) {
            fclose(fp);
            ok = true;
            goto done;
        }

        /* Stream body */
        if (bodyOffset > 0) fseek(fp, (long)bodyOffset, SEEK_SET);
        uint8_t chunk[2048];
        size_t totalSent = 0;
        bool err = false;
        while (!err && totalSent < bodyLen) {
            size_t want = sizeof(chunk);
            size_t remain = bodyLen - totalSent;
            if (remain < want) want = remain;
            size_t n = fread(chunk, 1, want, fp);
            if (n == 0) break;
            if (!sendAll(req->itsHandle, chunk, n)) { err = true; break; }
            totalSent += n;
        }
        fclose(fp);
        ok = !err && totalSent == bodyLen;
    }

done:
    /* If the response was cut short, kill the connection so the browser sees
     * a clean disconnect rather than ERR_CONTENT_LENGTH_MISMATCH. */
    if (!ok) itsDisconnect(req->itsHandle);
}

static void webFileWorkerOnReq(TaskHandle_t, const void* data, size_t len) {
    if (len < sizeof(web_file_req_t*)) return;
    web_file_req_t* req = nullptr;
    memcpy(&req, data, sizeof(req));
    if (!req) return;
    processFileRequest(req);
    int slot = req->slot;
    free(req);
    itsSendAuxByTaskHandle(webHandle, WEB_FILE_DONE_PORT, &slot, sizeof(slot),
                           portMAX_DELAY, ITS_WAIT_DELIVERY);
}

static void webFileWorkerFn(void*) {
    itsServerInit(sizeof(web_file_req_t*), WEB_FILE_INBOX_DEPTH);
    itsOnAux(WEB_FILE_REQ_PORT, webFileWorkerOnReq);
    for (;;) itsPoll();
}

static bool spawnFileTask(int h, const char* fsPath, const char* fallbackPath,
                          bool tryGz, const char* name, bool headOnly,
                          const char* rangeHdr) {
    auto* req = (web_file_req_t*)gp_alloc(sizeof(web_file_req_t));
    if (!req) { warn("spawnFileTask: malloc failed for %s\n", fsPath); return false; }
    safeStrncpy(req->path, fsPath, sizeof(req->path));
    safeStrncpy(req->fallbackPath, fallbackPath ? fallbackPath : "", sizeof(req->fallbackPath));
    /* Only the basename matters for mimeType — scanner probes send long
     * path-traversal noise that would otherwise spam truncation warnings. */
    const char* base = strrchr(name, '/');
    base = base ? base + 1 : name;
    safeStrncpy(req->name, base, sizeof(req->name));
    safeStrncpy(req->authRealm, handles[h].authRealm, sizeof(req->authRealm));
    safeStrncpy(req->rangeHdr, rangeHdr ? rangeHdr : "", sizeof(req->rangeHdr));
    req->tryGz = tryGz;
    req->headOnly = headOnly;
    req->authEnabled = authEnabled();
    req->acceptGzip = handles[h].acceptGzip;
    req->itsHandle = handles[h].itsHandle;
    req->slot = h;
    handles[h].sending = true;
    /* Non-blocking enqueue: if the inbox is full we'd rather fail fast than
     * stall the main web task. Inbox depth is generous (8) so this only hits
     * under genuinely pathological load. */
    if (!itsSendAux("web_file", WEB_FILE_REQ_PORT, &req, sizeof(req), 0)) {
        warn("file inbox full for %s\n", fsPath);
        handles[h].sending = false;
        free(req);
        return false;
    }
    return true;
}

/* ---- File serving helpers ---- */

static void serve404(int h);

/** Serve a file by handing the whole request — discovery, headers, body — off
 *  to a per-request DRAM-stack web_file task. Web never blocks on fs here.
 *  fallbackPath is tried if the primary doesn't exist (SPA index.html). */
static void serveFile(int h, const char* fsPath, const char* fallbackPath,
                      bool tryGz, const char* name,
                      bool headOnly = false, const char* rangeHdr = nullptr) {
    if (!spawnFileTask(h, fsPath, fallbackPath, tryGz, name, headOnly, rangeHdr))
        serve404(h);
}

static void serveDirIndex(int h, const std::string& urlDir, const char* fsDir) {
    auto& wh = handles[h];
    size_t genLen = 0;
    char* html = generateDirIndex(urlDir, fsDir, &genLen);
    if (!html) {
        wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
            "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
            "Connection: keep-alive\r\n\r\n");
        injectAuthHeader(wh);
        wh.hdrSent = 0;
        wh.state = HS_SEND_HDR;
        return;
    }
    wh.genBuf = (uint8_t*)html;
    wh.genLen = genLen;
    wh.bodyRemain = genLen;
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n"
        "Connection: keep-alive\r\n\r\n", (unsigned)genLen);
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.wLen = wh.wSent = 0;
    wh.state = HS_SEND_HDR;
}

static void serve404(int h) {
    auto& wh = handles[h];
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.state = HS_SEND_HDR;
}

static void serve201(int h) {
    auto& wh = handles[h];
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 201 Created\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.state = HS_SEND_HDR;
}

static void serve204(int h) {
    auto& wh = handles[h];
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\n\r\n");
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.state = HS_SEND_HDR;
}

static void serve301(int h, const char* location) {
    auto& wh = handles[h];
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 301 Moved Permanently\r\nLocation: %s\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n", location);
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.state = HS_SEND_HDR;
}

static void serve405(int h) {
    auto& wh = handles[h];
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.state = HS_SEND_HDR;
}

static void serve200lock(int h, const std::string& path) {
    auto& wh = handles[h];
    uint32_t hash = 0;
    for (auto c : path) hash = hash * 31 + c;
    char xml[512];
    int xlen = snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<D:prop xmlns:D=\"DAV:\"><D:lockdiscovery><D:activelock>\n"
        "<D:locktype><D:write/></D:locktype>\n"
        "<D:lockscope><D:exclusive/></D:lockscope>\n"
        "<D:depth>Infinity</D:depth>\n"
        "<D:timeout>Second-600</D:timeout>\n"
        "<D:locktoken><D:href>urn:uuid:%08x-0000-0000-0000-000000000000</D:href></D:locktoken>\n"
        "</D:activelock></D:lockdiscovery></D:prop>\n", (unsigned)hash);
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 200 OK\r\nContent-Type: application/xml; charset=\"utf-8\"\r\n"
        "Lock-Token: <urn:uuid:%08x-0000-0000-0000-000000000000>\r\n"
        "Content-Length: %d\r\nConnection: keep-alive\r\n\r\n",
        (unsigned)hash, xlen);
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.genBuf = (uint8_t*)heap_caps_malloc(xlen, MALLOC_CAP_SPIRAM);
    if (wh.genBuf) { memcpy(wh.genBuf, xml, xlen); wh.genLen = xlen; }
    wh.bodyRemain = xlen;
    wh.wLen = wh.wSent = 0;
    wh.state = HS_SEND_HDR;
}

static void serve409(int h) {
    auto& wh = handles[h];
    wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
        "HTTP/1.1 409 Conflict\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
    injectAuthHeader(wh);
    wh.hdrSent = 0;
    wh.state = HS_SEND_HDR;
}

/* ---- Consume request headers + body (for methods where body is ignored) ---- */

static void consumeRequest(web_handle_t& wh, int consumed) {
    char clHdr[16] = {}, teHdr[16] = {};
    webHeaderField(wh.rbuf, consumed, "Content-Length", clHdr, sizeof(clHdr));
    webHeaderField(wh.rbuf, consumed, "Transfer-Encoding", teHdr, sizeof(teHdr));
    bool chunked = (strcasestr(teHdr, "chunked") != nullptr);
    int bodyLen = atoi(clHdr);

    if (chunked) {
        /* Drain chunked body — read and discard all chunks */
        int bodyInBuf = wh.rLen - consumed;
        if (bodyInBuf > 0) memmove(wh.rbuf, wh.rbuf + consumed, bodyInBuf);
        wh.rLen = bodyInBuf > 0 ? bodyInBuf : 0;
        for (;;) {
            /* Skip \r\n between chunks */
            while (wh.rLen >= 2 && wh.rbuf[0] == '\r' && wh.rbuf[1] == '\n') {
                wh.rLen -= 2;
                if (wh.rLen > 0) memmove(wh.rbuf, wh.rbuf + 2, wh.rLen);
            }
            /* Read more if needed */
            int space = (int)sizeof(wh.rbuf) - wh.rLen;
            if (space > 0 && wh.rLen < 12) {
                size_t n = itsRecv(wh.itsHandle, wh.rbuf + wh.rLen, space, pdMS_TO_TICKS(200));
                if (n > 0) wh.rLen += n;
            }
            if (wh.rLen == 0) break;
            char* crlf = (char*)memmem(wh.rbuf, wh.rLen, "\r\n", 2);
            if (!crlf) { if (wh.rLen >= (int)sizeof(wh.rbuf)) { wh.rLen = 0; break; } continue; }
            *crlf = '\0';
            size_t chunkSz = strtoul(wh.rbuf, nullptr, 16);
            int lineLen = (int)(crlf + 2 - wh.rbuf);
            wh.rLen -= lineLen;
            if (wh.rLen > 0) memmove(wh.rbuf, crlf + 2, wh.rLen);
            if (chunkSz == 0) break;
            /* Discard chunk data */
            size_t remain = chunkSz;
            size_t bufSkip = (size_t)wh.rLen < remain ? (size_t)wh.rLen : remain;
            remain -= bufSkip; wh.rLen -= bufSkip;
            if (wh.rLen > 0) memmove(wh.rbuf, wh.rbuf + bufSkip, wh.rLen);
            while (remain > 0) {
                uint8_t discard[512];
                size_t n = itsRecv(wh.itsHandle, discard,
                                   remain < sizeof(discard) ? remain : sizeof(discard),
                                   pdMS_TO_TICKS(200));
                if (n == 0) break;
                remain -= n;
            }
        }
        wh.rLen = 0;
    } else {
        int bodyInBuf = wh.rLen - consumed;
        int skip = bodyLen < bodyInBuf ? bodyLen : bodyInBuf;
        wh.rLen -= consumed + skip;
        if (wh.rLen > 0) memmove(wh.rbuf, wh.rbuf + consumed + skip, wh.rLen);
        int remaining = bodyLen - skip;
        while (remaining > 0) {
            uint8_t discard[512];
            size_t n = itsRecv(wh.itsHandle, discard,
                               remaining < (int)sizeof(discard) ? remaining : sizeof(discard),
                               pdMS_TO_TICKS(50));
            if (n == 0) break;
            remaining -= n;
        }
        if (remaining > 0) wh.skipRemain = remaining;
    }
}

/* ---- Auth helpers ---- */

/** Check if realm is in a comma-delimited list (e.g. "admin,view"). */
static bool realmInList(const char* realm, const std::string& list) {
    if (!realm[0] || list.empty()) return false;
    size_t rlen = strlen(realm);
    const char* p = list.c_str();
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char* end = p;
        while (*end && *end != ',') end++;
        size_t len = end - p;
        /* Trim trailing spaces */
        while (len > 0 && p[len - 1] == ' ') len--;
        if (len == rlen && strncmp(p, realm, len) == 0) return true;
        p = end;
    }
    return false;
}

/** Resolve auth realm from the session cookie in the request headers. */
static void resolveAuth(web_handle_t& wh, const char* rbuf, int consumed) {
    wh.authRealm[0] = '\0';
    if (!authEnabled()) return;
    char cookie[64] = {};
    webExtractCookie(rbuf, consumed, "session", cookie, sizeof(cookie));
    if (cookie[0]) {
        std::string realm = authCheck(cookie);
        safeStrncpy(wh.authRealm, realm.c_str(), sizeof(wh.authRealm));
    }
}

/** Serve the "/" mapping's index.html (fwd-to-/ for unauthed map entries). */
static void serveRootIndex(int h) {
    for (int i = 0; i < webMapCount; i++) {
        if (webMaps[i].url == "/") {
            std::string indexPath = webMaps[i].files + "/index.html";
            serveFile(h, indexPath.c_str(), nullptr, true, "index.html");
            return;
        }
    }
    serve404(h);
}

/* ---- Request parsing ---- */

static void tryParseRequest(int h) {
    auto& wh = handles[h];
    const char* end = nullptr;
    for (int i = 0; i + 3 < wh.rLen; i++) {
        if (wh.rbuf[i] == '\r' && wh.rbuf[i+1] == '\n' &&
            wh.rbuf[i+2] == '\r' && wh.rbuf[i+3] == '\n') {
            end = wh.rbuf + i + 4;
            break;
        }
    }
    if (!end) return;

    int consumed = (int)(end - wh.rbuf);
    char method[12] = {};
    webGetMethod(wh.rbuf, consumed, method, sizeof(method));
    std::string path = extractPath(wh.rbuf, consumed);
    bool upgrade = isWsUpgrade(wh.rbuf, consumed);
    bool isGet = strcmp(method, "GET") == 0;
    dbg("%s /%s%s\n", method, path.c_str(), wh.tls ? " (TLS)" : "");
    bool isHead = strcmp(method, "HEAD") == 0;

    /* Content-negotiation: remember whether the client can accept gzip, so the
     * file worker only sends a .gz file's bytes verbatim (Content-Encoding:
     * gzip) when the client advertised it — otherwise it inflates first. */
    {
        char ae[64] = {};
        webHeaderField(wh.rbuf, consumed, "Accept-Encoding", ae, sizeof(ae));
        wh.acceptGzip = strcasestr(ae, "gzip") != nullptr;
    }

    /* ---- HTTPS redirect ---- */
    /* Loopback (the device fetching its own pages, e.g. the LCD viewer pulling
     * server-rendered Markdown from 127.0.0.1) is exempt: it talks plain HTTP so
     * there's no self-signed cert to validate against itself. */
    if (!wh.tls && httpsOnly && !ip_addr_isloopback(&wh.clientAddr)) {
        /* Check exception list */
        bool allowed = false;
        std::string fullPath = "/" + path;
        for (int i = 0; i < httpAllowedCount; i++) {
            if (fullPath.compare(0, httpAllowed[i].size(), httpAllowed[i]) == 0) {
                allowed = true; break;
            }
        }
        if (!allowed) {
            char host[64];
            webHeaderField(wh.rbuf, consumed, "Host", host, sizeof(host));
            /* Preserve the original request target (path + optional query) on redirect.
             * (We still enforce the httpAllowed prefix check based on the path only.) */
            const char* sp1 = (const char*)memchr(wh.rbuf, ' ', consumed);
            const char* sp2 = sp1 ? (const char*)memchr(sp1 + 1, ' ', consumed - (int)(sp1 + 1 - wh.rbuf)) : nullptr;
            std::string target = "/";
            if (sp1 && sp2 && sp2 > sp1 + 1) {
                target.assign(sp1 + 1, sp2 - (sp1 + 1));
                if (target.empty() || target[0] != '/') target = "/";
            }
            char loc[192];
            snprintf(loc, sizeof(loc), "https://%s%s", host, target.c_str());
            consumeRequest(wh, consumed);
            serve301(h, loc);
            return;
        }
    }

    /* ---- Registered path forwarding (WS and HTTP) ---- */
    web_path_t* wp = pathFind(path.c_str());

    /* Inject the *whole* received buffer (wh.rLen, not just `consumed`) — for a
     * POST whose body fits in the same TCP segment as the headers, the body
     * bytes are sitting in wh.rbuf[consumed..wh.rLen] and would otherwise be
     * dropped when wh.rLen=0 below. Target task reads headers via webGetHeader
     * (which stops at \r\n\r\n), then webReadBody picks up the body bytes that
     * came along — exactly the round-trip we want. */
    if (upgrade && wp && wp->kind == WEB_PATH_FORWARD) {
        int itsH = wh.itsHandle;
        itsInject(itsH, false, wh.rbuf, wh.rLen);
        net_connect_t cd = { 1, wh.tls, wh.clientAddr };
        int fwd = itsServerForwardByTaskHandle(itsH, wp->fwd.task, wp->fwd.itsPort, &cd, sizeof(cd));
        if (fwd < 0) {
            const char* r503 = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
            itsSendAll(itsH, r503, strlen(r503));
            itsDisconnect(itsH);
        }
        wh.state = HS_IDLE; wh.rLen = 0; wh.itsHandle = -1;
        return;
    }

    if (wp && !path.empty() && wp->kind == WEB_PATH_FORWARD) {
        int itsH = wh.itsHandle;
        itsInject(itsH, false, wh.rbuf, wh.rLen);
        net_connect_t cd = { 0, wh.tls, wh.clientAddr };
        int fwd = itsServerForwardByTaskHandle(itsH, wp->fwd.task, wp->fwd.itsPort, &cd, sizeof(cd));
        if (fwd < 0) {
            const char* r503 = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
            itsSendAll(itsH, r503, strlen(r503));
            itsDisconnect(itsH);
        }
        wh.state = HS_IDLE; wh.rLen = 0; wh.itsHandle = -1;
        return;
    }

    if (wp && !path.empty() && wp->kind == WEB_PATH_HANDLER) {
        /* In-web callback: handler runs synchronously on this task with the
         * already-parsed buffer. After it returns, drain the body (so any
         * un-consumed Content-Length bytes don't leak into the next request),
         * then keep the slot alive for the next request on the same TCP/TLS
         * connection (HTTP keep-alive). */
        int itsH = wh.itsHandle;
        wp->cb(itsH, wh.rbuf, wh.rLen);
        if (!itsConnected(itsH)) {
            handleReset(h);
            wh.itsHandle = -1;
            wh.state = HS_IDLE;
            return;
        }
        consumeRequest(wh, consumed);
        wh.state = HS_READING;
        handleTouch(h);
        return;
    }

    /* ---- File serving + WebDAV via configurable mappings ---- */
    web_map_t* map = findMapping(path);
    if (!map) { consumeRequest(wh, consumed); serve404(h); return; }

    /* ---- Auth check for map entries ---- */
    /* Loopback is trusted: a request from 127.0.0.1 is the device talking to its
     * own web server (the LCD viewer fetching server-rendered Markdown), which
     * carries no session cookie — so it bypasses realm checking entirely. */
    resolveAuth(wh, wh.rbuf, consumed);
    if (authEnabled() && !map->auth.empty() && !realmInList(wh.authRealm, map->auth) &&
        !ip_addr_isloopback(&wh.clientAddr)) {
        consumeRequest(wh, consumed);
        serveRootIndex(h);
        return;
    }

    std::string relPath = getRelPath(path, *map);
    if (hasPathTraversal(relPath)) { consumeRequest(wh, consumed); serve404(h); return; }
    if (hasHiddenComponent(relPath)) {
        /* DAV clients write ._files and .DS_Store — accept PUTs silently */
        if (map->dav && strcmp(method, "PUT") == 0) { consumeRequest(wh, consumed); serve201(h); }
        else if (map->dav && strcmp(method, "LOCK") == 0) { consumeRequest(wh, consumed); serve200lock(h, relPath); }
        else if (map->dav && strcmp(method, "UNLOCK") == 0) { consumeRequest(wh, consumed); serve204(h); }
        else if (map->dav && strcmp(method, "DELETE") == 0) { consumeRequest(wh, consumed); serve204(h); }
        else if (map->dav && strcmp(method, "PROPFIND") == 0) {
            /* macOS marker files: pretend they exist to suppress Spotlight/QL */
            static const char* markers[] = {
                ".metadata_never_index", ".metadata_never_index_unless_rootfs",
                ".metadata_direct_scope_only", ".Spotlight-V100",
                ".ql_disablethumbnails", ".ql_disablecache", nullptr
            };
            const char* name = relPath.c_str();
            const char* slash = strrchr(name, '/');
            if (slash) name = slash + 1;
            bool isMarker = false;
            for (auto* m = markers; *m; m++)
                if (strcmp(name, *m) == 0) { isMarker = true; break; }
            if (isMarker) {
                consumeRequest(wh, consumed);
                /* Fake 207 for marker file */
                std::string href = "/" + path;
                char xml[512];
                int xlen = snprintf(xml, sizeof(xml),
                    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<D:multistatus xmlns:D=\"DAV:\"><D:response><D:href>%s</D:href>"
                    "<D:propstat><D:prop><D:resourcetype/>"
                    "<D:getcontentlength>0</D:getcontentlength>"
                    "</D:prop><D:status>HTTP/1.1 200 OK</D:status>"
                    "</D:propstat></D:response></D:multistatus>\n", href.c_str());
                wh.genBuf = (uint8_t*)heap_caps_malloc(xlen, MALLOC_CAP_SPIRAM);
                if (wh.genBuf) { memcpy(wh.genBuf, xml, xlen); wh.genLen = xlen; }
                wh.bodyRemain = xlen;
                wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
                    "HTTP/1.1 207 Multi-Status\r\nContent-Type: application/xml; charset=utf-8\r\n"
                    "Content-Length: %d\r\nConnection: keep-alive\r\n\r\n", xlen);
                injectAuthHeader(wh);
                wh.hdrSent = 0; wh.wLen = wh.wSent = 0; wh.state = HS_SEND_HDR;
            } else {
                consumeRequest(wh, consumed); serve404(h);
            }
        }
        else { consumeRequest(wh, consumed); serve404(h); }
        return;
    }

    while (!relPath.empty() && relPath.back() == '/') relPath.pop_back();
    std::string fsPath = map->files;
    if (!relPath.empty()) fsPath += "/" + relPath;

    /* ---- WebDAV methods (only on dav-enabled mappings) ---- */
    if (map->dav) {
        if (strcmp(method, "OPTIONS") == 0) {
            consumeRequest(wh, consumed);
            wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
                "HTTP/1.1 200 OK\r\nDAV: 1, 2\r\n"
                "Allow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, MOVE, COPY, PROPFIND, LOCK, UNLOCK\r\n"
                "MS-Author-Via: DAV\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n");
            injectAuthHeader(wh);
            wh.hdrSent = 0; wh.state = HS_SEND_HDR;
            return;
        }
        if (strcmp(method, "PROPFIND") == 0) {
            char depthHdr[8] = "1";
            webHeaderField(wh.rbuf, consumed, "Depth", depthHdr, sizeof(depthHdr));
            int depth = atoi(depthHdr);
            if (depth > 1) depth = 1;
            consumeRequest(wh, consumed);
            size_t genLen = 0;
            char* xml = generatePropfind(path, fsPath.c_str(), depth, &genLen);
            if (!xml) { serve404(h); return; }
            wh.genBuf = (uint8_t*)xml;
            wh.genLen = genLen;
            wh.bodyRemain = genLen;
            wh.hdrLen = snprintf(wh.hdr, sizeof(wh.hdr),
                "HTTP/1.1 207 Multi-Status\r\nContent-Type: application/xml; charset=utf-8\r\n"
                "Content-Length: %u\r\nConnection: keep-alive\r\n\r\n", (unsigned)genLen);
            injectAuthHeader(wh);
            wh.hdrSent = 0; wh.wLen = wh.wSent = 0; wh.state = HS_SEND_HDR;
            return;
        }
        if (strcmp(method, "DELETE") == 0) {
            consumeRequest(wh, consumed);
            struct stat dst;
            if (fs_stat(fsPath.c_str(), &dst) != 0) { serve404(h); return; }
            int rc = fs_remove(fsPath.c_str());
            if (rc == 0) serve204(h); else serve409(h);
            return;
        }
        if (strcmp(method, "MKCOL") == 0) {
            consumeRequest(wh, consumed);
            if (fs_mkdir(fsPath.c_str()) == 0) serve201(h);
            else if (errno == EEXIST) serve405(h);
            else serve409(h);
            return;
        }
        if (strcmp(method, "PUT") == 0) {
            char clHdr[16] = {}, teHdr[16] = {};
            webHeaderField(wh.rbuf, consumed, "Content-Length", clHdr, sizeof(clHdr));
            webHeaderField(wh.rbuf, consumed, "Transfer-Encoding", teHdr, sizeof(teHdr));
            bool chunked = (strcasestr(teHdr, "chunked") != nullptr);
            size_t contentLen = clHdr[0] ? (size_t)atoi(clHdr) : 0;
            int f = fs_open(fsPath.c_str(), "wb");
            if (f < 0) { consumeRequest(wh, consumed); serve409(h); return; }
            /* Move any body bytes to front of rbuf */
            int bodyInBuf = wh.rLen - consumed;
            if (bodyInBuf > 0) memmove(wh.rbuf, wh.rbuf + consumed, bodyInBuf);
            wh.rLen = bodyInBuf > 0 ? bodyInBuf : 0;
            if (chunked) {
                wh.putFile = f; wh.putChunked = true; wh.putRemain = 0;
                wh.state = HS_RECV_BODY;
            } else if (contentLen > 0) {
                /* Write body bytes already in rbuf */
                size_t toWrite = (size_t)wh.rLen < contentLen ? (size_t)wh.rLen : contentLen;
                if (toWrite > 0) { fs_write(wh.rbuf, 1, toWrite, f); contentLen -= toWrite; }
                wh.rLen = 0;
                if (contentLen == 0) { fs_close(f); serve201(h); }
                else { wh.putFile = f; wh.putRemain = contentLen; wh.state = HS_RECV_BODY; }
            } else {
                fs_close(f); serve201(h);
            }
            return;
        }
        if (strcmp(method, "MOVE") == 0) {
            char dest[256] = {};
            webHeaderField(wh.rbuf, consumed, "Destination", dest, sizeof(dest));
            consumeRequest(wh, consumed);
            std::string destFs = urlToFsPath(dest, map);
            if (destFs.empty()) { serve409(h); return; }
            if (fs_rename(fsPath.c_str(), destFs.c_str()) == 0) serve201(h);
            else serve409(h);
            return;
        }
        if (strcmp(method, "COPY") == 0) {
            char dest[256] = {};
            webHeaderField(wh.rbuf, consumed, "Destination", dest, sizeof(dest));
            consumeRequest(wh, consumed);
            std::string destFs = urlToFsPath(dest, map);
            if (destFs.empty()) { serve409(h); return; }
            int src = fs_open(fsPath.c_str(), "rb");
            if (src < 0) { serve404(h); return; }
            int dst = fs_open(destFs.c_str(), "wb");
            if (dst < 0) { fs_close(src); serve409(h); return; }
            uint8_t cbuf[1024];
            size_t n;
            while ((n = fs_read(cbuf, 1, sizeof(cbuf), src)) > 0) fs_write(cbuf, 1, n, dst);
            fs_close(src); fs_close(dst);
            serve201(h);
            return;
        }
        if (strcmp(method, "LOCK") == 0) {
            consumeRequest(wh, consumed);
            serve200lock(h, fsPath);
            return;
        }
        if (strcmp(method, "UNLOCK") == 0) {
            consumeRequest(wh, consumed);
            serve204(h);
            return;
        }
    }

    /* ---- GET / HEAD: static file serving ---- */
    if (!isGet && !isHead) { consumeRequest(wh, consumed); serve405(h); return; }

    /* Save Range header before consuming request bytes */
    char rangeHdr[64] = {};
    if (isGet) webHeaderField(wh.rbuf, consumed, "Range", rangeHdr, sizeof(rangeHdr));

    consumeRequest(wh, consumed);
    dbg("/%s%s\n", path.c_str(), wh.tls ? " (TLS)" : "");

    /* Dir-listing mappings (WebDAV, /files, /config) still go through the
     * synchronous path — rare traffic, involves fs_listdir anyway. */
    if (map->dirIndex) {
        struct stat st;
        bool isDir = (fs_stat(fsPath.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        if (isDir) {
            /* Canonicalise: /state → /state/. Without the trailing slash,
             * relative hrefs in the listing (and any links pasted by the
             * user) resolve against the parent path instead of the dir,
             * and Apache/nginx-style clients expect the redirect. */
            if (path.empty() || path.back() != '/') {
                std::string loc = "/" + path + "/";
                serve301(h, loc.c_str());
                return;
            }
            std::string urlDir = map->url;
            if (urlDir.back() != '/') urlDir += '/';
            if (!relPath.empty()) urlDir += relPath + "/";
            serveDirIndex(h, urlDir, fsPath.c_str());
        } else {
            /* dirIndex mapping, exact file — no .gz lookup, no SPA fallback */
            serveFile(h, fsPath.c_str(), nullptr, false,
                      relPath.empty() ? "index.html" : relPath.c_str(),
                      isHead, rangeHdr);
        }
        return;
    }

    /* SPA-style mapping. Push discovery into the web_file task: it tries
     * path.gz, path, then the fallback (map's index.html) for extensionless
     * URLs and directory roots. Web task never touches the filesystem. */
    std::string fallback;
    if (relPath.empty() || !hasExtension(relPath))
        fallback = map->files + "/index.html";
    serveFile(h, fsPath.c_str(), fallback.empty() ? nullptr : fallback.c_str(),
              true,
              relPath.empty() ? "index.html" : relPath.c_str(),
              isHead, rangeHdr);
}

/* ---- ITS-based send helpers for HTTP responses ---- */

static bool trySendHdr(int h) {
    auto& wh = handles[h];
    while (wh.hdrSent < wh.hdrLen) {
        size_t n = itsSend(wh.itsHandle, wh.hdr + wh.hdrSent, wh.hdrLen - wh.hdrSent, 0);
        if (n == 0) return false;
        wh.hdrSent += n;
        handleTouch(h);
    }
    return true;
}

static bool trySendBody(int h) {
    auto& wh = handles[h];
    for (;;) {
        while (wh.wSent < wh.wLen) {
            size_t n = itsSend(wh.itsHandle, wh.wbuf + wh.wSent, wh.wLen - wh.wSent, 0);
            if (n == 0) return false;
            handleTouch(h);
            wh.wSent += n;
        }
        if (wh.bodyRemain == 0) return true;
        size_t toRead = sizeof(wh.wbuf) < wh.bodyRemain ? sizeof(wh.wbuf) : wh.bodyRemain;
        if (wh.file >= 0) {
            wh.wLen = (int)fs_read(wh.wbuf, 1, toRead, wh.file);
        } else if (wh.genBuf) {
            size_t offset = wh.genLen - wh.bodyRemain;
            memcpy(wh.wbuf, wh.genBuf + offset, toRead);
            wh.wLen = (int)toRead;
        } else {
            return true;
        }
        wh.wSent = 0;
        if (wh.wLen == 0) return true;
        wh.bodyRemain -= wh.wLen;
    }
}

/* ---- Task function ---- */

/* Defined in auth_web.cpp — registers the auth URL prefix handler. Called
 * once from webTaskFn after the URL-handler dispatch table is ready. */
extern "C" void authWebInit();

static void webTaskFn(void* arg) {
    webMaxHandles = storageGetInt("s.web.max_connections", 8);
    if (webMaxHandles < 2) webMaxHandles = 2;
    if (webMaxHandles > 16) webMaxHandles = 16;
    handles = new web_handle_t[webMaxHandles];
    for (int i = 0; i < webMaxHandles; i++) handles[i].itsHandle = -1;
    itsServerInit();
    /* Open ports 80 and 443 with the same handlers — web treats both
     * the same; the per-conn `tls` flag is in net_connect_t. */
    for (uint16_t p : { WEB_HTTP_PORT, WEB_HTTPS_PORT }) {
        itsServerPortOpen(p, false, webMaxHandles, WEB_ITS_TO_SIZE, WEB_ITS_FROM_SIZE);
        itsServerOnConnect(p, webItsConnect);
        itsServerOnBusy(p, webItsBusy);
        itsServerOnDisconnect(p, webItsDisconnect);
    }
    itsOnAux(WEB_PATH_REG_PORT,     webOnAux);
    itsOnAux(WEB_PATH_HANDLER_PORT, webOnHandlerAux);
    itsOnAux(WEB_FILE_DONE_PORT,    webOnFileDone);

    loadMappings();
    loadMimeTypes();
    /* auth core (storage defaults, CLI, login/passwd/logout API) was already
     * brought up by spangapInit(). authWebInit just wires the HTTP face. */
    authWebInit();

    storageSubscribeChanges("s.web.", ON_CHANGE {
        loadMappings();
        loadMimeTypes();
    });

    /* Close all live clients when there's no WiFi at all. Subscribed to both
     * wifi.{sta,ap}.up: each fires once per transition, the netIsUp() guard
     * skips fires where the other interface is still up. */
    static auto onWifiUpChange = [](const char*, const char*) {
        if (netIsUp()) return;
        for (int i = 0; i < webMaxHandles; i++) {
            if (handles[i].state != HS_IDLE && handles[i].itsHandle >= 0) {
                int itsH = handles[i].itsHandle;
                handleReset(i);
                handles[i].itsHandle = -1;
                itsDisconnect(itsH);
            }
        }
    };
    storageSubscribeChanges("wifi.sta.up", onWifiUpChange);
    storageSubscribeChanges("wifi.ap.up",  onWifiUpChange);

    /* Register HTTP/HTTPS ports with network */
    { net_port_msg_t reg = {};
      reg.itsPort = WEB_HTTP_PORT;
      safeStrncpy(reg.nvsKey, "http_port", sizeof(reg.nvsKey));
      reg.defaultPort = 80;
      reg.keepAlive = 1;
      while (!itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500)))
          vTaskDelay(pdMS_TO_TICKS(100));
    }
    { net_port_msg_t reg = {};
      reg.itsPort = WEB_HTTPS_PORT;
      safeStrncpy(reg.nvsKey, "https_port", sizeof(reg.nvsKey));
      reg.defaultPort = 443;
      reg.tls = 1;
      reg.keepAlive = 1;
      while (!itsSendAux("net", NET_PORT_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500)))
          vTaskDelay(pdMS_TO_TICKS(100));
    }

    info("ready (%d maps, %d mime types)\n", webMapCount, webMimeCount);

    for (;;) {
        bool anyActive = false;
        uint32_t now = millis();

        for (int h = 0; h < webMaxHandles; h++) {
            auto& wh = handles[h];
            if (wh.state == HS_IDLE || wh.itsHandle < 0) continue;
            /* Streamer is pushing the response on this handle — defer everything
             * until WEB_FILE_DONE_PORT aux fires. Counts as activity. */
            if (wh.sending) { anyActive = true; continue; }
            if (!itsConnected(wh.itsHandle)) { handleReset(h); wh.itsHandle = -1; continue; }

            if (now >= wh.deadline) {
                int itsH = wh.itsHandle;
                handleReset(h);
                wh.itsHandle = -1;
                itsDisconnect(itsH);
                continue;
            }
            anyActive = true;

            if (wh.state == HS_READING) {
                /* Drain any undrained body bytes from a previous rejected request */
                if (wh.skipRemain > 0) {
                    uint8_t discard[256];
                    size_t toRead = wh.skipRemain < sizeof(discard) ? wh.skipRemain : sizeof(discard);
                    size_t n = itsRecv(wh.itsHandle, discard, toRead, 0);
                    if (n > 0) { wh.skipRemain -= n; handleTouch(h); }
                } else {
                    int space = (int)sizeof(wh.rbuf) - wh.rLen;
                    if (space > 0) {
                        size_t n = itsRecv(wh.itsHandle, wh.rbuf + wh.rLen, space, 0);
                        if (n > 0) {
                            wh.rLen += n;
                            handleTouch(h);
                            tryParseRequest(h);
                        }
                    }
                }
            }

            if (wh.state == HS_RECV_BODY) {
                if (wh.putChunked) {
                    /* Chunked: read + process in tight loop */
                    for (int reads = 0; reads < 64; reads++) {
                        int space = (int)sizeof(wh.rbuf) - wh.rLen;
                        if (space > 0) {
                            size_t n = itsRecv(wh.itsHandle, wh.rbuf + wh.rLen, space, 0);
                            if (n > 0) { wh.rLen += n; handleTouch(h); }
                            else if (wh.rLen == 0) break;
                        }
                        bool done = false;
                        while (wh.rLen > 0) {
                            if (wh.putRemain > 0) {
                                size_t toWrite = (size_t)wh.rLen < wh.putRemain ? (size_t)wh.rLen : wh.putRemain;
                                fs_write(wh.rbuf, 1, toWrite, wh.putFile);
                                wh.putRemain -= toWrite;
                                wh.rLen -= toWrite;
                                if (wh.rLen > 0) memmove(wh.rbuf, wh.rbuf + toWrite, wh.rLen);
                            } else {
                                while (wh.rLen >= 2 && wh.rbuf[0] == '\r' && wh.rbuf[1] == '\n') {
                                    wh.rLen -= 2;
                                    if (wh.rLen > 0) memmove(wh.rbuf, wh.rbuf + 2, wh.rLen);
                                }
                                if (wh.rLen == 0) break;
                                char* crlf = (char*)memmem(wh.rbuf, wh.rLen, "\r\n", 2);
                                if (!crlf) break;
                                *crlf = '\0';
                                char* endp;
                                size_t chunkSz = strtoul(wh.rbuf, &endp, 16);
                                int lineLen = (int)(crlf + 2 - wh.rbuf);
                                wh.rLen -= lineLen;
                                if (wh.rLen > 0) memmove(wh.rbuf, crlf + 2, wh.rLen);
                                if (chunkSz == 0 && endp != wh.rbuf) {
                                    fs_close(wh.putFile); wh.putFile = -1;
                                    wh.putChunked = false; wh.rLen = 0;
                                    serve201(h); done = true;
                                    break;
                                }
                                wh.putRemain = chunkSz;
                            }
                        }
                        if (done) break;
                    }
                } else {
                    size_t space = sizeof(wh.wbuf) < wh.putRemain ? sizeof(wh.wbuf) : wh.putRemain;
                    size_t n = itsRecv(wh.itsHandle, wh.wbuf, space, 0);
                    if (n > 0) {
                        fs_write(wh.wbuf, 1, n, wh.putFile);
                        wh.putRemain -= n;
                        handleTouch(h);
                        if (wh.putRemain == 0) {
                            fs_close(wh.putFile); wh.putFile = -1;
                            serve201(h);
                        }
                    }
                }
            }

            if (wh.state == HS_SEND_HDR) {
                if (trySendHdr(h)) {
                    if (wh.file >= 0 || wh.genBuf) wh.state = HS_SEND_BODY;
                    else { wh.state = HS_READING; handleTouch(h); }
                }
            }
            if (wh.state == HS_SEND_BODY) {
                if (trySendBody(h)) {
                    if (wh.file >= 0) { fs_close(wh.file); wh.file = -1; }
                    if (wh.genBuf) { heap_caps_free(wh.genBuf); wh.genBuf = nullptr; }
                    wh.state = HS_READING;
                    handleTouch(h);
                }
            }
        }

        /* Sleep — woken by ITS notifications from network.
         * Must be >= 10ms: pdMS_TO_TICKS(3) = 0 at 100Hz tick rate = busy spin. */
        { static uint32_t loopCount = 0, lastReport = 0;
          loopCount++;
          if (millis() - lastReport >= 5000) {
            dbg("loop %u/s active=%d conns=%d\n",
                (unsigned)(loopCount * 1000 / (millis() - lastReport + 1)),
                anyActive, itsActiveTotal());
            loopCount = 0; lastReport = millis();
          }
        }
        /* First itsPoll blocks (10ms when active for send retries, forever when idle).
         * Subsequent calls drain without blocking. */
        { TickType_t t = anyActive ? pdMS_TO_TICKS(10) : portMAX_DELAY;
          while (itsPoll(t)) { t = 0; } }
    }
}

static void cmdWeb(const char* a) {
    if (cliWantsHelp(a)) { cliPrintf("%-*s show web server routes\n", CLI_HELP_COL, "web"); return; }
    cliPrintf("file mappings:\n");
    for (int i = 0; i < webMapCount; i++)
        cliPrintf("%s%s -> %s%s%s\n", webMaps[i].url.empty() ? "" : "/", webMaps[i].url.c_str(), webMaps[i].files.c_str(),
                  webMaps[i].dirIndex ? " [index]" : "", webMaps[i].dav ? " [dav]" : "");
    cliPrintf("registered paths:\n");
    for (int i = 0; i < webPathCount; i++) {
        if (webPaths[i].kind == WEB_PATH_FORWARD)
            cliPrintf("/%s -> [%s]:%d\n", webPaths[i].path,
                      pcTaskGetName(webPaths[i].fwd.task), webPaths[i].fwd.itsPort);
        else
            cliPrintf("/%s -> handler\n", webPaths[i].path);
    }
}

/* Append a URL→filesystem mapping to s.web.map if not already present.
 * Used by webInit (base mappings) and modules like acme (/.well-known). */
void webMapAddIfAbsent(const char* url, const char* files,
                       int index_dirs, int dav, const char* auth) {
    int n = storageArrayCount("s.web.map");
    for (int i = 0; i < n; i++) {
        char k[40], v[64];
        snprintf(k, sizeof(k), "s.web.map.%d.url", i);
        storageGetStr(k, v, sizeof(v));
        if (strcmp(v, url) == 0) return;  /* already there */
    }
    char k[40];
    snprintf(k, sizeof(k), "s.web.map.%d.url", n);
    storageSet(k, url);
    snprintf(k, sizeof(k), "s.web.map.%d.files", n);
    storageSet(k, files);
    if (index_dirs) {
        snprintf(k, sizeof(k), "s.web.map.%d.index", n);
        storageSet(k, 1);
    }
    if (dav) {
        snprintf(k, sizeof(k), "s.web.map.%d.dav", n);
        storageSet(k, 1);
    }
    if (auth) {
        snprintf(k, sizeof(k), "s.web.map.%d.auth", n);
        storageSet(k, auth);
    }
}

/* Module config version. Bump when adding/changing defaults. See duckdns.cpp.
 * MIME types live here as a normal default. */
#define WEB_VERSION 1

void webInit() {
    int v = storageGetInt("s.web.version", 0);
    if (v < WEB_VERSION) {
        storageDefaultTree("s.web", R"({
            "max_connections": 8,
            "https_only": 1,
            "http_allowed": ["/.well-known"],
            "mime": [
                { "ext": "html,htm",    "type": "text/html" },
                { "ext": "css",         "type": "text/css" },
                { "ext": "js",          "type": "application/javascript" },
                { "ext": "json",        "type": "application/json" },
                { "ext": "png",         "type": "image/png" },
                { "ext": "jpg,jpeg",    "type": "image/jpeg" },
                { "ext": "ico",         "type": "image/x-icon" },
                { "ext": "svg",         "type": "image/svg+xml" },
                { "ext": "txt,log,csv", "type": "text/plain" },
                { "ext": "avi",         "type": "video/x-msvideo" },
                { "ext": "wav",         "type": "audio/wav" }
            ]
        })");
        storageSet("s.web.version", WEB_VERSION);
    }

    /* Advertise our services over mDNS (net owns the mechanism; we own these
     * entries). The value is the port's config key, not a literal, so the
     * advertisement follows s.net.{http,https}_port. storageDefault is
     * idempotent, so a user who drops an entry to stop advertising isn't
     * overridden on the next boot. */
    storageDefault("s.net.mdns.http",  "s.net.http_port");
    storageDefault("s.net.mdns.https", "s.net.https_port");

    /* Base URL→filesystem mappings. webMapAddIfAbsent only adds if the URL
     * isn't already present — user customisations to s.web.map survive. */
    webMapAddIfAbsent("/",      "/fixed/webroot", 0, 0, nullptr);
    webMapAddIfAbsent("/state", "/state",         1, 1, "admin");
    webMapAddIfAbsent("/fixed", "/fixed",         1, 1, "admin");
#if CONFIG_SPANGAP_SDCARD
    webMapAddIfAbsent("/sdcard", "/sdcard",       1, 1, "admin");
#endif

    cliRegisterCmd("web", cmdWeb);
    /* 8 KB (was 5 KB): the file worker now also runs extension transforms
     * (e.g. viewer's markdown→HTML via MD4C) and gzip inflate inline; the
     * decompressor state and large buffers are on the heap, but MD4C parsing
     * wants a few KB of stack headroom over plain file streaming. */
    spawnTask(webFileWorkerFn, "web_file", 8192, nullptr, 1, 1, STACK_DRAM);
    webHandle = spawnTask(webTaskFn, "web", 8192, nullptr, 1, 1);
}

/* ---- WebSocket convenience functions (for tasks with forwarded WS connections) ---- */

int webGetHeader(int itsHandle, char* buf, int maxLen, int timeoutMs) {
    int total = 0;
    TickType_t ticks = pdMS_TO_TICKS(timeoutMs);
    while (total < maxLen - 1) {
        size_t n = itsRecv(itsHandle, buf + total, maxLen - 1 - total, ticks);
        if (n == 0) break;
        total += n;
        buf[total] = '\0';
        if (total >= 4 && strstr(buf + (total > 64 ? total - 64 : 0), "\r\n\r\n"))
            return total;
    }
    return total > 0 ? total : -1;
}

char* webReadBody(int itsHandle, const char* hdr, int total,
                  size_t maxLen, int* outLen) {
    /* Locate the headers/body boundary inside hdr[]. Body bytes after the
     * \r\n\r\n are already in the buffer (they came in the same TCP segment
     * as the headers); we copy them first, then itsRecv the remainder. */
    const char* boundary = (const char*)memmem(hdr, total, "\r\n\r\n", 4);
    int hdrEnd = boundary ? (int)((boundary - hdr) + 4) : total;
    int bodyInBuf = total - hdrEnd;

    char clStr[16] = {};
    webHeaderField(hdr, hdrEnd, "Content-Length", clStr, sizeof(clStr));
    int contentLen = atoi(clStr);
    if (contentLen <= 0 || (size_t)contentLen > maxLen) return nullptr;

    char* body = (char*)heap_caps_malloc(contentLen + 1, MALLOC_CAP_SPIRAM);
    if (!body) return nullptr;

    int pos = 0;
    if (bodyInBuf > 0) {
        int copy = bodyInBuf < contentLen ? bodyInBuf : contentLen;
        memcpy(body, hdr + hdrEnd, copy);
        pos = copy;
    }
    while (pos < contentLen) {
        size_t n = itsRecv(itsHandle, body + pos, contentLen - pos, pdMS_TO_TICKS(500));
        if (n == 0) break;
        pos += (int)n;
    }
    body[pos] = '\0';
    if (outLen) *outLen = pos;
    return body;
}

bool webHeaderField(const char* headers, int headersLen,
                    const char* field, char* out, int outLen) {
    out[0] = '\0';
    int flen = strlen(field);
    for (const char* p = headers; p < headers + headersLen; ) {
        const char* eol = (const char*)memchr(p, '\n', headersLen - (p - headers));
        if (!eol) break;
        /* Case-insensitive field match: "Field:" */
        if (eol - p > flen + 1 && p[flen] == ':' && strncasecmp(p, field, flen) == 0) {
            const char* val = p + flen + 1;
            while (*val == ' ') val++;
            int vlen = 0;
            while (val + vlen < eol && val[vlen] != '\r' && val[vlen] != '\n') vlen++;
            if (vlen >= outLen) vlen = outLen - 1;
            memcpy(out, val, vlen);
            out[vlen] = '\0';
            return true;
        }
        p = eol + 1;
    }
    return false;
}

bool webExtractCookie(const char* headers, int headersLen,
                      const char* name, char* out, int outLen) {
    out[0] = '\0';
    char cookieHdr[512];
    if (!webHeaderField(headers, headersLen, "Cookie", cookieHdr, sizeof(cookieHdr)))
        return false;

    int nlen = strlen(name);
    const char* p = cookieHdr;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char* val = p + nlen + 1;
            int i = 0;
            while (val[i] && val[i] != ';' && val[i] != ' ' && i < outLen - 1)
                { out[i] = val[i]; i++; }
            out[i] = '\0';
            return true;
        }
        /* Skip to next cookie */
        while (*p && *p != ';') p++;
    }
    return false;
}

bool webGetMethod(const char* hdr, int hdrLen, char* out, int outLen) {
    int i = 0;
    while (i < hdrLen && i < outLen - 1 && hdr[i] != ' ' && hdr[i] != '\r')
        { out[i] = hdr[i]; i++; }
    out[i] = '\0';
    return i > 0;
}

bool webGetPath(const char* hdr, int hdrLen, char* out, int outLen) {
    const char* p = (const char*)memchr(hdr, '/', hdrLen);
    if (!p) { out[0] = '\0'; return false; }
    p++;  /* skip leading / */
    int i = 0;
    while (p + i < hdr + hdrLen && i < outLen - 1 &&
           p[i] != ' ' && p[i] != '?' && p[i] != '\r')
        { out[i] = p[i]; i++; }
    out[i] = '\0';
    return true;
}

bool webGetQuery(const char* hdr, int hdrLen,
                 const char* key, char* out, int outLen) {
    out[0] = '\0';
    /* Find query string in request line */
    const char* eol = (const char*)memchr(hdr, '\r', hdrLen);
    if (!eol) eol = hdr + hdrLen;
    const char* q = (const char*)memchr(hdr, '?', eol - hdr);
    if (!q) return false;
    q++;
    int klen = strlen(key);
    while (q < eol) {
        if (strncmp(q, key, klen) == 0 && q[klen] == '=') {
            const char* val = q + klen + 1;
            int i = 0;
            while (val + i < eol && i < outLen - 1 && val[i] != '&' && val[i] != ' ')
                { out[i] = val[i]; i++; }
            out[i] = '\0';
            return true;
        }
        const char* amp = (const char*)memchr(q, '&', eol - q);
        if (!amp) break;
        q = amp + 1;
    }
    return false;
}

static const char* statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 207: return "Multi-Status";
        case 301: return "Moved Permanently";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 503: return "Service Unavailable";
        default:  return "Error";
    }
}

bool webSendResponse(int itsHandle, int status, const char* contentType,
                     const void* body, size_t bodyLen) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Connection: keep-alive\r\n\r\n",
        status, statusText(status), contentType, (unsigned)bodyLen);
    if (!itsSendAll(itsHandle, hdr, n)) return false;
    if (bodyLen > 0) return itsSendAll(itsHandle, body, bodyLen);
    return true;
}

bool webSendStatus(int itsHandle, int status) {
    char hdr[128];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n",
        status, statusText(status));
    return itsSendAll(itsHandle, hdr, n);
}

bool wsUpgrade(int itsHandle, const char* hdr, int hdrLen) {
    char wsKey[32];
    if (!webHeaderField(hdr, hdrLen, "Sec-WebSocket-Key", wsKey, sizeof(wsKey)))
        return false;
    char accept[64];
    wsComputeAccept(wsKey, accept, sizeof(accept));
    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    return itsSendAll(itsHandle, resp, rlen);
}

bool wsUpgrade(int itsHandle, bool must, int timeoutMs) {
    char hdr[1024];
    int hdrLen = webGetHeader(itsHandle, hdr, sizeof(hdr), timeoutMs);
    if (hdrLen <= 0) return false;
    char upgrade[32];
    if (!webHeaderField(hdr, hdrLen, "Upgrade", upgrade, sizeof(upgrade)) ||
        strcasecmp(upgrade, "websocket") != 0) {
        if (!must) itsInject(itsHandle, false, hdr, hdrLen);
        return false;
    }
    return wsUpgrade(itsHandle, hdr, hdrLen);
}

static bool wsSendFrame(int itsHandle, const void* data, size_t len, uint8_t opcode) {
    uint8_t hdr[4];
    int hdrLen;
    hdr[0] = 0x80 | opcode;
    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hdrLen = 2;
    } else {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len & 0xff);
        hdrLen = 4;
    }
    if (!itsSendAll(itsHandle, hdr, hdrLen)) return false;
    if (len > 0 && !itsSendAll(itsHandle, data, len)) return false;
    return true;
}

bool wsSendText(int itsHandle, const char* data, size_t len) {
    return wsSendFrame(itsHandle, data, len, 0x01);
}

bool wsSendBinary(int itsHandle, const void* data, size_t len) {
    return wsSendFrame(itsHandle, data, len, 0x02);
}

void wsSendClose(int itsHandle) {
    uint8_t cls[] = {0x88, 0x00};
    itsSendAll(itsHandle, cls, 2);
}

void wsSendClose(int itsHandle, uint16_t code) {
    uint8_t cls[] = {0x88, 0x02, (uint8_t)(code >> 8), (uint8_t)(code & 0xff)};
    itsSendAll(itsHandle, cls, 4);
}

int wsReadFrame(int itsHandle, uint8_t* buf, size_t bufSize, size_t* outLen,
                bool* binary) {
    if (binary) *binary = false;
    *outLen = 0;
    /* Check if data available */
    if (itsBytesAvailable(itsHandle) < 2) return 0;

    uint8_t hdr[2];
    if (itsRecv(itsHandle, hdr, 2, pdMS_TO_TICKS(10)) != 2) return -1;
    int opcode = hdr[0] & 0x0f;
    bool masked = (hdr[1] & 0x80) != 0;
    size_t len = hdr[1] & 0x7f;

    if (len == 126) {
        uint8_t ext[2];
        if (itsRecv(itsHandle, ext, 2, pdMS_TO_TICKS(10)) != 2) return -1;
        len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
        return -1;
    }

    uint8_t mask[4] = {};
    if (masked && itsRecv(itsHandle, mask, 4, pdMS_TO_TICKS(10)) != 4) return -1;
    if (len > bufSize) return -1;

    size_t got = 0;
    while (got < len) {
        size_t n = itsRecv(itsHandle, buf + got, len - got, pdMS_TO_TICKS(50));
        if (n == 0) return -1;
        got += n;
    }

    if (masked)
        for (size_t i = 0; i < len; i++) buf[i] ^= mask[i & 3];

    if (opcode == 0x08) { wsSendClose(itsHandle); return -1; }
    if (opcode == 0x09) {
        wsSendFrame(itsHandle, buf, len, 0x0A);  /* pong */
        return wsReadFrame(itsHandle, buf, bufSize, outLen, binary);
    }

    *outLen = len;
    if (binary) *binary = (opcode == 2);
    return (opcode == 1) ? 1 : (opcode == 2) ? 2 : 1;
}
