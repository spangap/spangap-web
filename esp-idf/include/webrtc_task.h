/**
 * webrtc_task — WebRTC DataChannel for browser-native UDP video streaming.
 *
 * ICE-lite + DTLS 1.2 + minimal SCTP + DCEP.
 * Sends JPEG video and audio as binary DataChannel messages.
 */
#ifndef SPANGAP_WEBRTC_TASK_H
#define SPANGAP_WEBRTC_TASK_H

#include <stdint.h>

/** WebRTC's ITS server port for the signaling WebSocket (forwarded by web).
 *  Convention: matches the default DataChannel UDP port. */
static constexpr uint16_t WEBRTC_PORT = 4433;

/** Create WebRTC DataChannel task. Call after wgInit() and tlsInit(). */
void webrtcInit();

#endif
