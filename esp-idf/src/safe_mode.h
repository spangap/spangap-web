/**
 * safe_mode — the recovery boot's HTTP face, as web.cpp needs to see it.
 *
 * Private to this component: it lives in src/, not include/, because nothing
 * outside spangap-web serves the page and nothing outside a safe-mode boot may.
 * See spangap-core/docs/safe-mode.md for what the mode is.
 */
#ifndef SPANGAP_WEB_SAFE_MODE_H
#define SPANGAP_WEB_SAFE_MODE_H

#include <cstddef>

/** The one URL prefix safe mode claims, registered with web by the safe-mode
 *  server task. Both halves need it: safe_mode.cpp to register it, web.cpp to
 *  404 anything under it that the endpoint did not claim. Serving the PAGE
 *  under this prefix would loop — the backup page navigates to the download on
 *  load, so a page served at the download URL re-navigates to itself for ever. */
#define SAFE_MODE_ENDPOINT "backup"

/** The page for this boot's operation, freshly built into a heap_caps buffer
 *  the caller owns (web's response machinery frees it). `authed` false yields
 *  the sign-in form instead. Returns nullptr if it could not allocate. */
char* safeModePage(bool authed, size_t* outLen);

/** Take the deep-sleep lock, arm the window's deadline and — for a backup or a
 *  restore — bring up the endpoint. No-op outside a safe-mode boot.
 *
 *  MUST be called from web's own task body, after its `itsOnAux` handlers are
 *  installed: registering a URL prefix is an aux message, and an aux to a port
 *  with no handler yet is dropped while the sender is told it was delivered. */
void safeModeInit();

#endif /* SPANGAP_WEB_SAFE_MODE_H */
