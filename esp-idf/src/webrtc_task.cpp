/**
 * webrtc_task — WebRTC: signaling + ICE-lite + DTLS + SCTP, plus a generic
 * router that bridges each DataChannel to a packet-mode ITS connection.
 *
 * Content-free: knows nothing about camera, audio, or playback. The DCEP
 * label is `"<taskname>:<port-number>"`; we parse and call
 * `itsConnect(taskname, port, protocolBytes)`. Data flows both directions
 * — SCTP DATA on a channel → `itsSend` on the paired handle; `itsRecv` on
 * a handle → `sctpSend` on the paired stream — with reliability honored by
 * SCTP's channel config.
 *
 * See docs/webrtc-for-everything.md.
 */
#include "webrtc_task.h"
#include "webrtc_sctp.h"
#include "storage.h"
#include "compat.h"
#include "pm.h"
#include "log.h"
#include "its.h"
#include "tls.h"
#include "net.h"
#include "web.h"
#include "auth.h"
#include "upnp.h"

#include "esp_netif.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
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

enum : uint8_t {
    DTLS_CT_CHANGE_CIPHER_SPEC = 20,
    DTLS_CT_ALERT            = 21,
    DTLS_CT_HANDSHAKE        = 22,
    DTLS_CT_APPLICATION_DATA = 23,
};

static uint64_t dtlsRecordSeq6(const uint8_t* buf) {
    return ((uint64_t)buf[5] << 40) | ((uint64_t)buf[6] << 32) |
           ((uint64_t)buf[7] << 24) | ((uint64_t)buf[8] << 16) |
           ((uint64_t)buf[9] << 8) | (uint64_t)buf[10];
}

static const char* dtlsAlertLevelStr(uint8_t level) {
    switch (level) {
        case 1: return "warning";
        case 2: return "fatal";
        default: return "?";
    }
}

static const char* dtlsAlertDescStr(uint8_t d) {
    switch (d) {
        case 0: return "close_notify";
        case 10: return "unexpected_message";
        case 20: return "bad_record_mac";
        case 21: return "decryption_failed";
        case 22: return "record_overflow";
        case 40: return "handshake_failure";
        case 42: return "bad_certificate";
        case 43: return "unsupported_certificate";
        case 44: return "certificate_revoked";
        case 45: return "certificate_expired";
        case 46: return "certificate_unknown";
        case 47: return "illegal_parameter";
        case 48: return "unknown_ca";
        case 49: return "access_denied";
        case 50: return "decode_error";
        case 51: return "decrypt_error";
        case 70: return "protocol_version";
        case 71: return "insufficient_security";
        case 80: return "internal_error";
        case 86: return "inappropriate_fallback";
        case 90: return "user_canceled";
        case 100: return "no_renegotiation";
        case 109: return "missing_extension";
        case 110: return "unsupported_extension";
        default: return "?";
    }
}

static void dtlsLogIncomingAlertRecord(const uint8_t* buf, size_t n) {
    if (n < 13) return;
    uint16_t epoch = r16(buf + 3);
    uint64_t seq = dtlsRecordSeq6(buf);
    uint16_t fragLen = r16(buf + 11);
    if (fragLen == 2 && n >= 15 && n >= 13u + fragLen) {
        uint8_t level = buf[13];
        uint8_t desc = buf[14];
        dbg("DTLS Alert plaintext: level=%u (%s) description=%u (%s) epoch=%u seq=%llu\n",
            (unsigned)level, dtlsAlertLevelStr(level),
            (unsigned)desc, dtlsAlertDescStr(desc),
            (unsigned)epoch, seq);
        return;
    }
    dbg("DTLS Alert record (ciphertext) len=%u epoch=%u seq=%llu\n",
        (unsigned)fragLen, (unsigned)epoch, seq);
}

/* ---- Task state ---- */

static TaskHandle_t webrtcHandle = nullptr;
static pm_lock_handle_t webrtcLockLS  = nullptr;
static pm_lock_handle_t webrtcLockCPU = nullptr;
static bool pmHeld = false;

/* Network */
static int udpFd = -1;
static struct sockaddr_in peerAddr = {};
static bool peerKnown = false;

/* ICE credentials */
static char iceUfrag[8];
static char icePwd[28];

/* DTLS */
static mbedtls_ssl_context dtls;
static mbedtls_ssl_config  dtlsConf;
static mbedtls_ssl_cookie_ctx cookieCtx;
static bool dtlsReady = false;
static bool dtlsSessionActive = false;
static bool dtlsConnected = false;

/* DTLS timer state (custom — MBEDTLS_TIMING_C is off on ESP-IDF) */
static uint32_t timerStart;
static uint32_t timerIntMs;
static uint32_t timerFinMs;

/* SCTP */
static sctp_assoc_t sctp;
static uint8_t sctpBuf[SCTP_BUF_SIZE];

/* Handshake dedup: last handshake packet length (avoid reprocessing retransmits) */
static size_t lastHandshakeLen = 0;

/* UDP activity */
static uint32_t lastUdpRxMs = 0;
static uint32_t lastUdpTxMs = 0;
static uint32_t udpTxDrops = 0;
static uint32_t udpTxDropLogMs = 0;
static constexpr uint32_t UDP_TIMEOUT_MS = 45000;

/* Signaling WS */
static int  itsHandle = -1;
static bool webrtcWsClient = false;

/* ---- Router state: streamID ↔ ITS handle ---- */

typedef struct {
    int      handle;    /* -1 = empty */
    uint16_t streamId;
    uint16_t priority;  /* snapshotted from dc_channel_t at onDceopen */
    /* If sctpSend returned 0 (reliable rexmit pool full), we stash the
       already-dequeued ITS packet here and retry later instead of dropping
       it. Reliable+ordered channels cannot tolerate silent drops. */
    uint8_t* pendingBuf;
    size_t   pendingLen;
} dc_its_map_t;

