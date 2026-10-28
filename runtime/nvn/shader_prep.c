// Shader variant records, the recorder and first-visit counts (see shader_prep.h).
#include <dirent.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "nvn.h"
#include "shader_prep.h"
#include "../settings.h"

uint64_t shader_code_hash(const uint8_t *code, size_t n) {  // FNV-1a, 64-bit
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= code[i]; h *= 0x100000001B3ull; }
    return h;
}

uint64_t shader_stage_hash(const struct NvnShaderStage *s) {
    if (!s->code_addr || !s->const_offset) return 0;
    return shader_code_hash((const uint8_t *)(uintptr_t)s->code_addr, (size_t)s->const_offset + s->const_size);
}

// ------------------------------------------------------------------ stale-program detector (shader_prep.h)
static uint32_t fp_len(const struct NvnShaderStage *s) { return s->const_offset ? s->const_offset + s->const_size : 0; }

static void fp_sample(const struct NvnShaderStage *s, uint32_t len, uint32_t w[4]) {
    const uint32_t *code = (const uint32_t *)(uintptr_t)s->code_addr;
    uint32_t n = len / 4;  // (len >= 4: checked by the caller)
    w[0] = code[n > 0x20 ? 0x20 : 0];  // the first instruction word (after the NVN header and the program header)
    w[1] = code[n / 3];
    w[2] = code[2 * (n / 3)];
    w[3] = code[n - 1];                // the last word of the constants
}

void shader_fp_check(ShaderFp *fp, const struct NvnShaderStage *s, const char *stage) {
    if (fp->state == 2) return;
    uint32_t len = fp_len(s), w[4];
    if (fp->state == 0) {
        if (!s->code_addr || len < 4) { fp->state = 2; return; }
        fp->hash = shader_stage_hash(s);
        fp->len = len;
        fp_sample(s, len, fp->w);
        fp->state = 1;
        return;
    }
    if (len == fp->len && s->code_addr) {
        fp_sample(s, len, w);
        if (!memcmp(w, fp->w, sizeof w)) return;
    }
    fp->state = 2;  // (reported once per fingerprint: no per-draw rehash after this)
    static _Atomic int logged;
    if (atomic_fetch_add(&logged, 1) < 16)
        LOG("[shader] stale program: code %" PRIx64 " was %016" PRIx64 " now %016" PRIx64 " (stage %s, %#x -> %#x bytes)\n",
            s->code_addr, fp->hash, s->code_addr && len ? shader_stage_hash(s) : 0, stage, fp->len, len);
}

uint8_t shader_topo_class(uint32_t mode) { return mode == 0 ? 0 : mode == 1 || mode == 3 ? 1 : 2; }

// ------------------------------------------------------------------ text form
// G vs=<hash> fs=<hash> f=<flags> a=<attrib,...> s=<stride/divisor,...> c=<format[:view],...> d=<depth> n=<samples>
//   m=<channel mask> b=<blend enables> B=<blend state of each target, 16 hex digits,...> k=<a2c> t=<topology> p=<fill>
// C cs=<hash> l=<x,y,z>
void shader_rec_format(const ShaderRec *r, char *out, size_t n) {
    size_t o = 0;
#define PUT(...) do { if (o < n) o += (size_t)snprintf(out + o, n - o, __VA_ARGS__); } while (0)
    if (r->kind == 'C') {
        PUT("C cs=%016" PRIx64 " l=%u,%u,%u", r->vs, r->local[0], r->local[1], r->local[2]);
        return;
    }
    PUT("G vs=%016" PRIx64 " fs=%016" PRIx64 " f=%x a=", r->vs, r->fs, r->flags);
    for (int i = 0; i < r->nattrib; i++) PUT("%s%x", i ? "," : "", r->attrib[i]);
    PUT(" s=");
    for (int i = 0; i < r->nstream; i++) PUT("%s%x/%x", i ? "," : "", r->stream[i][0], r->stream[i][1]);
    PUT(" c=");
    for (int i = 0; i < r->ncolor; i++) {
        PUT("%s%x", i ? "," : "", r->color[i]);
        if (r->cview[i]) PUT(":%x", r->cview[i]);
    }
    PUT(" d=%x n=%u m=%x b=%x B=", r->depth, r->samples, r->channel_mask, r->blend_on);
    bool first = true;
    for (int i = 0; i < 8; i++) {
        if (!((r->blend_on >> i) & 1)) continue;
        PUT("%s", first ? "" : ",");
        first = false;
        for (int k = 0; k < 8; k++) PUT("%02x", r->blend[i][k]);
    }
    PUT(" k=%u t=%u p=%u", r->a2c, r->topo, r->poly_fill);
#undef PUT
}

