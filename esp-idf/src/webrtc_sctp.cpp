/**
 * webrtc_sctp — Minimal SCTP for WebRTC DataChannel.
 *
 * Server-side only (browser initiates). Supports:
 *   - Four-way handshake with stateless cookie
 *   - DATA send (fragmented) + receive (small DCEP messages)
 *   - SACK generation + parsing
 *   - DCEP DATA_CHANNEL_OPEN/ACK
 *   - HEARTBEAT/HEARTBEAT-ACK
 *   - CRC32C checksum
 */
#include "webrtc_sctp.h"
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
    /* Compute CRC32C using polynomial 0x82F63B78 (Castagnoli, reflected) */
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

/* Round up to 4-byte boundary */
static inline size_t pad4(size_t n) { return (n + 3) & ~3; }

static void sctpSetChecksum(uint8_t* pkt, size_t len) {
    w32(pkt + 8, 0);
    /* CRC32C stored in SCTP in little-endian (RFC 3309 appendix, Chrome byte-swaps) */
    uint32_t c = crc32c(pkt, len);
    pkt[8] = c & 0xff;
    pkt[9] = (c >> 8) & 0xff;
    pkt[10] = (c >> 16) & 0xff;
    pkt[11] = (c >> 24) & 0xff;
}

/* Checksum verification skipped — DTLS provides integrity (RFC 8261) */

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
    /* HMAC = SHA256(secret || peerTag || myTag || peerTsn || myTsn || ports || ts) */
    uint8_t buf[32 + 20]; /* secret + fields */
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
    w32(out + 8, 0); /* checksum placeholder */
    return 12;
}

/* ---- INIT-ACK ---- */

static size_t buildInitAck(sctp_assoc_t* a, const uint8_t* initChunk, size_t initLen,
                           uint16_t peerPort, uint8_t* out, size_t outSize) {
    if (initLen < 16) return 0;

    /* INIT body: Initiate Tag(4) + A-RWND(4) + OS(2) + MIS(2) + Initial TSN(4) */
    uint32_t peerTag = r32(initChunk + 0);
    uint32_t peerTsn = r32(initChunk + 12);

    /* Generate our tag and TSN */
    uint32_t myTag = esp_random();
    if (myTag == 0) myTag = 1;
    uint32_t myTsn = esp_random();

    /* Build cookie */
    sctp_cookie cookie = {};
    cookie.peerTag = peerTag;
    cookie.myTag = myTag;
    cookie.peerTsn = peerTsn;
    cookie.myTsn = myTsn;
    cookie.peerPort = peerPort;
    cookie.myPort = a->myPort;
    cookie.timestamp = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
    cookieCompute(a, &cookie);

    /* Common header — verification tag = peer's initiate tag (RFC 9260 §8.5.1) */
    size_t pos = writeHeader(out, a->myPort, peerPort, peerTag);

    /* INIT-ACK chunk */
    size_t chunkStart = pos;
    out[pos++] = SCTP_INIT_ACK;
    out[pos++] = 0; /* flags */
    pos += 2; /* length placeholder */

    /* INIT-ACK body (same layout as INIT) */
    w32(out + pos, myTag); pos += 4;           /* Initiate Tag */
    w32(out + pos, 65535); pos += 4;           /* A-RWND */
    w16(out + pos, DC_MAX_CHANNELS); pos += 2; /* OS (outbound streams) */
    w16(out + pos, DC_MAX_CHANNELS); pos += 2; /* MIS (inbound streams) */
    w32(out + pos, myTsn); pos += 4;           /* Initial TSN */

    /* State Cookie parameter (type=7) */
    w16(out + pos, 7); pos += 2;              /* param type */
    w16(out + pos, 4 + sizeof(cookie)); pos += 2; /* param length */
    memcpy(out + pos, &cookie, sizeof(cookie));
    pos += pad4(sizeof(cookie));

    /* No optional parameters — absolute minimum INIT-ACK */

    /* Patch chunk length */
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
    w16(out + pos, 16); pos += 2; /* chunk length: 4 header + 12 body */
    w32(out + pos, a->peerTsn); pos += 4;  /* cumulative TSN ack */
    w32(out + pos, 65535); pos += 4;        /* a_rwnd */
    w16(out + pos, 0); pos += 2;            /* num gap ack blocks */
    w16(out + pos, 0); pos += 2;            /* num dup TSNs */
    sctpSetChecksum(out, pos);
    return pos;
}

/* ---- HEARTBEAT-ACK ---- */

