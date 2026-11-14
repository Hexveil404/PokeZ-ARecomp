// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../platform.h"
#include "../settings.h"

static void ok_logger(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[svc] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

// ------------------------------------------------------------------ pctl
static void pctl_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1031: case 1035: case 1018: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // restrictions off
    case 1032: OUT(uint32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[pctl] IParentalControlService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}

static void pctl_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0 || req->cmd == 1)
        ipc_return_object(resp, service_new("IParentalControlService", pctl_service, NULL));
}
SERVICE("pctl", pctl) { pctl_root(s, req, resp); }
SERVICE("pctl:s", pctl_s) { pctl_root(s, req, resp); }
SERVICE("pctl:a", pctl_a) { pctl_root(s, req, resp); }
SERVICE("pctl:r", pctl_r) { pctl_root(s, req, resp); }

// ------------------------------------------------------------------ nifm:u
// A console with no connection. The game asks for it before anything online (Link Play > Mystery Gift > Get via Internet,
// #107): nn::nifm::Initialize keeps a request from the start, and the logging stub returned no IGeneralService, so
// RequestClient's constructor called through a null pointer. The request never goes on hold (state 2: the only one
// SubmitAndWait waits in) and is never available (3): the game goes back to its list, with no message (a console shows
// the connection applet's error there).
#define NIFM_NO_NETWORK MAKE_RESULT(110, 1111)

// Mystery Gift (g_settings.mystery_gift, LARECOMP_MYSTERY_GIFT=1): a connection, so Link Play > Mystery Gift goes on to
// its downloads, which bcat.c serves from mystery_gift/. Nothing else reaches the PC's network from this: the game's
// sockets stay the console's stubs (net.c / hle/net_socket.c), so anything that really needs a server fails as it would
// on a console whose connection dropped.
bool nifm_online(void) {  // (acc.c: the account steps of going online)
    static int env = -1;
    if (env < 0) env = getenv("LARECOMP_MYSTERY_GIFT") && atoi(getenv("LARECOMP_MYSTERY_GIFT")) ? 1 : 0;
    return env || g_settings.mystery_gift;
}

static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:  // GetRequestState: 3 accepted (a connection) / 1 free
        OUT(uint32_t, 0) = nifm_online() ? 3 : 1;
        {   // (polled: logged when it changes)
            static int last = -1;
            extern volatile int g_mg_trace;
            extern void mystery_gift_hooks_install(void);
            extern volatile int g_fnprof, g_mg_fn;
            if (nifm_online()) {
                mystery_gift_hooks_install();
                // LARECOMP_MG_TRACE=1: the Mystery Gift diagnostics (dispatch.c mg_trace, fnprof.c mg_fn): every call
                // in its code to logs/mystery_gift_*.log. Slow (indirect calls uncached, every function entry seen).
                if (getenv("LARECOMP_MG_TRACE") && atoi(getenv("LARECOMP_MG_TRACE"))) g_mg_trace = 1, g_mg_fn = 1, g_fnprof = 1;
            }
            if (last != nifm_online()) LOG("[nifm] request state -> %s\n", nifm_online() ? "accepted (Mystery Gift)" : "no network");
            last = nifm_online();
        }
        resp_size(resp, 4);
        break;
    case 1:  // GetResult
        if (nifm_online()) break;
        resp->result = NIFM_NO_NETWORK;
        break;
    case 21:  // GetAppletInfo: failing, HandleNetworkRequestResult opens no connection applet (we have none) and doesn't retry
        resp->result = NIFM_NO_NETWORK;
        break;
    case 2:  // GetSystemEventReadableHandles: never signalled, nothing waits on them
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    default:  // (Cancel, Submit and the requirement setters)
        LOG("[nifm] IRequest cmd %u (ok)\n", req->cmd);
    }
}

static void nifm_general(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: {  // GetClientId: not 0, which reads as invalid
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 4) *(uint32_t *)(uintptr_t)b->addr = 1;
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    }
    case 4:  // CreateRequest
        ipc_return_object(resp, service_new("nifm:IRequest", nifm_request, NULL));
        break;
    case 5: case 12: case 15: case 18:  // GetCurrentNetworkProfile, GetCurrentIpAddress, GetCurrentIpConfigInfo, GetInternetConnectionStatus
        if (!nifm_online()) { resp->result = NIFM_NO_NETWORK; break; }
        if (req->cmd == 12) OUT(uint32_t, 0) = 0x0100007F;  // 127.0.0.1
        if (req->cmd == 18) { OUT(uint8_t, 0) = 2; OUT(uint8_t, 1) = 3; OUT(uint8_t, 2) = 4; }  // ethernet, full, connected
        resp_size(resp, 0x10);  // (the profile, a buffer: left zeroed)
        break;
    case 21:  // IsAnyInternetRequestAccepted
        OUT(uint8_t, 0) = nifm_online();
        resp_size(resp, 4);
        break;
    default:
        LOG("[nifm] IGeneralService cmd %u (ok)\n", req->cmd);
        resp_size(resp, 0x10);  // (IsAnyInternetRequestAccepted and the like: false)
    }
}

