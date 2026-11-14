#include <dirent.h>
// Mystery Gift: runtime/services/bcat.c driven the way the SDK's nn::bcat drives it (sync request, storage, directory
// and file services) over a made-up delivery cache, and runtime/services/mystery_gift_cards.c's reading of GitHub's
// folder list. The cards here are synthetic (0x2C8 bytes of zeros with a card id), not game data.
//   clang -std=gnu11 -O1 -Iruntime tools/test_bcat.c -o build/test-bcat -lpthread && build/test-bcat
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../runtime/hle.h"
#include "../runtime/ipc.h"
#include "../runtime/paths.h"
#include "../runtime/platform.h"
#include "../runtime/settings.h"

// ---- the runtime pieces the two files use
pthread_mutex_t g_klock = PTHREAD_MUTEX_INITIALIZER;
static KEvent g_ev;
KEvent *kernel_create_event_pair(uint32_t *w, uint32_t *r) { *w = 1; *r = 2; return &g_ev; }
void kernel_signal(KObject *o) { o->signaled = true; }
Service *service_new(const char *name, ServiceHandler h, void *state) {
    Service *s = calloc(1, sizeof *s); s->name = name; s->handler = h; s->state = state; return s;
}
void ipc_return_object(IpcResp *resp, Service *s) { resp->objects[resp->nobjects++] = s; }
void mg_trace_mark(const char *what) { (void)what; }
Settings g_settings;
static char g_root[1024];
const char *rt_data_root(void) { return g_root; }
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n) { (void)name; (void)dev_rel; (void)out; (void)n; return false; }
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) { (void)argv; (void)log_path; (void)append_log; return 0; }
int plat_proc_wait(PlatProc p, bool block) { (void)p; (void)block; return 1; }
int plat_rename(const char *from, const char *to) { return rename(from, to); }

#include "../runtime/services/bcat.c"
#include "../runtime/services/mystery_gift_cards.c"

static IpcResp call(Service *s, uint32_t cmd, const void *in, uint32_t in_size, void *out, uint64_t out_size) {
    IpcReq req = {0};
    IpcResp resp = {0};
    req.cmd = cmd, req.data = in, req.data_size = in_size;
    if (out) req.nb = 1, req.b[0].addr = (uint64_t)(uintptr_t)out, req.b[0].size = out_size;
    s->handler(s, &req, &resp);
    return resp;
}

static void put_cards(const char *path, const int *ids, int n) {
    FILE *f = fopen(path, "wb");
    assert(f);
    for (int i = 0; i < n; i++) {
        uint8_t card[0x2C8] = {0};
        card[8] = ids[i] & 0xFF, card[9] = ids[i] >> 8;
        assert(fwrite(card, 1, sizeof card, f) == sizeof card);
    }
    fclose(f);
}

