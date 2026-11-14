// Mystery Gift's cards (g_settings.mystery_gift): every Pokémon Legends: Z-A wonder card in Project Pokémon's
// EventsGallery (Released/Gen 9/ZA/Wondercards), put where bcat.c serves them from - mystery_gift/bcat/normal/, the
// game's "Get via Internet" list - one file per card (card_<id>). Nothing to keep up to date here: a card the gallery
// adds is listed from the next start. Nothing of the game's is in this repository; the cards are downloaded on the
// player's machine, through the system's curl (as the start screen's update check), from GitHub's API (api.github.com:
// its raw file answers, one host for the list and the cards).
//
// The cards are kept in cache/mystery_gift/cards/ (by card id and a hash of the gallery's file name), so a card is
// downloaded once and the list still works offline (the last list fetched: cache/mystery_gift/gallery.json). Several
// files for one card id (Spewpa's 16 patterns): one is picked per install and kept (cache/mystery_gift/pick_<id>).
// Other files the player puts in mystery_gift/bcat/normal/ (cards back to back, 0x2C8 bytes each) are listed too.
//
// It runs once per start, on its own thread, from the first use of bcat:u (the game opens it while it boots); the
// game's sync request (Get via Internet) waits for it, up to 30 s. curl's messages: cache/mystery_gift/curl.log.
// LARECOMP_MG_OFFLINE=1: no downloads.
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../hle.h"
#include "../paths.h"
#include "../platform.h"
#include "../settings.h"

#define MG_GALLERY "https://api.github.com/repos/projectpokemon/EventsGallery/contents/Released/Gen%209/ZA/Wondercards"
#define MG_CARD 0x2C8
#define MG_MAX 256

typedef struct {
    int id;
    int nfiles;
    char files[16][200];  // the gallery's file names for this card id
} MgCard;

static pthread_mutex_t g_mg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mg_cond = PTHREAD_COND_INITIALIZER;
static int g_mg_state;  // 0 not started, 1 running, 2 done

static char *mg_path(const char *rel) {
    static char buf[4][1100];
    static int n;
    char *b = buf[n++ & 3];
    snprintf(b, sizeof buf[0], "%s/%s", rt_data_root(), rel);
    return b;
}

// url -> out, the whole file or nothing (a .part, renamed). raw: GitHub's API answers with the file itself.
static bool mg_curl(const char *url, const char *out, bool raw) {
    if (getenv("LARECOMP_MG_OFFLINE") && atoi(getenv("LARECOMP_MG_OFFLINE"))) return false;
#ifdef _WIN32
    const char *sr = getenv("SystemRoot");
    char curl[300];
    snprintf(curl, sizeof curl, "%s\\System32\\curl.exe", sr ? sr : "C:\\Windows");
#else
    const char *curl = "/usr/bin/curl";
#endif
    char part[1100];
    snprintf(part, sizeof part, "%s.part", out);
    const char *av[] = {curl, "-fsSL", "--max-time", "20", "-A", "ZARecomp-MysteryGift", "-H",
                        raw ? "Accept: application/vnd.github.raw" : "Accept: application/vnd.github+json",
                        "-o", part, url, NULL};
    PlatProc p = plat_spawn(av, mg_path("cache/mystery_gift/curl.log"), true);
    int rc = p ? plat_proc_wait(p, true) : -1;
    if (rc != 0) {
        LOG("[mystery-gift] download failed (curl %d): %s\n", rc, url);
        remove(part);
        return false;
    }
    remove(out);
    return plat_rename(part, out) == 0;
}

static char *mg_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = 0; if (len) *len = (size_t)n; }
    return b;
}

// file name -> URL path: UTF-8 bytes and anything but [A-Za-z0-9-_.~] percent-encoded
static void mg_url_name(const char *name, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 4 < n; p++)
        o += (size_t)((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                              strchr("-_.~", *p)
                          ? snprintf(out + o, n - o, "%c", *p)
                          : snprintf(out + o, n - o, "%%%02X", *p));
    out[o] = 0;
}

