/**
 * webrtc_sctp — Minimal SCTP for WebRTC DataChannel.
 *
 * Implements only what browsers need:
 *   - INIT/INIT-ACK/COOKIE-ECHO/COOKIE-ACK handshake (server role)
 *   - DATA chunks: send (fragmented, ordered or unordered) + receive
 *   - SACK: generate on receive, parse on send
 *   - DCEP: DATA_CHANNEL_OPEN/ACK, including protocol string extraction
 *   - RE-CONFIG (RFC 6525): stream reset in and out, so DC close works without
 *     tearing down the association
 *   - FORWARD-TSN, HEARTBEAT-ACK, CRC32C
 *
 * Reliability is per-channel: the channelType/reliability parameters from
 * DCEP drive whether DATA chunks carry SSN (ordered), go into the rexmit
 * buffer (reliable or partial), or fire-and-forget (unreliable+unordered).
 *
 * Rexmit buffer: whole-message pool in PSRAM (default 1 MB). On SACK gap,
 * we rebuild the exact fragment from the stored message body.
 *
 * Runs over DTLS — caller handles encrypt/decrypt.
 */
#ifndef SPANGAP_WEBRTC_SCTP_H
#define SPANGAP_WEBRTC_SCTP_H

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
#define SCTP_ABORT          6
#define SCTP_COOKIE_ECHO    10
#define SCTP_COOKIE_ACK     11
#define SCTP_FORWARD_TSN    0xC0
#define SCTP_RECONFIG       0x82   /* RFC 6525 */

/* RE-CONFIG parameter types (RFC 6525 §4) */
#define RECONFIG_OUT_SSN_RESET    13
#define RECONFIG_IN_SSN_RESET     14
#define RECONFIG_SSN_TSN_RESET    15
#define RECONFIG_RESPONSE         16

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

/* DataChannel reliability types (DCEP channelType) */
#define DC_RELIABLE              0x00  /* ordered, no retransmit limit */
#define DC_RELIABLE_UNORDERED    0x80  /* unordered, no retransmit limit */
#define DC_UNRELIABLE_REXMIT     0x01  /* ordered, maxRetransmits */
#define DC_UNRELIABLE_REXMIT_UNO 0x81  /* unordered, maxRetransmits */
#define DC_UNRELIABLE_TIMED      0x02  /* ordered, maxPacketLifeTime */
#define DC_UNRELIABLE_TIMED_UNO  0x82  /* unordered, maxPacketLifeTime */

/* Max concurrently-active DataChannels we track state for. Leaves room for
   AV + V + A (streaming triple), plus storage / log / cli / play. */
#define DC_MAX_CHANNELS     16

/* Streams announced in SCTP INIT-ACK (OS / MIS). Must cover the total
   number of DCs opened over the lifetime of the association, not just
   concurrent, because SCTP stream IDs are not reused after RE-CONFIG
   reset. Each seek/live-play toggle consumes a fresh ID on the browser
   side. 65535 is the protocol max and leaves plenty of headroom. */
#define SCTP_ANNOUNCED_STREAMS 65535

/* Rexmit pool. One slot per in-flight message (not fragment), so slot
   count has to comfortably exceed the number of messages that can be in
   flight between SACKs at typical mixed-video+audio bitrates. */
#define SCTP_REXMIT_SLOTS        256
#define SCTP_REXMIT_POOL_BYTES   (1 * 1024 * 1024)

/* ---- Channel info ---- */
typedef struct {
    bool     open;
    uint16_t streamId;
    uint8_t  channelType;  /* DC_RELIABLE, DC_UNRELIABLE_REXMIT_UNO, etc. */
    uint32_t reliability;  /* maxRetransmits or maxPacketLifeTime (from DCEP) */
    uint16_t priority;     /* from DCEP OPEN (RFC 8832): higher = served first */
    uint16_t ssn;          /* outbound stream sequence number (ordered channels) */
    char     label[32];
    char     protocol[256];
    uint16_t protoLen;
    /* Inbound fragment reassembly (PSRAM, grown on demand, freed on E=1).
       A DC message from the peer may span multiple DATA chunks with same
       SSN and B=1…E=1 flags; we accumulate into rxBuf then fire onData. */
    uint8_t* rxBuf;
    size_t   rxLen;
    size_t   rxCap;
    uint32_t rxPpid;
} dc_channel_t;