static size_t buildHeartbeatAck(sctp_assoc_t* a, const uint8_t* hbChunk, size_t hbLen,
                                uint8_t* out) {
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    /* Echo the heartbeat chunk as heartbeat-ack (same body, different type) */
    out[pos] = SCTP_HEARTBEAT_ACK;
    memcpy(out + pos + 1, hbChunk + 1, hbLen - 1);
    pos += pad4(hbLen);
    sctpSetChecksum(out, pos);
    return pos;
}

/* ---- DCEP handling ---- */

static size_t handleDcepOpen(sctp_assoc_t* a, uint16_t streamId,
                             const uint8_t* data, size_t dataLen,
                             uint8_t* out) {
    if (dataLen < 4) return 0;

    /* Parse DCEP OPEN: type(1) channelType(1) priority(2) reliability(4) labelLen(2) protoLen(2) */
    uint8_t channelType = data[1];
    uint32_t reliability = 0;
    if (dataLen >= 8)
        reliability = r32(data + 4);
    uint16_t labelLen = 0;
    if (dataLen >= 12)
        labelLen = r16(data + 8);

    /* Check for duplicate stream ID (browser may send on both even and odd) */
    for (int i = 0; i < a->numChannels; i++) {
        if (a->channels[i].streamId == streamId) return 0; /* already open */
    }

    /* Register channel */
    if (a->numChannels < DC_MAX_CHANNELS) {
        dc_channel_t* ch = &a->channels[a->numChannels];
        ch->open = true;
        ch->streamId = streamId;
        ch->channelType = channelType;
        ch->reliability = reliability;
        if (labelLen > 0 && labelLen < sizeof(ch->label) && 12 + labelLen <= dataLen)
            memcpy(ch->label, data + 12, labelLen);
        ch->label[labelLen < sizeof(ch->label) ? labelLen : sizeof(ch->label) - 1] = '\0';
        a->numChannels++;
        dbg("DC channel %d: \"%s\" type=%d stream=%u\n",
             a->numChannels - 1, ch->label, channelType, streamId);
    }

    /* Build DCEP ACK as SCTP DATA chunk */
    size_t pos = writeHeader(out, a->myPort, a->peerPort, a->peerTag);
    size_t chunkStart = pos;
    out[pos++] = SCTP_DATA;
    out[pos++] = SCTP_DATA_E | SCTP_DATA_B; /* complete message, ordered */
    pos += 2; /* length placeholder */
    w32(out + pos, a->myTsn++); pos += 4; /* TSN */
    w16(out + pos, streamId); pos += 2;   /* Stream ID */
    w16(out + pos, 0); pos += 2;          /* SSN */
    w32(out + pos, PPID_DCEP); pos += 4;  /* PPID */
    out[pos++] = DCEP_ACK;                /* payload: single byte */
    /* pad to 4 bytes */
    while ((pos - chunkStart) % 4) out[pos++] = 0;
    w16(out + chunkStart + 2, (uint16_t)(pos - chunkStart));

    sctpSetChecksum(out, pos);
    return pos;
}

/* ---- Process incoming DATA chunk ---- */

static size_t processDataChunk(sctp_assoc_t* a, const uint8_t* chunk, size_t chunkLen,
                               uint8_t* out, size_t outSize) {
    if (chunkLen < 16) return 0;

    uint32_t tsn = r32(chunk + 4);
    uint16_t streamId = r16(chunk + 8);
    /* uint16_t ssn = r16(chunk + 10); */
    uint32_t ppid = r32(chunk + 12);
    const uint8_t* data = chunk + 16;
    size_t dataLen = chunkLen - 16;
    /* Remove padding bytes from data */
    uint16_t realChunkLen = r16(chunk + 2);
    if (realChunkLen > 16 && realChunkLen - 16 < dataLen)
        dataLen = realChunkLen - 16;

    /* Update peer TSN tracking */
    a->peerTsn = tsn;

    size_t outLen = 0;

    if (ppid == PPID_DCEP && dataLen > 0 && data[0] == DCEP_OPEN) {
        outLen = handleDcepOpen(a, streamId, data, dataLen, out);
    }
    /* Other PPIDs from browser (string/binary commands) could be handled here */

    return outLen;
}

/* ---- Public API ---- */

void sctpInit(sctp_assoc_t* a, uint8_t* outBuf, size_t outBufSize, uint16_t sctpPort) {
    memset(a, 0, sizeof(*a));
    a->outBuf = outBuf;
    a->outBufSize = outBufSize;
    a->myPort = sctpPort;
    esp_fill_random(a->cookieSecret, sizeof(a->cookieSecret));
}

