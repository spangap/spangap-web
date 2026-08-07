/**
 * safe_mode — the recovery boot's entire HTTP face.
 *
 *   GET  /backup/state.tgz     → 200 chunked <the whole state store>  → reboot
 *   POST /backup/<name>.tgz    → 200 {"ok":true,…}                    → reboot
 *   PUT  /backup/<name>.tgz    → same, for `curl -T`
 *   GET  /backup/cancel        → 200 {"ok":true} (restore mode only) → reboot
 *   anything else              → the operation's own page
 *
 * A safe-mode boot exists to do exactly one thing to the state store with
 * nothing else touching it (spangap.h, spangapSafeMode()). That is what lets
 * this file be short: there is no flush hold, no write seal, no originating-task
 * exemption — the hostile concurrent world simply isn't booted.
 *
 * NOTHING IS EVER RESIDENT. Neither direction may hold the archive: there is no
 * room for it in RAM and none on flash. Both stream (see targz.h), and the
 * consequence is that a restore has a point of no return in the middle of it —
 * the store is formatted before the first byte is written, which is also what
 * buys the space to expand into and makes every failure land in the same clean
 * place (fs.h, fsSetRestoreMarker()).
 *
 * The endpoint for the operation this boot was NOT asked to perform does not
 * exist — it is never registered, and the handler 404s the wrong verb. So the
 * window in which a device will hand out an archive containing every secret it
 * holds exists only when an operator explicitly asked for a backup.
 *
 * EXIT IS ALWAYS A REBOOT, decided here: last chunk drained, or extraction
 * verified, or a restore cancelled before it started, or the deadline elapsed. The client is never asked and never
 * acknowledges; it sees the connection go away, which is what it would see
 * anyway. The restart rides a one-shot timer rather than firing inline, because
 * the response still has to reach the wire.
 */
#include "safe_mode.h"

#include "spangap.h"
#include "targz.h"
#include "web.h"
#include "auth.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace {

static_assert(sizeof(SAFE_MODE_ENDPOINT) <= sizeof(web_handler_msg_t::path),
              "endpoint prefix must fit a web handler registration");

/* One fixed deadline for the whole window. A state store is a few hundred KB
 * over WiFi; a transfer that has not finished inside this has died, and
 * progress-extension plumbing would serve only already-dead transfers. */
constexpr int SAFE_DEADLINE_S = 600;

/* File body I/O bite. Large enough that the per-op fs proxy round-trip is
 * noise, small enough that one flash write window doesn't stall the network
 * task feeding us. */
constexpr size_t IO_CHUNK = 16384;

/* Each flash program disables the PSRAM cache; writing a whole 32 KB inflate
 * run in one go holds every PSRAM-stack task off for the duration. Break it up
 * and yield, the same discipline storage's flush uses — not optional here,
 * because we are receiving over TCP while writing. */
constexpr size_t WRITE_CHUNK = 8192;

constexpr int WALK_MAX_DEPTH = 8;
/* Entries listed per directory level, in one fs_listdir round-trip. The array
 * is PSRAM and lives for as long as that level's recursion, so the cap is what
 * bounds peak usage (~21 KB a level). A store that overflows it says so — a
 * backup that silently dropped half a directory is worse than no backup. */
constexpr int WALK_MAX_ENTRIES = 256;

pm_lock_handle_t   s_pmLock      = nullptr;
esp_timer_handle_t s_deadline    = nullptr;
esp_timer_handle_t s_rebootTimer = nullptr;

void rebootCb(void*) { esp_restart(); }

/** Reboot in `ms`, off this task, so a response drains first. */
void rebootSoon(int ms) {
    if (!s_rebootTimer) {
        esp_timer_create_args_t a = {};
        a.callback = rebootCb;
        a.name = "safemode_reboot";
        if (esp_timer_create(&a, &s_rebootTimer) != ESP_OK) { esp_restart(); }
    }
    esp_timer_start_once(s_rebootTimer, (int64_t)ms * 1000);
}

/* ---- small HTTP helpers ---- */

/** Write the whole buffer with backpressure. False if the peer went away. */
bool sendAll(int h, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    int stalls = 0;
    while (len) {
        size_t n = itsSend(h, p, len, pdMS_TO_TICKS(200));
        if (n == 0) {
            if (!itsConnected(h)) return false;
            if (++stalls > 150) return false;   /* ~30 s with no drain */
            continue;
        }
        stalls = 0;
        p += n;
        len -= n;
    }
    return true;
}

void bail(int h, int status, const char* msg) {
    char body[160];
    int n = snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}",
                     msg ? msg : "");
    webSendResponse(h, status, "application/json", body, (size_t)n);
    itsSendDrain(h, 2000);
    itsDisconnect(h);
}

/** admin realm, or open when no password is set — see the page's own gate. */
bool authorized(const char* hdr, int hlen) {
    if (!authEnabled()) return true;
    char cookie[64];
    if (!webExtractCookie(hdr, hlen, "session", cookie, sizeof(cookie)))
        return false;
    return authCheck(cookie) == "admin";
}

/* ---- the filename is the manifest ---- */
/*
 *   <fw stub>_<host>_<yyyymmddhhmmss>_<used>kB.tgz
 *
 * Only the first field is ever machine-read, and only on the way back in. The
 * rest exist for the human reading a directory listing.
 */