static dc_its_map_t dcMap[DC_MAX_CHANNELS];

/* Forward decl — dtlsSctpSend is defined further down (after DTLS setup). */
static int dtlsSctpSend(const uint8_t* pkt, size_t len, void* ctx);

/* Receive buffer for packets coming in from ITS target (PSRAM) —
   sized for the largest packet we'd ever forward (e.g. a full JPEG). */
static constexpr size_t ROUTER_RECV_BUF_SIZE = 256 * 1024;
static uint8_t* routerRecvBuf = nullptr;

static int dcMapFindByStream(uint16_t streamId) {
    for (int i = 0; i < DC_MAX_CHANNELS; i++)
        if (dcMap[i].handle >= 0 && dcMap[i].streamId == streamId) return i;
    return -1;
}

static int dcMapAllocSlot() {
    for (int i = 0; i < DC_MAX_CHANNELS; i++)
        if (dcMap[i].handle < 0) return i;
    return -1;
}

static void dcMapFreePending(int idx) {
    if (dcMap[idx].pendingBuf) {
        heap_caps_free(dcMap[idx].pendingBuf);
        dcMap[idx].pendingBuf = nullptr;
        dcMap[idx].pendingLen = 0;
    }
}

static void dcMapClear(int idx) {
    if (idx < 0 || idx >= DC_MAX_CHANNELS) return;
    dcMapFreePending(idx);
    dcMap[idx].handle = -1;
    dcMap[idx].streamId = 0;
}

static void dcMapClearAll() {
    for (int i = 0; i < DC_MAX_CHANNELS; i++) {
        if (dcMap[i].handle >= 0) itsDisconnect(dcMap[i].handle);
        dcMapFreePending(i);
        dcMap[i].handle = -1;
        dcMap[i].streamId = 0;
    }
}

/* Try to flush a dc's pending send. Returns true if pending cleared
   (either sent or no pending existed). */
static bool dcMapFlushPending(int idx) {
    if (!dcMap[idx].pendingBuf) return true;
    if (!sctp.established || !dtlsConnected) return false;
    int r = sctpSend(&sctp, dcMap[idx].streamId, PPID_BINARY,
                     dcMap[idx].pendingBuf, dcMap[idx].pendingLen,
                     dtlsSctpSend, nullptr);
    if (r <= 0) return false;
    dcMapFreePending(idx);
    return true;
}

/* ---- Strict-priority scheduler with round-robin within a level ----
 *
 * Outbound DATA is drained in DCEP-priority order (higher first). Among
 * channels tied at the same priority we round-robin on each send, so a
 * single chatty channel can't starve its siblings at the same level. An
 * RFC 8260-ish PRIO scheduler with a per-level RR tiebreaker.
 *
 * `lastServedSlot` tracks the dcMap index of the most recent successful
 * send; the next pick at any priority level starts scanning from the
 * slot after it (wrapping). Higher priorities always preempt — the RR
 * cursor is only consulted among the equal-priority candidates.
 *
 * One send per inner iteration keeps per-call CPU bounded so UDP/DTLS
 * work in the main loop still gets its turn.
 */
static int lastServedSlot = -1;

static void schedulerPass() {
    if (!sctp.established || !dtlsConnected) return;

    auto channelHasWork = [](int i) {
        if (dcMap[i].handle < 0) return false;
        if (dcMap[i].pendingBuf) return true;
        return itsBytesAvailable(dcMap[i].handle) > 4;
    };

    /* Cap messages per pass so the main loop keeps cycling and inbound
       SACKs get drained close to the rate Chrome sends them. Each send
       can be many fragments × mbedtls encrypt, so too large a batch
       stalls recvfrom for long enough to show as bursty SACK clusters. */
    for (int iter = 0; iter < 8; iter++) {
        /* Highest priority that has anything ready to send. */
        int topPrio = -1;
        for (int i = 0; i < DC_MAX_CHANNELS; i++) {
            if (!channelHasWork(i)) continue;
            int p = (int)dcMap[i].priority;
            if (p > topPrio) topPrio = p;
        }
        if (topPrio < 0) break;  /* nothing to send */

        /* Among equal-priority channels with work, round-robin from the
           slot after the last one we served. */
        int pick = -1;
        for (int step = 1; step <= DC_MAX_CHANNELS; step++) {
            int i = (lastServedSlot + step) % DC_MAX_CHANNELS;
            if ((int)dcMap[i].priority != topPrio) continue;
            if (!channelHasWork(i)) continue;
            pick = i;
            break;
        }
        if (pick < 0) break;

        /* Flush stash first (don't pull new bytes until the one we already
           dequeued goes out). */
        if (dcMap[pick].pendingBuf) {
            if (dcMapFlushPending(pick)) { lastServedSlot = pick; continue; }
            /* Still stuck. Every equal-priority reliable sibling shares the
               same pool quota, so trying them is pointless; move on without
               this iteration counting. */
            break;
        }

        /* Fresh pull from the ITS buffer. */
        size_t nb = itsRecv(dcMap[pick].handle, routerRecvBuf,
                            ROUTER_RECV_BUF_SIZE, 0);
        if (nb == 0) continue;   /* race: drained before we could read */

        int r = sctpSend(&sctp, dcMap[pick].streamId, PPID_BINARY,
                         routerRecvBuf, nb, dtlsSctpSend, nullptr);
        if (r > 0) {
            lastServedSlot = pick;
            /* Yield every few iterations so mbedtls encrypt bursts can't
               starve IDLE0 (DC task is prio 2 on core 0). */
            if ((iter & 3) == 3) vTaskDelay(1);
            continue;
        }

        /* Rexmit pool at this priority is full — stash and stop this pass.
           Higher priorities or unreliable channels will get their chance
           on the next pass once SACKs free slots. */
        verb("sched stash: stream=%u len=%u rexmitBytes=%u peerRwnd=%u\n",
             (unsigned)dcMap[pick].streamId, (unsigned)nb,
             (unsigned)sctp.rexmitBytes, (unsigned)sctp.peerRwnd);
        uint8_t* sb = (uint8_t*)heap_caps_malloc(nb, MALLOC_CAP_SPIRAM);
        if (sb) {
            memcpy(sb, routerRecvBuf, nb);
            dcMap[pick].pendingBuf = sb;
            dcMap[pick].pendingLen = nb;
        } else {
            err("router: stash alloc failed, dropping %u bytes\n", (unsigned)nb);
        }
        break;
    }
}

