/**
 * webrtc_sctp — Minimal SCTP for WebRTC DataChannel.
 *
 * Server-side only (browser initiates). Supports:
 *   - Four-way handshake with stateless cookie
 *   - DATA send (fragmented, ordered or unordered) + receive (small DCEP)
 *   - SACK generation + parsing
 *   - DCEP DATA_CHANNEL_OPEN/ACK with protocol string extraction
 *   - RE-CONFIG stream reset (RFC 6525) in both directions
 *   - HEARTBEAT/HEARTBEAT-ACK
 *   - CRC32C checksum
 */
#include "webrtc_sctp.h"
#include "compat.h"    /* millis() — must share a clock with main loop's RTO check */
#include "log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>
#include <cstdio>
#include "mbedtls/sha256.h"
#include "esp_random.h"

/* ---- CRC32C (Castagnoli) ---- */

static const uint32_t crc32c_table[256] = {
    0x00000000,0xF26B8303,0xE13B70F7,0x1350F3F4,0xC79A971F,0x35F1141C,0x26A1E7E8,0xD4CA64EB,
    0x8AD958CF,0x78B2DBCC,0x6BE22838,0x9989AB3B,0x4D43CFD0,0xBF284CD3,0xAC78BF27,0x5E133C24,
    0x105EC76F,0xE235446C,0xF165B798,0x030E349B,0xD7C45070,0x25AFD373,0x36FF2087,0xC494A384,
    0x9A879FA0,0x68EC1CA3,0x7BBCEF57,0x89D76C54,0x5D1D08BF,0xAF768BBC,0xBC267848,0x4E4DFB4B,
    0x20BD8EDE,0xD2D60DDD,0xC186FE29,0x33ED7D2A,0xE72719C1,0x154C9AC2,0x061C6936,0xF477EA35,
    0xAA64D611,0x580F5512,0x4B5FA6E6,0xB93425E5,0x6DFE410E,0x9F95C20D,0x8CC531F9,0x7EAEB2FA,
    0x30E349B1,0xC288CAB2,0xD1D83946,0x23B3BA45,0xF779DEAE,0x05125DAD,0x1642AE59,0xE4292D5A,
    0xBA3A117E,0x4851927D,0x5B016189,0xA96AE28A,0x7DA08661,0x8FCB0562,0x9C9BF696,0x6EF07595,
    0x417B1DBC,0xB3109EBF,0xA0406D4B,0x524BEE48,0x862281A3,0x744902A0,0x6719F154,0x95727257,
    0xCB614E73,0x390ACD70,0x2A5A3E84,0xD831BD87,0x0CFBD96C,0xFE905A6F,0xEDC0A99B,0x1FAB2A98,
    0x51E6D1D3,0xA38D52D0,0xB0DDA124,0x42B62227,0x967C46CC,0x6417C5CF,0x7747363B,0x852CB538,
    0xDB3F891C,0x29540A1F,0x3A04F9EB,0xC86F7AE8,0x1CA51E03,0xEECE9D00,0xFD9E6EF4,0x0FF5EDF7,
    0x61058062,0x936E0361,0x803EF095,0x72557396,0xA69F177D,0x54F4947E,0x47A4678A,0xB5CFE489,
    0xEBDCD8AD,0x19B75BAE,0x0AE7A85A,0xF88C2B59,0x2C464FB2,0xDE2DCCB1,0xCD7D3F45,0x3F16BC46,
    0x715B470D,0x8330C40E,0x903037FA,0x625BB4F9,0xB691D012,0x44FA5311,0x57AAA0E5,0xA5C123E6,
    0xFBD21FC2,0x09B99CC1,0x1AE96F35,0xE882EC36,0x3C4888DD,0xCE230BDE,0xDD73F82A,0x2F187B29,
    0x82F63B78,0x70BDBF7B,0x639DB98F,0x91F63A8C,0x4536E667,0xB75D6564,0xA40D9690,0x56661593,
    0x087529B7,0xFA1EAAB4,0xE94E5940,0x1B25DA43,0xCFEFBEA8,0x3D843DAB,0x2ED4CE5F,0xDCBF4D5C,
    0x92F2B617,0x60993514,0x7349C6E0,0x812245E3,0x55E82108,0xA783A20B,0xB4D351FF,0x46B8D2FC,
    0x18ABEED8,0xEAC06DDB,0xF9909E2F,0x0BFB1D2C,0xDFF179C7,0x2D9AFAC4,0x3ECA0930,0xCC018A33,
    0xA2F1BFA6,0x509A3CA5,0x43CACF51,0xB1A14C52,0x656B28B9,0x97009BBA,0x8450684E,0x763BEB4D,
    0x2828D769,0xDA43546A,0xC913A79E,0x3B78249D,0xEFB24076,0x1DD9C375,0x0E893081,0xFCE2B382,
    0xB2AF48C9,0x40C4CBCA,0x5394383E,0xA1FFBB3D,0x75358FD6,0x875E0CD5,0x940EFF21,0x66657C22,
    0x38764006,0xCA1DC305,0xD94D30F1,0x2B26B3F2,0xFF0CD719,0x0D67541A,0x1E37A7EE,0xEC5C24ED,
    0xA411DF88,0x567A5C8B,0x452AAF7F,0xB741CC7C,0x6381E897,0x91EA6B94,0x82BA9860,0x70D11B63,
    0x2EC22747,0xDCA9A444,0xCFF957B0,0x3D92D4B3,0xE958B058,0x1B33335B,0x0863C0AF,0xFA0843AC,
    0xB445B8E7,0x462E3BE4,0x557EC810,0xA7154B13,0x73DF2FF8,0x81B4ACFB,0x92E45F0F,0x608FDC0C,
    0x3E9CE028,0xCCF7632B,0xDFA790DF,0x2DCC13DC,0xF9067737,0x0B6DF434,0x183D07C0,0xEA5684C3,
    0x84A6B156,0x76CD3255,0x659DC1A1,0x97F642A2,0x43BC2649,0xB1D7A54A,0xA28756BE,0x50ECD5BD,
    0x0EFFE999,0xFC946A9A,0xEFC4996E,0x1DAF1A6D,0xC9657E86,0x3B0EFD85,0x285E0E71,0xDA358D72,
    0x94787639,0x6613F53A,0x754306CE,0x872885CD,0x53E2E126,0xA1896225,0xB2D991D1,0x40B212D2,
    0x1EA12EF6,0xECCAADF5,0xFFBA5E01,0x0DD1DD02,0xD91BB9E9,0x2B703AEA,0x3820C91E,0xCA4B4A1D,
};

