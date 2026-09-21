/**
 * webrtc_port (host) — the platform's answers for the DataChannel.
 *
 * The chip's are in `../webrtc_port.cpp`; see `include/webrtc_port.h` for what
 * each one is for. Everything else about the DataChannel — ICE, DTLS, SCTP and
 * the router above them — is the same source on both targets, which is the
 * point: a browser talking to a simulated station talks to it the same way it
 * talks to a board.
 */
#include "webrtc_port.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <cstdio>

/* The station's own address, from the board straddle. Weak with a sane
 * default, because a platform straddle may not depend on a board one — see
 * `hw-linux`. */
extern "C" __attribute__((weak)) const char* hwLinuxBindAddr(void) { return "127.0.0.1"; }

extern "C" int webrtcLocalIps(char ips[][16], int max)
{
    if (max < 1) return 0;
    snprintf(ips[0], 16, "%s", hwLinuxBindAddr());
    return 1;
}

extern "C" uint32_t webrtcBindAddrV4(void)
{
    /* Every station on this target is a process on one shared network stack,
     * so binding the wildcard would hand the port to whichever started first.
     * Each binds the address that is its own. */
    return inet_addr(hwLinuxBindAddr());
}

extern "C" uint32_t webrtcCrc32(const uint8_t* data, size_t len)
{
    /* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — the chip's ROM
     * routine. Computed a bit at a time: the only caller is the STUN
     * FINGERPRINT over a message of tens of bytes, so a table would cost more
     * than it saves. */
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}