/* ---- PM lock helpers ---- */

static void pmAcquire() {
    if (pmHeld) return;
    pmLockAcquire(webrtcLockLS);
    pmLockAcquire(webrtcLockCPU);
    pmHeld = true;
}

static void pmRelease() {
    if (!pmHeld) return;
    pmLockRelease(webrtcLockCPU);
    pmLockRelease(webrtcLockLS);
    pmHeld = false;
}

/* ---- DTLS timer callbacks ---- */

static void dcTimerSet(void* ctx, uint32_t intMs, uint32_t finMs) {
    (void)ctx;
    timerStart = millis();
    timerIntMs = intMs;
    timerFinMs = finMs;
}

static int webrtcTimerGet(void* ctx) {
    (void)ctx;
    if (timerFinMs == 0) return -1;
    uint32_t elapsed = millis() - timerStart;
    if (elapsed >= timerFinMs) return 2;
    if (elapsed >= timerIntMs) return 1;
    return 0;
}

/* ---- DTLS BIO callbacks (UDP sendto/recvfrom) ----
   On lwIP EAGAIN/ENOMEM we briefly yield and retry the same record rather
   than propagate WANT_WRITE up on the first hiccup. sendto is atomic per
   call: either the datagram is queued or none of it is. */
static int webrtcBioSend(void* ctx, const unsigned char* buf, size_t len) {
    (void)ctx;
    if (!peerKnown) return MBEDTLS_ERR_SSL_WANT_WRITE;
    int n = -1;
    for (int attempt = 0; attempt < 5; attempt++) {
        n = sendto(udpFd, buf, len, MSG_DONTWAIT,
                   (struct sockaddr*)&peerAddr, sizeof(peerAddr));
        if (n >= 0) break;
        if (errno != EAGAIN && errno != ENOMEM) break;
        vTaskDelay(1);
    }
    if (n > 0) { netTrafficOut(n); lastUdpTxMs = millis(); }
    if (n < 0) {
        udpTxDrops++;
        if (errno == EAGAIN || errno == ENOMEM) return MBEDTLS_ERR_SSL_WANT_WRITE;
        dbg("sendto failed: errno=%d (%s)\n", errno, strerror(errno));
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return n;
}

static uint8_t* bioRecvBuf = nullptr;
static size_t   bioRecvLen = 0;

static int webrtcBioRecv(void* ctx, unsigned char* buf, size_t len) {
    (void)ctx;
    if (!bioRecvBuf || bioRecvLen == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    size_t n = bioRecvLen < len ? bioRecvLen : len;
    memcpy(buf, bioRecvBuf, n);
    bioRecvBuf = nullptr;
    bioRecvLen = 0;
    return (int)n;
}

/* ClientHello reassembly for mbedTLS 3.6 (see original comment for rationale). */
static uint8_t* chReasmBuf = nullptr;
static size_t   chReasmLen = 0;
static size_t   chReasmBodyLen = 0;
static size_t   chReasmFilled = 0;
static uint16_t chReasmMsgSeq = 0;

static void chReasmFree() {
    if (chReasmBuf) free(chReasmBuf);
    chReasmBuf = nullptr;
    chReasmLen = chReasmBodyLen = chReasmFilled = 0;
}

static int tryReassembleClientHello(const uint8_t* pkt, size_t n) {
    if (n < 25) return 0;
    if (pkt[0] != DTLS_CT_HANDSHAKE) return 0;
    uint8_t hsType = pkt[13];
    if (hsType != 1 /* ClientHello */) return 0;
    uint16_t recLen  = r16(pkt + 11);
    uint32_t hsLen   = ((uint32_t)pkt[14] << 16) | ((uint32_t)pkt[15] << 8) | pkt[16];
    uint16_t msgSeq  = r16(pkt + 17);
    uint32_t fragOff = ((uint32_t)pkt[19] << 16) | ((uint32_t)pkt[20] << 8) | pkt[21];
    uint32_t fragLen = ((uint32_t)pkt[22] << 16) | ((uint32_t)pkt[23] << 8) | pkt[24];
    if (fragLen == hsLen && fragOff == 0) return 0;  /* not fragmented */
    if (recLen < 12 + fragLen) return 0;
    if (fragOff == 0 || chReasmMsgSeq != msgSeq || !chReasmBuf) {
        chReasmFree();
        chReasmLen = 25 + hsLen;
        chReasmBuf = (uint8_t*)malloc(chReasmLen);
        if (!chReasmBuf) return 0;
        memcpy(chReasmBuf, pkt, 25);
        uint16_t recLen2 = (uint16_t)(12 + hsLen);
        chReasmBuf[11] = recLen2 >> 8;
        chReasmBuf[12] = recLen2 & 0xff;
        chReasmBuf[19] = 0; chReasmBuf[20] = 0; chReasmBuf[21] = 0;
        chReasmBuf[22] = (hsLen >> 16) & 0xff;
        chReasmBuf[23] = (hsLen >> 8)  & 0xff;
        chReasmBuf[24] =  hsLen        & 0xff;
        chReasmBodyLen = hsLen;
        chReasmMsgSeq  = msgSeq;
    }
    if (fragOff + fragLen > chReasmBodyLen) return 0;
    memcpy(chReasmBuf + 25 + fragOff, pkt + 25, fragLen);
    chReasmFilled += fragLen;
    if (chReasmFilled >= chReasmBodyLen) {
        info("DTLS: reassembled fragmented ClientHello (%u bytes)\n", (unsigned)chReasmLen);
        return 1;
    }
    return -1;
}

/* ---- ICE-lite: STUN binding request/response ---- */

static const uint32_t STUN_MAGIC = 0x2112A442;

static bool isStunPacket(const uint8_t* buf, size_t len) {
    if (len < 20) return false;
    if (buf[0] & 0xC0) return false;
    return r32(buf + 4) == STUN_MAGIC;
}

static void handleStunRequest(const uint8_t* req, size_t reqLen,
                              const struct sockaddr_in* from) {
    if (reqLen < 20) return;
    uint16_t msgType = r16(req);
    if (msgType != 0x0001) return;

    uint8_t resp[256];
    size_t pos = 0;
    w16(resp, 0x0101); pos = 2;
    pos += 2;
    memcpy(resp + 4, req + 4, 4);
    memcpy(resp + 8, req + 8, 12);
    pos = 20;

    w16(resp + pos, 0x0020); pos += 2;
    w16(resp + pos, 8); pos += 2;
    resp[pos++] = 0;
    resp[pos++] = 0x01;
    uint16_t xPort = ntohs(from->sin_port) ^ (uint16_t)(STUN_MAGIC >> 16);
    w16(resp + pos, xPort); pos += 2;
    uint32_t xAddr = ntohl(from->sin_addr.s_addr) ^ STUN_MAGIC;
    w32(resp + pos, xAddr); pos += 4;

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
    w16(resp + pos, 0x0008); pos += 2;
    w16(resp + pos, 20); pos += 2;
    memcpy(resp + pos, hmac, 20); pos += 20;

    w16(resp + 2, (uint16_t)(pos - 20 + 8));
    uint32_t fp = esp_rom_crc32_le(0, resp, pos) ^ 0x5354554E;
    w16(resp + pos, 0x8028); pos += 2;
    w16(resp + pos, 4); pos += 2;
    w32(resp + pos, fp); pos += 4;

    w16(resp + 2, (uint16_t)(pos - 20));

    int sent = sendto(udpFd, resp, pos, MSG_DONTWAIT,
                      (const struct sockaddr*)from, sizeof(*from));
    if (sent > 0) netTrafficOut(sent);
    else dbg("STUN response sendto failed: errno=%d\n", errno);
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
    mbedtls_ssl_conf_renegotiation(&dtlsConf, MBEDTLS_SSL_RENEGOTIATION_DISABLED);

    static const int ciphersuites[] = {
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        0
    };
    mbedtls_ssl_conf_ciphersuites(&dtlsConf, ciphersuites);

    ret = mbedtls_ssl_cookie_setup(&cookieCtx, mbedtls_ctr_drbg_random, tlsGetRng());
    if (ret != 0) { err("dtls cookie: -0x%04x\n", -ret); return; }
    mbedtls_ssl_conf_dtls_cookies(&dtlsConf,
                                  mbedtls_ssl_cookie_write,
                                  mbedtls_ssl_cookie_check,
                                  &cookieCtx);

    dtlsReady = true;
}

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
        dbg("KEYLOG %s\n", line);
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
    pmAcquire();
}

static void dtlsSessionFree() {
    mbedtls_ssl_free(&dtls);
    dtlsSessionActive = false;
    dtlsConnected = false;
    chReasmFree();
    pmRelease();
}

static int dtlsSctpSend(const uint8_t* pkt, size_t len, void* ctx) {
    (void)ctx;
    if (!dtlsConnected) return -1;
    int ret = mbedtls_ssl_write(&dtls, pkt, len);
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        dbg("DTLS write failed: -0x%04x len=%d\n", -ret, (int)len);
        return -1;
    }
    verb("DTLS write %d/%d bytes\n", ret, (int)len);
    return ret;
}