static bool mg_name_ok(const char *s) {  // a gallery file name: no path, ends in .wa9
    size_t n = strlen(s);
    return n > 4 && n < 200 && !strpbrk(s, "/\\:") && !strstr(s, "..") && !strcmp(s + n - 4, ".wa9");
}

static uint32_t mg_hash(const char *s) {  // FNV-1a: the cache's name for a gallery file
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static const char *mg_title(const char *name) {  // a gallery file's name without its id ("0101 ZA - X.wa9" -> "ZA - X.wa9")
    while (*name >= '0' && *name <= '9') name++;
    while (*name == ' ') name++;
    return name;
}

static bool mg_same_gift(const MgCard *a, const MgCard *b) {  // a file name shared, id aside
    for (int i = 0; i < a->nfiles; i++)
        for (int j = 0; j < b->nfiles; j++)
            if (!strcmp(mg_title(a->files[i]), mg_title(b->files[j]))) return true;
    return false;
}

// The gallery's list (GitHub's JSON for the folder): its .wa9 files, grouped by the card id their name starts with
static int mg_parse_list(const char *json, MgCard *cards, int max) {
    int n = 0;
    for (const char *p = json; p && (p = strstr(p, "\"name\":")); ) {
        p = strchr(p + 7, '"');
        const char *q = p ? strchr(p + 1, '"') : NULL;
        if (!q) break;
        char name[200];
        snprintf(name, sizeof name, "%.*s", (int)(q - p - 1), p + 1);
        p = q + 1;
        int id = atoi(name);
        if (id <= 0 || id > 0xFFFF || !mg_name_ok(name)) continue;
        int i = 0;
        while (i < n && cards[i].id != id) i++;
        if (i == n) {
            if (n == max) continue;
            memset(&cards[n], 0, sizeof cards[n]);
            cards[n++].id = id;
        }
        if (cards[i].nfiles < 16) snprintf(cards[i].files[cards[i].nfiles++], 200, "%s", name);
    }
    // the same gift run again under a new id ("0101 ZA - PokéCenter Audino" and "0105 ZA - PokéCenter Audino"): the
    // console only ever offers one of them, so only the newest (highest id) is listed
    int kept = 0;
    for (int i = 0; i < n; i++) {
        bool rerun = false;
        for (int j = 0; j < n && !rerun; j++)
            rerun = cards[j].id > cards[i].id && mg_same_gift(&cards[i], &cards[j]);
        if (!rerun) cards[kept++] = cards[i];
    }
    return kept;
}

// One of a card's files: the same one every time for this install
static const char *mg_pick(const MgCard *c) {
    if (c->nfiles == 1) return c->files[0];
    char keep[64];
    snprintf(keep, sizeof keep, "cache/mystery_gift/pick_%d", c->id);
    char *had = mg_read(mg_path(keep), NULL);
    if (had) {
        for (char *e = had + strlen(had); e > had && (e[-1] == '\n' || e[-1] == '\r'); ) *--e = 0;
        for (int i = 0; i < c->nfiles; i++)
            if (!strcmp(had, c->files[i])) { free(had); return c->files[i]; }
        free(had);
    }
    const char *pick = c->files[(unsigned)time(NULL) % (unsigned)c->nfiles];
    FILE *f = fopen(mg_path(keep), "w");
    if (f) { fprintf(f, "%s\n", pick); fclose(f); }
    return pick;
}

static bool mg_write(const char *rel, const void *data, size_t n) {  // whole or not at all
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.part", mg_path(rel));
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (ok) { remove(mg_path(rel)); ok = plat_rename(tmp, mg_path(rel)) == 0; }
    if (!ok) remove(tmp);
    return ok;
}

static void *mg_thread(void *arg) {
    (void)arg;
    const char *dirs[] = {"cache", "cache/mystery_gift", "cache/mystery_gift/cards", "mystery_gift", "mystery_gift/bcat",
                          "mystery_gift/bcat/normal"};
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) mkdir(mg_path(dirs[i]), 0755);

    bool fresh = mg_curl(MG_GALLERY, mg_path("cache/mystery_gift/gallery.json"), false);
    char *list = mg_read(mg_path("cache/mystery_gift/gallery.json"), NULL);
    static MgCard cards[MG_MAX];
    int n = list ? mg_parse_list(list, cards, MG_MAX) : 0;
    LOG("[mystery-gift] EventsGallery: %d card(s) (%s)\n", n, fresh ? "list fetched" : list ? "last list" : "no list");

    int served = 0;
    for (int i = 0; i < n; i++) {
        const char *name = mg_pick(&cards[i]);
        char rel[128], url[1100], enc[700];
        snprintf(rel, sizeof rel, "cache/mystery_gift/cards/%d-%08x.wa9", cards[i].id, mg_hash(name));
        size_t len = 0;
        char *card = mg_read(mg_path(rel), &len);
        if (!card || len != MG_CARD) {
            free(card);
            mg_url_name(name, enc, sizeof enc);
            snprintf(url, sizeof url, "%s/%s", MG_GALLERY, enc);
            card = mg_curl(url, mg_path(rel), true) ? mg_read(mg_path(rel), &len) : NULL;
        }
        if (!card || len != MG_CARD || ((card[8] & 0xFF) | (card[9] & 0xFF) << 8) != cards[i].id) {
            if (card) remove(mg_path(rel));  // (not a card: fetched again next time)
            LOG("[mystery-gift] card %d: %s\n", cards[i].id, card ? "not a card" : "not downloaded");
            free(card);
            continue;
        }
        snprintf(rel, sizeof rel, "mystery_gift/bcat/normal/card_%d", cards[i].id);
        if (mg_write(rel, card, MG_CARD)) served++;
        free(card);
    }
    // a card the gallery no longer lists: its file goes (only ours, card_<id>; the player's own files stay)
    DIR *d = list ? opendir(mg_path("mystery_gift/bcat/normal")) : NULL;
    for (struct dirent *e; d && (e = readdir(d));) {
        if (strncmp(e->d_name, "card_", 5)) continue;
        int id = atoi(e->d_name + 5);
        bool listed = false;
        for (int i = 0; i < n && !listed; i++) listed = cards[i].id == id;
        if (!listed) {
            char rel[300];
            snprintf(rel, sizeof rel, "mystery_gift/bcat/normal/%s", e->d_name);
            remove(mg_path(rel));
        }
    }
    if (d) closedir(d);
    LOG("[mystery-gift] %d card(s) listed in Get via Internet\n", served);
    free(list);
    pthread_mutex_lock(&g_mg_lock);
    g_mg_state = 2;
    pthread_cond_broadcast(&g_mg_cond);
    pthread_mutex_unlock(&g_mg_lock);
    return NULL;
}

void mystery_gift_cards_start(void) {  // (bcat.c: bcat:u's first use, and each sync request)
    if (!g_settings.mystery_gift) return;
    pthread_mutex_lock(&g_mg_lock);
    bool go = g_mg_state == 0;
    if (go) g_mg_state = 1;
    pthread_mutex_unlock(&g_mg_lock);
    if (!go) return;
    pthread_t th;
    if (pthread_create(&th, NULL, mg_thread, NULL) == 0) {
        pthread_detach(th);
    } else {
        pthread_mutex_lock(&g_mg_lock);
        g_mg_state = 2;
        pthread_mutex_unlock(&g_mg_lock);
    }
}

void mystery_gift_cards_wait(void) {  // (bcat.c: a sync request - up to 30 s)
    struct timespec until;
    timespec_get(&until, TIME_UTC);
    until.tv_sec += 30;
    pthread_mutex_lock(&g_mg_lock);
    while (g_mg_state == 1)
        if (pthread_cond_timedwait(&g_mg_cond, &g_mg_lock, &until)) break;
    pthread_mutex_unlock(&g_mg_lock);
}