static const char *field(const char *line, const char *name) {  // value after " name=" (or at the start)
    size_t l = strlen(name);
    for (const char *p = line; (p = strstr(p, name)); p++)
        if ((p == line || p[-1] == ' ') && p[l] == '=') return p + l + 1;
    return NULL;
}

static int hex_list(const char *p, uint32_t *out, int max) {  // "a,b,c" (hex) up to a space / the end
    int n = 0;
    while (p && *p && *p != ' ' && *p != '\n' && *p != '\r' && n < max) {
        char *e;
        out[n++] = (uint32_t)strtoul(p, &e, 16);
        if (e == p) return -1;
        p = *e == ',' ? e + 1 : e;
    }
    return n;
}

bool shader_rec_parse(const char *line, ShaderRec *r) {
    memset(r, 0, sizeof *r);
    while (*line == ' ' || *line == '\t') line++;
    if (line[0] != 'G' && line[0] != 'C') return false;
    r->kind = line[0];
    const char *v;
    if (r->kind == 'C') {
        if (!(v = field(line, "cs"))) return false;
        r->vs = strtoull(v, NULL, 16);
        if (!(v = field(line, "l"))) return false;
        for (int i = 0; i < 3; i++) {  // (decimal)
            char *e;
            r->local[i] = (uint32_t)strtoul(v, &e, 10);
            if (e == v) return false;
            v = *e == ',' ? e + 1 : e;
        }
        return r->vs != 0;
    }
    if (!(v = field(line, "vs"))) return false;
    r->vs = strtoull(v, NULL, 16);
    if (!(v = field(line, "fs"))) return false;
    r->fs = strtoull(v, NULL, 16);
    if ((v = field(line, "f"))) r->flags = (uint8_t)strtoul(v, NULL, 16);
    int n = (v = field(line, "a")) ? hex_list(v, r->attrib, 16) : 0;
    if (n < 0) return false;
    r->nattrib = (uint8_t)n;
    if ((v = field(line, "s"))) {
        while (*v && *v != ' ' && r->nstream < 16) {
            char *e;
            r->stream[r->nstream][0] = (uint32_t)strtoul(v, &e, 16);
            if (*e != '/') return false;
            r->stream[r->nstream][1] = (uint32_t)strtoul(e + 1, &e, 16);
            r->nstream++;
            v = *e == ',' ? e + 1 : e;
        }
    }
    if ((v = field(line, "c"))) {
        while (*v && *v != ' ' && r->ncolor < 8) {
            char *e;
            r->color[r->ncolor] = (uint32_t)strtoul(v, &e, 16);
            if (*e == ':') r->cview[r->ncolor] = (uint32_t)strtoul(e + 1, &e, 16);
            r->ncolor++;
            v = *e == ',' ? e + 1 : e;
        }
    }
    if ((v = field(line, "d"))) r->depth = (uint32_t)strtoul(v, NULL, 16);
    r->samples = (v = field(line, "n")) ? (uint32_t)strtoul(v, NULL, 16) : 1;
    if (!r->samples) r->samples = 1;
    if ((v = field(line, "m"))) r->channel_mask = (uint32_t)strtoul(v, NULL, 16);
    if ((v = field(line, "b"))) r->blend_on = (uint8_t)strtoul(v, NULL, 16);
    if ((v = field(line, "B"))) {
        for (int i = 0; i < 8; i++) {
            if (!((r->blend_on >> i) & 1)) continue;
            for (int k = 0; k < 8; k++) {
                unsigned b;
                if (sscanf(v, "%2x", &b) != 1) return false;
                r->blend[i][k] = (uint8_t)b;
                v += 2;
            }
            if (*v == ',') v++;
        }
    }
    if ((v = field(line, "k"))) r->a2c = (uint8_t)strtoul(v, NULL, 16);
    r->topo = (v = field(line, "t")) ? (uint8_t)strtoul(v, NULL, 16) : 2;
    r->poly_fill = (v = field(line, "p")) ? (uint8_t)strtoul(v, NULL, 16) : 1;
    return r->vs && r->fs;
}