/* ---- SDP generation ---- */

static std::string generateSdpAnswer(const char* offerSdp) {
    (void)offerSdp;
    int port = storageGetInt("s.net.webrtc_port", 0);

    char fingerprint[128] = {};
    tlsCertFingerprint(fingerprint, sizeof(fingerprint));

    char ips[4][16] = {};
    int numIps = 0;

    { esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if (netif) {
          esp_netif_ip_info_t info;
          if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
              esp_ip4addr_ntoa(&info.ip, ips[numIps], sizeof(ips[0]));
              numIps++;
          }
      }
    }
    { esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
      if (netif) {
          esp_netif_ip_info_t info;
          if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
              esp_ip4addr_ntoa(&info.ip, ips[numIps], sizeof(ips[0]));
              numIps++;
          }
      }
    }
    { char wgAddr[16] = {};
      storageGetStr("s.wg.address", wgAddr, sizeof(wgAddr));
      if (wgAddr[0] && storageGetInt("wg.up", 0)) {
          safeStrncpy(ips[numIps], wgAddr, sizeof(ips[0]));
          numIps++;
      }
    }

    const char* primaryIp = numIps > 0 ? ips[0] : "0.0.0.0";

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

    for (int i = 0; i < numIps; i++) {
        snprintf(line, sizeof(line),
            "a=candidate:%d 1 UDP %u %s %d typ host\r\n",
            i + 1, (unsigned)(2130706431 - i), ips[i], port);
        sdp += line;
    }

    const char* extIp = upnpExternalIp();
    if (extIp[0] && numIps > 0) {
        snprintf(line, sizeof(line),
            "a=candidate:%d 1 UDP %u %s %d typ srflx raddr %s rport %d\r\n",
            numIps + 1, (unsigned)(2130706431 - numIps - 1), extIp, port, ips[0], port);
        sdp += line;
    }

    return sdp;
}

