// Code mods: an on mod's exefs/subsdk0-9 (an exlaunch / Hakkun program, as Atmosphère loads it next to the game) run
// with the game. Its NSO is loaded and linked like the game's own modules (loader.c), its code is its own library
// (runtime/modlib.h, built by runtime/gamebuild.c modbuild_run), its start code runs after the SDK's (main.c
// rtld_init_sdk), and what it writes into the game's code becomes hooks (runtime/hle/codehooks.c).
// Off unless turned on for the mod on the start screen's Mods page (settings mods_code_on; LARECOMP_CODE_MODS=1: all).
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "gamebuild.h"
#include "gamelib.h"
#include "modlib.h"
#include "rt.h"
#include "services/mod_layout.h"
#include "settings.h"

typedef struct { char name[256], nso[1100]; uint64_t base, size, host_lo, host_hi; const ModLibTables *t; } CodeMod;
static CodeMod mods[MAX_CODE_MODS];
static int nmods;

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static bool find_subsdk(const char *exefs, char *out, size_t n) {
    DIR *d = opendir(exefs);
    bool found = false;
    for (struct dirent *e; d && !found && (e = readdir(d));)
        if (mod_is_subsdk(e->d_name)) found = snprintf(out, n, "%s/%s", exefs, e->d_name) < (int)n;
    if (d) closedir(d);
    return found;
}

static const ModLibTables *open_modlib(const char *path) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    ModLibEntry entry = h ? (ModLibEntry)(void *)GetProcAddress(h, MODLIB_ENTRY) : NULL;
    if (!h) LOG("[mods] can't load %s (error %lu)\n", path, GetLastError());
#else
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    ModLibEntry entry = h ? (ModLibEntry)dlsym(h, MODLIB_ENTRY) : NULL;
    if (!h) LOG("[mods] can't load %s (%s)\n", path, dlerror());
#endif
    const ModLibTables *t = entry ? entry() : NULL;
    return t && t->abi == MODLIB_ABI ? t : NULL;
}

// The on mods' .pchtxt patches outside main's .text, written into the loaded main: the lifted code has the .text ones
// (gen.py --patch), but the game reads its .rodata / .data from guest memory, which loader.c loaded unpatched.
static void patch_main_data(const char *game_dir) {
    static char files[MAX_PATCH_FILES][1100];
    int n = gamelib_patch_files(game_dir, files, MAX_PATCH_FILES);
    Module *m = &g_modules[0];
    for (int i = 0; i < n; i++) {
        FILE *f = fopen(files[i], "rb");
        long len = f && !fseek(f, 0, SEEK_END) ? ftell(f) : -1;
        char *text = len >= 0 ? calloc(1, (size_t)len + 1) : NULL;  // (the whole file: a patch past a cut is a patch lost)
        if (text && (fseek(f, 0, SEEK_SET) || fread(text, 1, (size_t)len, f) != (size_t)len)) free(text), text = NULL;
        if (f) fclose(f);
        // (the image's end as tools/nso.py has it, .data + .bss to 4 KB - not the module's 16 KB: the same patches out of
        // bounds for both, or gen.py skips a file this writes)
        uint64_t end = (m->data_off + m->data_size + 0xFFF) & ~0xFFFull;
        int w = text ? pchtxt_apply(text, (uint8_t *)(uintptr_t)m->base, end, m->text_off + m->text_size) : -1;
        LOG("[mods] %s: %d data bytes%s\n", files[i], w < 0 ? 0 : w, w < 0 ? " (not a byte patch: skipped, as gen.py does)" : "");
        free(text);
    }
}

// The on code mods with a program (exefs/subsdk0-9), in load order (by name), each with the guest address its code
// goes to. The boot loads these, and the start screen builds their libraries first (codemods_unbuilt / _build): both
// from here, so they agree on the bases.
typedef struct { char name[256], nso[1100]; uint64_t base, size; } ModPlan;
static int plan_mods(const char *game_dir, ModPlan *out, bool log) {
    DIR *d = opendir("mods");
    if (!d) return 0;
    char names[15][256];
    int n = 0, k = 0;
    for (struct dirent *e; (e = readdir(d)) && n < 15;)
        if (e->d_name[0] != '.' && e->d_name[0] != '_' && settings_mod_code_on(e->d_name))
            snprintf(names[n++], sizeof *names, "%s", e->d_name);
    closedir(d);
    qsort(names, n, sizeof *names, name_cmp);
    char mp[1100];
    snprintf(mp, sizeof mp, "%s/exefs/main", game_dir);
    uint64_t main_size = nso_image_size(mp), sizes[MAX_CODE_MODS];
    for (int i = 0; i < n && k < MAX_CODE_MODS; i++) {
        ModPlan *p = &out[k];
        char dir[1100], exefs[1100];
        snprintf(dir, sizeof dir, "mods/%s", names[i]);
        if (!mod_exefs_dir(dir, exefs, sizeof exefs) || !find_subsdk(exefs, p->nso, sizeof p->nso)) continue;
        snprintf(p->name, sizeof p->name, "%s", names[i]);
        sizes[k] = p->size = nso_image_size(p->nso);
        p->base = p->size ? mod_base(main_size, k, sizes) : 0;
        if (!p->base) {
            if (log)
                LOG("[mods] %s: no room for its code (%llu KB; room after main: %llu KB)\n", names[i],
                    (unsigned long long)(p->size >> 10), (unsigned long long)((SDK_BASE - MAIN_BASE - main_size) >> 10));
            continue;
        }
        k++;
    }
    return k;
}