/** The space this archive needs allocated at the far end, in kB.
 *
 *  Both media answer the same question — how much room the CONTENT takes once
 *  written to a block-allocated store — rather than each answering whichever
 *  question was cheap to ask. LittleFS reports allocated blocks directly; on SD
 *  the store is a plain directory, so we sum the files and round each one up the
 *  same way a block allocator would. The alternative, a raw byte sum on SD
 *  against LittleFS's allocated figure on flash, made the same set of files
 *  report ~400 kB from one medium and ~640 kB from the other — two different
 *  numbers for one archive, in a field a human reads to judge whether it fits.
 *
 *  Informational either way: nothing parses it, and there is no size precheck. */
constexpr uint32_t STORE_BLOCK = 4096;

uint32_t stateUsedKb() {
    if (!fsStateOnSd()) {
        size_t total = 0, used = 0;
        if (fsLittlefsInfo("state", &total, &used) == ESP_OK)
            return (uint32_t)((used + 1023) / 1024);
        return 0;
    }
    struct Sum {
        static uint64_t walk(const std::string& dir, int depth) {
            if (depth > WALK_MAX_DEPTH) return 0;
            auto* list = (fs_listing_t*)heap_caps_malloc(
                WALK_MAX_ENTRIES * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
            if (!list) return 0;
            int n = fs_listdir(dir.c_str(), list, WALK_MAX_ENTRIES);
            uint64_t sum = 0;
            for (int i = 0; i < n; i++) {
                if (list[i].name[0] == '.') continue;
                if (list[i].isDir) { sum += walk(dir + "/" + list[i].name, depth + 1); continue; }
                /* Round up per file, as a block allocator would — a store is
                 * mostly small files, so this is most of the difference. */
                sum += ((uint64_t)list[i].size + STORE_BLOCK - 1)
                       / STORE_BLOCK * STORE_BLOCK;
            }
            heap_caps_free(list);
            return sum;
        }
    };
    return (uint32_t)((Sum::walk(fsStateDir(), 0) + 1023) / 1024);
}

std::string backupFilename() {
    char host[48];
    storageGetStr("s.net.hostname", host, sizeof(host), CONFIG_SPANGAP_FW_STUB);
    /* Sanitise: the value ends up in a Content-Disposition filename AND in the
     * page's own HTML, which names the file before the transfer starts. */
    for (char* p = host; *p; p++)
        if (*p == '"' || *p == '\\' || *p == '/' || *p == ' ' || *p == '<' ||
            *p == '>' || *p == '&' || *p == '\'' || *p < 0x20) *p = '-';

    char stamp[20] = "nodate";
    if (storageGetInt("sys.time.valid", 0)) {
        time_t now = time(nullptr);
        struct tm tm {};
        localtime_r(&now, &tm);
        strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);
    }

    char name[128];
    snprintf(name, sizeof(name), "%s_%s_%s_%ukB.tgz",
             CONFIG_SPANGAP_FW_STUB, host, stamp, (unsigned)stateUsedKb());
    return name;
}

/** The archive's name, fixed for the boot. The page names the file it is about
 *  to hand over and the download's Content-Disposition carries the same name;
 *  recomputing would let the timestamp field tick between the two and print one
 *  name while saving another. */
const std::string& backupName() {
    static const std::string name = backupFilename();
    return name;
}

/** In-place percent-decode; the upload URL carries a name the page took from
 *  the File object and encodeURIComponent'd. */
void urlDecode(char* s) {
    char* w = s;
    for (char* r = s; *r; r++) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, nullptr, 16);
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/** The fw stub from an upload's filename: everything before the first '_'.
 *
 * This is the only field parsed, and it earns its parser because its failure
 * mode is the one exception to "every failure lands in the clean factory path":
 * a cross-project restore SUCCEEDS and then destroys itself. `s.sys.project`
 * mismatch makes the next boot factory-reset the flash store (or, on an
 * SD-backed store, reset-loop), so the archive silently takes out what it just
 * restored. Refuse on a mismatch; a stripped or unparseable name cannot be told
 * from a foreign one, so it warns and proceeds. */
bool stubMismatch(const char* filename, std::string& theirs) {
    const char* us = CONFIG_SPANGAP_FW_STUB;
    const char* underscore = strchr(filename, '_');
    if (!underscore || underscore == filename) {
        warn("restore: no firmware stub in '%s' — proceeding unchecked\n", filename);
        return false;
    }
    theirs.assign(filename, underscore - filename);
    return theirs != us;
}

/* ======================================================================
 * The pages
 * ==================================================================== */

/* Shared chrome. One string, no external anything: a recovery page must not
 * depend on a webroot that may be exactly what is broken.
 *
 * Every page is ONE MODAL CARD on a dimmed scrim, the shape the app's own
 * confirmations use. The operator arrives here from a dialog in the SPA that
 * said the device would reboot; landing on a card in the same style makes safe
 * mode read as the next step of that dialog rather than as another program. */