/* ---- Router callbacks ---- */

static void webrtcOnItsDisconnect(int ref);

/* Target task closed the connection we initiated. */
static void webrtcOnItsDisconnect(int ref) {
    if (ref < 0 || ref >= DC_MAX_CHANNELS) return;
    uint16_t streamId = dcMap[ref].streamId;
    dcMapClear(ref);
    if (sctp.established && dtlsConnected)
        sctpStreamReset(&sctp, streamId, dtlsSctpSend, nullptr);
    info("DC stream %u closed by target\n", streamId);
}

/* SCTP new-channel callback: parse label, route to an ITS server. */
static void webrtcDcOpen(sctp_assoc_t* a, int chIdx) {
    dc_channel_t* ch = &a->channels[chIdx];
    info("DC OPEN label=\"%s\" proto=\"%.*s\" type=0x%02x stream=%u\n",
         ch->label, (int)ch->protoLen, ch->protocol,
         ch->channelType, ch->streamId);

    const char* colon = strchr(ch->label, ':');
    if (!colon) {
        err("DC label missing ':' — refusing\n");
        ch->open = false;
        return;
    }
    char taskName[32];
    size_t nameLen = (size_t)(colon - ch->label);
    if (nameLen == 0 || nameLen >= sizeof(taskName)) {
        err("DC label has empty or oversized task name — refusing\n");
        ch->open = false;
        return;
    }
    memcpy(taskName, ch->label, nameLen);
    taskName[nameLen] = '\0';

    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) {
        err("DC label has bad port — refusing\n");
        ch->open = false;
        return;
    }

    int slot = dcMapAllocSlot();
    if (slot < 0) {
        err("router full — refusing DC\n");
        ch->open = false;
        return;
    }

    /* Generous timeout: a target task mid-seek can be off the inbox for
       a few hundred ms of SD work, and a rapid second DC OPEN has to
       wait through that plus onBusy eviction + accept. No per-handle
       onRecv — the scheduler polls ITS buffers in priority order from
       the main loop; itsSend notifications still wake us. */
    int handle = itsConnect(taskName, (uint16_t)port,
                            ch->protoLen > 0 ? ch->protocol : nullptr,
                            ch->protoLen,
                            pdMS_TO_TICKS(3000),
                            slot,
                            nullptr, webrtcOnItsDisconnect);
    if (handle < 0) {
        err("itsConnect(%s:%d) failed\n", taskName, port);
        ch->open = false;
        return;
    }
    dcMap[slot].handle   = handle;
    dcMap[slot].streamId = ch->streamId;
    dcMap[slot].priority = ch->priority;
    info("DC routed: %s:%d stream=%u prio=%u → handle=%d\n",
         taskName, port, ch->streamId, (unsigned)ch->priority, handle);
}

/* SCTP stream-reset from peer (browser closed the DC). */
static void webrtcDcReset(sctp_assoc_t* a, uint16_t streamId) {
    (void)a;
    int idx = dcMapFindByStream(streamId);
    if (idx < 0) return;
    int h = dcMap[idx].handle;
    dcMapClear(idx);
    if (h >= 0) itsDisconnect(h);
    info("DC stream %u closed by peer\n", streamId);
}

/* SCTP inbound user-data (fully reassembled): route to the paired ITS
   handle as one packet-mode itsSend. PPID distinguishes string/binary
   but the target task sees the bytes regardless. */
static void webrtcDcData(sctp_assoc_t* a, uint16_t streamId,
                          uint32_t ppid, const uint8_t* data, size_t dataLen) {
    (void)a; (void)ppid;
    int idx = dcMapFindByStream(streamId);
    if (idx < 0) return;
    int h = dcMap[idx].handle;
    if (h < 0) return;
    /* Short timeout: don't stall the webrtc main loop if the target's
       buffer is briefly full. Browser's SCTP will slow down naturally
       via the rwnd / SACK feedback loop if we stop draining. */
    size_t sent = itsSend(h, data, dataLen, pdMS_TO_TICKS(50));
    if (sent == 0) {
        warn("DC→ITS drop: stream=%u len=%u (target buffer full)\n",
             (unsigned)streamId, (unsigned)dataLen);
    }
}

/* ---- Signaling WS (ITS server for web-forwarded connections) ---- */

/** WS close codes. 4xxx are application-defined (per RFC 6455 §7.4.2).
 *    4401 — auth required / cookie invalid (matches other endpoints).
 *    4409 — device busy (another session active). Client may retry with
 *           `?force=1` to evict.
 *    4008 — evicted by another session (served to the victim on force). */
enum : uint16_t {
    WS_CLOSE_AUTH  = 4401,
    WS_CLOSE_BUSY  = 4409,
    WS_CLOSE_KICK  = 4008,
};

/* Port is opened with maxHandles=2 so a new connect always lands in
   onConnect (not onBusy) where we can do WS upgrade + wsSendClose with a
   specific code. The second slot is transient: we either reject (4409)
   or promote (evicting the first). A 3rd simultaneous attempt hits
   onBusy and is rejected without a coded close — rare enough to ignore. */

static void webrtcSessionTeardown() {
    dcMapClearAll();
    if (dtlsSessionActive) dtlsSessionFree();
    peerKnown = false;
    lastUdpRxMs = 0;
    sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
}

