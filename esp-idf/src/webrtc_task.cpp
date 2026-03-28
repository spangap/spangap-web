/**
 * webrtc_task — WebRTC DataChannel: ICE-lite + DTLS + SCTP + camera/audio streaming.
 *
 * Registers /webrtc WebSocket endpoint via ITS. Browser sends SDP offer, we reply
 * with SDP answer. Browser does ICE checks + DTLS handshake on our UDP port.
 * After SCTP association + DCEP, we stream JPEG frames and audio as binary
 * DataChannel messages — fire-and-forget UDP, no TCP window limits.
 */
#include "webrtc_task.h"
#include "webrtc_sctp.h"
#include "storage.h"
#include "compat.h"
#include "pm.h"
#include "log.h"
#include "its.h"
#include "tls.h"
#include "camera.h"
#include "audio.h"
#include "net.h"
#include "web.h"
#include "net.h"

#include "esp_netif.h"
#include <cstring>
#include <cstdio>
#include <string>
#include <lwip/sockets.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_rom_crc.h"

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cookie.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/sha1.h"
#include "mbedtls/md.h"
#include "mbedtls/error.h"

/* ---- Byte-order helpers ---- */

static inline uint16_t r16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static inline uint32_t r32(const uint8_t* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static inline void w16(uint8_t* p, uint16_t v) { p[0]=v>>8; p[1]=v; }
static inline void w32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

/* ---- Constants ---- */

static const uint16_t SCTP_PORT = 5000;      /* from SDP a=sctp-port */
static const size_t   SCTP_BUF_SIZE = 2048;  /* outgoing SCTP packet buffer */

/* ---- Task state ---- */

static TaskHandle_t webrtcHandle = nullptr;
static pm_lock_handle_t webrtcLockLS = nullptr;
static pm_lock_handle_t webrtcLockCPU = nullptr;

/* Network */
static int udpFd = -1;
static struct sockaddr_in peerAddr = {};
static bool peerKnown = false;

/* ICE credentials */
static char iceUfrag[8];
static char icePwd[28];

/* DTLS */
static mbedtls_ssl_context dtls;
static mbedtls_ssl_config dtlsConf;
static mbedtls_ssl_cookie_ctx cookieCtx;
static bool dtlsReady = false;       /* one-time config initialized */
static bool dtlsSessionActive = false; /* per-session ssl context initialized */
static bool dtlsConnected = false;   /* handshake complete */

/* DTLS timer state (custom, since MBEDTLS_TIMING_C is disabled on ESP-IDF) */
static uint32_t timerStart;
static uint32_t timerIntMs;
static uint32_t timerFinMs;

/* SCTP */
static sctp_assoc_t sctp;
static uint8_t sctpBuf[SCTP_BUF_SIZE];

/* Handshake dedup: track last handshake packet to avoid retransmit re-processing */
static size_t lastHandshakeLen = 0;

/* Streaming */
#define MAX_JPEG_SIZE (200 * 1024)
static uint8_t* frameBuf = nullptr;   /* PSRAM buffer for JPEG frame copy */
static bool camSubscribed = false;
static bool streaming = false;
static uint32_t streamStartMs = 0;

/* Audio: 5-byte header [codec(1) timestamp(4)] + 1280 bytes encoded data */
#define DC_AUDIO_DATA  1280
#define DC_AUDIO_HDR   5
static uint8_t audioBuf[DC_AUDIO_HDR + DC_AUDIO_DATA];

/* Inactivity timeout: no UDP packets for this long → tear down session */
static constexpr uint32_t UDP_TIMEOUT_MS = 10000;
static uint32_t lastUdpRxMs = 0;

/* ITS signaling */
static int itsHandle = -1;  /* current signaling WS client */

/* ---- DTLS timer callbacks ---- */

static void dcTimerSet(void* ctx, uint32_t intMs, uint32_t finMs) {
    (void)ctx;
    timerStart = millis();
    timerIntMs = intMs;
    timerFinMs = finMs;
}

static int webrtcTimerGet(void* ctx) {
    (void)ctx;
    if (timerFinMs == 0) return -1; /* cancelled */
    uint32_t elapsed = millis() - timerStart;
    if (elapsed >= timerFinMs) return 2;
    if (elapsed >= timerIntMs) return 1;
    return 0;
}

/* ---- DTLS BIO callbacks (UDP sendto/recvfrom) ---- */

/* Send buffer for DTLS output — goes straight to UDP */
static int webrtcBioSend(void* ctx, const unsigned char* buf, size_t len) {
    (void)ctx;
    if (!peerKnown) return MBEDTLS_ERR_SSL_WANT_WRITE;
    int n = sendto(udpFd, buf, len, MSG_DONTWAIT,
                   (struct sockaddr*)&peerAddr, sizeof(peerAddr));
    if (n > 0) netTrafficOut(n);
    /* Log DTLS record header: type(1) version(2) epoch(2) seq(6) length(2) = 13 bytes */
    if (len >= 13) {
        uint16_t epoch = (buf[3]<<8)|buf[4];
        uint64_t seq = ((uint64_t)buf[5]<<40)|((uint64_t)buf[6]<<32)|
                       ((uint64_t)buf[7]<<24)|((uint64_t)buf[8]<<16)|
                       ((uint64_t)buf[9]<<8)|(uint64_t)buf[10];
        dbg("WebRTC: BIO send %d bytes, DTLS type=%d ver=%02x%02x epoch=%d seq=%llu len=%d\n",
             (int)len, buf[0], buf[1], buf[2], epoch, seq, (buf[11]<<8)|buf[12]);
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == ENOMEM) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

/* Receive buffer — filled by main loop before calling mbedtls_ssl_handshake/read */
static uint8_t* bioRecvBuf = nullptr;
static size_t bioRecvLen = 0;

static int webrtcBioRecv(void* ctx, unsigned char* buf, size_t len) {
    (void)ctx;
    if (!bioRecvBuf || bioRecvLen == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = bioRecvLen < len ? bioRecvLen : len;
    memcpy(buf, bioRecvBuf, n);
    bioRecvBuf = nullptr;
    bioRecvLen = 0;
    return (int)n;
}

/* ---- ICE-lite: STUN binding request/response ---- */

static const uint32_t STUN_MAGIC = 0x2112A442;

static bool isStunPacket(const uint8_t* buf, size_t len) {
    if (len < 20) return false;
    /* First 2 bits must be 0, magic cookie at offset 4 */
    if (buf[0] & 0xC0) return false;
    return r32(buf + 4) == STUN_MAGIC;
}

static void handleStunRequest(const uint8_t* req, size_t reqLen,
                              const struct sockaddr_in* from) {
    if (reqLen < 20) return;
    uint16_t msgType = r16(req);
    if (msgType != 0x0001) {
        dbg("WebRTC: STUN non-binding msg type 0x%04x\n", msgType);
        return;
    }
    dbg("WebRTC: STUN binding request from %s:%d\n",
        inet_ntoa(from->sin_addr), ntohs(from->sin_port));

    uint8_t resp[256];
    size_t pos = 0;

    /* Response header: Binding Success Response (0x0101) */
    w16(resp, 0x0101);
    pos = 2;
    pos += 2; /* message length placeholder */
    memcpy(resp + 4, req + 4, 4);   /* magic cookie */
    memcpy(resp + 8, req + 8, 12);  /* transaction ID */
    pos = 20;

    /* XOR-MAPPED-ADDRESS attribute (type=0x0020) */
    w16(resp + pos, 0x0020); pos += 2;
    w16(resp + pos, 8); pos += 2;          /* attr length */
    resp[pos++] = 0;                        /* reserved */
    resp[pos++] = 0x01;                     /* family: IPv4 */
    uint16_t xPort = ntohs(from->sin_port) ^ (uint16_t)(STUN_MAGIC >> 16);
    w16(resp + pos, xPort); pos += 2;
    uint32_t xAddr = ntohl(from->sin_addr.s_addr) ^ STUN_MAGIC;
    w32(resp + pos, xAddr); pos += 4;

    /* MESSAGE-INTEGRITY (HMAC-SHA1 over message up to this point, keyed by ice-pwd) */
    /* Temporarily set message length to include MESSAGE-INTEGRITY (24 bytes) */
    w16(resp + 2, (uint16_t)(pos - 20 + 24));
    uint8_t hmac[20];
    {
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);
        mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
        mbedtls_md_hmac_starts(&ctx, (const uint8_t*)icePwd, strlen(icePwd));
        mbedtls_md_hmac_update(&ctx, resp, pos);
        mbedtls_md_hmac_finish(&ctx, hmac);
        mbedtls_md_free(&ctx);
    }

    w16(resp + pos, 0x0008); pos += 2; /* MESSAGE-INTEGRITY type */
    w16(resp + pos, 20); pos += 2;     /* length */
    memcpy(resp + pos, hmac, 20); pos += 20;

    /* FINGERPRINT (standard CRC32 ISO 3309, NOT CRC32C — different polynomial) */
    w16(resp + 2, (uint16_t)(pos - 20 + 8)); /* update length for fingerprint */
    uint32_t fp = esp_rom_crc32_le(0, resp, pos) ^ 0x5354554E;
    w16(resp + pos, 0x8028); pos += 2;
    w16(resp + pos, 4); pos += 2;
    w32(resp + pos, fp); pos += 4;

    /* Final message length */
    w16(resp + 2, (uint16_t)(pos - 20));

    int sent = sendto(udpFd, resp, pos, MSG_DONTWAIT,
                      (const struct sockaddr*)from, sizeof(*from));
    if (sent > 0) netTrafficOut(sent);
}

/* ---- DTLS setup / teardown ---- */

static void dtlsSetup() {
    if (dtlsReady) return;
    if (!tlsReady()) return;

    int ret;
    mbedtls_ssl_config_init(&dtlsConf);
    mbedtls_ssl_cookie_init(&cookieCtx);

    ret = mbedtls_ssl_config_defaults(&dtlsConf, MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { err("dtls config: -0x%04x\n", -ret); return; }

    mbedtls_ssl_conf_rng(&dtlsConf, mbedtls_ctr_drbg_random, tlsGetRng());
    mbedtls_ssl_conf_ca_chain(&dtlsConf, tlsGetCert(), nullptr);
    ret = mbedtls_ssl_conf_own_cert(&dtlsConf, tlsGetCert(), tlsGetKey());
    if (ret != 0) { err("dtls cert: -0x%04x\n", -ret); return; }

    mbedtls_ssl_conf_authmode(&dtlsConf, MBEDTLS_SSL_VERIFY_NONE);

    /* ChaCha20-Poly1305 only — no heap allocation per record (unlike AES-GCM) */
    static const int ciphersuites[] = {
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        0
    };
    mbedtls_ssl_conf_ciphersuites(&dtlsConf, ciphersuites);

    /* DTLS cookie for DoS protection */
    ret = mbedtls_ssl_cookie_setup(&cookieCtx, mbedtls_ctr_drbg_random, tlsGetRng());
    if (ret != 0) { err("dtls cookie: -0x%04x\n", -ret); return; }
    mbedtls_ssl_conf_dtls_cookies(&dtlsConf,
                                   mbedtls_ssl_cookie_write,
                                   mbedtls_ssl_cookie_check,
                                   &cookieCtx);

    dtlsReady = true;
}

/* Export DTLS master secret for Wireshark decryption */
static void dtlsExportKeys(void *p_expkey,
                           mbedtls_ssl_key_export_type type,
                           const unsigned char *secret, size_t secret_len,
                           const unsigned char client_random[32],
                           const unsigned char server_random[32],
                           mbedtls_tls_prf_types tls_prf_type) {
    (void)p_expkey; (void)server_random; (void)tls_prf_type;
    if (type == MBEDTLS_SSL_KEY_EXPORT_TLS12_MASTER_SECRET) {
        char line[256];
        int pos = snprintf(line, sizeof(line), "CLIENT_RANDOM ");
        for (int i = 0; i < 32; i++)
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x", client_random[i]);
        pos += snprintf(line + pos, sizeof(line) - pos, " ");
        for (size_t i = 0; i < secret_len; i++)
            pos += snprintf(line + pos, sizeof(line) - pos, "%02x", secret[i]);
        dbg("WebRTC: KEYLOG %s\n", line);
    }
}

static void dtlsSessionInit() {
    mbedtls_ssl_init(&dtls);
    int ret = mbedtls_ssl_setup(&dtls, &dtlsConf);
    if (ret != 0) { err("dtls setup: -0x%04x\n", -ret); return; }

    mbedtls_ssl_set_bio(&dtls, nullptr, webrtcBioSend, webrtcBioRecv, nullptr);
    mbedtls_ssl_set_timer_cb(&dtls, nullptr, dcTimerSet, webrtcTimerGet);
    mbedtls_ssl_set_export_keys_cb(&dtls, dtlsExportKeys, nullptr);
    dtlsSessionActive = true;
}

static void dtlsSessionFree() {
    mbedtls_ssl_free(&dtls);
    dtlsSessionActive = false;
    dtlsConnected = false;
}

/* ---- Send SCTP data through DTLS ---- */

static int dtlsSctpSend(const uint8_t* pkt, size_t len, void* ctx) {
    (void)ctx;
    if (!dtlsConnected) return -1;
    int ret = mbedtls_ssl_write(&dtls, pkt, len);
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        err("WebRTC: DTLS write failed: -0x%04x\n", -ret);
        return -1;
    }
    dbg("WebRTC: DTLS write %d/%d bytes\n", ret, (int)len);
    return ret;
}

/* ---- SDP generation ---- */

static std::string generateSdpAnswer(const char* offerSdp) {
    /* Parse peer's ice-ufrag and ice-pwd from offer (for ICE, though we don't use them) */
    /* We only need our own credentials and cert fingerprint */

    int port = storageGetInt("s.net.webrtc_port", 0);

    char fingerprint[128] = {};
    tlsCertFingerprint(fingerprint, sizeof(fingerprint));

    /* Collect all interface IPs for ICE candidates */
    char ips[4][16] = {};
    int numIps = 0;

    /* WiFi STA */
    { esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if (netif) {
          esp_netif_ip_info_t info;
          if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
              esp_ip4addr_ntoa(&info.ip, ips[numIps], sizeof(ips[0]));
              numIps++;
          }
      }
    }
    /* WiFi AP */
    { esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
      if (netif) {
          esp_netif_ip_info_t info;
          if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
              esp_ip4addr_ntoa(&info.ip, ips[numIps], sizeof(ips[0]));
              numIps++;
          }
      }
    }
    /* WireGuard (check wg_address config) */
    { char wgAddr[16] = {};
      storageGetStr("s.wg.address", wgAddr, sizeof(wgAddr));
      if (wgAddr[0] && storageGetInt("wg.up", 0)) {
          safeStrncpy(ips[numIps], wgAddr, sizeof(ips[0]));
          numIps++;
      }
    }

    const char* primaryIp = numIps > 0 ? ips[0] : "0.0.0.0";

    /* SDP answer */
    std::string sdp;
    char line[512];
    snprintf(line, sizeof(line), "v=0\r\no=- %u 1 IN IP4 %s\r\ns=-\r\nt=0 0\r\na=ice-lite\r\n",
             (unsigned)esp_random(), primaryIp);
    sdp += line;
    snprintf(line, sizeof(line),
        "m=application %d UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "c=IN IP4 %s\r\n"
        "a=ice-ufrag:%s\r\n"
        "a=ice-pwd:%s\r\n"
        "a=fingerprint:%s\r\n"
        "a=setup:passive\r\n"
        "a=mid:0\r\n"
        "a=sctp-port:%d\r\n"
        "a=max-message-size:65536\r\n",
        port, primaryIp, iceUfrag, icePwd, fingerprint, SCTP_PORT);
    sdp += line;

    /* Add ICE candidate for each interface IP */
    for (int i = 0; i < numIps; i++) {
        snprintf(line, sizeof(line),
            "a=candidate:%d 1 UDP %u %s %d typ host\r\n",
            i + 1, (unsigned)(2130706431 - i), ips[i], port);
        sdp += line;
    }

    return sdp;
}