/* ---- Rexmit entry: whole DCEP message ---- */
typedef struct {
    uint32_t firstTsn;     /* TSN of first fragment */
    uint32_t lastTsn;      /* TSN of last fragment */
    uint16_t streamId;
    uint16_t ssn;          /* SSN used for ordered messages */
    uint32_t ppid;
    bool     ordered;      /* true = SSN matters, false = unordered flag set */
    bool     reliableInfinite; /* retry forever until cumulative-ack covers */
    uint8_t  maxRexmit;    /* only used when !reliableInfinite */
    uint8_t  rexmitCount;
    uint8_t* data;         /* full message body (PPID payload) */
    size_t   dataLen;
} rexmit_msg_t;

/* ---- Inbound reorder buffer ----
   SCTP DATA chunks ride UDP, so they can arrive out of order or be dropped.
   A correct ordered-reliable receiver must deliver to the upper layer — and
   advance the SACK cumulative-ack point — in strict TSN order, holding any
   chunk that arrives past a gap until the missing TSNs fill in. Each slot
   holds one DATA chunk's payload in PSRAM. The pool is bounded: when full we
   drop (and leave un-acked) the chunk so the reliable peer retransmits it.
   64 slots covers realistic reordering / loss-recovery windows for the
   inbound (browser→device) control + data streams. */
#define SCTP_REORDER_SLOTS 64
typedef struct {
    bool     used;
    uint32_t tsn;
    uint16_t streamId;
    uint32_t ppid;
    uint8_t  flags;        /* B/E/U fragment flags, needed for reassembly */
    uint8_t* data;         /* PSRAM, dataLen bytes (nullptr if dataLen==0) */
    size_t   dataLen;
} sctp_reorder_t;

/* Forward decl for callback signatures */
struct sctp_assoc_s;
typedef void (*sctp_dceopen_cb_t)(struct sctp_assoc_s* a, int chIdx);
typedef void (*sctp_dcreset_cb_t)(struct sctp_assoc_s* a, uint16_t streamId);
/** Fired per fully-reassembled inbound DATA message (one user-level
 *  message, after fragment reassembly). PPID tells the caller whether
 *  the payload is a string (51) or binary (53). */
typedef void (*sctp_data_cb_t)(struct sctp_assoc_s* a, uint16_t streamId,
                                uint32_t ppid, const uint8_t* data, size_t dataLen);