static void bcat_service_test(void) {
    mkdir("mystery_gift", 0755), mkdir("mystery_gift/bcat", 0755), mkdir("mystery_gift/bcat/normal", 0755);
    int ids[] = {1613, 103};
    put_cards("mystery_gift/bcat/normal/distribution", ids, 2);

    Service root = {.name = "bcat:u", .handler = bcat_u};
    // a sync request: done at once, its event signalled
    Service *bcat = call(&root, 0, NULL, 0, NULL, 0).objects[0];
    Service *prog = call(bcat, 10100, NULL, 0, NULL, 0).objects[0];
    assert(call(prog, 0, NULL, 0, NULL, 0).ncopy == 1 && g_ev.signaled);
    int32_t impl[0x80] = {0};
    call(prog, 1, NULL, 0, impl, sizeof impl);
    assert(impl[0] == 9);  // Done

    Service *st = call(&root, 1, NULL, 0, NULL, 0).objects[0];
    char dirs[4][0x20] = {{0}};
    IpcResp r = call(st, 10, NULL, 0, dirs, sizeof dirs);
    assert(r.result == 0 && *(int32_t *)r.data == 1 && !strcmp(dirs[0], "normal"));

    Service *dir = call(st, 1, NULL, 0, NULL, 0).objects[0];
    char name[0x40] = "normal";
    assert(call(dir, 0, name, 0x20, NULL, 0).result == 0);
    uint8_t ents[0x38 * 4];
    r = call(dir, 1, NULL, 0, ents, sizeof ents);
    assert(*(int32_t *)r.data == 1 && !strcmp((char *)ents, "distribution") && *(int64_t *)(ents + 0x20) == 0x2C8 * 2);
    memset(name, 0, sizeof name), strcpy(name, "missing");
    assert(call(dir, 0, name, 0x20, NULL, 0).result != 0);  // (no _default: not found)

    Service *f = call(st, 0, NULL, 0, NULL, 0).objects[0];
    memset(name, 0, sizeof name), strcpy(name, "normal"), strcpy(name + 0x20, "../x");
    assert(call(f, 0, name, 0x40, NULL, 0).result != 0);  // nothing outside the folder
    memset(name, 0, sizeof name), strcpy(name, "../../x"), strcpy(name + 0x20, "distribution");
    assert(call(f, 0, name, 0x40, NULL, 0).result != 0);
    memset(name, 0, sizeof name), strcpy(name, "normal"), strcpy(name + 0x20, "distribution");
    assert(call(f, 0, name, 0x40, NULL, 0).result == 0);
    assert(*(int64_t *)call(f, 2, NULL, 0, NULL, 0).data == 0x2C8 * 2);
    static uint8_t buf[0x1000];
    int64_t off = 0x2C8;  // the second card
    r = call(f, 1, &off, 8, buf, sizeof buf);
    assert(*(uint64_t *)r.data == 0x2C8 && (buf[8] | buf[9] << 8) == 103);

    // _default: a directory or file the game asks for that isn't there
    mkdir("mystery_gift/bcat/_default", 0755);
    put_cards("mystery_gift/bcat/_default/_default", ids, 1);
    memset(name, 0, sizeof name), strcpy(name, "other"), strcpy(name + 0x20, "file");
    assert(call(f, 0, name, 0x40, NULL, 0).result == 0 && *(int64_t *)call(f, 2, NULL, 0, NULL, 0).data == 0x2C8);
    printf("bcat service: ok\n");
}

static void gallery_list_test(void) {
    // GitHub's JSON for a folder (trimmed): names, a sub-folder, one card id with several files
    const char *json =
        "[{\"name\":\"0101 ZA - Pok\xc3\xa9" "Center Audino.wa9\",\"size\":712},"
        "{\"name\": \"1613 ZA - Kalos Shiny Xerneas.wa9\",\"size\":712},"
        "{\"name\":\"1615 ZA - (Trainer) Spewpa (Ocean Pattern).wa9\"},{\"name\":\"1615 ZA - (Trainer) Spewpa (Sun Pattern).wa9\"},"
        "{\"name\":\"HOME Simulated\",\"type\":\"dir\"},{\"name\":\"../../evil.wa9\"},{\"name\":\"0000 x.wa9\"},"
        "{\"name\":\"0105 ZA - Pok\xc3\xa9" "Center Audino.wa9\"}]";  // (Audino run again: only 0105 listed)
    static MgCard cards[MG_MAX];
    int n = mg_parse_list(json, cards, MG_MAX);
    assert(n == 3 && cards[0].id == 1613 && cards[1].id == 1615 && cards[1].nfiles == 2 && cards[2].id == 105);
    mkdir("cache", 0755), mkdir("cache/mystery_gift", 0755);
    const char *a = mg_pick(&cards[1]), *b = mg_pick(&cards[1]);  // the same pick every time
    assert(a == b && strstr(a, "Spewpa"));
    assert(!mg_name_ok("../x.wa9") && !mg_name_ok("a/b.wa9") && !mg_name_ok("x.bin"));
    char enc[600];
    mg_url_name("0102 ZA - \xe3\x83\x9d Slowpoke.wa9", enc, sizeof enc);
    assert(!strcmp(enc, "0102%20ZA%20-%20%E3%83%9D%20Slowpoke.wa9"));
    printf("gallery list: ok\n");
}

static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *e;
    char sub[600];
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            rm_tree(sub);
        }
    closedir(d);
    rmdir(path);
}

int main(void) {
    snprintf(g_root, sizeof g_root, "build/test-bcat-data");
    mkdir("build", 0755);
    rm_tree(g_root);  // a clean folder every run
    mkdir(g_root, 0755);
    assert(chdir(g_root) == 0);
    assert(getcwd(g_root, sizeof g_root));
    bcat_service_test();
    gallery_list_test();
    return 0;
}