/* ---- ITS callbacks (signaling WS from web) ---- */

static void stopStreaming();
static bool webrtcWsClient = false;

static int webrtcItsConnect(int handle, int itsPort, const void* data, size_t len) {
    if (itsHandle >= 0) itsServerKick(itsHandle);
    itsHandle = handle;
    webrtcWsClient = false;
    if (len >= sizeof(net_connect_t) && ((const net_connect_t*)data)->ws) {
        if (!wsUpgrade(handle)) { info("WebRTC: WS upgrade failed\n"); return -1; }
        webrtcWsClient = true;
    }
    info("WebRTC: signaling client connected%s\n", webrtcWsClient ? " (WS)" : "");
    return 0;
}

static bool webrtcItsBusy(int itsPort, const void* data, size_t len) {
    /* Kick existing client */
    if (itsHandle >= 0) itsServerKick(itsHandle);
    return false; /* retry with freed slot */
}

static void webrtcItsDisconnect(int handle) {
    if (handle == itsHandle) itsHandle = -1;
    webrtcWsClient = false;
    info("WebRTC: signaling client disconnected\n");
    stopStreaming();
    if (dtlsSessionActive) dtlsSessionFree();
    peerKnown = false;
    lastUdpRxMs = 0;
    sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
}

/* ---- Signaling message handling ---- */

static void handleSignalingMsg(const char* msg, size_t len) {
    /* Look for {"type":"offer","sdp":"..."} */
    std::string_view sv(msg, len);

    auto findVal = [&](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\":\"";
        auto pos = sv.find(needle);
        if (pos == sv.npos) return "";
        pos += needle.size();
        auto end = sv.find('"', pos);
        if (end == sv.npos) return "";
        return std::string(sv.substr(pos, end - pos));
    };

    std::string type = findVal("type");
    if (type == "offer") {
        std::string sdpRaw = findVal("sdp");
        /* Unescape \\r\\n → \r\n */
        std::string sdpOffer;
        for (size_t i = 0; i < sdpRaw.size(); i++) {
            if (sdpRaw[i] == '\\' && i + 1 < sdpRaw.size()) {
                char c = sdpRaw[i + 1];
                if (c == 'r') { sdpOffer += '\r'; i++; }
                else if (c == 'n') { sdpOffer += '\n'; i++; }
                else if (c == '\\') { sdpOffer += '\\'; i++; }
                else sdpOffer += sdpRaw[i];
            } else {
                sdpOffer += sdpRaw[i];
            }
        }

        dbg("WebRTC: received SDP offer (%d bytes)\n", (int)sdpOffer.size());

        /* Ensure DTLS is ready (may have missed net.up notification if TLS wasn't ready) */
        dtlsSetup();
        if (!dtlsReady) {
            err("WebRTC: DTLS not ready (TLS cert missing?)\n");
            return;
        }

        /* Generate answer */
        std::string answer = generateSdpAnswer(sdpOffer.c_str());

        /* Escape for JSON */
        std::string escaped;
        for (char c : answer) {
            if (c == '\r') escaped += "\\r";
            else if (c == '\n') escaped += "\\n";
            else if (c == '"') escaped += "\\\"";
            else escaped += c;
        }

        std::string resp = "{\"type\":\"answer\",\"sdp\":\"" + escaped + "\"}";
        dbg("WebRTC: SDP answer:\n%s\n", answer.c_str());
        if (itsHandle >= 0) {
            if (webrtcWsClient)
                wsSendText(itsHandle, resp.c_str(), resp.size());
            else
                itsSend(itsHandle, resp.c_str(), resp.size(), pdMS_TO_TICKS(1000));
        }

        /* Reset DTLS + SCTP for new session */
        stopStreaming();
        if (dtlsConnected)
            dtlsSessionFree();
        dtlsSessionInit();
        sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
        peerKnown = false;
        lastHandshakeLen = 0;

        dbg("WebRTC: sent SDP answer, waiting for ICE+DTLS\n");
    }
}

