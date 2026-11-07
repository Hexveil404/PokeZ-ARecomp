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

// ------------------------------------------------------------------ friend:u / friend:a
// Pokémon Legends Arceus opens the friend service at boot (Z-A doesn't): an offline Switch with no friends. The
// logging stub returned no IFriendService, and the SDK read through the missing object (a crash at boot).
static void friend_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = new_event_handle(); break;  // GetCompletionEvent
    case 10100: OUT(int32_t, 0) = 0; resp_size(resp, 4); break;            // GetFriendCount: none
    case 10101: case 10102: case 10110: case 20101: case 20102:            // friend lists, counts: empty
        OUT(int32_t, 0) = 0; resp_size(resp, 4); break;
    default: LOG("[friend] IFriendService cmd %u (ok)\n", req->cmd); resp_size(resp, 0x10);
    }
}
static void friend_notification(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) resp->copy_handles[resp->ncopy++] = new_event_handle();  // GetEvent (never signalled)
    else LOG("[friend] INotificationService cmd %u (ok)\n", req->cmd);
}
static void friend_root(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("IFriendService", friend_service, NULL));
    else if (req->cmd == 1) ipc_return_object(resp, service_new("INotificationService", friend_notification, NULL));
    else LOG("[friend] cmd %u (ok)\n", req->cmd);
}
SERVICE("friend:u", friend_u) { friend_root(s, req, resp); }
SERVICE("friend:a", friend_a) { friend_root(s, req, resp); }
SERVICE("friend:v", friend_v) { friend_root(s, req, resp); }

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