uint32_t crc32c(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0x82F63B78 & (-(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

/* ---- Helpers ---- */

static inline uint16_t r16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
static inline uint32_t r32(const uint8_t* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static inline void w16(uint8_t* p, uint16_t v) { p[0]=v>>8; p[1]=v; }
static inline void w32(uint8_t* p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

static inline size_t pad4(size_t n) { return (n + 3) & ~3; }

static void sctpSetChecksum(uint8_t* pkt, size_t len) {
    w32(pkt + 8, 0);
    uint32_t c = crc32c(pkt, len);
    pkt[8] = c & 0xff;
    pkt[9] = (c >> 8) & 0xff;
    pkt[10] = (c >> 16) & 0xff;
    pkt[11] = (c >> 24) & 0xff;
}

/* ---- Cookie: HMAC-SHA256 based stateless verification ---- */

struct sctp_cookie {
    uint32_t peerTag;
    uint32_t myTag;
    uint32_t peerTsn;
    uint32_t myTsn;
    uint16_t peerPort;
    uint16_t myPort;
    uint32_t timestamp;
    uint8_t  hmac[32];
};

static void cookieCompute(const sctp_assoc_t* a, sctp_cookie* c) {
    uint8_t buf[32 + 20];
    memcpy(buf, a->cookieSecret, 32);
    w32(buf + 32, c->peerTag);
    w32(buf + 36, c->myTag);
    w32(buf + 40, c->peerTsn);
    w32(buf + 44, c->myTsn);
    w16(buf + 48, c->peerPort);
    w16(buf + 50, c->myPort);
    mbedtls_sha256(buf, 52, c->hmac, 0);
}

static bool cookieVerify(const sctp_assoc_t* a, const sctp_cookie* c) {
    sctp_cookie tmp = *c;
    cookieCompute(a, &tmp);
    return memcmp(tmp.hmac, c->hmac, 32) == 0;
}

/* ---- SCTP common header (12 bytes) ---- */

static size_t writeHeader(uint8_t* out, uint16_t srcPort, uint16_t dstPort, uint32_t vTag) {
    w16(out, srcPort);
    w16(out + 2, dstPort);
    w32(out + 4, vTag);
    w32(out + 8, 0);
    return 12;
}

/* ---- INIT-ACK ---- */

static size_t buildInitAck(sctp_assoc_t* a, const uint8_t* initChunk, size_t initLen,
                           uint16_t peerPort, uint8_t* out, size_t outSize) {
    if (initLen < 16) return 0;

    uint32_t peerTag  = r32(initChunk + 0);
    uint32_t peerArwnd = r32(initChunk + 4);   /* peer's receive window */
    uint32_t peerTsn  = r32(initChunk + 12);

    /* Stash the advertised receiver window so sctpSend can honor it
       before the first SACK arrives. Safe even though INIT is stateless
       on our end — `a` is the single global association, and cookie_echo
       just confirms the same peer. */
    if (peerArwnd > 0) a->peerRwnd = peerArwnd;

    uint32_t myTag = esp_random();
    if (myTag == 0) myTag = 1;
    uint32_t myTsn = esp_random();

    sctp_cookie cookie = {};
    cookie.peerTag = peerTag;
    cookie.myTag = myTag;
    cookie.peerTsn = peerTsn;
    cookie.myTsn = myTsn;
    cookie.peerPort = peerPort;
    cookie.myPort = a->myPort;
    cookie.timestamp = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    cookieCompute(a, &cookie);

    size_t pos = writeHeader(out, a->myPort, peerPort, peerTag);

    size_t chunkStart = pos;
    out[pos++] = SCTP_INIT_ACK;
    out[pos++] = 0;
    pos += 2;

    w32(out + pos, myTag); pos += 4;
    w32(out + pos, 65535); pos += 4;
    w16(out + pos, SCTP_ANNOUNCED_STREAMS); pos += 2; /* OS */
    w16(out + pos, SCTP_ANNOUNCED_STREAMS); pos += 2; /* MIS */
    w32(out + pos, myTsn); pos += 4;

    /* State Cookie parameter (type=7) */
    w16(out + pos, 7); pos += 2;
    w16(out + pos, 4 + sizeof(cookie)); pos += 2;
    memcpy(out + pos, &cookie, sizeof(cookie));
    pos += pad4(sizeof(cookie));

    /* Forward-TSN-Supported parameter (0xC000) — required for PR-SCTP (RFC 3758) */
    w16(out + pos, 0xC000); pos += 2;
    w16(out + pos, 4); pos += 2;

    /* Supported Extensions parameter (0x8008) — advertise RE-CONFIG + FORWARD-TSN */
    w16(out + pos, 0x8008); pos += 2;
    w16(out + pos, 4 + 2); pos += 2;
    out[pos++] = SCTP_FORWARD_TSN;
    out[pos++] = SCTP_RECONFIG;
    pos = pad4(pos);

    w16(out + chunkStart + 2, (uint16_t)(pos - chunkStart));

    sctpSetChecksum(out, pos);
    dbg("SCTP INIT-ACK %d bytes, vTag=0x%08x myTag=0x%08x\n",
         (int)pos, (unsigned)peerTag, (unsigned)myTag);
    return pos;
}

/* ---- COOKIE-ACK ---- */

static size_t buildCookieAck(sctp_assoc_t* a, uint8_t* out) {
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    out[pos++] = SCTP_COOKIE_ACK;
    out[pos++] = 0;
    w16(out + pos, 4); pos += 2;
    sctpSetChecksum(out, pos);
    return pos;
}

/* ---- SACK ---- */

static size_t buildSack(sctp_assoc_t* a, uint8_t* out) {
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    out[pos++] = SCTP_SACK;
    out[pos++] = 0;
    w16(out + pos, 16); pos += 2;
    w32(out + pos, a->peerTsn); pos += 4;
    w32(out + pos, 65535); pos += 4;
    w16(out + pos, 0); pos += 2;
    w16(out + pos, 0); pos += 2;
    sctpSetChecksum(out, pos);
    return pos;
}

/* ---- HEARTBEAT-ACK ---- */

static size_t buildHeartbeatAck(sctp_assoc_t* a, const uint8_t* hbChunk, size_t hbLen,
                                uint8_t* out) {
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    out[pos] = SCTP_HEARTBEAT_ACK;
    memcpy(out + pos + 1, hbChunk + 1, hbLen - 1);
    pos += pad4(hbLen);
    sctpSetChecksum(out, pos);
    return pos;
}

/* ---- FORWARD-TSN (RFC 3758) ---- */

static size_t buildForwardTsn(sctp_assoc_t* a, uint32_t newCumTsn, uint8_t* out) {
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    size_t chunkStart = pos;
    out[pos++] = SCTP_FORWARD_TSN;
    out[pos++] = 0;
    pos += 2;
    w32(out + pos, newCumTsn); pos += 4;
    /* RFC 3758 §3.2: list each stream whose TSNs are being abandoned so the
       receiver flushes its per-stream reassembly queue. */
    for (int i = 0; i < a->numChannels; i++) {
        if (!a->channels[i].open) continue;
        w16(out + pos, a->channels[i].streamId); pos += 2;
        w16(out + pos, a->channels[i].ssn); pos += 2;
    }
    w16(out + chunkStart + 2, (uint16_t)(pos - chunkStart));
    sctpSetChecksum(out, pos);
    return pos;
}

/* ---- RE-CONFIG (RFC 6525) ---- */

/* Build a RE-CONFIG chunk with a Reconfiguration Response parameter
   acknowledging an incoming Outgoing SSN Reset Request. */
static size_t buildReconfigResponse(sctp_assoc_t* a, uint32_t responseSn,
                                    uint32_t result, uint8_t* out) {
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    size_t chunkStart = pos;
    out[pos++] = SCTP_RECONFIG;
    out[pos++] = 0;
    pos += 2;  /* length placeholder */

    /* Reconfiguration Response parameter (type 16): reqSn(4) result(4) */
    w16(out + pos, RECONFIG_RESPONSE); pos += 2;
    w16(out + pos, 12); pos += 2;
    w32(out + pos, responseSn); pos += 4;
    w32(out + pos, result); pos += 4;

    w16(out + chunkStart + 2, (uint16_t)(pos - chunkStart));
    pos = pad4(pos);
    sctpSetChecksum(out, pos);
    return pos;
}

/* Build a RE-CONFIG chunk with an Outgoing SSN Reset Request parameter
   closing the specified stream. */
static size_t buildReconfigOutReset(sctp_assoc_t* a, uint16_t streamId,
                                    uint32_t reqSn, uint8_t* out) {
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    size_t chunkStart = pos;
    out[pos++] = SCTP_RECONFIG;
    out[pos++] = 0;
    pos += 2;  /* length placeholder */

    /* Outgoing SSN Reset Request (type 13):
       reqSn(4) responseSn(4) senderLastTsn(4) streamIds(2 each) */
    w16(out + pos, RECONFIG_OUT_SSN_RESET); pos += 2;
    w16(out + pos, 16 + 2); pos += 2;  /* header + 12 body + 2 for one streamId */
    w32(out + pos, reqSn); pos += 4;
    w32(out + pos, reqSn); pos += 4;   /* response-sn reused */
    w32(out + pos, a->myTsn - 1); pos += 4;
    w16(out + pos, streamId); pos += 2;
    pos = pad4(pos);

    w16(out + chunkStart + 2, (uint16_t)(pos - chunkStart));
    sctpSetChecksum(out, pos);
    return pos;
}

/* Forward decls — defined below, used up here by handleReconfigChunk /
   handleDcepOpen. */
static void rexmitPurgeStream(sctp_assoc_t* a, uint16_t streamId);
static int  rexmitInsert(sctp_assoc_t* a, uint16_t streamId, uint32_t ppid,
                         uint16_t ssn, uint16_t priority,
                         bool ordered, bool reliableInfinite,
                         uint8_t maxRexmit, uint32_t firstTsn, uint32_t lastTsn,
                         const uint8_t* data, size_t dataLen);

static void handleReconfigChunk(sctp_assoc_t* a, const uint8_t* chunk, size_t chunkLen,
                                uint8_t* out, size_t* outLen) {
    /* Walk parameters inside the chunk (skip 4-byte chunk header). */
    size_t pOff = 4;
    uint32_t responseSn = 0;
    bool sawOutReset = false;
    while (pOff + 4 <= chunkLen) {
        uint16_t ptype = r16(chunk + pOff);
        uint16_t plen  = r16(chunk + pOff + 2);
        if (plen < 4 || pOff + plen > chunkLen) break;

        if (ptype == RECONFIG_OUT_SSN_RESET && plen >= 16) {
            /* reqSn(4) responseSn(4) lastTsn(4) streamIds(...) */
            responseSn = r32(chunk + pOff + 4);
            size_t idsOff = pOff + 16;
            size_t idsEnd = pOff + plen;
            while (idsOff + 2 <= idsEnd) {
                uint16_t sid = r16(chunk + idsOff);
                idsOff += 2;
                /* Mark channel closed, purge its rexmit data, free any
                   in-flight inbound reassembly buffer, fire callback */
                for (int i = 0; i < a->numChannels; i++) {
                    if (a->channels[i].open && a->channels[i].streamId == sid) {
                        a->channels[i].open = false;
                        if (a->channels[i].rxBuf) {
                            heap_caps_free(a->channels[i].rxBuf);
                            a->channels[i].rxBuf = nullptr;
                            a->channels[i].rxLen = a->channels[i].rxCap = 0;
                        }
                        rexmitPurgeStream(a, sid);
                        if (a->onDcreset) a->onDcreset(a, sid);
                        dbg("DC stream %u closed via RE-CONFIG\n", sid);
                        break;
                    }
                }
            }
            sawOutReset = true;
        } else if (ptype == RECONFIG_RESPONSE && plen >= 12) {
            /* Peer's response to our outgoing reset — no action required beyond log */
            uint32_t rSn = r32(chunk + pOff + 4);
            uint32_t result = r32(chunk + pOff + 8);
            dbg("RE-CONFIG response reqSn=%u result=%u\n",
                (unsigned)rSn, (unsigned)result);
        }
        /* Parameters pad to 4-byte boundary */
        pOff += pad4(plen);
    }

    if (sawOutReset) {
        /* Respond success */
        *outLen = buildReconfigResponse(a, responseSn, 0 /* success */, out);
    }
}

/* ---- DCEP handling ---- */

static size_t handleDcepOpen(sctp_assoc_t* a, uint16_t streamId,
                             const uint8_t* data, size_t dataLen,
                             uint8_t* out) {
    if (dataLen < 12) return 0;

    /* Parse DCEP OPEN: type(1) channelType(1) priority(2) reliability(4) labelLen(2) protoLen(2) */
    uint8_t  channelType = data[1];
    uint16_t priority    = r16(data + 2);
    uint32_t reliability = r32(data + 4);
    uint16_t labelLen    = r16(data + 8);
    uint16_t protoLen    = r16(data + 10);

    /* Duplicate stream: re-use the existing slot if already open, otherwise refuse. */
    for (int i = 0; i < a->numChannels; i++) {
        if (a->channels[i].open && a->channels[i].streamId == streamId) return 0;
    }

    /* Find free slot — may reuse a closed slot. */
    int slot = -1;
    for (int i = 0; i < a->numChannels; i++) {
        if (!a->channels[i].open) { slot = i; break; }
    }
    if (slot < 0 && a->numChannels < DC_MAX_CHANNELS) slot = a->numChannels++;
    if (slot < 0) {
        err("DC OPEN rejected: no free channel slot\n");
        return 0;
    }

    dc_channel_t* ch = &a->channels[slot];
    memset(ch, 0, sizeof(*ch));
    ch->open = true;
    ch->streamId = streamId;
    ch->channelType = channelType;
    ch->reliability = reliability;
    ch->priority = priority;
    ch->ssn = 0;

    /* Label */
    size_t off = 12;
    if (labelLen > 0 && labelLen < sizeof(ch->label) && off + labelLen <= dataLen) {
        memcpy(ch->label, data + off, labelLen);
        ch->label[labelLen] = '\0';
    } else {
        ch->label[0] = '\0';
    }
    off += labelLen;

    /* Protocol */
    if (protoLen > 0 && off + protoLen <= dataLen) {
        uint16_t copy = protoLen < sizeof(ch->protocol) ? protoLen : (sizeof(ch->protocol) - 1);
        memcpy(ch->protocol, data + off, copy);
        ch->protocol[copy] = '\0';
        ch->protoLen = copy;
    } else {
        ch->protocol[0] = '\0';
        ch->protoLen = 0;
    }

    dbg("DC channel %d: label=\"%s\" proto=\"%s\" type=0x%02x rel=%u prio=%u stream=%u\n",
        slot, ch->label, ch->protocol, channelType,
        (unsigned)reliability, (unsigned)priority, streamId);

    /* Fire onDceopen so higher layer can itsConnect before we ACK. */
    if (a->onDceopen) a->onDceopen(a, slot);

    /* If higher layer closed the channel during onDceopen, skip ACK. */
    if (!ch->open) return 0;

    /* DCEP ACK rides the channel as an ordered SCTP DATA chunk — application
       DATA on this stream must come after it in SSN order, so we consume an
       SSN here. The ACK also goes into the rexmit pool so it survives a
       dropped UDP packet (without that, a single lost ACK leaves the peer's
       cumTsn stuck one TSN short of every subsequent message). */
    uint32_t tsn = a->myTsn++;
    uint16_t ssn = ch->ssn++;
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    size_t chunkStart = pos;
    out[pos++] = SCTP_DATA;
    out[pos++] = SCTP_DATA_E | SCTP_DATA_B;
    pos += 2;
    w32(out + pos, tsn); pos += 4;
    w16(out + pos, streamId); pos += 2;
    w16(out + pos, ssn); pos += 2;
    w32(out + pos, PPID_DCEP); pos += 4;
    out[pos++] = DCEP_ACK;
    while ((pos - chunkStart) % 4) out[pos++] = 0;
    w16(out + chunkStart + 2, (uint16_t)(pos - chunkStart));

    sctpSetChecksum(out, pos);

    /* Put the ACK in the rexmit pool at `high` priority so application data
       can't crowd it out of the pool's byte quota. */
    static const uint8_t dcepAckPayload = DCEP_ACK;
    rexmitInsert(a, streamId, PPID_DCEP, ssn, /*priority=*/1024,
                 /*ordered=*/true, /*reliableInfinite=*/true,
                 /*maxRexmit=*/0, tsn, tsn,
                 &dcepAckPayload, 1);

    return pos;
}

/* ---- Rexmit pool helpers ---- */

static void rexmitFreeSlot(sctp_assoc_t* a, int i) {
    rexmit_msg_t& e = a->rexmit[i];
    if (e.data) {
        if (a->rexmitBytes >= e.dataLen) a->rexmitBytes -= e.dataLen;
        else a->rexmitBytes = 0;
        free(e.data);
        e.data = nullptr;
        e.dataLen = 0;
    }
}

/* After a stream reset purges that stream's rexmit entries, the peer is
   still waiting on those TSNs — its cumTsn can't advance past data we've
   forgotten. Compute the highest TSN safe to tell the peer to skip: one
   below the lowest unacked TSN still in the pool, or myTsn-1 if the pool
   is empty. The caller sends a FORWARD-TSN with that value. 0 = no-op. */
static uint32_t rexmitComputeForwardTsn(sctp_assoc_t* a) {
    bool any = false;
    uint32_t lowest = 0;
    for (int i = 0; i < SCTP_REXMIT_SLOTS; i++) {
        if (!a->rexmit[i].data) continue;
        uint32_t ft = a->rexmit[i].firstTsn;
        if (!any || (int32_t)(ft - lowest) < 0) { lowest = ft; any = true; }
    }
    uint32_t advanceTo = any ? (lowest - 1) : (a->myTsn - 1);
    /* Only useful if it actually moves cumTsn forward. */
    if ((int32_t)(advanceTo - a->sackCumTsn) <= 0) return 0;
    return advanceTo;
}

static void rexmitPurgeStream(sctp_assoc_t* a, uint16_t streamId) {
    bool anyPurged = false;
    for (int i = 0; i < SCTP_REXMIT_SLOTS; i++) {
        if (a->rexmit[i].data && a->rexmit[i].streamId == streamId) {
            rexmitFreeSlot(a, i);
            anyPurged = true;
        }
    }
    if (anyPurged) {
        uint32_t advanceTo = rexmitComputeForwardTsn(a);
        if (advanceTo != 0) a->pendingFwdTsn = advanceTo;
    }
}

/* Per-priority rexmit-pool high-water mark. Higher-priority channels can
   consume more of the pool; lower-priority ones are capped earlier so a
   video flood never starves an audio or EPL burst. Thresholds chosen from
   the four standard DCEP priority levels (128/256/512/1024) with gaps left
   on purpose for future intermediate classes. */
static size_t rexmitQuota(uint16_t priority) {
    if (priority >= 1024) return SCTP_REXMIT_POOL_BYTES;             /* high: full pool */
    if (priority >=  512) return (SCTP_REXMIT_POOL_BYTES * 7) / 8;   /* medium: 7/8 */
    if (priority >=  256) return (SCTP_REXMIT_POOL_BYTES * 3) / 4;   /* low: 3/4 */
    return SCTP_REXMIT_POOL_BYTES / 2;                                /* very-low: 1/2 */
}

/* Try to insert a fresh message into the rexmit pool. Returns slot index,
   or -1 if quota full for this priority (caller should back off). */
static int rexmitInsert(sctp_assoc_t* a, uint16_t streamId, uint32_t ppid,
                        uint16_t ssn, uint16_t priority,
                        bool ordered, bool reliableInfinite,
                        uint8_t maxRexmit, uint32_t firstTsn, uint32_t lastTsn,
                        const uint8_t* data, size_t dataLen) {
    if (dataLen + a->rexmitBytes > rexmitQuota(priority)) return -1;

    /* Find empty slot (linear probe from head) */
    int slot = -1;
    for (int n = 0; n < SCTP_REXMIT_SLOTS; n++) {
        int i = (a->rexmitHead + n) % SCTP_REXMIT_SLOTS;
        if (!a->rexmit[i].data) { slot = i; break; }
    }
    if (slot < 0) return -1;  /* all slots full */

    uint8_t* buf = (uint8_t*)heap_caps_malloc(dataLen, MALLOC_CAP_SPIRAM);
    if (!buf) return -1;
    memcpy(buf, data, dataLen);

    rexmit_msg_t& e = a->rexmit[slot];
    e.firstTsn = firstTsn;
    e.lastTsn = lastTsn;
    e.streamId = streamId;
    e.ssn = ssn;
    e.ppid = ppid;
    e.ordered = ordered;
    e.reliableInfinite = reliableInfinite;
    e.maxRexmit = maxRexmit;
    e.rexmitCount = 0;
    e.data = buf;
    e.dataLen = dataLen;
    a->rexmitBytes += dataLen;
    a->rexmitHead = (slot + 1) % SCTP_REXMIT_SLOTS;
    return slot;
}

/* ---- Build a DATA fragment packet ---- */

static const size_t MAX_PAYLOAD = 1400;  /* leave room for DTLS+UDP+IP+WG */
static const size_t CHUNK_HDR = 16;      /* SCTP DATA chunk header */
static const size_t MAX_DATA = MAX_PAYLOAD - 12 - CHUNK_HDR;

static size_t buildDataFragment(sctp_assoc_t* a, uint32_t tsn, uint16_t streamId,
                                uint16_t ssn, uint32_t ppid, bool unordered,
                                bool first, bool last,
                                const uint8_t* data, size_t fragLen,
                                uint8_t* pkt) {
    size_t pos = writeHeader(pkt, a->myPort, a->peerPort, a->peerTag);

    uint8_t flags = 0;
    if (unordered) flags |= SCTP_DATA_U;
    if (first)     flags |= SCTP_DATA_B;
    if (last)      flags |= SCTP_DATA_E | SCTP_DATA_I;

    size_t chunkStart = pos;
    pkt[pos++] = SCTP_DATA;
    pkt[pos++] = flags;
    pos += 2;
    w32(pkt + pos, tsn); pos += 4;
    w16(pkt + pos, streamId); pos += 2;
    w16(pkt + pos, ssn); pos += 2;
    w32(pkt + pos, ppid); pos += 4;
    memcpy(pkt + pos, data, fragLen);
    pos += fragLen;

    w16(pkt + chunkStart + 2, (uint16_t)(pos - chunkStart));
    while (pos % 4) pkt[pos++] = 0;

    sctpSetChecksum(pkt, pos);
    return pos;
}

/* ---- Process incoming DATA chunk ---- */

/** Reassemble inbound DATA fragments per channel. Browser fragments any
 *  dc.send() larger than MAX_PAYLOAD (1400B) into multiple DATA chunks
 *  with B=1 on the first and E=1 on the last; ordered channels arrive in
 *  TSN order thanks to SCTP's cumulative TSN semantics. We accumulate
 *  into the channel's rxBuf and fire onData once E=1 lands. PSRAM,
 *  capped at 64KB per message (matches SDP max-message-size). */
static void processDataFragment(sctp_assoc_t* a, uint16_t streamId,
                                uint32_t ppid, uint8_t flags,
                                const uint8_t* data, size_t dataLen) {
    int chIdx = sctpFindChannelByStream(a, streamId);
    if (chIdx < 0) return;  /* unknown stream — ignore */
    dc_channel_t* c = &a->channels[chIdx];

    bool beginning = (flags & SCTP_DATA_B) != 0;
    bool end       = (flags & SCTP_DATA_E) != 0;

    /* Fast path: whole message in one chunk — no alloc. */
    if (beginning && end) {
        if (a->onData) a->onData(a, streamId, ppid, data, dataLen);
        return;
    }

    if (beginning) {
        /* Start a fresh reassembly buffer. Drop any orphan from a prior
           incomplete message on this stream (shouldn't happen with
           ordered delivery, but defensive). */
        if (c->rxBuf) { heap_caps_free(c->rxBuf); c->rxBuf = nullptr; }
        c->rxCap = dataLen < 4096 ? 4096 : dataLen * 2;
        c->rxBuf = (uint8_t*)heap_caps_malloc(c->rxCap, MALLOC_CAP_SPIRAM);
        c->rxLen = 0;
        c->rxPpid = ppid;
    }
    if (!c->rxBuf) return;  /* alloc failed or orphan continuation */

    /* Grow buffer if this fragment would overflow. Cap at 64KB. */
    constexpr size_t MAX_MESSAGE = 65536;
    if (c->rxLen + dataLen > c->rxCap) {
        size_t newCap = c->rxCap;
        while (newCap < c->rxLen + dataLen) newCap *= 2;
        if (newCap > MAX_MESSAGE) newCap = MAX_MESSAGE;
        if (c->rxLen + dataLen > newCap) {
            warn("SCTP reassembly overflow on stream %u (>%u) — dropping\n",
                 (unsigned)streamId, (unsigned)MAX_MESSAGE);
            heap_caps_free(c->rxBuf);
            c->rxBuf = nullptr;
            c->rxLen = c->rxCap = 0;
            return;
        }
        uint8_t* nb = (uint8_t*)heap_caps_realloc(c->rxBuf, newCap, MALLOC_CAP_SPIRAM);
        if (!nb) {
            heap_caps_free(c->rxBuf);
            c->rxBuf = nullptr;
            c->rxLen = c->rxCap = 0;
            return;
        }
        c->rxBuf = nb;
        c->rxCap = newCap;
    }

    memcpy(c->rxBuf + c->rxLen, data, dataLen);
    c->rxLen += dataLen;

    if (end) {
        if (a->onData) a->onData(a, streamId, c->rxPpid, c->rxBuf, c->rxLen);
        heap_caps_free(c->rxBuf);
        c->rxBuf = nullptr;
        c->rxLen = c->rxCap = 0;
    }
}

static size_t processDataChunk(sctp_assoc_t* a, const uint8_t* chunk, size_t chunkLen,
                               uint8_t* out, size_t outSize) {
    if (chunkLen < 16) return 0;

    uint8_t  flags    = chunk[1];
    uint32_t tsn      = r32(chunk + 4);
    uint16_t streamId = r16(chunk + 8);
    uint32_t ppid     = r32(chunk + 12);
    const uint8_t* data = chunk + 16;
    size_t dataLen = chunkLen - 16;
    uint16_t realChunkLen = r16(chunk + 2);
    if (realChunkLen > 16 && realChunkLen - 16 < dataLen)
        dataLen = realChunkLen - 16;

    a->peerTsn = tsn;

    size_t outLen = 0;

    if (ppid == PPID_DCEP && dataLen > 0 && data[0] == DCEP_OPEN) {
        outLen = handleDcepOpen(a, streamId, data, dataLen, out);
    } else if (ppid != PPID_DCEP) {
        /* User data — forward to onData with per-stream reassembly. */
        processDataFragment(a, streamId, ppid, flags, data, dataLen);
    }

    return outLen;
}

/* ---- Public API ---- */

void sctpInit(sctp_assoc_t* a, uint8_t* outBuf, size_t outBufSize, uint16_t sctpPort) {
    /* Keep callbacks across init (caller may have registered before we ran). */
    sctp_dceopen_cb_t saveOpen = a->onDceopen;
    sctp_dcreset_cb_t saveReset = a->onDcreset;
    sctp_data_cb_t    saveData  = a->onData;
    sctpRexmitFree(a);
    /* Free any in-flight inbound reassembly buffers before zeroing. */
    for (int i = 0; i < a->numChannels; i++) {
        if (a->channels[i].rxBuf) {
            heap_caps_free(a->channels[i].rxBuf);
            a->channels[i].rxBuf = nullptr;
        }
    }
    memset(a, 0, sizeof(*a));
    a->outBuf = outBuf;
    a->outBufSize = outBufSize;
    a->myPort = sctpPort;
    a->onDceopen = saveOpen;
    a->onDcreset = saveReset;
    a->onData    = saveData;
    esp_fill_random(a->cookieSecret, sizeof(a->cookieSecret));
}

int sctpInput(sctp_assoc_t* a, const uint8_t* pkt, size_t pktLen, size_t* outLen) {
    *outLen = 0;
    if (pktLen < 12) return -1;
    int peerAbort = 0;

    uint16_t srcPort = r16(pkt);
    uint16_t dstPort = r16(pkt + 2);
    uint32_t vTag = r32(pkt + 4);
    verb("SCTP hdr: src=%d dst=%d vTag=0x%08x (our port=%d)\n",
        srcPort, dstPort, (unsigned)vTag, a->myPort);

    size_t offset = 12;
    while (offset + 4 <= pktLen) {
        uint8_t type = pkt[offset];
        uint16_t chunkLen = r16(pkt + offset + 2);
        if (chunkLen < 4 || offset + chunkLen > pktLen) break;

        const uint8_t* chunk = pkt + offset;

        verb("SCTP chunk type=%d len=%d\n", type, chunkLen);

        switch (type) {
            case SCTP_INIT:
                if (vTag != 0) { dbg("SCTP INIT bad vTag=%u\n", (unsigned)vTag); break; }
                *outLen = buildInitAck(a, chunk + 4, chunkLen - 4, srcPort,
                                       a->outBuf, a->outBufSize);
                break;

            case SCTP_COOKIE_ECHO: {
                if (chunkLen < 4 + sizeof(sctp_cookie)) break;
                const sctp_cookie* cookie = (const sctp_cookie*)(chunk + 4);
                if (!cookieVerify(a, cookie)) {
                    dbg("SCTP cookie verify failed\n");
                    break;
                }

                a->established = true;
                a->peerTag = cookie->peerTag;
                a->myTag = cookie->myTag;
                a->myTsn = cookie->myTsn;
                a->peerTsn = cookie->peerTsn - 1;
                a->peerPort = cookie->peerPort;
                /* peerRwnd was set from INIT's a_rwnd in buildInitAck. */

                *outLen = buildCookieAck(a, a->outBuf);
                info("SCTP association established\n");
                break;
            }

            case SCTP_DATA:
                if (!a->established || vTag != a->myTag) break;
                *outLen = processDataChunk(a, chunk, chunkLen, a->outBuf, a->outBufSize);
                if (*outLen == 0)
                    *outLen = buildSack(a, a->outBuf);
                break;

            case SCTP_SACK:
                if (!a->established) break;
                if (chunkLen >= 16) {
                    uint32_t cumTsn = r32(chunk + 4);
                    a->peerRwnd = r32(chunk + 8);
                    a->sackCumTsn = cumTsn;
                    uint16_t numGaps = r16(chunk + 12);
                    uint16_t numDups = r16(chunk + 14);
                    /* Store gap blocks so retransmit can target only the
                       holes. Each block is two 16-bit offsets from cumTsn
                       describing an already-received range (start..end). */
                    size_t gapOff = 16;
                    uint16_t storeGaps = numGaps;
                    if (storeGaps > 128) storeGaps = 128;
                    for (uint16_t g = 0; g < storeGaps; g++) {
                        if (gapOff + 4 > chunkLen) { storeGaps = g; break; }
                        a->sackGapStart[g] = r16(chunk + gapOff);
                        a->sackGapEnd[g]   = r16(chunk + gapOff + 2);
                        gapOff += 4;
                    }
                    a->sackNumGaps = storeGaps;
                    /* Must match the RTO check's clock (millis()) — mixing
                       clocks here causes wraparound in the elapsed-time
                       subtraction and fires spurious retransmits. */
                    a->lastSackMs = millis();
                    /* Free any pool entry whose TSN range the peer has
                       entirely acknowledged — either covered by cumTsn,
                       or lying fully within a Gap Ack Block (received
                       out of order past an earlier loss). Gap-block
                       freeing is important when cumTsn is stuck behind
                       an abandoned stream's TSNs: new in-flight data
                       still clears out normally. */
                    for (int i = 0; i < SCTP_REXMIT_SLOTS; i++) {
                        rexmit_msg_t& e = a->rexmit[i];
                        if (!e.data) continue;
                        if ((int32_t)(e.lastTsn - cumTsn) <= 0) {
                            rexmitFreeSlot(a, i);
                            continue;
                        }
                        for (uint16_t g = 0; g < storeGaps; g++) {
                            int32_t lo = (int32_t)(e.firstTsn - cumTsn);
                            int32_t hi = (int32_t)(e.lastTsn  - cumTsn);
                            if (lo >= (int32_t)a->sackGapStart[g] &&
                                hi <= (int32_t)a->sackGapEnd[g]) {
                                rexmitFreeSlot(a, i);
                                break;
                            }
                        }
                    }
                    if (numGaps > 0) a->sackHasGaps = true;
                    verb("SACK cumTsn=%u rwnd=%u gaps=%u dups=%u rexmitBytes=%u myTsn=%u\n",
                         (unsigned)cumTsn, (unsigned)a->peerRwnd,
                         numGaps, numDups,
                         (unsigned)a->rexmitBytes, (unsigned)a->myTsn);
                }
                break;

            case SCTP_HEARTBEAT:
                if (!a->established) break;
                dbg("SCTP HEARTBEAT received (%u bytes)\n", chunkLen);
                *outLen = buildHeartbeatAck(a, chunk, chunkLen, a->outBuf);
                break;

            case SCTP_ABORT:
                if (a->established) {
                    size_t cOff = 4;
                    while (cOff + 4 <= chunkLen) {
                        uint16_t causeCode = r16(chunk + cOff);
                        uint16_t causeLen  = r16(chunk + cOff + 2);
                        dbg("SCTP ABORT cause=%u len=%u\n", causeCode, causeLen);
                        if (causeLen > 4 && cOff + causeLen <= chunkLen) {
                            size_t valLen = causeLen - 4;
                            char reason[64];
                            if (valLen >= sizeof(reason)) valLen = sizeof(reason) - 1;
                            memcpy(reason, chunk + cOff + 4, valLen);
                            reason[valLen] = '\0';
                            dbg("SCTP ABORT reason: %s\n", reason);
                        }
                        if (causeLen < 4) break;
                        cOff += pad4(causeLen);
                    }
                    /* Fire onDcreset for every open channel before teardown,
                       and free any inbound reassembly buffers. */
                    for (int i = 0; i < a->numChannels; i++) {
                        if (a->channels[i].open && a->onDcreset)
                            a->onDcreset(a, a->channels[i].streamId);
                        a->channels[i].open = false;
                        if (a->channels[i].rxBuf) {
                            heap_caps_free(a->channels[i].rxBuf);
                            a->channels[i].rxBuf = nullptr;
                            a->channels[i].rxLen = a->channels[i].rxCap = 0;
                        }
                    }
                    a->established = false;
                    a->numChannels = 0;
                    sctpRexmitFree(a);
                    peerAbort = 1;
                }
                break;

            case SCTP_RECONFIG:
                if (!a->established) break;
                handleReconfigChunk(a, chunk, chunkLen, a->outBuf, outLen);
                break;

            case SCTP_FORWARD_TSN:
                if (chunkLen >= 8)
                    a->peerTsn = r32(chunk + 4);
                break;

            default:
                dbg("SCTP unknown chunk type %d\n", type);
                break;
        }

        offset += pad4(chunkLen);
    }

    return peerAbort;
}

/* ---- sctpSend ---- */

int sctpSend(sctp_assoc_t* a, uint16_t streamId, uint32_t ppid,
             const uint8_t* data, size_t dataLen,
             sctp_send_fn sendFn, void* ctx) {
    if (!a->established) return -1;

    /* Look up channel to pick ordered/reliability */
    dc_channel_t* ch = nullptr;
    for (int i = 0; i < a->numChannels; i++) {
        if (a->channels[i].open && a->channels[i].streamId == streamId) {
            ch = &a->channels[i];
            break;
        }
    }

    /* Defaults if no channel (e.g. raw DCEP): ordered, reliable, top priority. */
    bool unordered        = false;
    bool reliableInfinite = true;
    uint8_t maxRexmit     = 0;
    uint16_t priority     = 1024;   /* treat control as top priority */

    if (ch) {
        unordered = (ch->channelType & 0x80) != 0;
        uint8_t base = ch->channelType & 0x7F;
        reliableInfinite = (base == 0x00);
        bool partialRexmit = (base == 0x01);
        bool timed         = (base == 0x02);
        if (partialRexmit) {
            maxRexmit = (ch->reliability > 255) ? 255 : (uint8_t)ch->reliability;
        } else if (timed) {
            /* PR_SCTP TIMED not modelled precisely — treat as small retry budget. */
            reliableInfinite = false;
            maxRexmit = 3;
        }
        priority = ch->priority;
    }

    /* Number of fragments */
    size_t nFrags = dataLen ? ((dataLen + MAX_DATA - 1) / MAX_DATA) : 1;

    /* Allocate contiguous TSN range */
    uint32_t firstTsn = a->myTsn;
    uint32_t lastTsn  = firstTsn + (uint32_t)nFrags - 1;
    a->myTsn += (uint32_t)nFrags;

    /* SSN (ordered only) */
    uint16_t ssn = 0;
    if (!unordered && ch) {
        ssn = ch->ssn++;
    }

    /* Honor peer's advertised receive window: peerRwnd is the free
       buffer space the peer reports. If this message wouldn't fit, back
       off and wait for a SACK to reopen the window. peerRwnd is updated
       on every SACK (receiver's authoritative free-space reading) and
       decremented locally below once we commit to sending, so we don't
       over-send between SACKs. */
    if ((uint64_t)dataLen > a->peerRwnd) {
        a->myTsn -= (uint32_t)nFrags;
        if (!unordered && ch && ch->ssn > 0) ch->ssn--;
        return 0;
    }

    /* Insert into rexmit pool if this channel wants retransmission */
    bool wantRexmit = reliableInfinite || maxRexmit > 0;
    if (wantRexmit) {
        int slot = rexmitInsert(a, streamId, ppid, ssn, priority, !unordered,
                                reliableInfinite, maxRexmit,
                                firstTsn, lastTsn, data, dataLen);
        if (slot < 0 && reliableInfinite) {
            /* Quota full at this priority — back off. Higher-priority sends
               can still insert because their quota is larger. */
            a->myTsn -= (uint32_t)nFrags;
            if (!unordered && ch && ch->ssn > 0) ch->ssn--;
            return 0;
        }
        /* For partial-reliable, losing the rexmit copy just drops reliability
           for this message — not fatal; continue and send fire-and-forget. */
    }

    /* Account for this message in the peer's buffer — don't over-send
       before the next SACK updates peerRwnd. SACK will snap us back to
       the receiver's authoritative value when it arrives. */
    if (a->peerRwnd >= dataLen) a->peerRwnd -= dataLen;
    else a->peerRwnd = 0;

    /* Fragment + send */
    uint8_t pkt[12 + CHUNK_HDR + MAX_PAYLOAD + 4];
    int nPkts = 0;
    size_t sent = 0;
    uint32_t tsn = firstTsn;

    while (sent < dataLen || (dataLen == 0 && nPkts == 0)) {
        size_t fragLen = dataLen - sent;
        if (fragLen > MAX_DATA) fragLen = MAX_DATA;
        bool first = (sent == 0);
        bool last  = (sent + fragLen >= dataLen);

        size_t pktLen = buildDataFragment(a, tsn, streamId, ssn, ppid,
                                          unordered, first, last,
                                          data + sent, fragLen, pkt);

        int ret = sendFn(pkt, pktLen, ctx);
        if (ret == 0) {
            /* WANT_WRITE — yield + retry, same packet */
            for (int attempt = 0; attempt < 5 && ret == 0; attempt++) {
                vTaskDelay(1);
                ret = sendFn(pkt, pktLen, ctx);
            }
        }
        if (ret <= 0) {
            /* Give up for now — stored message (if reliable) will retransmit
               missing fragments on SACK gap. */
            return nPkts > 0 ? nPkts : -1;
        }

        sent += fragLen;
        tsn++;
        nPkts++;
        if (dataLen == 0) break;
    }

    return nPkts;
}

int sctpStreamReset(sctp_assoc_t* a, uint16_t streamId,
                    sctp_send_fn sendFn, void* ctx) {
    if (!a->established) return -1;

    /* Mark channel closed locally so further sctpSend on it no-ops.
       Purge any rexmit entries — the peer drops the stream on our reset,
       so retransmitting its TSNs is pointless and pins pool space. */
    for (int i = 0; i < a->numChannels; i++) {
        if (a->channels[i].open && a->channels[i].streamId == streamId) {
            a->channels[i].open = false;
            break;
        }
    }
    rexmitPurgeStream(a, streamId);

    size_t pktLen = buildReconfigOutReset(a, streamId, ++a->myReconfigReqSn, a->outBuf);
    int ret = sendFn(a->outBuf, pktLen, ctx);
    if (ret == 0) {
        vTaskDelay(1);
        ret = sendFn(a->outBuf, pktLen, ctx);
    }
    return ret > 0 ? 1 : -1;
}

int sctpFindChannel(sctp_assoc_t* a, const char* label) {
    for (int i = 0; i < a->numChannels; i++)
        if (a->channels[i].open && strcmp(a->channels[i].label, label) == 0) return i;
    return -1;
}

int sctpFindChannelByStream(sctp_assoc_t* a, uint16_t streamId) {
    for (int i = 0; i < a->numChannels; i++)
        if (a->channels[i].open && a->channels[i].streamId == streamId) return i;
    return -1;
}

int sctpRetransmit(sctp_assoc_t* a, sctp_send_fn sendFn, void* ctx, bool forceAll) {
    if (!a->established) return 0;
    if (!forceAll && !a->sackHasGaps) return 0;
    a->sackHasGaps = false;
    int count = 0;

    uint8_t pkt[12 + CHUNK_HDR + MAX_PAYLOAD + 4];

    /* Cap the burst so a full-pool RTO retransmit (up to ~1 MB of ChaCha20
       encryption) doesn't monopolize the core past the 5 s watchdog.
       Unsent fragments roll over to the next RTO call. */
    const int MAX_FRAGS_PER_CALL = 32;

    for (int i = 0; i < SCTP_REXMIT_SLOTS && count < MAX_FRAGS_PER_CALL; i++) {
        rexmit_msg_t& e = a->rexmit[i];
        if (!e.data) continue;

        /* Fully covered by cumulative ack? */
        if ((int32_t)(e.lastTsn - a->sackCumTsn) <= 0) { rexmitFreeSlot(a, i); continue; }

        /* Retry budget exceeded? */
        if (!e.reliableInfinite && e.rexmitCount >= e.maxRexmit) {
            dbg("rexmit give up stream=%u firstTsn=%u\n",
                e.streamId, (unsigned)e.firstTsn);
            rexmitFreeSlot(a, i);
            continue;
        }

        /* Walk each fragment TSN past cumTsn and resend ONLY those the
           peer hasn't covered by a Gap Ack Block. Stale gap info from a
           now-old SACK is still more accurate than ignoring it — peer
           won't acknowledge duplicate retransmits quickly and the link
           wastes bandwidth. */
        size_t nFrags = e.dataLen ? ((e.dataLen + MAX_DATA - 1) / MAX_DATA) : 1;
        bool unordered = !e.ordered;
        bool anySent = false;
        for (size_t f = 0; f < nFrags && count < MAX_FRAGS_PER_CALL; f++) {
            uint32_t tsn = e.firstTsn + (uint32_t)f;
            if ((int32_t)(tsn - a->sackCumTsn) <= 0) continue;

            /* Is this TSN covered by a Gap Ack Block? If so, peer has it. */
            uint32_t offFromCum = tsn - a->sackCumTsn;
            bool received = false;
            for (uint16_t g = 0; g < a->sackNumGaps; g++) {
                if (offFromCum >= a->sackGapStart[g] &&
                    offFromCum <= a->sackGapEnd[g]) {
                    received = true;
                    break;
                }
            }
            if (received) continue;

            size_t off = f * MAX_DATA;
            size_t fragLen = e.dataLen - off;
            if (fragLen > MAX_DATA) fragLen = MAX_DATA;
            bool first = (f == 0);
            bool last  = (f == nFrags - 1) || (e.dataLen == 0);

            size_t pktLen = buildDataFragment(a, tsn, e.streamId, e.ssn, e.ppid,
                                              unordered, first, last,
                                              e.data + off, fragLen, pkt);

            int ret = sendFn(pkt, pktLen, ctx);
            if (ret == 0) { vTaskDelay(1); ret = sendFn(pkt, pktLen, ctx); }
            if (ret > 0) {
                anySent = true;
                count++;
                /* Feed IDLE0 every few fragments — ChaCha20 encrypt +
                   sendto in a tight loop can starve the watchdog feeder
                   on core 0 otherwise. */
                if ((count & 7) == 0) vTaskDelay(1);
            }
            else break;
        }
        if (anySent) e.rexmitCount++;
    }
    return count;
}

void sctpBuildForwardTsn(sctp_assoc_t* a, uint32_t newCumTsn, size_t* outLen) {
    *outLen = buildForwardTsn(a, newCumTsn, a->outBuf);
    a->fwdTsnSent = newCumTsn;
}

void sctpRexmitFree(sctp_assoc_t* a) {
    for (int i = 0; i < SCTP_REXMIT_SLOTS; i++) {
        if (a->rexmit[i].data) rexmitFreeSlot(a, i);
    }
    a->rexmitHead = 0;
    a->rexmitBytes = 0;
}