/* ---- Start/stop streaming ---- */

static void onCameraFrame(const camera_fb_t* fb) {
    if (!streaming || !sctp.established) return;
    size_t copyLen = fb->len < MAX_JPEG_SIZE ? fb->len : MAX_JPEG_SIZE;
    memcpy(frameBuf, fb->buf, copyLen);
    int vidCh = sctpFindChannel(&sctp, "video");
    if (vidCh >= 0) {
        sctpSend(&sctp, sctp.channels[vidCh].streamId,
                 PPID_BINARY, frameBuf, copyLen,
                 dtlsSctpSend, nullptr);
    }
}

static void startStreaming() {
    if (streaming) return;

    /* Copy stored settings to ephemeral namespace */
    storageCopy("s.camera.", "camera.");
    storageCopy("s.audio.", "audio.");
    storageCopy("s.stream.camera.", "camera.");
    storageCopy("s.stream.audio.", "audio.");

    int fps = storageGetInt("camera.fps", 20);
    cameraSubscribe(fps, onCameraFrame);
    camSubscribed = true;

    /* Audio: subscribe with buffer sized for one DC bundle (1280 bytes) */
    static audio_meta_t audSettings, audSamples;
    audSettings.buf = audioBuf + DC_AUDIO_HDR;
    audSettings.len = DC_AUDIO_DATA;
    audSettings.ms = -1;
    audSettings.codec = AUDIO_PCM16;
    audSettings.hpf = true;
    audSettings.gain = storageGetInt("audio.gain", 1);
    int agcMax = storageGetInt("audio.agc_max", 0);
    if (agcMax > 0) {
        audSettings.agc_target = storageGetInt("audio.agc_target", 8000);
        audSettings.agc_attack = storageGetInt("audio.agc_attack", 10);
        audSettings.agc_release = storageGetInt("audio.agc_release", 500);
        audSettings.agc_max = agcMax;
    }
    audioSubscribe(&audSettings, &audSamples, []() {
        if (!streaming || !sctp.established) return;
        uint32_t ts = audSamples.timestamp - streamStartMs;
        audioBuf[0] = (uint8_t)audSamples.codec;
        audioBuf[1] = (uint8_t)(ts >> 24);
        audioBuf[2] = (uint8_t)(ts >> 16);
        audioBuf[3] = (uint8_t)(ts >> 8);
        audioBuf[4] = (uint8_t)(ts);
        int audCh = sctpFindChannel(&sctp, "audio");
        if (audCh >= 0) {
            sctpSend(&sctp, sctp.channels[audCh].streamId,
                     PPID_BINARY, audioBuf, DC_AUDIO_HDR + DC_AUDIO_DATA,
                     dtlsSctpSend, nullptr);
        }
    });

    pmLockAcquire(webrtcLockLS);
    pmLockAcquire(webrtcLockCPU);
    streaming = true;
    streamStartMs = millis();
    storageSet("webrtc.up", "1");
    info("WebRTC: streaming started\n");
}