/* ---- SCTP association state ---- */
typedef struct sctp_assoc_s {
    /* Association */
    bool     established;
    uint32_t myTag;         /* our verification tag */
    uint32_t peerTag;       /* peer's verification tag */
    uint32_t myTsn;         /* next TSN to send */
    uint32_t peerTsn;       /* inbound cumulative TSN ack point: highest TSN
                               below which everything has been delivered in
                               order. Out-of-order chunks past it wait in
                               reorder[] until the gap closes. */
    uint16_t myPort;        /* our SCTP port */
    uint16_t peerPort;      /* peer's SCTP port */
    uint32_t peerRwnd;      /* peer's advertised receiver window */

    /* Cookie secret (for stateless INIT handling) */
    uint8_t  cookieSecret[32];

    /* Channels */
    dc_channel_t channels[DC_MAX_CHANNELS];
    int          numChannels;

    /* Inbound reorder buffer: DATA chunks received past a gap in the
       cumulative TSN, held until the gap fills so delivery + SACKing stay
       in strict TSN order. */
    sctp_reorder_t reorder[SCTP_REORDER_SLOTS];
    int            reorderCount;

    /* Callbacks (optional — nullptr is fine) */
    sctp_dceopen_cb_t onDceopen;
    sctp_dcreset_cb_t onDcreset;
    sctp_data_cb_t    onData;

    /* Retransmit buffer (whole-message entries in PSRAM) */
    rexmit_msg_t rexmit[SCTP_REXMIT_SLOTS];
    int          rexmitHead;      /* next slot to write */
    size_t       rexmitBytes;     /* total data bytes currently stored */
    uint32_t     sackCumTsn;      /* last cumulative TSN acked by peer */
    bool         sackHasGaps;     /* set when SACK reports gaps — triggers retransmit */
    uint32_t     lastSackMs;      /* millis() at last SACK — RTO retransmit timer */
    /* Gap Ack Blocks from the most recent SACK: each pair is a
       received-range expressed as 16-bit offsets from sackCumTsn.
       Retransmit uses these to resend only the TSNs the peer is
       actually missing, not every TSN > cumTsn. RFC-capped at 128 —
       any real SCTP SACK fits in well under that. */
    uint16_t     sackGapStart[128];
    uint16_t     sackGapEnd[128];
    uint16_t     sackNumGaps;

    /* Staged FORWARD-TSN target after a stream reset purge (tells the peer
       to advance its cumTsn past TSNs we've abandoned). Main loop sends
       the chunk and zeroes this. 0 = nothing pending. */
    uint32_t     pendingFwdTsn;

    /* FORWARD-TSN: advance peer's cumulative TSN past abandoned data */
    uint32_t fwdTsnSent;

    /* RE-CONFIG request sequence numbers */
    uint32_t myReconfigReqSn;     /* our next outgoing reconfig request SN */
    uint32_t peerReconfigReqSn;   /* last peer reconfig request SN we responded to */

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
 *  Returns 1 if peer sent ABORT (caller should tear down). */
int sctpInput(sctp_assoc_t* a, const uint8_t* pkt, size_t pktLen,
              size_t* outLen);

/** Build a DATA chunk message for sending on a DataChannel.
 *  Fragments large messages into multiple SCTP packets; honors the
 *  channel's ordered / unordered flag; inserts into the rexmit buffer for
 *  reliable or partial-reliable channels.
 *  Returns number of packets sent, or -1 on error. */
typedef int (*sctp_send_fn)(const uint8_t* pkt, size_t len, void* ctx);
int sctpSend(sctp_assoc_t* a, uint16_t streamId, uint32_t ppid,
             const uint8_t* data, size_t dataLen,
             sctp_send_fn sendFn, void* ctx);

/** Send an outgoing RE-CONFIG (Outgoing SSN Reset) to close a single
 *  DataChannel stream without affecting other streams. */
int sctpStreamReset(sctp_assoc_t* a, uint16_t streamId,
                    sctp_send_fn sendFn, void* ctx);

/** Find channel by label. Returns channel index or -1. */
int sctpFindChannel(sctp_assoc_t* a, const char* label);

/** Find channel by streamId. Returns channel index or -1. */
int sctpFindChannelByStream(sctp_assoc_t* a, uint16_t streamId);

/** Retransmit missing fragments. Call after sctpInput() (SACK-triggered
 *  gap fills) and from an RTO timer (when SACKs have been silent too
 *  long). `forceAll=true` ignores gap-block info and resends every
 *  fragment past cumTsn — needed when a burst of packets was lost
 *  silently and the peer never reported them. */
int sctpRetransmit(sctp_assoc_t* a, sctp_send_fn sendFn, void* ctx, bool forceAll);

/** Free retransmit buffer entries. Call on session teardown. */
void sctpRexmitFree(sctp_assoc_t* a);

/** Build a FORWARD-TSN chunk to advance peer past abandoned TSNs.
 *  newCumTsn: advance peer's cumulative TSN to this value.
 *  Returns packet in a->outBuf, length in *outLen. */
void sctpBuildForwardTsn(sctp_assoc_t* a, uint32_t newCumTsn, size_t* outLen);

/** CRC32C (used by SCTP). */
uint32_t crc32c(const uint8_t* data, size_t len);

#endif