const char* PAGE_CSS =
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{background:#14161a;color:#fff;font:15px/1.5 system-ui,sans-serif;margin:0}"
    "#scrim{position:fixed;inset:0;background:rgba(0,0,0,.48);display:flex;"
    "align-items:center;justify-content:center;padding:1rem}"
    ".card{background:#1d1d1d;border-radius:6px;width:100%;max-width:26rem;"
    "padding:1.1rem 1.3rem 1.3rem;box-shadow:0 10px 30px rgba(0,0,0,.6);"
    "max-height:calc(100vh - 2rem);overflow:auto}"
    "h1{font-size:1.15rem;font-weight:600;margin:0 0 .6rem}"
    "p{color:#c9ced4;margin:.6rem 0}"
    "button,input[type=file],input[type=password]{font:inherit}"
    "button{background:#3b82f6;border:0;border-radius:4px;color:#fff;"
    "padding:.6rem 1.1rem;cursor:pointer}"
    "button:disabled{opacity:.5;cursor:default}"
    "button.ok{background:#21ba45}"
    "button.danger{background:#c10015;font-size:1.05rem;font-weight:600;"
    "padding:.85rem 1.2rem;width:100%}"
    ".acts{display:flex;flex-direction:column;gap:.6rem;margin-top:1rem}"
    "#bar{height:8px;border-radius:4px;background:#242830;overflow:hidden;margin:1.2rem 0}"
    "#bar>i{display:block;height:100%;width:0;background:#3b82f6;transition:width .4s linear}"
    "#drop{border:2px dashed #3a4048;border-radius:8px;padding:1.4rem;text-align:center;margin:1rem 0}"
    "#drop.over{border-color:#3b82f6}"
    ".err{color:#f87171}"
    ".warn{color:#fbbf24}"
    "</style>";

/* Shared page script. Every operation has exactly two states — one message
 * while it runs, one when it is done — and the done state ends the same way:
 * show it for five seconds, then go to the app.
 *
 * The hand-back does NOT probe first. Probing looked tidier and was worse: a
 * restore can change the hostname AND the TLS certificate, and a browser fetch
 * cannot tell "certificate the browser won't accept" from "device still down" —
 * both are an opaque rejected promise. The page would then wait out a device
 * that was already up and serving. Navigating hands the problem to the browser,
 * which is the one component that CAN show it: a certificate interstitial, a
 * name that no longer resolves, or the app. */
const char* PAGE_JS =
    "<script>"
    "const $=i=>document.getElementById(i);"
    "function say(t,c){const m=$('msg');m.textContent=t;m.className=c||''}"
    "function note(t){$('note').textContent=t||''}"
    "function handBack(){setTimeout(()=>{location.href='/'},5000)}"
    "</script>";

/** Wrap a card body in the page's title, chrome and scrim. */
std::string page(const char* title, const std::string& body) {
    return std::string("<title>") + title + "</title>" + PAGE_CSS + PAGE_JS +
           "<div id=scrim><div class=card>" + body + "</div></div>";
}

std::string loginPage() {
    return page("Safe mode",
        "<h1>Safe mode</h1>"
        "<p>This device is waiting to perform a state-store operation. "
        "Sign in as admin to continue.</p>"
        "<p><input type=password id=pw autofocus> <button onclick=go()>Sign in</button></p>"
        "<p id=msg class=err></p>"
        "<script>"
        "async function go(){"
        "const r=await fetch('/auth/login',{method:'POST',"
        "headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({password:document.getElementById('pw').value,realm:'admin'})});"
        "const j=await r.json();"
        "if(j.result===0){document.cookie='session='+j.cookie+'; path=/; SameSite=Strict';"
        "location.reload()}else{document.getElementById('msg').textContent='Wrong password.'}}"
        "document.getElementById('pw').addEventListener('keydown',e=>{if(e.key==='Enter')go()});"
        "</script>");
}

std::string backupPage() {
    /* Fetched rather than navigated to. A navigation to an attachment gives the
     * page no completion event, and completion is the whole point here: it is
     * what tells the operator the archive is whole and what starts the hand-back
     * to normal operation. Reading the body also surfaces a truncated chunked
     * stream as a rejected read, which a navigation would have saved silently as
     * a short file. A state store is a few hundred KB, so holding it in the
     * browser for the moment it takes to save costs nothing. */
    return page("Backing up",
        "<h1>Backing up</h1>"
        /* The name is served with the page, not read off the download, because
         * the operator is told it before there is anything to read it from. */
        "<p id=msg>Stand by as your data is gathered and sent to your browser as "
        + backupName() + ".</p>"
        "<p id=note></p>"
        "<script>"
        "(async()=>{"
        "let r;"
        "try{r=await fetch('/backup/state.tgz',{cache:'no-store'})}"
        "catch(e){return say('Could not reach the device.','err')}"
        "if(!r.ok)return say('The device refused the backup ('+r.status+').','err');"
        "let name='backup.tgz';"
        "const m=/filename=\"([^\"]+)\"/.exec(r.headers.get('content-disposition')||'');"
        "if(m)name=m[1];"
        "let b;"
        "try{b=await r.blob()}"
        "catch(e){say('The download was cut short \\u2014 nothing was saved.','err');"
        "return note('Reload this page to try again.')}"
        "const a=document.createElement('a');const u=URL.createObjectURL(b);"
        "a.href=u;a.download=name;document.body.appendChild(a);a.click();a.remove();"
        "setTimeout(()=>URL.revokeObjectURL(u),30000);"
        "say('You should have the file in your download folder now. Rebooting "
        "back to normal operation');"
        "note('');"
        "handBack();"
        "})();"
        "</script>");
}

std::string restorePage() {
    /* The only page that waits for the operator, and the only one with buttons.
     * Picking the archive and committing to it are two separate gestures: the
     * pick is reversible and the commit is not, and a drop that started the
     * erase by itself would make an accidental drag the last thing that ever
     * happened to this device's state. Choosing is therefore inert — it only
     * reveals the button that does the irreversible thing — and until that
     * button is pressed, "cancel and reboot" leaves with nothing touched. */
    return page("Restore system",
        "<h1>Restore system</h1>"
        "<p id=msg>Select or drop file to upload</p>"
        "<div id=drop>Drop a <code>.tgz</code> here, or "
        "<input type=file id=f accept='.tgz,application/gzip'></div>"
        "<p id=note></p>"
        "<div class=acts>"
        "<button id=go class=danger hidden onclick=send()>DELETE state and "
        "restore from this file.</button>"
        "<button id=cancel class=ok onclick=quit()>cancel and reboot</button>"
        "</div>"
        "<script>"
        "const d=$('drop');let sel=null;"
        "d.addEventListener('dragover',e=>{e.preventDefault();d.classList.add('over')});"
        "d.addEventListener('dragleave',()=>d.classList.remove('over'));"
        "d.addEventListener('drop',e=>{e.preventDefault();d.classList.remove('over');"
        "if(e.dataTransfer.files[0])pick(e.dataTransfer.files[0])});"
        "$('f').addEventListener('change',e=>{"
        "if(e.target.files[0])pick(e.target.files[0])});"
        "function pick(file){sel=file;$('go').hidden=false;"
        "note(file.name+' \\u2014 '+Math.round(file.size/1024)+' kB. Nothing on "
        "the device has been touched.')}"
        /* Leaving is a device-side decision like every other exit from safe
         * mode: the endpoint reboots, the page only says so. */
        "async function quit(){"
        "$('go').hidden=true;$('cancel').hidden=true;d.style.display='none';"
        "say('Rebooting back to normal operation');note('');"
        "try{await fetch('/backup/cancel',{cache:'no-store'})}catch(e){}"
        "handBack()}"
        /* Check the archive BEFORE the device commits to it. A restore formats
         * first — it has to, there is nowhere to stage — so a damaged archive
         * costs the operator everything that was on the device and leaves them
         * at factory defaults. The browser is holding the whole file and can
         * inflate it end to end, CRC and all, at no cost to the device: the one
         * place in this design where the truncation can be caught while it is
         * still free. If the browser has no DecompressionStream we proceed —
         * the device's own footer check still backstops it, just later and at
         * the price the format already exacted. */
        "async function verify(file){"
        "if(!('DecompressionStream' in window))return true;"
        "try{const rd=file.stream().pipeThrough(new DecompressionStream('gzip')).getReader();"
        "let n=0;for(;;){const c=await rd.read();if(c.done)break;n+=c.value.length}"
        "return n>0}catch(e){return false}}"
        "async function send(){"
        "const file=sel;if(!file)return;"
        "d.style.display='none';$('go').hidden=true;$('cancel').hidden=true;"
        "say('Checking '+file.name+' \\u2014 nothing on the device has been "
        "touched yet.');"
        "note('');"
        "if(!await verify(file)){"
        "say('That archive is damaged \\u2014 it does not inflate cleanly.','err');"
        "note('Nothing on the device was touched. Take a fresh backup, or try "
        "another archive.');"
        "d.style.display='';$('go').hidden=false;$('cancel').hidden=false;return}"
        "say('Restoring '+file.name+' \\u2014 this takes a few seconds.');"
        "note('Do not power the device off.');"
        "let j;"
        "try{const r=await fetch('/backup/'+encodeURIComponent(file.name),"
        "{method:'POST',body:file});j=await r.json()}"
        /* Both failure paths hand back too: the device reboots whatever the
         * outcome, so a page that only reloads on success sits there for ever
         * over a device that has long since come back. */
        "catch(e){say('The connection dropped mid-restore.','err');"
        "note('The device will come back from factory defaults \\u2014 nothing "
        "of the old state survived, and nothing of the archive did either.');"
        "return handBack()}"
        "if(!j.ok){say('Restore failed: '+j.error+'.','err');"
        "note('The device is restarting from factory defaults.');"
        "return handBack()}"
        "say('Restored '+j.entries+' entries \\u2014 '+Math.round(j.bytes/1024)+' kB.');"
        /* The hostname came out of the archive, so a restore from another
         * device moves this one to that device's name — and this page's own
         * address stops resolving to it. Say so before the hand-back tries. */
        "note('The device is restarting into normal operation. It now uses the "
        "hostname from the archive, so if that came from another device you may "
        "have to look for it under that name.');"
        "handBack();"
        "}"
        "</script>");
}

std::string factoryPage() {
    uint32_t start = 0, size = 0;
    fsFactoryWipeExtent(&start, &size);
    /* Estimate from the measured per-MB cost, plus a quarter for margin. The
     * bar is the entire progress mechanism: no endpoint and no polling, because
     * the wipe makes HTTP janky anyway — every erase stalls flash-resident
     * tasks — and an estimate with margin says the same thing at no cost. */
    uint32_t ms = (uint32_t)((uint64_t)size * FS_WIPE_MS_PER_MB / (1024 * 1024));
    ms = ms + ms / 4 + 3000;

    char script[768];
    snprintf(script, sizeof(script),
        "<script>const T=%u,t0=Date.now(),b=document.querySelector('#bar>i');"
        "const iv=setInterval(()=>{const p=Math.min(100,(Date.now()-t0)/T*100);"
        "b.style.width=p+'%%';if(p>=100){clearInterval(iv);"
        "say('Done \\u2014 the device has restarted as a new one.');"
        "note('Connect to its own access point to set it up again. This page "
        "cannot follow it there.')}"
        "},250);</script>", (unsigned)ms);

    /* The only operation with no hand-back: the wipe took the WiFi credentials
     * with it, so the device comes up on its own access point and this
     * browser's network is no longer where it is. */
    return page("Erasing",
        std::string(
        "<h1>Erasing</h1>")+
        "<p id=msg>Every trace of this device's configuration, keys and "
        "identity is being overwritten.</p>"
        "<div id=bar><i></i></div>"
        "<p id=note>Do not power the device off.</p>" + script);
}

}  // namespace

