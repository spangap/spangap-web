/**
 * webrtc_sctp — Minimal SCTP for WebRTC DataChannel.
 *
 * Implements only what browsers need:
 *   - INIT/INIT-ACK/COOKIE-ECHO/COOKIE-ACK handshake (server role)
 *   - DATA chunks: send (fragmented) + receive (reassemble small DCEP messages)
 *   - SACK: generate on receive, parse on send
 *   - DCEP: DATA_CHANNEL_OPEN/ACK
 *   - CRC32C checksum
 *
 * Runs over DTLS — caller handles encrypt/decrypt.
 */
#ifndef SECCAM_WEBRTC_SCTP_H
#define SECCAM_WEBRTC_SCTP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- SCTP chunk types ---- */
#define SCTP_DATA           0
#define SCTP_INIT           1
#define SCTP_INIT_ACK       2
#define SCTP_SACK           3
#define SCTP_HEARTBEAT      4
#define SCTP_HEARTBEAT_ACK  5
#define SCTP_COOKIE_ECHO    10
#define SCTP_COOKIE_ACK     11
#define SCTP_FORWARD_TSN    0xC0

/* DATA chunk flags */
#define SCTP_DATA_E  0x01  /* end fragment */
#define SCTP_DATA_B  0x02  /* begin fragment */
#define SCTP_DATA_U  0x04  /* unordered */
#define SCTP_DATA_I  0x08  /* immediate (request SACK) */

/* DCEP PPIDs */
#define PPID_DCEP           50
#define PPID_STRING         51
#define PPID_BINARY         53
#define PPID_STRING_EMPTY   56
#define PPID_BINARY_EMPTY   57

/* DCEP message types */
#define DCEP_OPEN           0x03
#define DCEP_ACK            0x02

/* DataChannel reliability types */
#define DC_RELIABLE              0x00
#define DC_RELIABLE_UNORDERED    0x01
#define DC_UNRELIABLE_REXMIT     0x02
#define DC_UNRELIABLE_REXMIT_UNO 0x03
#define DC_UNRELIABLE_TIMED      0x04
#define DC_UNRELIABLE_TIMED_UNO  0x05

/* Max DataChannels */
#define DC_MAX_CHANNELS     4

/* ---- Channel info ---- */
typedef struct {
    bool     open;
    uint16_t streamId;
    uint8_t  channelType;  /* DC_RELIABLE, DC_UNRELIABLE_REXMIT_UNO, etc. */
    char     label[32];
} dc_channel_t;

/* ---- SCTP association state ---- */
typedef struct {
    /* Association */
    bool     established;
    uint32_t myTag;         /* our verification tag */
    uint32_t peerTag;       /* peer's verification tag */
    uint32_t myTsn;         /* next TSN to send */
    uint32_t peerTsn;       /* highest cumulative TSN received */
    uint16_t myPort;        /* our SCTP port */
    uint16_t peerPort;      /* peer's SCTP port */
    uint32_t peerRwnd;      /* peer's advertised receiver window */

    /* Cookie secret (for stateless INIT handling) */
    uint8_t  cookieSecret[32];

    /* Channels */
    dc_channel_t channels[DC_MAX_CHANNELS];
    int          numChannels;

    /* Send state */
    uint16_t ssn[DC_MAX_CHANNELS]; /* per-stream sequence number (for ordered) */

    /* Output buffer — caller provides */
    uint8_t* outBuf;
    size_t   outBufSize;
} sctp_assoc_t;

/** Initialize a fresh SCTP association state.
 *  outBuf: caller-owned buffer for building outgoing SCTP packets.
 *  sctpPort: local SCTP port (e.g. 5000, from SDP). */
void sctpInit(sctp_assoc_t* a, uint8_t* outBuf, size_t outBufSize, uint16_t sctpPort);

/** Process one incoming SCTP packet (already decrypted from DTLS).
 *  Returns outgoing response packet(s) in a->outBuf, length in *outLen.
 *  Returns 0 if nothing to send. Caller sends response via DTLS. */
int sctpInput(sctp_assoc_t* a, const uint8_t* pkt, size_t pktLen,
              size_t* outLen);

/** Build a DATA chunk message for sending on a DataChannel.
 *  Fragments large messages into multiple SCTP packets.
 *  Calls sendFn for each packet (already checksummed).
 *  ppid: PPID_BINARY (53) for camera data, etc.
 *  Returns number of packets sent, or -1 on error. */
typedef int (*sctp_send_fn)(const uint8_t* pkt, size_t len, void* ctx);
int sctpSend(sctp_assoc_t* a, uint16_t streamId, uint32_t ppid,
             const uint8_t* data, size_t dataLen,
             sctp_send_fn sendFn, void* ctx);

/** Find channel by label. Returns channel index or -1. */
int sctpFindChannel(sctp_assoc_t* a, const char* label);

/** CRC32C (used by SCTP). */
uint32_t crc32c(const uint8_t* data, size_t len);

#endif