static int webrtcItsConnect(int handle, const void* data, size_t len) {
    /* Require WS (signaling has no meaningful raw-TCP use). */
    if (len < sizeof(net_connect_t) || !((const net_connect_t*)data)->ws) {
        warn("non-WS connect rejected\n");
        return -1;
    }

    char hdr[1024];
    int hdrLen = webGetHeader(handle, hdr, sizeof(hdr));
    if (hdrLen <= 0) { info("no headers\n"); return -1; }
    if (!wsUpgrade(handle, hdr, hdrLen)) { info("WS upgrade failed\n"); return -1; }

    /* Auth check — always first, so `?force=1` can never be evaluated
       without a valid session cookie. */
    if (authEnabled()) {
        char cookie[64] = {};
        webExtractCookie(hdr, hdrLen, "session", cookie, sizeof(cookie));
        if (authCheck(cookie).empty()) {
            wsSendClose(handle, WS_CLOSE_AUTH);
            /* Let the net proxy flush the close frame before connFree
               zaps the stream buffer on return -1. */
            itsSendDrain(handle, 200);
            info("WS auth failed\n");
            return -1;
        }
    }

    /* Single-session: if another session is active, reject with BUSY
       unless client explicitly opted into eviction with ?force=1. */
    if (itsHandle >= 0 && itsHandle != handle) {
        char forceStr[8] = {};
        webGetQuery(hdr, hdrLen, "force", forceStr, sizeof(forceStr));
        bool force = (forceStr[0] == '1');
        if (!force) {
            wsSendClose(handle, WS_CLOSE_BUSY);
            /* Drain the send buffer so the 4409 close frame actually
               reaches the browser. Without this, net's proxy sees
               itsConnected=false after connFree and drops the bytes —
               browser sees raw TCP close (code 1006) instead of 4409
               and would auto-reconnect forever. */
            itsSendDrain(handle, 200);
            info("BUSY — rejecting (active handle=%d)\n", itsHandle);
            return -1;
        }
        /* Force takeover: tell the current session it was kicked, flush
           the close frame, then tear down and promote the new handle. */
        info("force takeover — evicting handle=%d\n", itsHandle);
        wsSendClose(itsHandle, WS_CLOSE_KICK);
        itsSendDrain(itsHandle, 200);
        int victim = itsHandle;
        itsHandle = -1;
        webrtcWsClient = false;
        itsDisconnect(victim);
        webrtcSessionTeardown();
    }

    itsHandle = handle;
    webrtcWsClient = true;
    info("signaling client connected\n");
    return 0;
}

static bool webrtcItsBusy(const void* data, size_t len) {
    (void)data; (void)len;
    /* Slots 0 and 1 are both in flight. A third attempt got here — just
       reject. The client sees a raw WS close without a code (1006). */
    return true;
}

static void webrtcItsDisconnect(int ref) {
    (void)ref;
    /* Only tear down if the disconnecting handle is the current active
       session. Rejected secondary connects (BUSY/AUTH) don't match and
       don't touch the running session. */
    if (itsHandle < 0) return;
    itsHandle = -1;
    webrtcWsClient = false;
    info("signaling client disconnected\n");
    webrtcSessionTeardown();
}

/* ---- Signaling message handling ---- */

static void handleSignalingMsg(const char* msg, size_t len) {
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
    if (type != "offer") return;

    std::string sdpRaw = findVal("sdp");
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

    dbg("received SDP offer (%d bytes)\n", (int)sdpOffer.size());

    dtlsSetup();
    if (!dtlsReady) {
        err("DTLS not ready (TLS cert missing?)\n");
        return;
    }

    std::string answer = generateSdpAnswer(sdpOffer.c_str());

    std::string escaped;
    for (char c : answer) {
        if (c == '\r') escaped += "\\r";
        else if (c == '\n') escaped += "\\n";
        else if (c == '"') escaped += "\\\"";
        else escaped += c;
    }

    std::string resp = "{\"type\":\"answer\",\"sdp\":\"" + escaped + "\"}";
    dbg("SDP answer:\n%s\n", answer.c_str());
    if (itsHandle >= 0) {
        if (webrtcWsClient)
            wsSendText(itsHandle, resp.c_str(), resp.size());
        else
            itsSend(itsHandle, resp.c_str(), resp.size(), pdMS_TO_TICKS(1000));
    }

    /* Fresh DTLS + SCTP for this offer. */
    dcMapClearAll();
    if (dtlsConnected) dtlsSessionFree();
    dtlsSessionInit();
    sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
    sctp.onDceopen = webrtcDcOpen;
    sctp.onDcreset = webrtcDcReset;
    sctp.onData    = webrtcDcData;
    peerKnown = false;
    lastHandshakeLen = 0;
    dbg("sent SDP answer, waiting for ICE+DTLS\n");
}

/* ---- UDP packet handling ---- */