/* Built fresh per request into a heap_caps buffer, because that is what web's
 * response machinery frees. */
char* safeModePage(bool authed, size_t* outLen) {
    std::string html;
    if (!authed) {
        html = loginPage();
    } else {
        switch (spangapSafeMode()) {
            case SAFE_MODE_BACKUP:        html = backupPage();  break;
            case SAFE_MODE_RESTORE:       html = restorePage(); break;
            case SAFE_MODE_FACTORY_RESET: html = factoryPage(); break;
            default:                      html = loginPage();   break;
        }
    }
    char* out = (char*)heap_caps_malloc(html.size(), MALLOC_CAP_SPIRAM);
    if (!out) return nullptr;
    memcpy(out, html.data(), html.size());
    if (outLen) *outLen = html.size();
    return out;
}

/* ======================================================================
 * Backup — walk the store, tar it, deflate it, chunk it out
 * ==================================================================== */

namespace {

/* HTTP has two ways to say where a body ends: a Content-Length up front, or
 * chunked framing. We cannot declare a length — the body is generated while
 * walking the filesystem, and knowing its size in advance would mean producing
 * it in advance, which means storing it. That leaves chunked or
 * connection-close, and connection-close is the trap: under it a truncated
 * transfer is byte-for-byte indistinguishable from a complete one, so the
 * operator keeps a short archive and finds out at restore time. Under chunked
 * the terminating zero chunk never arrives, curl exits non-zero and the browser
 * marks the download failed. */
struct sink_ctx_t { int h; uint64_t sent; };

bool chunkSink(void* ctx, const uint8_t* data, size_t len) {
    auto* s = (sink_ctx_t*)ctx;
    if (!len) return true;
    char hdr[16];
    int n = snprintf(hdr, sizeof(hdr), "%x\r\n", (unsigned)len);
    if (!sendAll(s->h, hdr, (size_t)n) || !sendAll(s->h, data, len) ||
        !sendAll(s->h, "\r\n", 2))
        return false;
    s->sent += len;
    return true;
}

/** Excluded from a state backup: in-flight atomic writes, the restore marker,
 *  and the updater's staged firmware image (megabytes, and meaningless here).
 *  Logs and recordings live under /sdcard, outside the store, and fall out of
 *  the walk on their own. */
bool excluded(const char* name) {
    size_t n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".new") == 0) return true;
    if (strcmp(name, "flashme.bin") == 0) return true;
    if (name[0] == '.') return true;
    return false;
}