SERVICE("nifm:u", nifm_u) {
    (void)s;
    if (req->cmd == 4 || req->cmd == 5) {  // CreateGeneralServiceOld, CreateGeneralService
        ipc_return_object(resp, service_new("nifm:IGeneralService", nifm_general, NULL));
        return;
    }
    LOG("[nifm] IStaticService cmd %u (ok)\n", req->cmd);
}

// ------------------------------------------------------------------ csrng
// GenerateRandomBytes (cmd 0) into the output buffer. Z-A asks it for the seeds of its rolls, hundreds of times a
// session; the logging stub left the buffer as it was, so every roll came out the same - each ball caught (#61).
SERVICE("csrng", csrng) {
    (void)s;
    IpcBuf *b = req->cmd == 0 ? out_buf(req, 0) : NULL;
    if (!b) { LOG("[csrng] cmd %u (ok)\n", req->cmd); return; }
    uint8_t *p = (uint8_t *)(uintptr_t)b->addr;
    for (uint64_t i = 0; i < b->size; i += 4) {
        uint32_t r = arc4random();
        memcpy(p + i, &r, b->size - i < 4 ? b->size - i : 4);
    }
}

// ------------------------------------------------------------------ aoc:u
// Add-on content (DLC): the game root's aoc/<title id, 16 hex digits>.bin files, each the DLC's RomFS image (fs.c
// OpenDataStorageByDataId serves them). Their title ids share the base (application id + 0x1000); the game sees
// their indices (title id - base). Sorted so the indices do not depend on the host directory order (the Switch 2
// Edition add-on ...5001 is index 1, the Mega Dimension DLC ...5002 index 2). None: no DLC.
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int aoc_ids(uint64_t *ids, int max) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", g_romfs_dir ? g_romfs_dir : "extracted/romfs");
    char *slash = strrchr(dir, '/');
    snprintf(slash ? slash + 1 : dir, sizeof dir - (slash ? (size_t)(slash + 1 - dir) : 0), "aoc");
    DIR *d = opendir(dir);
    int n = 0;
    for (struct dirent *e; d && (e = readdir(d)) && n < max;) {
        unsigned long long id;
        char ext[8] = "";
        if (strlen(e->d_name) == 20 && sscanf(e->d_name, "%16llx.%3s", &id, ext) == 2 && !strcmp(ext, "bin")) ids[n++] = id;
    }
    if (d) closedir(d);
    if (n > 1) qsort(ids, (size_t)n, sizeof ids[0], cmp_u64);
    return n;
}

SERVICE("aoc:u", aoc_u) {
    (void)s;
    uint64_t ids[64];
    int n = aoc_ids(ids, 64);
    uint64_t base = n ? ids[0] & ~0xFFFull : 0x01006BB00C6F1000ull;
    static int logged;
    if (!logged++) {
        LOG("[aoc] %d add-on content(s) installed", n);
        for (int i = 0; i < n; i++) LOG(" %016llx", (unsigned long long)ids[i]);
        LOG("\n");
    }
    switch (req->cmd) {
    case 2: OUT(uint32_t, 0) = (uint32_t)n; resp_size(resp, 4); break;  // CountAddOnContent
    case 3: {  // ListAddOnContent(offset, count) -> their indices
        uint32_t off = IN(uint32_t, 0), cnt = IN(uint32_t, 4), k = 0;
        IpcBuf *b = out_buf(req, 0);
        for (int i = (int)off; i < n && k < cnt && b && (k + 1) * 4 <= b->size; i++, k++)
            ((uint32_t *)(uintptr_t)b->addr)[k] = (uint32_t)(ids[i] - base);
        OUT(uint32_t, 0) = k;
        resp_size(resp, 4);
        break;
    }
    case 5: OUT(uint64_t, 0) = base; resp_size(resp, 8); break;  // GetAddOnContentBaseId
    case 7: break;  // PrepareAddOnContent
    case 50: break;  // CheckAddOnContentMountStatus
    case 8: case 10: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // GetAddOnContentListChangedEvent(WithProcessId)
    default: ok_logger(s, req, resp);
    }
}