// ------------------------------------------------------------------ recorder (LARECOMP_RECORD_KEYS)
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *rec_seen;  // line hashes (open addressing, 0 = empty)
static size_t rec_cap, rec_n;

static bool seen_add(uint64_t h) {  // false: already there
    if (!h) h = 1;
    if (rec_n * 2 >= rec_cap) {
        size_t cap = rec_cap ? rec_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < rec_cap; i++)
            if (rec_seen[i]) {
                size_t j = rec_seen[i] & (cap - 1);
                while (t[j]) j = (j + 1) & (cap - 1);
                t[j] = rec_seen[i];
            }
        free(rec_seen);
        rec_seen = t, rec_cap = cap;
    }
    size_t j = h & (rec_cap - 1);
    while (rec_seen[j]) {
        if (rec_seen[j] == h) return false;
        j = (j + 1) & (rec_cap - 1);
    }
    rec_seen[j] = h;
    rec_n++;
    return true;
}

static const char *rec_path(void) {
    static const char *p;
    static int done;
    if (!done) { p = getenv("LARECOMP_RECORD_KEYS"); if (p && !*p) p = NULL; done = 1; }
    return p;
}

bool shader_rec_recording(void) { return rec_path() != NULL || shader_seen_on(); }

void shader_rec_note(const ShaderRec *r) {
    shader_seen_note(r);  // (the player's own list: queued, written off this thread)
    const char *path = rec_path();
    if (!path) return;
    char line[2048];
    shader_rec_format(r, line, sizeof line);
    pthread_mutex_lock(&rec_lock);
    static bool loaded;
    if (!loaded) {  // (an earlier run's records: appended to, never repeated)
        loaded = true;
        FILE *f = fopen(path, "r");
        char l[2048];
        while (f && fgets(l, sizeof l, f)) {
            l[strcspn(l, "\r\n")] = 0;
            if (l[0] == 'G' || l[0] == 'C') seen_add(shader_code_hash((const uint8_t *)l, strlen(l)));
        }
        if (f) fclose(f);
    }
    if (seen_add(shader_code_hash((const uint8_t *)line, strlen(line)))) {
        FILE *f = fopen(path, "a");
        if (f) { fprintf(f, "%s\n", line); fclose(f); }
    }
    pthread_mutex_unlock(&rec_lock);
}

// ------------------------------------------------------------------ first visits
// (once per program / pipeline / compute shader for the process: a reboot into another seed builds them again, from
// the system's shader caches)
void shader_first_visit(const char *what, uint64_t a, uint64_t b, uint64_t id) {
    static _Atomic int n;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t *seen;
    static size_t cap, cnt;
    uint64_t h = id ^ (uint64_t)(uint8_t)what[1] << 56;
    if (!h) h = 1;
    pthread_mutex_lock(&lock);
    if (cnt * 2 >= cap) {
        size_t nc = cap ? cap * 2 : 1024;
        uint64_t *t = calloc(nc, sizeof *t);
        for (size_t i = 0; i < cap; i++)
            if (seen[i]) {
                size_t j = seen[i] & (nc - 1);
                while (t[j]) j = (j + 1) & (nc - 1);
                t[j] = seen[i];
            }
        free(seen);
        seen = t, cap = nc;
    }
    size_t j = h & (cap - 1);
    while (seen[j] && seen[j] != h) j = (j + 1) & (cap - 1);
    bool fresh = !seen[j];
    if (fresh) seen[j] = h, cnt++;
    pthread_mutex_unlock(&lock);
    if (!fresh) return;
    int k = ++n;
    if (b) LOG("[shader] first visit %d: %s %016" PRIx64 " %016" PRIx64 "\n", k, what, a, b);
    else LOG("[shader] first visit %d: %s %016" PRIx64 "\n", k, what, a);
}

// ------------------------------------------------------------------ index of the game files' shaders
// Every shader in the player's game files, by code hash: the BNSH containers (nn::gfx shader files) in the shader
// archives (*.bfsha), the effects (*.ptcl) and the UI archives (*.arc). Only locations are kept (file, offset).
typedef struct { uint64_t hash; uint32_t file, off, size, const_offset, const_size; } IdxEnt;
static IdxEnt *idx_tab;
static size_t idx_cap, idx_n;
typedef struct { char *path; uint8_t *mem; } IdxSrc;  // a game file, or (the main module's read-only data) memory
static IdxSrc *idx_files;
static int idx_nfiles;