bool walkInto(targz_writer_t* w, const std::string& abs, const std::string& rel,
              int depth, uint8_t* io, uint32_t* files) {
    if (depth > WALK_MAX_DEPTH) {
        warn("backup: depth cap reached at %s\n", abs.c_str());
        return true;
    }
    auto* list = (fs_listing_t*)heap_caps_malloc(
        WALK_MAX_ENTRIES * sizeof(fs_listing_t), MALLOC_CAP_SPIRAM);
    if (!list) return false;
    int n = fs_listdir(abs.c_str(), list, WALK_MAX_ENTRIES);
    if (n >= WALK_MAX_ENTRIES)
        err("backup: %s has more than %d entries — THIS ARCHIVE IS INCOMPLETE\n",
            abs.c_str(), WALK_MAX_ENTRIES);
    bool ok = true;

    for (int i = 0; i < n && ok; i++) {
        if (strcmp(list[i].name, ".") == 0 || strcmp(list[i].name, "..") == 0)
            continue;
        if (excluded(list[i].name)) continue;
        std::string childAbs = abs + "/" + list[i].name;
        /* Paths are stored RELATIVE to the state dir, so an archive taken from
         * /state restores onto an SD store and the other way round. The archive
         * is the one place the two stores meet. */
        std::string childRel = rel.empty() ? std::string(list[i].name)
                                           : rel + "/" + list[i].name;
        if (list[i].isDir) {
            ok = targzWriterAddDir(w, childRel.c_str(), (uint32_t)list[i].mtime) &&
                 walkInto(w, childAbs, childRel, depth + 1, io, files);
            continue;
        }
        int f = fs_open(childAbs.c_str(), "rb");
        if (f < 0) { warn("backup: cannot read %s\n", childAbs.c_str()); continue; }
        ok = targzWriterAddFile(w, childRel.c_str(), list[i].size,
                                (uint32_t)list[i].mtime);
        uint32_t left = list[i].size;
        while (ok && left) {
            size_t want = left < IO_CHUNK ? left : IO_CHUNK;
            size_t got = fs_read(io, 1, want, f);
            if (!got) break;                       /* file shrank; writer pads */
            ok = targzWriterData(w, io, got);
            left -= (uint32_t)got;
        }
        fs_close(f);
        (*files)++;
    }
    heap_caps_free(list);
    return ok;
}

void handleBackup(int h) {
    const std::string& name = backupName();
    char resp[320];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/gzip\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n", name.c_str());
    if (!sendAll(h, resp, (size_t)n)) { itsDisconnect(h); return; }

    sink_ctx_t sink = { h, 0 };
    uint8_t* io = (uint8_t*)heap_caps_malloc(IO_CHUNK, MALLOC_CAP_SPIRAM);
    targz_writer_t* w = io ? targzWriterOpen(chunkSink, &sink) : nullptr;
    if (!w) {
        /* Headers are already out, so there is no status left to send. Drop the
         * connection without the terminating chunk: the client sees exactly
         * what it should — an incomplete download. */
        heap_caps_free(io);
        err("backup: could not allocate the compressor\n");
        itsDisconnect(h);
        rebootSoon(500);
        return;
    }

    uint32_t files = 0;
    /* Nothing is writing to the store in safe mode, so there is nothing to
     * freeze. This is a plain read. */
    info("backup: walking %s\n", fsStateDir());
    bool ok = walkInto(w, fsStateDir(), "", 0, io, &files);
    ok = targzWriterClose(w) && ok;
    heap_caps_free(io);

    /* Remaining stack at the deepest point this task ever reached. The walk
     * recurses and tdefl sits under it, so this is the number that says whether
     * the task is sized right — print it either way. */
    unsigned headroom = (unsigned)uxTaskGetStackHighWaterMark(nullptr);

    if (ok) {
        ok = sendAll(h, "0\r\n\r\n", 5);
        info("backup: %s — %u files, %u kB sent (stack headroom %u B)\n",
             name.c_str(), (unsigned)files, (unsigned)(sink.sent / 1024),
             headroom * (unsigned)sizeof(StackType_t));
    } else {
        /* No terminating chunk: the client sees a truncated download, which is
         * the honest report and exactly what chunked framing exists to make
         * visible. The byte count says how far we got. */
        warn("backup: FAILED after %u files, %u B sent (stack headroom %u B)\n",
             (unsigned)files, (unsigned)sink.sent,
             headroom * (unsigned)sizeof(StackType_t));
    }
    itsSendDrain(h, 5000);
    itsDisconnect(h);
    rebootSoon(500);
}