int codemods_unbuilt(const char *game_dir) {
    ModPlan p[MAX_CODE_MODS];
    int n = plan_mods(game_dir, p, false), k = 0;
    for (int i = 0; i < n; i++) k += !modbuild_ready(p[i].nso, p[i].base);
    return k;
}

// (the bar over all the mods being built: each its share)
static const GameBuildOpts *build_o;
static double build_lo, build_span;
static void build_progress(double f, const char *step) { build_o->progress(build_lo + build_span * f, step); }

bool codemods_build(const char *game_dir, const GameBuildOpts *o, char *why, size_t why_size) {
    ModPlan p[MAX_CODE_MODS];
    int n = plan_mods(game_dir, p, true), todo[MAX_CODE_MODS], k = 0;
    for (int i = 0; i < n; i++)
        if (!modbuild_ready(p[i].nso, p[i].base)) todo[k++] = i;
    GameBuildOpts mine = {o && o->progress ? build_progress : NULL, o ? o->jobs : 0, o ? o->cancelled : NULL};
    build_o = o;
    for (int j = 0; j < k; j++) {
        ModPlan *m = &p[todo[j]];
        char dll[1100], w[512] = "";
        build_lo = (double)j / k, build_span = 1.0 / k;
        if (!modbuild_run(game_dir, m->nso, m->base, dll, sizeof dll, &mine, w, sizeof w)) {
            if (!strcmp(w, "cancelled")) snprintf(why, why_size, "cancelled");
            else snprintf(why, why_size, "%s: %s", m->name, w);
            return false;
        }
    }
    return true;
}

void codemods_load(const char *game_dir) {
    nmods = 0;
    patch_main_data(game_dir);
    ModPlan p[MAX_CODE_MODS];
    int n = plan_mods(game_dir, p, true);
    for (int i = 0; i < n && nmods < MAX_CODE_MODS; i++) {
        CodeMod *cm = &mods[nmods];
        char dll[1100], why[512] = "";
        snprintf(cm->nso, sizeof cm->nso, "%s", p[i].nso);
        snprintf(cm->name, sizeof cm->name, "mod:%s", p[i].name);
        cm->size = p[i].size, cm->base = p[i].base;
        // (built on the start screen first, with its progress bar; here only when it wasn't: --play, a test run)
        if (!modbuild_run(game_dir, cm->nso, cm->base, dll, sizeof dll, NULL, why, sizeof why)) {
            LOG("[mods] %s: its code wasn't built: %s\n", p[i].name, why);
            continue;
        }
        cm->t = open_modlib(dll);
        if (!cm->t || cm->t->base != cm->base) {
            LOG("[mods] %s: %s isn't its library for %llx\n", p[i].name, dll, (unsigned long long)cm->base);
            continue;
        }
        if (!loader_load_mod(cm->nso, cm->base, cm->name)) break;
        LOG("[mods] %s: code at %llx (%llu KB, %llu functions)\n", p[i].name, (unsigned long long)cm->base,
            (unsigned long long)(cm->size >> 10), (unsigned long long)*cm->t->count);
        cm->host_lo = UINT64_MAX, cm->host_hi = 0;  // (its library's code, roughly: its first function to past its last)
        for (uint64_t k = 0; k < *cm->t->count; k++) {
            uint64_t h = (uint64_t)(uintptr_t)cm->t->fns[k];
            if (h < cm->host_lo) cm->host_lo = h;
            if (h > cm->host_hi) cm->host_hi = h;
        }
        cm->host_hi += 0x10000;
        nmods++;
        rt_mods_on++;  // (the crash dialog: "you have mods on")
    }
}

// The code mod whose library holds host address pc (the crash dialog names it), NULL: none.
const char *codemods_at(uint64_t pc) {
    for (int i = 0; i < nmods; i++)
        if (pc >= mods[i].host_lo && pc < mods[i].host_hi) return mods[i].name + 4;  // (past "mod:")
    return NULL;
}

void codemods_tables(void) {
    for (int i = 0; i < nmods; i++) dispatch_add_table(mods[i].t->addrs, mods[i].t->fns, *mods[i].t->count);
}