static void idx_add(uint64_t h, uint32_t file, uint32_t off, uint32_t size, uint32_t coff, uint32_t csize) {
    if (!h) return;
    if (idx_n * 2 >= idx_cap) {
        size_t cap = idx_cap ? idx_cap * 2 : 16384;
        IdxEnt *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < idx_cap; i++)
            if (idx_tab[i].hash) {
                size_t j = idx_tab[i].hash & (cap - 1);
                while (t[j].hash) j = (j + 1) & (cap - 1);
                t[j] = idx_tab[i];
            }
        free(idx_tab);
        idx_tab = t, idx_cap = cap;
    }
    size_t j = h & (idx_cap - 1);
    while (idx_tab[j].hash) {
        if (idx_tab[j].hash == h) return;
        j = (j + 1) & (idx_cap - 1);
    }
    idx_tab[j] = (IdxEnt){h, file, off, size, coff, csize};
    idx_n++;
}

static const IdxEnt *idx_find(uint64_t h) {
    if (!idx_cap || !h) return NULL;
    for (size_t j = h & (idx_cap - 1); idx_tab[j].hash; j = (j + 1) & (idx_cap - 1))
        if (idx_tab[j].hash == h) return &idx_tab[j];
    return NULL;
}

static uint32_t rd32(const uint8_t *d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
static uint64_t rd64(const uint8_t *d, size_t o) { uint64_t v; memcpy(&v, d + o, 8); return v; }

// Code offsets seen in the current file
static uint64_t *off_tab;
static size_t off_cap, off_n;
static void off_reset(void) { if (off_tab) memset(off_tab, 0, off_cap * sizeof *off_tab); off_n = 0; }
static bool off_add(uint64_t o) {  // false: seen
    o++;  // (0 = empty)
    if (off_n * 2 >= off_cap) {
        size_t cap = off_cap ? off_cap * 2 : 4096;
        uint64_t *t = calloc(cap, sizeof *t);
        for (size_t i = 0; i < off_cap; i++)
            if (off_tab[i]) {
                size_t j = (off_tab[i] * 0x9E3779B97F4A7C15ull >> 20) & (cap - 1);
                while (t[j]) j = (j + 1) & (cap - 1);
                t[j] = off_tab[i];
            }
        free(off_tab);
        off_tab = t, off_cap = cap;
    }
    size_t j = (o * 0x9E3779B97F4A7C15ull >> 20) & (off_cap - 1);
    while (off_tab[j]) {
        if (off_tab[j] == o) return false;
        j = (j + 1) & (off_cap - 1);
    }
    off_tab[j] = o;
    off_n++;
    return true;
}

// One BNSH container at d[b..]: its shader programs' stage codes. Every offset is checked against the container.
static void idx_bnsh(const uint8_t *d, size_t n, size_t b, uint32_t file) {
    if (b + 0x20 > n || memcmp(d + b + 0xC, "\xff\xfe", 2)) return;
    size_t fsz = rd32(d, b + 0x1C), first;
    if (fsz < 0x40 || b + fsz > n) return;
    first = (size_t)d[b + 0x16] | (size_t)d[b + 0x17] << 8;
    if (first + 0x28 > fsz || memcmp(d + b + first, "grsc", 4)) return;
    size_t c = first + 0x10;                      // container data: variation count +0xC, variations +0x10
    uint32_t nvar = rd32(d, b + c + 0xC);
    uint64_t pvar = rd64(d, b + c + 0x10);
    if (pvar >= fsz || nvar > (fsz - pvar) / 0x40) return;
    for (uint32_t v = 0; v < nvar; v++) {
        uint64_t pbin = rd64(d, b + pvar + (uint64_t)v * 0x40 + 0x10);  // variation: binary program
        if (!pbin || pbin + 0x38 > fsz) continue;
        for (int s = 0; s < 6; s++) {                                     // program: stage codes +8 (VS..CS)
            uint64_t pc = rd64(d, b + pbin + 8 + 8 * (uint64_t)s);
            if (!pc || pc + 0x20 > fsz) continue;
            uint64_t pctl = rd64(d, b + pc + 8), pcode = rd64(d, b + pc + 0x10);
            uint32_t csz = rd32(d, b + pc + 0x18);
            if (pctl + 0x750 > fsz || pcode >= fsz || csz > fsz - pcode) continue;
            uint32_t coff = rd32(d, b + pctl + 0x700), cs = rd32(d, b + pctl + 0x6FC);
            if (!coff || coff > csz || cs > csz - coff) continue;
            uint32_t len = coff + cs;
            if (!off_add(b + pcode)) continue;  // (variations share code: hash each once)
            idx_add(shader_code_hash(d + b + pcode, len), file, (uint32_t)(b + pcode), len, coff, cs);
        }
    }
}

static void idx_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 && n < 0x7FFFFFFF ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) == (size_t)n) {
        uint32_t file = (uint32_t)idx_nfiles;
        bool any = false;
        off_reset();
        for (size_t i = 0; i + 8 <= (size_t)n; i += 8)  // (containers are aligned)
            if (!memcmp(d + i, "BNSH\0\0\0\0", 8)) { any = true; idx_bnsh(d, (size_t)n, i, file); }
        if (any) {
            idx_files = realloc(idx_files, sizeof *idx_files * (size_t)(idx_nfiles + 1));
            idx_files[idx_nfiles++] = (IdxSrc){strdup(path), NULL};
        }
    }
    free(d);
    fclose(f);
}