static void handleUdpPacket(const uint8_t* buf, size_t n,
                            const struct sockaddr_in* from) {
    verb("UDP recv %d bytes from %s:%d\n", (int)n,
         inet_ntoa(from->sin_addr), ntohs(from->sin_port));

    peerAddr = *from;
    peerKnown = true;
    lastUdpRxMs = millis();

    if (isStunPacket(buf, n)) {
        handleStunRequest(buf, n, from);
        return;
    }
    if (!dtlsSessionActive) return;

    if (!dtlsConnected) {
        uint8_t clientId[6];
        memcpy(clientId, &from->sin_addr, 4);
        memcpy(clientId + 4, &from->sin_port, 2);
        mbedtls_ssl_set_client_transport_id(&dtls, clientId, 6);
    } else if (n >= 1) {
        uint8_t ct = buf[0];
        if (ct == DTLS_CT_HANDSHAKE) {
            dbg("DTLS handshake record after connected, skip\n");
            return;
        }
        if (ct == DTLS_CT_ALERT) {
            if (n >= 13) dtlsLogIncomingAlertRecord(buf, n);
        } else if (ct != DTLS_CT_APPLICATION_DATA) {
            dbg("DTLS record type=%u after connected, skip\n", (unsigned)ct);
            return;
        }
    }

    if (!dtlsConnected) {
        int r = tryReassembleClientHello(buf, n);
        if (r < 0) return;
        if (r > 0) { bioRecvBuf = chReasmBuf; bioRecvLen = chReasmLen; }
        else       { bioRecvBuf = (uint8_t*)buf; bioRecvLen = n; }
    } else {
        bioRecvBuf = (uint8_t*)buf;
        bioRecvLen = n;
    }

    if (!dtlsConnected && dtlsSessionActive) {
        if (bioRecvLen == (size_t)lastHandshakeLen) {
            dbg("skipping likely retransmit (%d bytes)\n", (int)bioRecvLen);
            return;
        }
        lastHandshakeLen = bioRecvLen;
        int ret = mbedtls_ssl_handshake(&dtls);
        if (ret == 0) {
            dtlsConnected = true;
            info("DTLS connected\n");
        } else if (ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED) {
            mbedtls_ssl_session_reset(&dtls);
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                   ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf[128];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            err("DTLS handshake: -0x%04x %s\n", -ret, errbuf);
            mbedtls_ssl_session_reset(&dtls);
        }
    } else {
        uint8_t plainBuf[2048];
        int ret = mbedtls_ssl_read(&dtls, plainBuf, sizeof(plainBuf));
        verb("DTLS read: ret=%d\n", ret);
        if (ret > 0) {
            size_t outLen = 0;
            int sctpSt = sctpInput(&sctp, plainBuf, ret, &outLen);
            if (sctpSt == 1) {
                info("SCTP aborted by peer\n");
                dcMapClearAll();
                sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
                sctp.onDceopen = webrtcDcOpen;
                sctp.onDcreset = webrtcDcReset;
                sctp.onData    = webrtcDcData;
                lastUdpRxMs = 0;
            } else if (outLen > 0) {
                int wr = dtlsSctpSend(sctp.outBuf, outLen, nullptr);
                if (wr <= 0) dbg("SCTP response send failed: %d (len=%d)\n", wr, (int)outLen);
                sctpRetransmit(&sctp, dtlsSctpSend, nullptr, /*forceAll=*/false);
            } else {
                sctpRetransmit(&sctp, dtlsSctpSend, nullptr, /*forceAll=*/false);
            }
        } else if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
                   ret == MBEDTLS_ERR_SSL_CONN_EOF) {
            info("DTLS closed by peer\n");
            dcMapClearAll();
            dtlsSessionFree();
            dtlsSessionInit();
            sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
            sctp.onDceopen = webrtcDcOpen;
            sctp.onDcreset = webrtcDcReset;
            sctp.onData    = webrtcDcData;
            peerKnown = false;
        }
    }
}

static void openUdpSocket() {
    int port = storageGetInt("s.net.webrtc_port", 0);
    if (port <= 0) return;
    if (udpFd >= 0) return;

    udpFd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpFd < 0) { err("socket: %d\n", errno); return; }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(udpFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        err("bind port %d: %d\n", port, errno);
        close(udpFd); udpFd = -1; return;
    }
    fcntl(udpFd, F_SETFL, fcntl(udpFd, F_GETFL, 0) | O_NONBLOCK);
    info("UDP port %d open\n", port);
}

static void closeUdpSocket() {
    if (udpFd >= 0) { close(udpFd); udpFd = -1; }
}

/* ---- Main task ---- */

