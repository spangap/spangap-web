/**
 * webrtc_task — WebRTC DataChannel for browser-native UDP video streaming.
 *
 * ICE-lite + DTLS 1.2 + minimal SCTP + DCEP.
 * Sends JPEG video and audio as binary DataChannel messages.
 */
#ifndef SECCAM_WEBRTC_TASK_H
#define SECCAM_WEBRTC_TASK_H

/** Create WebRTC DataChannel task. Call after wgInit() and tlsInit(). */
void webrtcInit();

#endif