// The main module (exefs/main, an NSO) holds a few shaders of its own in its data (LZ4-compressed in the file): its
// read-only and read-write data segments are kept decompressed for the run when they have any
size_t rt_lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);
static void idx_nso(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *raw = n > 0x100 && n < 0x7FFFFFFF ? malloc((size_t)n) : NULL;
    if (raw && fread(raw, 1, (size_t)n, f) == (size_t)n && !memcmp(raw, "NSO0", 4)) {
        uint32_t flags = rd32(raw, 0xC);
        for (int sg = 1; sg < 3; sg++) {  // header: segment {file offset, memory offset, size} at 0x10 + 16 * i,
            uint32_t foff = rd32(raw, 0x10 + 16 * sg), size = rd32(raw, 0x18 + 16 * sg), csize = rd32(raw, 0x60 + 4 * sg);
            uint8_t *seg = size && size < (256u << 20) ? malloc(size) : NULL;  // file sizes at 0x60 + 4 * i
            bool ok = seg && (uint64_t)foff + csize <= (uint64_t)n;
            if (ok && (flags & (1u << sg))) ok = rt_lz4_decompress(raw + foff, csize, seg, size) == size;
            else if (ok) ok = csize >= size, memcpy(seg, raw + foff, ok ? size : 0);
            bool any = false;
            uint32_t file = (uint32_t)idx_nfiles;
            off_reset();
            for (size_t i = 0; ok && i + 8 <= size; i += 8)
                if (!memcmp(seg + i, "BNSH\0\0\0\0", 8)) { any = true; idx_bnsh(seg, size, i, file); }
            if (any) {
                idx_files = realloc(idx_files, sizeof *idx_files * (size_t)(idx_nfiles + 1));
                idx_files[idx_nfiles++] = (IdxSrc){NULL, seg};
            } else free(seg);
        }
    }
    free(raw);
    fclose(f);
}

// The shaders' code this player's runs kept (shader_seen.c shader_seen_code: Pokémon Z-A's are in no file the index
// reads). Read by header only; a broken tail is left out (shader_seen.c starts such a file again)
static void idx_store(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long end = ftell(f), off = 0;
    uint32_t file = (uint32_t)idx_nfiles;
    bool any = false;
    ShaderCodeHdr h;
    while (off < end && !fseek(f, off, SEEK_SET) && fread(&h, sizeof h, 1, f) == 1 && h.size &&
           h.size <= (uint64_t)(end - off - (long)sizeof h) &&
           (uint64_t)h.const_offset + h.const_size <= h.size) {  // (the constants inside the code)
        idx_add(h.hash, file, (uint32_t)(off + (long)sizeof h), h.size, h.const_offset, h.const_size);
        any = true;
        off += (long)sizeof h + h.size;
    }
    fclose(f);
    if (any) {
        idx_files = realloc(idx_files, sizeof *idx_files * (size_t)(idx_nfiles + 1));
        idx_files[idx_nfiles++] = (IdxSrc){strdup(path), NULL};
    }
}

static void idx_dir(const char *dir, int depth) {
    DIR *dd = depth < 8 ? opendir(dir) : NULL;
    if (!dd) return;
    struct dirent *e;
    while ((e = readdir(dd))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t l = strlen(e->d_name);
        const char *ext = l > 5 ? e->d_name + l - 5 : "";
        if (!strcmp(ext, "bfsha") || !strcmp(ext, ".ptcl") || (l > 4 && !strcmp(e->d_name + l - 4, ".arc"))) idx_file(p);
        else {
            struct stat sb;
            if (stat(p, &sb) == 0 && S_ISDIR(sb.st_mode)) idx_dir(p, depth + 1);
        }
    }
    closedir(dd);
}