int sctpInput(sctp_assoc_t* a, const uint8_t* pkt, size_t pktLen, size_t* outLen) {
    *outLen = 0;
    if (pktLen < 12) return -1;

    /* Skip checksum verification — DTLS provides integrity (RFC 8261).
     * Browser may send CRC32C or zero; either way, DTLS already verified. */

    uint16_t srcPort = r16(pkt);
    uint16_t dstPort = r16(pkt + 2);
    uint32_t vTag = r32(pkt + 4);
    dbg("SCTP hdr: src=%d dst=%d vTag=0x%08x (our port=%d)\n",
        srcPort, dstPort, (unsigned)vTag, a->myPort);

    /* Process chunks */
    size_t offset = 12;
    while (offset + 4 <= pktLen) {
        uint8_t type = pkt[offset];
        uint16_t chunkLen = r16(pkt + offset + 2);
        if (chunkLen < 4 || offset + chunkLen > pktLen) break;

        const uint8_t* chunk = pkt + offset;

        dbg("SCTP chunk type=%d len=%d\n", type, chunkLen);

        switch (type) {
            case SCTP_INIT:
                /* INIT must have vTag=0 */
                if (vTag != 0) { dbg("SCTP INIT bad vTag=%u\n", (unsigned)vTag); break; }
                *outLen = buildInitAck(a, chunk + 4, chunkLen - 4, srcPort,
                                       a->outBuf, a->outBufSize);
                break;

            case SCTP_COOKIE_ECHO: {
                /* Validate cookie */
                if (chunkLen < 4 + sizeof(sctp_cookie)) break;
                const sctp_cookie* cookie = (const sctp_cookie*)(chunk + 4);
                if (!cookieVerify(a, cookie)) {
                    dbg("SCTP cookie verify failed\n");
                    break;
                }

                /* Establish association */
                a->established = true;
                a->peerTag = cookie->peerTag;
                a->myTag = cookie->myTag;
                a->myTsn = cookie->myTsn;
                a->peerTsn = cookie->peerTsn - 1; /* will be incremented by first DATA */
                a->peerPort = cookie->peerPort;
                a->peerRwnd = 65535;

                *outLen = buildCookieAck(a, a->outBuf);
                info("SCTP association established\n");
                break;
            }

            case SCTP_DATA:
                if (!a->established || vTag != a->myTag) break;
                *outLen = processDataChunk(a, chunk, chunkLen, a->outBuf, a->outBufSize);
                /* Send SACK if no DCEP response piggybacked */
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
                    /* Free acked entries from retransmit buffer */
                    for (int i = 0; i < SCTP_REXMIT_SLOTS; i++) {
                        auto& e = a->rexmit[i];
                        if (!e.data) continue;
                        /* TSN <= cumTsn means acked */
                        int32_t diff = (int32_t)(e.tsn - cumTsn);
                        if (diff <= 0) { free(e.data); e.data = nullptr; continue; }
                        /* Check if TSN falls in a gap ack block (= received) */
                        const uint8_t* gaps = chunk + 16;
                        for (uint16_t g = 0; g < numGaps && 16 + g * 4 + 3 < chunkLen; g++) {
                            uint16_t start = r16(gaps + g * 4);
                            uint16_t end   = r16(gaps + g * 4 + 2);
                            uint32_t gapStart = cumTsn + start;
                            uint32_t gapEnd   = cumTsn + end;
                            if (e.tsn >= gapStart && e.tsn <= gapEnd) {
                                free(e.data); e.data = nullptr; break;
                            }
                        }
                    }
                    /* Only trigger retransmit when there are actual gaps */
                    if (numGaps > 0) a->sackHasGaps = true;
                }
                break;

            case SCTP_HEARTBEAT:
                if (!a->established) break;
                *outLen = buildHeartbeatAck(a, chunk, chunkLen, a->outBuf);
                break;

            case SCTP_FORWARD_TSN:
                /* Browser may send this — just update our TSN tracking */
                if (chunkLen >= 8)
                    a->peerTsn = r32(chunk + 4);
                break;

            default:
                dbg("SCTP unknown chunk type %d\n", type);
                break;
        }

        offset += pad4(chunkLen);
    }

    return 0;
}