// ------------------------------------------------------------------ Mystery Gift: what going online asks next
// (only with Mystery Gift on: nifm_online). nsd:a GetEnvironmentIdentifier -> "lp1" (the retail servers' name); ntc's
// network clock check: done at once, success (the PC's clock is the console's clock here). ssl:s CreateContext fails:
// nothing here speaks TLS, so the game gets a clean "couldn't connect" from its own code instead of a missing object.
SERVICE("nsd:a", nsd_a) {
    (void)s;
    IpcBuf *b = out_buf(req, 0);
    if (req->cmd == 11 && b && b->size) {  // GetEnvironmentIdentifier
        memset((void *)(uintptr_t)b->addr, 0, (size_t)b->size);
        memcpy((void *)(uintptr_t)b->addr, "lp1", b->size < 3 ? (size_t)b->size : 3);
        return;
    }
    LOG("[nsd] cmd %u (ok)\n", req->cmd);
    resp_size(resp, 0x10);
}
SERVICE("nsd:u", nsd_u) { nsd_a(s, req, resp); }

static void ntc_ensure(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: break;  // StartTask
    case 1: {       // GetFinishNotificationEvent: signalled
        uint32_t w, r;
        KEvent *e = kernel_create_event_pair(&w, &r);
        pthread_mutex_lock(&g_klock);
        kernel_signal(e);
        pthread_mutex_unlock(&g_klock);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 2: break;  // GetResult: success
    case 3: break;  // Cancel
    case 4: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // IsProcessing
    default: LOG("[ntc] IEnsureNetworkClockAvailabilityService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
SERVICE("ntc", ntc) {
    (void)s;
    if (req->cmd == 0) {  // OpenEnsureNetworkClockAvailabilityService
        LOG("[ntc] network clock: available\n");
        ipc_return_object(resp, service_new("ntc:IEnsureNetworkClockAvailabilityService", ntc_ensure, NULL));
        return;
    }
    LOG("[ntc] cmd %u (ok)\n", req->cmd);
    resp_size(resp, 0x10);
}

#define SSL_NO_TLS MAKE_RESULT(123, 4)
SERVICE("ssl:s", ssl_s) {
    (void)s;
    if (req->cmd == 0) {  // CreateContext
        LOG("[ssl] CreateContext: refused (no TLS here)\n");
        resp->result = SSL_NO_TLS;
        return;
    }
    LOG("[ssl] cmd %u (ok)\n", req->cmd);
    resp_size(resp, 0x10);
}
SERVICE("ssl", ssl_u) { ssl_s(s, req, resp); }

// sfdnsres: with Mystery Gift on, names don't resolve (nothing here reaches the internet): the lookups fail with an
// error code instead of the logging stub's zeroed "success" (the game read a result that wasn't there).
SERVICE("sfdnsres", sfdnsres) {
    (void)s;
    switch (req->cmd) {
    case 2: case 3: case 6: case 8: case 10: case 12: case 14:  // GetHostByName / GetHostByAddr / GetAddrInfo (+ WithOptions)
        LOG("[dns] lookup (cmd %u): no network, fails\n", req->cmd);
        OUT(int32_t, 0) = 8;   // EAI_NONAME / HOST_NOT_FOUND
        OUT(uint32_t, 4) = 0;  // errno
        OUT(uint32_t, 8) = 0;  // nothing written
        resp_size(resp, 0x10);
        break;
    default:
        LOG("[dns] cmd %u (ok)\n", req->cmd);
        resp_size(resp, 0x10);
    }
}

// prepo (play reports): logged, not sent. A report is an event name (an x buffer) and a MessagePack map of values
// (an a buffer): printed as text with the bytes that aren't, so Mystery Gift's reports (its result codes) are readable.
static void prepo_log(IpcReq *req) {
    char line[1024];
    size_t n = 0;
    for (int k = 0; k < req->nx + req->na && n < sizeof line - 8; k++) {
        IpcBuf *b = k < req->nx ? &req->x[k] : &req->a[k - req->nx];
        const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr;
        n += (size_t)snprintf(line + n, sizeof line - n, "%s[", k ? " " : "");
        for (uint64_t i = 0; i < b->size && i < 400 && n < sizeof line - 8; i++)
            n += (size_t)(p[i] >= 0x20 && p[i] < 0x7f ? snprintf(line + n, sizeof line - n, "%c", p[i])
                                                       : snprintf(line + n, sizeof line - n, "\\x%02x", p[i]));
        n += (size_t)snprintf(line + n, sizeof line - n, "]");
    }
    LOG("[prepo] report (cmd %u): %s\n", req->cmd, line);
    extern void mg_trace_mark(const char *what);
    mg_trace_mark(line);
}
SERVICE("prepo:u", prepo_u) { (void)s; prepo_log(req); resp_size(resp, 0x10); }
SERVICE("prepo:a", prepo_a) { (void)s; prepo_log(req); resp_size(resp, 0x10); }