// The shader's code (+ its constants), read from the game files; NULL: not there
static uint8_t *blob_read(const IdxEnt *e, ShaderBlob *b, FILE **files) {
    uint8_t *d = calloc(1, (size_t)e->size + 0x100);  // (zero padding past the constants)
    if (idx_files[e->file].mem) memcpy(d, idx_files[e->file].mem + e->off, e->size);
    else {
        FILE **f = &files[e->file];
        if (!*f && !(*f = fopen(idx_files[e->file].path, "rb"))) { free(d); return NULL; }
        if (fseek(*f, (long)e->off, SEEK_SET) || fread(d, 1, e->size, *f) != e->size) { free(d); return NULL; }
    }
    *b = (ShaderBlob){d, e->size, NULL, e->const_offset, e->const_size};
    return d;
}

// ------------------------------------------------------------------ cache tags
#ifndef SHADER_TRANSLATOR_ID
#define SHADER_TRANSLATOR_ID "dev"
#endif
const char *larecomp_version(void);  // runtime/ui/launcher.mm

void shader_cache_tag(char *out, size_t n, const char *gpu) {
    snprintf(out, n, "%s translator %s cache %d | %s", larecomp_version(), SHADER_TRANSLATOR_ID, SHADER_CACHE_VERSION, gpu);
}

bool shader_cache_tag_ok(const char *tag_path, const char *tag) {
    FILE *f = fopen(tag_path, "rb");
    if (!f) return false;
    char have[1024];
    size_t got = fread(have, 1, sizeof have - 1, f);
    fclose(f);
    have[got] = 0;
    return !strcmp(have, tag);
}

