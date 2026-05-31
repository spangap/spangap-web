/**
 * auth_web — HTTP wrapper around spangap-core's auth (login/passwd/logout).
 *
 * The realm/password/cookie store + CLI live in spangap-core's auth.cpp.
 * This file owns only the JSON-over-HTTP face: it parses request bodies,
 * dispatches by path, and writes JSON responses using web.cpp helpers.
 *
 * Registered via authWebInit(), called once from webInit() — sits inline
 * on the web task, no own task.
 */
#include "auth.h"
#include "log.h"
#include "web.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <string>

static constexpr int AUTH_BODY_MAX = 1024;

static void authReplyJson(int h, const char* json) {
    webSendResponse(h, 200, "application/json", json, strlen(json));
}

static void authReplyResultCode(int h, int result) {
    char json[40];
    snprintf(json, sizeof(json), "{\"result\":%d}", result);
    authReplyJson(h, json);
}

static void authHandleLogin(int h, const char* hdr, int hlen) {
    char* body = webReadBody(h, hdr, hlen, AUTH_BODY_MAX, nullptr);
    if (!body) { webSendStatus(h, 400); return; }

    cJSON* req = cJSON_Parse(body);
    heap_caps_free(body);
    if (!req) { webSendStatus(h, 400); return; }

    const char* password = cJSON_GetStringValue(cJSON_GetObjectItem(req, "password"));
    const char* tryRealm = "";
    cJSON* realmItem = cJSON_GetObjectItem(req, "realm");
    if (cJSON_IsString(realmItem)) tryRealm = realmItem->valuestring;

    if (!password) {
        authReplyResultCode(h, AUTH_WRONG_PASSWORD);
    } else {
        std::string outRealm, outCookie;
        auth_err_t e = authLogin(password, tryRealm, outRealm, outCookie);
        if (e == AUTH_OK) {
            char json[256];
            snprintf(json, sizeof(json),
                "{\"result\":0,\"realm\":\"%s\",\"cookie\":\"%s\"}",
                outRealm.c_str(), outCookie.c_str());
            authReplyJson(h, json);
        } else {
            authReplyResultCode(h, (int)e);
        }
    }
    cJSON_Delete(req);
}

static void authHandlePasswd(int h, const char* hdr, int hlen) {
    char* body = webReadBody(h, hdr, hlen, AUTH_BODY_MAX, nullptr);
    if (!body) { webSendStatus(h, 400); return; }

    cJSON* req = cJSON_Parse(body);
    heap_caps_free(body);
    if (!req) { webSendStatus(h, 400); return; }

    const char* realm = cJSON_GetStringValue(cJSON_GetObjectItem(req, "realm"));
    const char* oldPw = cJSON_GetStringValue(cJSON_GetObjectItem(req, "old"));
    const char* newPw = cJSON_GetStringValue(cJSON_GetObjectItem(req, "new"));

    if (!realm || !oldPw || !newPw) {
        authReplyResultCode(h, AUTH_WRONG_PASSWORD);
    } else {
        auth_err_t e = authPasswd(realm, oldPw, newPw);
        authReplyResultCode(h, (int)e);
    }
    cJSON_Delete(req);
}

static void authHandleLogout(int h, const char* hdr, int hlen) {
    char cookie[64] = {};
    webExtractCookie(hdr, hlen, "session", cookie, sizeof(cookie));
    authLogout(cookie);
    /* Cross-connection eviction (force-close every other web conn from the
     * same client IP) was in the original implementation. Skipped here —
     * the cookie is already deleted, so any later request authenticating
     * with it will fail authCheck and be rejected. */
    authReplyJson(h, "{\"result\":0}");
}

static void authUrlHandler(int h, const char* hdr, int hlen) {
    char method[8];
    char path[64];
    webGetMethod(hdr, hlen, method, sizeof(method));
    webGetPath(hdr, hlen, path, sizeof(path));

    if (strcmp(path, "auth/logout") == 0) {
        authHandleLogout(h, hdr, hlen);
    } else if (strcmp(method, "POST") != 0) {
        webSendStatus(h, 405);
    } else if (strcmp(path, "auth/login") == 0) {
        authHandleLogin(h, hdr, hlen);
    } else if (strcmp(path, "auth/passwd") == 0) {
        authHandlePasswd(h, hdr, hlen);
    } else {
        webSendStatus(h, 404);
    }
}

/** Wire the auth URL prefix handler. Called from webInit. */
extern "C" void authWebInit() {
    webRegisterHandler("auth", authUrlHandler);
}
