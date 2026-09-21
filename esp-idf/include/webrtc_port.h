#pragma once
/**
 * webrtc_port — the three things the DataChannel needs from the platform.
 *
 * Everything else in webrtc_task/webrtc_sctp is the same code on a chip and on
 * the Linux host target: sockets, mbedtls, FreeRTOS and the SCTP state machine
 * all exist on both. Only these differ, and each has exactly one caller.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The local addresses to offer as ICE host candidates, newest first.
 *
 * On a chip that is the WiFi station and soft-AP interfaces, whichever are up.
 * On the host the station has one address of its own and that is the answer.
 * Writes at most `max` dotted-quad strings into `ips` and returns how many.
 */
int webrtcLocalIps(char ips[][16], int max);

/** The address the UDP socket binds.
 *
 * `INADDR_ANY` on a chip, which has one network stack to itself. On the host
 * every station is a process sharing one stack, so each binds its own loopback
 * address — otherwise the second station to start finds the port taken.
 */
uint32_t webrtcBindAddrV4(void);

/** CRC-32 (IEEE, reflected) over `len` bytes, for the STUN FINGERPRINT.
 *
 * The chip has one in ROM; the host computes it.
 */
uint32_t webrtcCrc32(const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif
