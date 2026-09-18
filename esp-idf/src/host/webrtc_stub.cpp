/**
 * webrtc_stub — the WebRTC half's boot hook on a host that does not build it.
 *
 * The DataChannel transport is an lwIP/UDP stack with its own SCTP on top of
 * it; on the host the browser reaches the station's web server directly and
 * there is nothing for it to do. The generated boot dispatch still calls the
 * hook, so it exists and does nothing.
 */
#include "webrtc_task.h"

void webrtcInit() {}