static void webrtcTaskFn(void*) {
    /* ITS server for signaling WS, plus ITS client for outbound routing
       to content tasks (live, play, …). */
    itsServerInit();
    itsClientInit(DC_MAX_CHANNELS);
    /* maxHandles=2: lets a second connect run through onConnect so we can
       do WS upgrade + wsSendClose(4409) or evict-on-force. At most one
       of the two slots is the "active" session at any moment. */
    itsServerPortOpen(WEBRTC_PORT, false, 2, 4096, 4096);
    itsServerOnConnect(WEBRTC_PORT, webrtcItsConnect);
    itsServerOnBusy(WEBRTC_PORT, webrtcItsBusy);
    itsServerOnDisconnect(WEBRTC_PORT, webrtcItsDisconnect);

    for (int i = 0; i < DC_MAX_CHANNELS; i++) dcMap[i].handle = -1;

    /* Register /webrtc WebSocket endpoint with web task */
    { web_path_msg_t reg = {};
      reg.itsPort = WEBRTC_PORT;
      safeStrncpy(reg.path, "webrtc", sizeof(reg.path));
      while (!itsSendAux("web", WEB_PATH_REG_PORT, &reg, sizeof(reg), pdMS_TO_TICKS(500)))
          vTaskDelay(pdMS_TO_TICKS(100));
    }

    storageSubscribeChanges("s.net.webrtc_port", ON_CHANGE {
        dcMapClearAll();
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
            dcMapClearAll();
            if (dtlsConnected) dtlsSessionFree();
            closeUdpSocket();
            peerKnown = false;
        }
    });

    /* ICE credentials (ICE-lite; peer creds ignored) */
    { uint32_t r = esp_random();
      snprintf(iceUfrag, sizeof(iceUfrag), "%04X", (unsigned)(r & 0xFFFF)); }
    esp_fill_random((uint8_t*)icePwd, 22);
    for (int i = 0; i < 22; i++)
        icePwd[i] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"[
            (uint8_t)icePwd[i] % 62];
    icePwd[22] = '\0';

    if (netIsUp()) {
        dtlsSetup();
        openUdpSocket();
    }

    for (;;) {
        /* Unconditional yield per loop — under sustained load itsPoll(1)
           can return immediately and starve IDLE0 past the watchdog. */
        vTaskDelay(1);

        /* Drain inbox + per-connection recv callbacks. When UDP open, poll
           briefly so recvfrom also gets its turn; otherwise block until an
           inbox message wakes us. */
        for (;;) {
            if (!itsPoll(udpFd >= 0 ? 1 : portMAX_DELAY)) break;
        }

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

        /* Scheduler: strict-priority drain of outbound packets (plus any
           stashed ones from a prior pool-full) onto the DTLS/SCTP pipe.
           Runs after UDP drain so fresh SACKs have freed rexmit slots. */
        schedulerPass();

        /* Drain signaling WS */
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
                    itsDisconnect(itsHandle);
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

        if (sctp.established && millis() - udpTxDropLogMs > 2000) {
            if (udpTxDrops > 0) {
                dbg("UDP TX: %u drops in last %ums\n",
                    (unsigned)udpTxDrops, (unsigned)(millis() - udpTxDropLogMs));
                udpTxDrops = 0;
            }
            if (lastUdpTxMs && millis() - lastUdpTxMs > 2000)
                dbg("TX stall: no UDP send for %ums\n",
                    (unsigned)(millis() - lastUdpTxMs));
            udpTxDropLogMs = millis();
        }

        /* Drain any FORWARD-TSN queued by a stream-reset purge, so the
           peer's cumTsn can advance past TSNs we've abandoned. */
        if (sctp.established && dtlsConnected && sctp.pendingFwdTsn != 0) {
            size_t fwdLen = 0;
            sctpBuildForwardTsn(&sctp, sctp.pendingFwdTsn, &fwdLen);
            if (fwdLen > 0) {
                int wr = dtlsSctpSend(sctp.outBuf, fwdLen, nullptr);
                if (wr > 0) {
                    verb("FORWARD-TSN sent: advanceTo=%u (was cumTsn=%u)\n",
                         (unsigned)sctp.pendingFwdTsn, (unsigned)sctp.sackCumTsn);
                    sctp.pendingFwdTsn = 0;
                }
            } else {
                sctp.pendingFwdTsn = 0;
            }
        }

        /* RTO retransmit. If pool has data and no SACK has arrived in a
           while, resend the fragments the last SACK's gap blocks didn't
           cover. Threshold is generous — Chrome's delayed-SACK cadence
           can run 600+ ms under load, and firing too eagerly floods the
           link with duplicates. */
        if (sctp.established && dtlsConnected && sctp.rexmitBytes > 0 &&
            sctp.lastSackMs > 0 &&
            (uint32_t)(millis() - sctp.lastSackMs) > 1500) {
            verb("RTO retransmit: %ums since last SACK, rexmitBytes=%u\n",
                 (unsigned)(millis() - sctp.lastSackMs),
                 (unsigned)sctp.rexmitBytes);
            sctpRetransmit(&sctp, dtlsSctpSend, nullptr, /*forceAll=*/true);
            /* Don't spin RTO every loop; a real SACK will overwrite. */
            sctp.lastSackMs = millis();
        }

        /* Once per second, dump SCTP send-side state at verb for when
           the association misbehaves (enable with `log webrtc verbose`). */
        static uint32_t lastStateLogMs = 0;
        if (sctp.established && millis() - lastStateLogMs >= 1000) {
            lastStateLogMs = millis();
            int stashedCh = 0, openCh = 0;
            size_t stashedBytes = 0;
            for (int i = 0; i < DC_MAX_CHANNELS; i++) {
                if (dcMap[i].handle < 0) continue;
                openCh++;
                if (dcMap[i].pendingBuf) { stashedCh++; stashedBytes += dcMap[i].pendingLen; }
            }
            verb("state: peerRwnd=%u rexmitBytes=%u myTsn=%u openDC=%d stashedDC=%d stashedBytes=%u\n",
                 (unsigned)sctp.peerRwnd, (unsigned)sctp.rexmitBytes,
                 (unsigned)sctp.myTsn, openCh, stashedCh, (unsigned)stashedBytes);
        }

        /* Inactivity: peer gone without clean close. */
        if (sctp.established && lastUdpRxMs && millis() - lastUdpRxMs > UDP_TIMEOUT_MS) {
            info("no UDP activity for %us, tearing down\n", (unsigned)(UDP_TIMEOUT_MS / 1000));
            dcMapClearAll();
            dtlsSessionFree();
            dtlsSessionInit();
            sctpInit(&sctp, sctpBuf, sizeof(sctpBuf), SCTP_PORT);
            sctp.onDceopen = webrtcDcOpen;
            sctp.onDcreset = webrtcDcReset;
            sctp.onData    = webrtcDcData;
            peerKnown = false;
            lastUdpRxMs = 0;
        }

        /* DTLS handshake retransmission timer */
        if (!dtlsConnected && peerKnown && webrtcTimerGet(nullptr) >= 1) {
            bioRecvBuf = nullptr;
            bioRecvLen = 0;
            int ret = mbedtls_ssl_handshake(&dtls);
            if (ret == 0) {
                dtlsConnected = true;
                info("DTLS connected (timer retry)\n");
            }
        }
    }
}

/* ---- Init ---- */

void webrtcInit() {
    routerRecvBuf = (uint8_t*)heap_caps_malloc(ROUTER_RECV_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!routerRecvBuf) { err("router recv buf alloc failed\n"); return; }
    for (int i = 0; i < DC_MAX_CHANNELS; i++) dcMap[i].handle = -1;
    pmLockCreate(PM_NO_LIGHT_SLEEP, "webrtc", &webrtcLockLS);
    pmLockCreate(PM_CPU_FREQ_MAX,   "webrtc", &webrtcLockCPU);
    xTaskCreatePinnedToCoreWithCaps(webrtcTaskFn, "webrtc", 12288, nullptr, 2,
                                    &webrtcHandle, 0, MALLOC_CAP_SPIRAM);
}