/* ======================================================================
 * Restore — stream in, format, expand
 * ==================================================================== */

/* The upload body arrives either Content-Length-delimited (what a browser
 * `fetch(body: File)` and `curl -T` both send) or chunked. One reader over
 * both, handing out plain body bytes. */
struct body_reader_t {
    int         h;
    const char* pre;        /* body bytes that came in the header's TCP segment */
    size_t      preLen;
    bool        chunked;
    uint64_t    remain;     /* Content-Length left, or bytes left in this chunk */
    uint64_t    got;        /* body bytes handed out so far */
    uint64_t    expect;     /* Content-Length, or 0 when chunked */
    bool        done;
    bool        stalled;    /* ended on silence/disconnect, not on the last byte */
    uint8_t     buf[2048];
    size_t      bufLen, bufPos;
};

/* A read that comes back empty does NOT mean the upload finished. We only read
 * between flash writes, and LittleFS can hold us off for a while — an erase
 * ahead of an allocation is ~150 ms and they come in runs — so silence on the
 * socket is the normal shape of a busy restore, not the end of the body. The
 * old single 10 s timeout turned any long pause into "body complete", which
 * then surfaced as `archive truncated` with nothing to say which it was.
 * Wait while the peer is still there, and record which way it ended. */
constexpr int BODY_STALL_S = 30;

size_t bodyRecv(body_reader_t& b, uint8_t* out, size_t max) {
    for (int s = 0; s < BODY_STALL_S; s++) {
        size_t n = itsRecv(b.h, out, max, pdMS_TO_TICKS(1000));
        if (n) return n;
        if (!itsConnected(b.h)) break;
    }
    b.stalled = true;
    return 0;
}

/** Next raw byte from the connection, or -1 when it has ended. */
int rawByte(body_reader_t& b) {
    if (b.preLen) { b.preLen--; return (uint8_t)*b.pre++; }
    if (b.bufPos < b.bufLen) return b.buf[b.bufPos++];
    b.bufLen = bodyRecv(b, b.buf, sizeof(b.buf));
    b.bufPos = 0;
    if (!b.bufLen) return -1;
    return b.buf[b.bufPos++];
}

/** Raw bytes without copying byte-at-a-time, up to `max`. 0 = ended. */
size_t rawRead(body_reader_t& b, uint8_t* out, size_t max) {
    if (b.preLen) {
        size_t n = b.preLen < max ? b.preLen : max;
        memcpy(out, b.pre, n);
        b.pre += n; b.preLen -= n;
        return n;
    }
    if (b.bufPos < b.bufLen) {
        size_t n = b.bufLen - b.bufPos;
        if (n > max) n = max;
        memcpy(out, b.buf + b.bufPos, n);
        b.bufPos += n;
        return n;
    }
    return bodyRecv(b, out, max);
}

/** Read the next chunk-size line. False at the terminating zero chunk. */
bool chunkHeader(body_reader_t& b) {
    char line[24];
    size_t i = 0;
    for (;;) {
        int c = rawByte(b);
        if (c < 0) return false;
        if (c == '\n') break;
        if (c != '\r' && i + 1 < sizeof(line)) line[i++] = (char)c;
    }
    line[i] = '\0';
    if (!i) return chunkHeader(b);            /* the CRLF after a chunk body */
    b.remain = strtoull(line, nullptr, 16);
    return b.remain != 0;
}

/** Body bytes. Returns 0 at the end of the body (or on a dead connection). */
size_t bodyRead(body_reader_t& b, uint8_t* out, size_t max) {
    if (b.done) return 0;
    if (b.chunked && b.remain == 0 && !chunkHeader(b)) { b.done = true; return 0; }
    if (b.remain == 0) { b.done = true; return 0; }
    if (max > b.remain) max = (size_t)b.remain;
    size_t n = rawRead(b, out, max);
    if (!n) { b.done = true; return 0; }
    b.remain -= n;
    b.got    += n;
    return n;
}

/* ---- extraction sink ---- */

struct restore_ctx_t {
    int      file = -1;
    uint32_t files = 0;
};

/** Create every parent directory of a state-relative path. An archive lists
 *  directories before their contents, but only if the producer walked them —
 *  a hand-made tar need not, so never depend on it. */
void ensureParents(const std::string& abs) {
    size_t slash = abs.find_last_of('/');
    if (slash == std::string::npos) return;
    fs_mkdirp(abs.substr(0, slash).c_str());
}

bool onDir(void* c, const char* name) {
    (void)c;
    fs_mkdirp(fsStatePath(("/" + std::string(name)).c_str()).c_str());
    return true;
}

