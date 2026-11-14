// BCAT (bcat:u): the Switch's data delivery service, served from a folder. Pokémon Z-A reads Mystery Gift's
// "Get via Internet" cards (and, after a code, the card the code names) from its delivery cache: a file there is the
// cards one after another, 0x2C8 bytes each (WA9, the parser at main+0x2db7350 refuses any other size).
//
// The delivery cache is mystery_gift/bcat/<directory>/<file> in the data folder, read as it is. A directory or file
// that isn't there is looked for in mystery_gift/bcat/_default/ (a file: the same name, else _default/_default), for
// when the names the game asks for are not known yet: the log line says which was served. Nothing comes from
// Nintendo: the folder is what the player (or the start screen's Mystery Gift panel) put there. Every directory and
// file the game asks for is logged ("[bcat]"), found or not, so the names the game uses show up in lasrecomp.log.
//
// Interfaces (switchbrew "BCAT services"): IServiceCreator -> IBcatService (sync requests, answered as done at once:
// there is nothing to download) and IDeliveryCacheStorageService -> IDeliveryCacheFileService /
// IDeliveryCacheDirectoryService. Names are 0x20-byte, NUL-padded strings.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../hle.h"
#include "../ipc.h"

void mg_trace_mark(const char *what);  // (dispatch.c: the Mystery Gift trace)
void mystery_gift_cards_start(void), mystery_gift_cards_wait(void);  // (mystery_gift_cards.c: today's cards)

#define BCAT_ROOT "mystery_gift/bcat"
#define BCAT_NAME 0x20
#define BCAT_MAX_FILES 100   // nn::bcat: files in one directory
#define BCAT_MAX_DIRS 100

#define BCAT_ERR(desc) MAKE_RESULT(122, desc)
#define BCAT_NOT_FOUND BCAT_ERR(2)
#define BCAT_NOT_OPEN BCAT_ERR(6)
#define BCAT_INVALID BCAT_ERR(1)

// A name from the game: printable, no path separators or "..", at most 0x1F characters.
static bool bcat_name(const uint8_t *raw, char out[BCAT_NAME + 1]) {
    memcpy(out, raw, BCAT_NAME);
    out[BCAT_NAME] = 0;
    if (!out[0] || !strcmp(out, ".") || !strcmp(out, "..")) return false;
    for (const char *p = out; *p; p++)
        if (*p < 0x20 || *p > 0x7E || *p == '/' || *p == '\\' || *p == ':') return false;
    return true;
}