static void stopStreaming() {
    if (!streaming) return;
    if (camSubscribed) { cameraUnsubscribe(); camSubscribed = false; }
    audioStop();
    sctpRexmitFree(&sctp);
    pmLockRelease(webrtcLockCPU);
    pmLockRelease(webrtcLockLS);
    streaming = false;
    storageSet("webrtc.up", "0");
    info("WebRTC: streaming stopped\n");
}

/* ---- UDP socket management ---- */

static void handleUdpPacket(const uint8_t* buf, size_t n,
                            const struct sockaddr_in* from) {
    dbg("UDP recv %d bytes from %s:%d\n", (int)n,
        inet_ntoa(from->sin_addr), ntohs(from->sin_port));

    peerAddr = *from;
    peerKnown = true;
    lastUdpRxMs = millis();

    if (isStunPacket(buf, n)) {
        handleStunRequest(buf, n, from);
        return;
    }
    if (!dtlsSessionActive) return;

    /* Set DTLS client transport ID on first non-STUN (DTLS) packet */
    if (!dtlsConnected) {
        uint8_t clientId[6];
        memcpy(clientId, &from->sin_addr, 4);
        memcpy(clientId + 4, &from->sin_port, 2);
        mbedtls_ssl_set_client_transport_id(&dtls, clientId, 6);
    } else if (n >= 1 && buf[0] != 23) {
        dbg("WebRTC: ignoring DTLS record type=%d after handshake\n", buf[0]);
        return;
    }

    /* Feed to DTLS */
    bioRecvBuf = (uint8_t*)buf;
    bioRecvLen = n;

    if (!dtlsConnected && dtlsSessionActive) {
        if (n == lastHandshakeLen) {
            dbg("WebRTC: skipping likely retransmit (%d bytes)\n", (int)n);
            return;
        }
        lastHandshakeLen = n;
        dbg("WebRTC: DTLS handshake step (%d bytes from %s)\n",
             (int)n, inet_ntoa(from->sin_addr));
        int ret = mbedtls_ssl_handshake(&dtls);
        if (ret == 0) {
            dtlsConnected = true;
            info("WebRTC: DTLS connected\n");
        } else if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
            dbg("WebRTC: DTLS hello verify required (normal)\n");
            mbedtls_ssl_session_reset(&dtls);
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                   ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf[128];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            err("WebRTC: DTLS handshake: -0x%04x %s\n", -ret, errbuf);
            mbedtls_ssl_session_reset(&dtls);
        }
    } else {
        uint8_t plainBuf[2048];
        int ret = mbedtls_ssl_read(&dtls, plainBuf, sizeof(plainBuf));
        dbg("WebRTC: DTLS read: ret=%d\n", ret);
        if (ret > 0) {
            size_t outLen = 0;
            sctpInput(&sctp, plainBuf, ret, &outLen);
            dbg("WebRTC: SCTP input %d bytes, response %d bytes, channels=%d\n",
                 ret, (int)outLen, sctp.numChannels);
            if (outLen > 0) {
                int wr = dtlsSctpSend(sctp.outBuf, outLen, nullptr);
                dbg("WebRTC: dtlsSend(%d) = %d\n", (int)outLen, wr);
            }
            /* Retransmit any missing packets reported by SACK */
            sctpRetransmit(&sctp, dtlsSctpSend, nullptr);
            if (sctp.numChannels > 0 && !streaming)
                startStreaming();
        } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
                   ret == MBEDTLS_ERR_SSL_CONN_EOF) {
            info("WebRTC: DTLS closed by peer\n");
            stopStreaming();
            dtlsSessionFree();
            dtlsSessionInit();
            sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
            peerKnown = false;
        }
    }
}