bool onFile(void* c, const char* name, uint32_t size) {
    (void)size;
    auto* ctx = (restore_ctx_t*)c;
    std::string abs = fsStatePath(("/" + std::string(name)).c_str());
    ensureParents(abs);
    ctx->file = fs_open(abs.c_str(), "wb");
    if (ctx->file < 0) {
        err("restore: cannot create %s\n", abs.c_str());
        return false;
    }
    ctx->files++;
    return true;
}

bool onData(void* c, const void* data, size_t len) {
    auto* ctx = (restore_ctx_t*)c;
    if (ctx->file < 0) return false;
    const uint8_t* p = (const uint8_t*)data;
    while (len) {
        size_t n = len < WRITE_CHUNK ? len : WRITE_CHUNK;
        if (fs_write(p, 1, n, ctx->file) != n) return false;
        p += n; len -= n;
        if (len) vTaskDelay(1);   /* let the network task feeding us breathe */
    }
    return true;
}

bool onFileEnd(void* c) {
    auto* ctx = (restore_ctx_t*)c;
    if (ctx->file >= 0) fs_close(ctx->file);
    ctx->file = -1;
    return true;
}

void handleRestore(int h, const char* hdr, int hlen, const char* path) {
    /* `path` is "backup/<name>" — the upload body is raw, so it carries no
     * filename of its own and the name IS the last URL segment. That costs
     * nothing at either end: the page appends the File object's name, and
     * `curl -T f.tgz https://host/backup/` appends the local one by itself. */
    char name[128] = {};
    const char* slash = strrchr(path, '/');
    safeStrncpy(name, slash ? slash + 1 : path, sizeof(name));
    urlDecode(name);

    std::string theirs;
    if (name[0] && stubMismatch(name, theirs)) {
        warn("restore: '%s' is a %s archive, this is %s — refusing\n",
             name, theirs.c_str(), CONFIG_SPANGAP_FW_STUB);
        bail(h, 409, "archive is from a different firmware");
    }

    body_reader_t b = {};
    b.h = h;
    /* Body bytes that arrived in the same TCP segment as the headers. */
    const char* boundary = (const char*)memmem(hdr, hlen, "\r\n\r\n", 4);
    int hdrEnd = boundary ? (int)((boundary - hdr) + 4) : hlen;
    b.pre    = hdr + hdrEnd;
    b.preLen = (size_t)(hlen - hdrEnd);

    char te[24] = {}, cl[24] = {};
    webHeaderField(hdr, hdrEnd, "Transfer-Encoding", te, sizeof(te));
    webHeaderField(hdr, hdrEnd, "Content-Length",    cl, sizeof(cl));
    b.chunked = strcasestr(te, "chunked") != nullptr;
    /* Content-Length is an early sanity check, not the truncation detector —
     * that is the gzip footer, checked after the last byte is written. */
    b.remain  = b.chunked ? 0 : strtoull(cl, nullptr, 10);
    b.expect  = b.remain;
    if (!b.chunked && b.remain == 0) bail(h, 411, "no body");
    info("restore: %s, %llu B declared\n", name,
         (unsigned long long)b.expect);

    /* Peek the gzip magic BEFORE the point of no return, so an operator who
     * uploaded the wrong file still has their device. */
    uint8_t first[2] = {};
    size_t have = 0;
    while (have < sizeof(first)) {
        size_t n = bodyRead(b, first + have, sizeof(first) - have);
        if (!n) break;
        have += n;
    }
    if (have < 2 || first[0] != 0x1f || first[1] != 0x8b)
        bail(h, 415, "not a gzip archive");

    /* ---- point of no return ---- */
    storageStopFlushing();
    info("restore: formatting %s\n", fsStateDir());
    if (!fsFormatStateStore()) bail(h, 500, "could not empty the state store");
    fsSetRestoreMarker(true);

    restore_ctx_t ctx;
    targz_reader_cb_t cb = { onDir, onFile, onData, onFileEnd };
    targz_reader_t* r = targzReaderOpen(&cb, &ctx);
    if (!r) bail(h, 500, targzErrStr(TARGZ_ERR_MEM));

    targz_err_t e = targzReaderFeed(r, first, have);
    uint8_t* io = (uint8_t*)heap_caps_malloc(IO_CHUNK, MALLOC_CAP_SPIRAM);
    if (!io) e = TARGZ_ERR_MEM;
    while (e == TARGZ_OK) {
        size_t n = bodyRead(b, io, IO_CHUNK);
        if (!n) break;
        e = targzReaderFeed(r, io, n);
    }
    heap_caps_free(io);

    uint32_t entries = 0;
    uint64_t bytes = 0;
    targz_err_t fin = targzReaderFinish(r, &entries, &bytes);
    if (e == TARGZ_OK) e = fin;
    if (ctx.file >= 0) fs_close(ctx.file);

    if (e != TARGZ_OK) {
        /* The marker stays: the next boot sees a store that is a partial
         * expansion of somebody's archive, formats it, and repopulates from the
         * factory seeds. Every failure lands here rather than in a
         * plausible-looking corrupt store. */
        err("restore failed: %s\n", targzErrStr(e));
        /* `archive truncated` has two quite different causes and they need
         * telling apart: the upload stopped short (we never received what the
         * client said it would send) or the archive itself ends early (we got
         * every declared byte and the gzip stream still had no footer). Say
         * which — the byte counts are the whole diagnosis. */
        err("restore: received %llu of %llu declared B, %u entries, %s\n",
            (unsigned long long)b.got, (unsigned long long)b.expect,
            (unsigned)entries,
            b.stalled       ? "upload STALLED — the client stopped sending"
          : (b.expect && b.got < b.expect) ? "upload SHORT — connection ended early"
                            : "upload COMPLETE — the archive itself ends early");
        char body[192];
        int n = snprintf(body, sizeof(body),
                         "{\"ok\":false,\"error\":\"%s\"}", targzErrStr(e));
        webSendResponse(h, 500, "application/json", body, (size_t)n);
        itsSendDrain(h, 3000);
        itsDisconnect(h);
        rebootSoon(500);
        return;
    }

    fsSetRestoreMarker(false);
    info("restore: %u entries, %llu bytes\n", (unsigned)entries,
         (unsigned long long)bytes);
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"entries\":%u,\"bytes\":%llu}",
                     (unsigned)entries, (unsigned long long)bytes);
    webSendResponse(h, 200, "application/json", body, (size_t)n);
    itsSendDrain(h, 3000);
    itsDisconnect(h);
    rebootSoon(500);
}

