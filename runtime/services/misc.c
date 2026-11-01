// Small services: parental control, add-on content, and other mostly-stubbed interfaces.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../ipc.h"
#include "../platform.h"

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

static void nifm_request(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:  // GetRequestState
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    case 1:  // GetResult
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
        resp->result = NIFM_NO_NETWORK;
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
// their indices (title id - base). None: no DLC.
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
    return n;
}

SERVICE("aoc:u", aoc_u) {
    (void)s;
    uint64_t ids[64];
    int n = aoc_ids(ids, 64);
    uint64_t base = n ? ids[0] & ~0xFFFull : 0x01006BB00C6F1000ull;
    static int logged;
    if (!logged++) LOG("[aoc] %d add-on content(s) installed\n", n);
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