static void openUdpSocket() {
    int port = storageGetInt("s.net.webrtc_port", 0);
    if (port <= 0) return;
    if (udpFd >= 0) return;

    udpFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpFd < 0) { err("WebRTC: socket: %d\n", errno); return; }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(udpFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        err("WebRTC: bind port %d: %d\n", port, errno);
        close(udpFd); udpFd = -1; return;
    }
    fcntl(udpFd, F_SETFL, fcntl(udpFd, F_GETFL, 0) | O_NONBLOCK);
    info("WebRTC: UDP port %d open\n", port);
}

static void closeUdpSocket() {
    if (udpFd >= 0) { close(udpFd); udpFd = -1; }
}

/* ---- Main task ---- */

static void webrtcTaskFn(void*) {
    /* ITS server for signaling WS */
    itsServerInit(1, 4096, 4096);
    itsServerOnConnect(webrtcItsConnect);
    itsServerOnBusy(webrtcItsBusy);
    itsServerOnDisconnect(webrtcItsDisconnect);

    /* Register /dc WebSocket endpoint with web task */
    { web_path_msg_t reg = {};
      reg.itsPort = 4433;  /* convention: use DC's UDP port */
      safeStrncpy(reg.path, "webrtc", sizeof(reg.path));
      while (!itsSendAux("web", &reg, sizeof(reg), pdMS_TO_TICKS(500)))
          vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Subscribe to config changes — callbacks fire via itsPoll() */
    storageSubscribeChanges("s.net.webrtc_port", ON_CHANGE {
        stopStreaming();
        if (dtlsConnected) dtlsSessionFree();
        closeUdpSocket();
        peerKnown = false;
        openUdpSocket();
    });
    storageSubscribeChanges("net.up", ON_CHANGE {
        if (atoi(val)) {
            dtlsSetup();
            openUdpSocket();
        } else {
            stopStreaming();
            if (dtlsConnected) dtlsSessionFree();
            closeUdpSocket();
            peerKnown = false;
        }
    });

    /* Generate ICE credentials */
    { uint32_t r = esp_random();
      snprintf(iceUfrag, sizeof(iceUfrag), "%04X", (unsigned)(r & 0xFFFF)); }
    esp_fill_random(icePwd, 22);
    for (int i = 0; i < 22; i++)
        icePwd[i] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[
            (uint8_t)icePwd[i] % 62];
    icePwd[22] = '\0';

    /* Network may already be up before we registered */
    if (netIsUp()) {
        dtlsSetup();
        openUdpSocket();
    }

    for (;;) {
        /* ITS messages (camera/audio/signaling) + 1ms timeout for UDP poll */
        while (itsPoll(udpFd >= 0 ? 1 : portMAX_DELAY)) {}

        /* Drain UDP socket */
        if (udpFd >= 0) {
            uint8_t rxBuf[1500];
            struct sockaddr_in from;
            socklen_t fromLen;
            for (;;) {
                fromLen = sizeof(from);
                int n = recvfrom(udpFd, rxBuf, sizeof(rxBuf), MSG_DONTWAIT,
                                 (struct sockaddr*)&from, &fromLen);
                if (n <= 0) break;
                netTrafficIn(n);
                handleUdpPacket(rxBuf, n, &from);
            }
        }

        if (itsHandle >= 0) {
            char sigBuf[2048];
            size_t n = 0;
            if (webrtcWsClient) {
                uint8_t wsBuf[2048];
                size_t wsLen = 0;
                int op = wsReadFrame(itsHandle, wsBuf, sizeof(wsBuf), &wsLen);
                if (op > 0 && wsLen > 0) {
                    memcpy(sigBuf, wsBuf, wsLen);
                    n = wsLen;
                } else if (op < 0) {
                    /* WS closed */
                    itsServerKick(itsHandle);
                    itsHandle = -1;
                    webrtcWsClient = false;
                }
            } else {
                n = itsRecv(itsHandle, sigBuf, sizeof(sigBuf) - 1, 0);
            }
            if (n > 0) {
                sigBuf[n] = '\0';
                handleSignalingMsg(sigBuf, n);
            }
        }

        /* UDP packets now arrive via ITS (handleUdpPacket callback) */

        /* Inactivity timeout — peer gone without clean close */
        if (streaming && lastUdpRxMs && millis() - lastUdpRxMs > UDP_TIMEOUT_MS) {
            info("WebRTC: no UDP activity for %us, tearing down\n", (unsigned)(UDP_TIMEOUT_MS / 1000));
            stopStreaming();
            dtlsSessionFree();
            dtlsSessionInit();
            sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
            peerKnown = false;
            lastUdpRxMs = 0;
        }

        /* Continue DTLS handshake retransmission timer */
        if (!dtlsConnected && peerKnown && webrtcTimerGet(nullptr) >= 1) {
            bioRecvBuf = nullptr;
            bioRecvLen = 0;
            int ret = mbedtls_ssl_handshake(&dtls);
            if (ret == 0) {
                dtlsConnected = true;
                info("WebRTC: DTLS connected (timer retry)\n");
            }
        }

    }
}

/* ---- Init ---- */

void webrtcInit() {
    frameBuf = (uint8_t*)heap_caps_malloc(MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
    pmLockCreate(PM_NO_LIGHT_SLEEP, "webrtc", &webrtcLockLS);
    pmLockCreate(PM_CPU_FREQ_MAX, "webrtc", &webrtcLockCPU);
    xTaskCreatePinnedToCoreWithCaps(webrtcTaskFn, "webrtc", 12288, nullptr, 2, &webrtcHandle, 0, MALLOC_CAP_SPIRAM);
}