/* ======================================================================
 * Endpoint plumbing
 * ==================================================================== */

/* The endpoint, as an IN-WEB HANDLER rather than a forwarded connection.
 *
 * A forward would be wrong for a request with a body, and wrong in a way that
 * only shows under load. web parses headers into a 1 KB `rbuf`, then hands the
 * connection over with `itsInject(…, asServer=false, rbuf, rLen)` — and inject
 * APPENDS to the client→server ring. Anything net pushed into that ring while
 * web was parsing is already queued, so the injected earlier bytes land after
 * the later ones and the body is reordered; the inject also passes timeout 0
 * and ignores its return, so whatever does not fit is dropped. A few hundred KB
 * arriving at line rate keeps that ring occupied, which made a restore fail
 * with `archive truncated` on a provably intact archive — intermittently, on
 * timing.
 *
 * A handler has neither problem: web calls it with `rbuf`/`rLen` directly and
 * the rest of the stream is still in the ring, in order, for us to read. It
 * also runs on web's own task, which IS the connection's server task, so
 * itsSend and itsRecv resolve their direction (they answer 0 to anyone else).
 *
 * It blocks web's task for the whole transfer, which the handler contract warns
 * against — and which is right here: in safe mode web has one page and this
 * endpoint to serve, and the page is already on screen by the time a transfer
 * starts. */
void safeModeHandler(int h, const char* hdr, int hdrLen) {
    char method[12] = {}, path[160] = {};
    webGetMethod(hdr, hdrLen, method, sizeof(method));
    webGetPath(hdr, hdrLen, path, sizeof(path));

    if (!authorized(hdr, hdrLen)) { bail(h, 401, "not signed in"); return; }

    safe_mode_t mode = spangapSafeMode();
    bool wantsOut = strcmp(method, "GET") == 0;
    bool wantsIn  = strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0;

    /* The way out of the one mode that can be left with nothing done. A restore
     * waits for a gesture, so it needs a gesture that means "never mind"; the
     * other two are already under way by the time a page loads. A GET cannot
     * collide with an upload (POST/PUT), so `cancel` is not a reserved
     * filename. */
    if (wantsOut && mode == SAFE_MODE_RESTORE &&
        strcmp(path, SAFE_MODE_ENDPOINT "/cancel") == 0) {
        info("restore: cancelled — rebooting with the store untouched\n");
        webSendResponse(h, 200, "application/json", "{\"ok\":true}", 11);
        itsSendDrain(h, 2000);
        itsDisconnect(h);
        rebootSoon(500);
        return;
    }

    /* The endpoint for the operation this boot was not asked to perform does
     * not exist. Both handlers end in a reboot, so nothing here runs twice. */
    if (wantsOut && mode == SAFE_MODE_BACKUP) { handleBackup(h); return; }
    if (wantsIn && mode == SAFE_MODE_RESTORE) {
        handleRestore(h, hdr, hdrLen, path);
        return;
    }
    bail(h, 404, "not found");
}

void deadlineCb(void*) {
    warn("safe mode: %d s deadline reached — rebooting\n", SAFE_DEADLINE_S);
    esp_restart();
}

}  // namespace

void safeModeInit() {
    safe_mode_t mode = spangapSafeMode();
    if (mode == SAFE_MODE_NONE) return;

    /* Nothing may deep-sleep mid-window. */
    pmLockCreate(PM_NO_DEEP_SLEEP, "safemode", &s_pmLock);
    if (s_pmLock) pmLockAcquire(s_pmLock);

    /* One fixed deadline, armed at boot, regardless of progress. A factory
     * reset ignores every client and reboots itself when the wipe ends; this
     * covers the two transfers, and the case where nobody ever turns up. */
    esp_timer_create_args_t a = {};
    a.callback = deadlineCb;
    a.name = "safemode_deadline";
    if (esp_timer_create(&a, &s_deadline) == ESP_OK)
        esp_timer_start_once(s_deadline, (int64_t)SAFE_DEADLINE_S * 1000000);

    /* The endpoint runs on web's task (see safeModeHandler). webInit() calls us
     * from inside that task's body, after its aux handlers are installed — a
     * registration is an aux message, and an aux to a port with no handler yet
     * is dropped while the sender is told it was delivered. */
    if (mode == SAFE_MODE_BACKUP || mode == SAFE_MODE_RESTORE) {
        webRegisterHandler(SAFE_MODE_ENDPOINT, safeModeHandler);
        info("safe mode: /%s ready\n", SAFE_MODE_ENDPOINT);
    }

    info("safe mode: serving the %s page\n",
         mode == SAFE_MODE_BACKUP  ? "backup"
       : mode == SAFE_MODE_RESTORE ? "restore" : "factory-reset");
}