static bool bcat_is_dir(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

// ------------------------------------------------------------------ IDeliveryCacheFileService
typedef struct {
    FILE *f;
    uint64_t size;
    char dir[BCAT_NAME + 1], file[BCAT_NAME + 1];
} BcatFile;

static void bcat_file_close(BcatFile *bf) {
    if (bf->f) fclose(bf->f);
    bf->f = NULL;
    bf->size = 0;
}

static void bcat_file_free(Service *s) {
    if (s->state) bcat_file_close(s->state);
    free(s->state);
    s->state = NULL;
}

static void bcat_file_service(Service *s, IpcReq *req, IpcResp *resp) {
    BcatFile *bf = s->state;
    switch (req->cmd) {
    case 0: {  // Open(DirectoryName, FileName)
        bcat_file_close(bf);
        if (req->data_size < 2 * BCAT_NAME || !bcat_name(req->data, bf->dir) ||
            !bcat_name(req->data + BCAT_NAME, bf->file)) {
            LOG("[bcat] file open: bad name\n");
            resp->result = BCAT_INVALID;
            break;
        }
        char path[256];
        const char *via = "";
        snprintf(path, sizeof path, BCAT_ROOT "/%s/%s", bf->dir, bf->file);
        bf->f = fopen(path, "rb");
        if (!bf->f) {
            snprintf(path, sizeof path, BCAT_ROOT "/_default/%s", bf->file);
            if ((bf->f = fopen(path, "rb"))) via = " via _default/";
        }
        if (!bf->f && (bf->f = fopen(BCAT_ROOT "/_default/_default", "rb"))) via = " via _default/_default";
        if (bf->f) {
            fseek(bf->f, 0, SEEK_END);
            bf->size = (uint64_t)ftell(bf->f);
            fseek(bf->f, 0, SEEK_SET);
        }
        LOG("[bcat] file open %s/%s: %s%s (%llu bytes)\n", bf->dir, bf->file, bf->f ? "found" : "missing", via,
            (unsigned long long)bf->size);
        mg_trace_mark("bcat file open");
        if (!bf->f) resp->result = BCAT_NOT_FOUND;
        break;
    }
    case 1: {  // Read(s64 offset, out buffer) -> u64 bytes read
        IpcBuf *b = out_buf(req, 0);
        if (!bf->f) { resp->result = BCAT_NOT_OPEN; break; }
        int64_t off = IN(int64_t, 0);
        uint64_t n = 0;
        if (b && off >= 0 && (uint64_t)off < bf->size) {
            uint64_t want = bf->size - (uint64_t)off;
            if (want > b->size) want = b->size;
            fseek(bf->f, (long)off, SEEK_SET);
            n = fread((void *)(uintptr_t)b->addr, 1, (size_t)want, bf->f);
        }
        OUT(uint64_t, 0) = n;
        resp_size(resp, 8);
        break;
    }
    case 2:  // GetSize -> s64
        if (!bf->f) { resp->result = BCAT_NOT_OPEN; break; }
        OUT(int64_t, 0) = (int64_t)bf->size;
        resp_size(resp, 8);
        break;
    case 3:  // GetDigest -> 0x10 bytes (the game's loader doesn't check it)
        if (!bf->f) { resp->result = BCAT_NOT_OPEN; break; }
        memset(&OUT(uint8_t, 0), 0, 0x10);
        resp_size(resp, 0x10);
        break;
    default:
        LOG("[bcat] IDeliveryCacheFileService cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ IDeliveryCacheDirectoryService
typedef struct {
    bool open;
    char dir[BCAT_NAME + 1];
    char path[256];  // the folder served for it (its own, or _default)
} BcatDir;

// The directory's files, sorted (the host's order is not the game's business). Returns the count.
static int bcat_list(const char *path, char names[][BCAT_NAME + 1], uint64_t *sizes, int max) {
    DIR *d = opendir(path);
    int n = 0;
    for (struct dirent *e; d && n < max && (e = readdir(d));) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > BCAT_NAME - 1) continue;
        char full[512];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (stat(full, &st) || !S_ISREG(st.st_mode)) continue;
        int i = n++;
        while (i > 0 && strcmp(names[i - 1], e->d_name) > 0) {
            memcpy(names[i], names[i - 1], BCAT_NAME + 1);
            if (sizes) sizes[i] = sizes[i - 1];
            i--;
        }
        snprintf(names[i], BCAT_NAME + 1, "%s", e->d_name);
        if (sizes) sizes[i] = (uint64_t)st.st_size;
    }
    if (d) closedir(d);
    return n;
}

static void bcat_dir_service(Service *s, IpcReq *req, IpcResp *resp) {
    BcatDir *bd = s->state;
    char names[BCAT_MAX_FILES][BCAT_NAME + 1];
    uint64_t sizes[BCAT_MAX_FILES];
    switch (req->cmd) {
    case 0:  // Open(DirectoryName)
        bd->open = false;
        if (req->data_size < BCAT_NAME || !bcat_name(req->data, bd->dir)) { resp->result = BCAT_INVALID; break; }
        snprintf(bd->path, sizeof bd->path, BCAT_ROOT "/%s", bd->dir);
        bd->open = bcat_is_dir(bd->path);
        if (!bd->open && bcat_is_dir(BCAT_ROOT "/_default")) {
            snprintf(bd->path, sizeof bd->path, BCAT_ROOT "/_default");
            bd->open = true;
            LOG("[bcat] directory open %s: missing, served from _default\n", bd->dir);
        } else {
            LOG("[bcat] directory open %s: %s\n", bd->dir, bd->open ? "found" : "missing");
        }
        if (!bd->open) resp->result = BCAT_NOT_FOUND;
        break;
    case 1: {  // Read(out DeliveryCacheDirectoryEntry[]) -> s32 count; an entry: name[0x20], s64 size, digest[0x10]
        if (!bd->open) { resp->result = BCAT_NOT_OPEN; break; }
        IpcBuf *b = out_buf(req, 0);
        int n = bcat_list(bd->path, names, sizes, BCAT_MAX_FILES), cap = b ? (int)(b->size / 0x38) : 0;
        if (n > cap) n = cap;
        for (int i = 0; i < n; i++) {
            uint8_t *e = (uint8_t *)(uintptr_t)(b->addr + (uint64_t)i * 0x38);
            memset(e, 0, 0x38);
            memcpy(e, names[i], strlen(names[i]));
            memcpy(e + 0x20, &sizes[i], 8);
        }
        LOG("[bcat] directory read %s: %d file(s)\n", bd->dir, n);
        OUT(int32_t, 0) = n;
        resp_size(resp, 4);
        break;
    }
    case 2:  // GetCount -> s32
        if (!bd->open) { resp->result = BCAT_NOT_OPEN; break; }
        OUT(int32_t, 0) = bcat_list(bd->path, names, NULL, BCAT_MAX_FILES);
        resp_size(resp, 4);
        break;
    default:
        LOG("[bcat] IDeliveryCacheDirectoryService cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ IDeliveryCacheStorageService
static void bcat_storage(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // CreateFileService
        Service *f = service_new("bcat:IDeliveryCacheFileService", bcat_file_service, calloc(1, sizeof(BcatFile)));
        f->on_close = bcat_file_free;
        ipc_return_object(resp, f);
        break;
    }
    case 1:  // CreateDirectoryService
        ipc_return_object(resp, service_new("bcat:IDeliveryCacheDirectoryService", bcat_dir_service,
                                            calloc(1, sizeof(BcatDir))));
        break;
    case 10: {  // EnumerateDeliveryCacheDirectory(out DirectoryName[]) -> s32 count
        IpcBuf *b = out_buf(req, 0);
        DIR *d = opendir(BCAT_ROOT);
        int n = 0, cap = b ? (int)(b->size / BCAT_NAME) : 0;
        for (struct dirent *e; d && n < cap && n < BCAT_MAX_DIRS && (e = readdir(d));) {
            char path[512];
            if (e->d_name[0] == '.' || strlen(e->d_name) > BCAT_NAME - 1) continue;
            snprintf(path, sizeof path, BCAT_ROOT "/%s", e->d_name);
            if (!bcat_is_dir(path)) continue;
            uint8_t *o = (uint8_t *)(uintptr_t)(b->addr + (uint64_t)n * BCAT_NAME);
            memset(o, 0, BCAT_NAME);
            memcpy(o, e->d_name, strlen(e->d_name));
            LOG("[bcat] directory %s\n", e->d_name);
            n++;
        }
        if (d) closedir(d);
        LOG("[bcat] enumerate: %d director%s\n", n, n == 1 ? "y" : "ies");
        OUT(int32_t, 0) = n;
        resp_size(resp, 4);
        break;
    }
    default:
        LOG("[bcat] IDeliveryCacheStorageService cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ IBcatService
// A sync request: nothing to download, so its progress reads done (status 9, result 0) from the start and its event
// is signalled. DeliveryCacheProgressImpl (0x200 bytes): s32 status, Result result, directory and file names, the
// byte counts.
static void bcat_progress(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: {  // GetEvent
        uint32_t w, r;
        KEvent *e = kernel_create_event_pair(&w, &r);
        pthread_mutex_lock(&g_klock);
        kernel_signal(e);
        pthread_mutex_unlock(&g_klock);
        resp->copy_handles[resp->ncopy++] = r;
        break;
    }
    case 1: {  // GetImpl(out 0x200)
        IpcBuf *b = out_buf(req, 0);
        if (b && b->size >= 8) {
            memset((void *)(uintptr_t)b->addr, 0, (size_t)b->size);
            *(int32_t *)(uintptr_t)b->addr = 9;  // Done
        }
        break;
    }
    default:
        LOG("[bcat] IDeliveryCacheProgressService cmd %u (ok)\n", req->cmd);
    }
}

static void bcat_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 10100:  // RequestSyncDeliveryCache
    case 10101:  // RequestSyncDeliveryCacheWithDirectoryName
        mystery_gift_cards_start();  // (turned on since the game started: F1 > Online)
        mystery_gift_cards_wait();   // (Mystery Gift on: today's cards are in place first)
        LOG("[bcat] sync request (cmd %u): served from " BCAT_ROOT "\n", req->cmd);
        mg_trace_mark("bcat sync");
        ipc_return_object(resp, service_new("bcat:IDeliveryCacheProgressService", bcat_progress, NULL));
        break;
    default:
        LOG("[bcat] IBcatService cmd %u (ok)\n", req->cmd);
    }
}

// ------------------------------------------------------------------ bcat:u (IServiceCreator)
SERVICE("bcat:u", bcat_u) {
    (void)s;
    mystery_gift_cards_start();  // (once: the game opens bcat:u while it boots)
    switch (req->cmd) {
    case 0:  // CreateBcatService(pid)
        ipc_return_object(resp, service_new("bcat:IBcatService", bcat_service, NULL));
        break;
    case 1:  // CreateDeliveryCacheStorageService(pid)
    case 2:  // CreateDeliveryCacheStorageServiceWithApplicationId
        ipc_return_object(resp, service_new("bcat:IDeliveryCacheStorageService", bcat_storage, NULL));
        break;
    default:
        LOG("[bcat] IServiceCreator cmd %u (ok)\n", req->cmd);
    }
}