bool shader_cache_write(const char *path, const void *data, size_t n) {
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    mkdir("cache", 0755);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    ok &= fclose(f) == 0;
    if (ok) {
#ifdef _WIN32
        remove(path);  // (rename doesn't replace on Windows; a crash here loses the cache, never corrupts it)
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

void shader_cache_tag_write(const char *tag_path, const char *tag) { shader_cache_write(tag_path, tag, strlen(tag)); }

// ------------------------------------------------------------------ the run
static const char shipped_list[] =
#include "shader_variants.inc"
    ;

const char *shader_prep_shipped_list(void) { return shipped_list; }

// The game files' shaders as one id (order-independent): another game version has another
static uint64_t idx_game_id(void) {
    uint64_t g = idx_n;
    for (size_t i = 0; i < idx_cap; i++)
        if (idx_tab[i].hash) g += (idx_tab[i].hash ^ (idx_tab[i].hash >> 31)) * 0x9E3779B97F4A7C15ull;
    return g ? g : 1;
}

static double ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static bool prep_run_locked(const char *romfs, void (*progress)(int done, int total), const volatile int *cancel);
bool shader_prep_run(const char *romfs, void (*progress)(int done, int total), const volatile int *cancel) {
    static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;  // (one run at a time: the index, the translator)
    pthread_mutex_lock(&one);
    bool ok = prep_run_locked(romfs, progress, cancel);
    pthread_mutex_unlock(&one);
    return ok;
}

static bool prep_run_locked(const char *romfs, void (*progress)(int done, int total), const volatile int *cancel) {
    double t0 = ms_now();
    char *list = NULL;
    if (getenv("LARECOMP_SHADER_LIST")) {
        FILE *f = fopen(getenv("LARECOMP_SHADER_LIST"), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            list = calloc(1, (size_t)(n > 0 ? n : 0) + 1);
            if (n > 0 && fread(list, 1, (size_t)n, f) != (size_t)n) list[0] = 0;
            fclose(f);
        }
    } else list = strdup(shipped_list);
    if (!list || !*list) { free(list); LOG("[shader] preparation: no variant list\n"); return false; }
    if (!renderer_prepare_begin()) { free(list); LOG("[shader] preparation: no GPU device yet\n"); return false; }
    uint64_t seen_game;
    char *seen = shader_seen_read(&seen_game);  // (the player's own list: after the shipped one)
    int total = 0;
    for (char *p = list; *p; p++) total += *p == '\n';
    for (char *p = seen; p && *p; p++) total += *p == '\n';
    if (progress) progress(0, total + 1);
    if (!idx_n) {
        idx_dir(romfs, 0);
        char main_nso[1100];  // (<game files>/exefs/main, next to romfs/)
        snprintf(main_nso, sizeof main_nso, "%s/../exefs/main", romfs);
        idx_nso(main_nso);
        idx_store(SHADER_CODE_FILE);
    }
    double t_idx = ms_now() - t0;
    if (idx_n) {  // (the player's list of other game files - none yet, or an update: only what this game has)
        uint64_t game = idx_game_id();
        char *keep = NULL;
        if (seen && seen_game != game) {
            size_t n = 0;
            keep = calloc(1, strlen(seen) + 1);
            for (char *line = seen, *next; *line; line = next) {
                next = strchr(line, '\n');
                size_t l = next ? (size_t)(next - line) : strlen(line);
                next = next ? next + 1 : line + l;
                char one[2048];
                ShaderRec r;
                if (l >= sizeof one) continue;
                memcpy(one, line, l), one[l] = 0;
                if (!shader_rec_parse(one, &r) || !idx_find(r.vs) || (r.kind == 'G' && !idx_find(r.fs))) continue;
                memcpy(keep + n, line, l), n += l, keep[n++] = '\n';
            }
            free(seen);
            seen = strdup(keep);
        }
        shader_seen_prep_done(game, keep);
        free(keep);
    }
    FILE **files = calloc((size_t)idx_nfiles + 1, sizeof *files);
    int done = 0, prepared = 0, missing = 0, failed = 0, prepared_seen = 0;
    for (int own = 0; own < 2; own++)  // the shipped list, then the player's own
    for (char *line = own ? seen : list, *next; line && *line && !(cancel && *cancel); line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        else next = line + strlen(line);
        ShaderRec r;
        if (!shader_rec_parse(line, &r)) continue;
        if (own && !getenv("LARECOMP_SHADER_LIST") && shader_seen_shipped(line)) continue;  // (prepared already)
        const IdxEnt *ev = idx_find(r.vs), *ef = r.kind == 'G' ? idx_find(r.fs) : NULL;
        ShaderBlob bv = {0}, bf = {0};
        uint8_t *dv = ev ? blob_read(ev, &bv, files) : NULL, *df = ef ? blob_read(ef, &bf, files) : NULL;
        if (!dv || (r.kind == 'G' && !df)) missing++;
        else if (renderer_prepare(&r, &bv, &bf)) *(own ? &prepared_seen : &prepared) += 1;
        else failed++;
        free(dv);
        free(df);
        if (progress && (++done & 7) == 0) progress(done - renderer_prepare_pending(), total + 1);
    }
    while (!(cancel && *cancel) && renderer_prepare_pending()) {  // (the last builds)
        if (progress) progress(done - renderer_prepare_pending(), total + 1);
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
    if (!(cancel && *cancel)) renderer_prepare_end();
    if (progress) progress(total + 1, total + 1);
    for (int i = 0; i < idx_nfiles; i++) if (files[i]) fclose(files[i]);
    free(files);
    free(list);
    free(seen);
    LOG("[shader] prepared %d + %d seen before, not found %d%s%s (%d failed; index %zu shaders in %.0f ms, total %.0f ms)\n",
        prepared, prepared_seen, missing, cancel && *cancel ? ", " : "", cancel && *cancel ? "skipped" : "", failed, idx_n, t_idx, ms_now() - t0);
    return true;
}

// (at the game's GPU device setup) Pokémon Z-A: always - the start screen can't prepare its shaders (vulkan/exec.c
// renderer_prepare_begin), and here, during its logos, the ones its earlier runs kept (shader_seen_code) take well
// under a second with warm caches. LARECOMP_SHADER_PREP=0: not; =1: Link's Awakening too (tests).
void shader_prep_boot(const char *romfs) {
    extern int g_game_is_la;  // (runtime/rt.h)
    // (Pokémon Z-A only; F1 > Compatibility, off on AMD by default: #39)
    if (g_game_is_la || !gpu_feature_on(GPU_SHADER_PREP, "LARECOMP_SHADER_PREP")) {
        const char *e = getenv("LARECOMP_SHADER_PREP");
        if (!(e && atoi(e))) return;  // (=1 still forces it, Link's Awakening included)
    }
    shader_prep_run(romfs, NULL, NULL);
}