int sctpSend(sctp_assoc_t* a, uint16_t streamId, uint32_t ppid,
             const uint8_t* data, size_t dataLen,
             sctp_send_fn sendFn, void* ctx) {
    if (!a->established) return -1;

    /* Max payload per SCTP packet — room for DTLS(13) + UDP(8) + IP(20) + WG(60) */
    const size_t MAX_PAYLOAD = 1400;
    const size_t CHUNK_HDR = 16; /* DATA chunk header */
    const size_t maxData = MAX_PAYLOAD - 12 - CHUNK_HDR; /* minus common header + chunk header */

    /* Look up channel for reliability info */
    uint8_t maxRexmit = 0;
    for (int i = 0; i < a->numChannels; i++) {
        if (a->channels[i].streamId == streamId) {
            uint8_t ct = a->channels[i].channelType;
            if (ct == DC_UNRELIABLE_REXMIT || ct == DC_UNRELIABLE_REXMIT_UNO)
                maxRexmit = (uint8_t)(a->channels[i].reliability & 0xFF);
            break;
        }
    }

    int nPkts = 0;
    size_t sent = 0;

    while (sent < dataLen || (sent == 0 && dataLen == 0)) {
        size_t fragLen = dataLen - sent;
        if (fragLen > maxData) fragLen = maxData;
        bool first = (sent == 0);
        bool last = (sent + fragLen >= dataLen);

        uint8_t flags = SCTP_DATA_U; /* unordered */
        if (first) flags |= SCTP_DATA_B;
        if (last)  flags |= SCTP_DATA_E | SCTP_DATA_I;

        uint32_t tsn = a->myTsn++;

        /* Build packet on stack */
        uint8_t pkt[12 + CHUNK_HDR + MAX_PAYLOAD + 4]; /* +4 for padding */
        size_t pos = writeHeader(pkt, a->myPort, a->peerPort, a->peerTag);

        size_t chunkStart = pos;
        pkt[pos++] = SCTP_DATA;
        pkt[pos++] = flags;
        pos += 2; /* length placeholder */
        w32(pkt + pos, tsn); pos += 4;             /* TSN */
        w16(pkt + pos, streamId); pos += 2;        /* Stream ID */
        w16(pkt + pos, 0); pos += 2;               /* SSN (0 for unordered) */
        w32(pkt + pos, ppid); pos += 4;            /* PPID */
        memcpy(pkt + pos, data + sent, fragLen);
        pos += fragLen;

        /* Patch chunk length (actual, not padded) */
        w16(pkt + chunkStart + 2, (uint16_t)(pos - chunkStart));

        /* Pad to 4-byte boundary */
        while (pos % 4) pkt[pos++] = 0;

        sctpSetChecksum(pkt, pos);

        /* Save to retransmit buffer if channel has reliability */
        if (maxRexmit > 0) {
            int slot = a->rexmitHead;
            a->rexmitHead = (slot + 1) % SCTP_REXMIT_SLOTS;
            auto& e = a->rexmit[slot];
            if (e.data) free(e.data);  /* evict oldest */
            e.data = (uint8_t*)heap_caps_malloc(pos, MALLOC_CAP_SPIRAM);
            if (e.data) {
                memcpy(e.data, pkt, pos);
                e.len = (uint16_t)pos;
                e.tsn = tsn;
                e.streamId = streamId;
                e.maxRexmit = maxRexmit;
                e.rexmitCount = 0;
            }
        }

        int ret = sendFn(pkt, pos, ctx);
        if (ret == 0) {
            /* WANT_WRITE — retry once after yielding */
            vTaskDelay(1);
            ret = sendFn(pkt, pos, ctx);
        }
        if (ret <= 0) return nPkts > 0 ? nPkts : -1;

        sent += fragLen;
        nPkts++;

        if (dataLen == 0) break; /* empty message */
    }

    return nPkts;
}

int sctpFindChannel(sctp_assoc_t* a, const char* label) {
    for (int i = 0; i < a->numChannels; i++)
        if (strcmp(a->channels[i].label, label) == 0) return i;
    return -1;
}

int sctpRetransmit(sctp_assoc_t* a, sctp_send_fn sendFn, void* ctx) {
    if (!a->established || !a->sackHasGaps) return 0;
    a->sackHasGaps = false;
    int count = 0;
    for (int i = 0; i < SCTP_REXMIT_SLOTS; i++) {
        auto& e = a->rexmit[i];
        if (!e.data) continue;
        /* Skip if already acked */
        int32_t diff = (int32_t)(e.tsn - a->sackCumTsn);
        if (diff <= 0) { free(e.data); e.data = nullptr; continue; }
        /* Retransmit if below max */
        if (e.rexmitCount < e.maxRexmit) {
            e.rexmitCount++;
            sendFn(e.data, e.len, ctx);
            count++;
        } else {
            /* Exhausted retransmits */
            free(e.data); e.data = nullptr;
        }
    }
    return count;
}

void sctpRexmitFree(sctp_assoc_t* a) {
    for (int i = 0; i < SCTP_REXMIT_SLOTS; i++) {
        if (a->rexmit[i].data) { free(a->rexmit[i].data); a->rexmit[i].data = nullptr; }
    }
    a->rexmitHead = 0;
}
