// NSO loading, relocation, and symbol resolution.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "hle.h"  // (RT_ON_RESET)
#include "rt.h"

Module g_modules[6];  // main, sdk, up to 4 code mods (runtime/modlib.h MAX_CODE_MODS)
int g_nmodules;

enum { DT_NULL = 0, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
       DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027 };

typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64Sym;
typedef struct { uint64_t off, info; int64_t addend; } Elf64Rela;

size_t rt_lz4_decompress(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {  // (LZ4 block; also shader_prep.c)
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { b = *s++; lit += b; } while (b == 255); }
        if (d + lit > de || s + lit > se) break;
        memcpy(d, s, lit);
        d += lit; s += lit;
        if (s >= se) break;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        uint8_t *m = d - off;
        if (m < dst || d + ml > de) break;
        while (ml--) *d++ = *m++;
    }
    return d - dst;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    fread(p, 1, *len, f);
    fclose(f);
    return p;
}

static void load_nso(Module *m, const char *path, uint64_t base) {
    // (a reboot reuses the slots, maybe for another code mod: the tags below are only set when the NSO has them, so a
    // previous module's DT_INIT / DT_HASH would stay - its start code run inside this one)
    const char *name = m->name;
    memset(m, 0, sizeof *m);
    m->name = name;
    size_t len;
    uint8_t *raw = read_file(path, &len);
    if (!raw || memcmp(raw, "NSO0", 4)) FATAL("cannot load %s\n", path);
    uint32_t flags = *(uint32_t *)(raw + 0xC);
    uint32_t seg[3][3];  // file_off, mem_off, size
    for (int i = 0; i < 3; i++) memcpy(seg[i], raw + 0x10 + i * 0x10, 12);
    uint32_t bss = *(uint32_t *)(raw + 0x3C);
    uint64_t size = (seg[2][1] + seg[2][2] + bss + 0x3FFF) & ~0x3FFFull;
    plat_commit(base, size);  // inside the guest window: zero-filled, read/write
    for (int i = 0; i < 3; i++) {
        uint32_t csize = *(uint32_t *)(raw + 0x60 + i * 4);
        uint8_t *dst = (uint8_t *)base + seg[i][1];
        if (flags & (1 << i)) {
            size_t got = rt_lz4_decompress(raw + seg[i][0], csize, dst, seg[i][2]);
            if (got != seg[i][2]) FATAL("lz4 %s seg %d: %zu != %u\n", path, i, got, seg[i][2]);
        } else {
            memcpy(dst, raw + seg[i][0], seg[i][2]);
        }
    }
    free(raw);
    m->base = base;
    m->size = size;
    m->text_off = seg[0][1]; m->text_size = seg[0][2];
    m->ro_off = seg[1][1];   m->ro_size = seg[1][2];
    m->data_off = seg[2][1]; m->data_size = seg[2][2] + bss;

    uint32_t mod0 = *(uint32_t *)(base + 4);
    uint64_t mod = base + mod0;
    m->dynamic = mod + *(int32_t *)(mod + 4);
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        switch (d[0]) {
        case DT_SYMTAB: m->symtab = base + d[1]; break;
        case DT_STRTAB: m->strtab = base + d[1]; break;
        case DT_HASH: m->nsyms = *(uint32_t *)(base + d[1] + 4); break;
        case 12: m->init = base + d[1]; break;  // DT_INIT
        case 13: m->fini = base + d[1]; break;  // DT_FINI
        case DT_INIT_ARRAY: m->init_array = base + d[1]; break;
        case DT_INIT_ARRAYSZ: m->init_array_sz = d[1]; break;
        case DT_FINI_ARRAY: m->fini_array = base + d[1]; break;
        case DT_FINI_ARRAYSZ: m->fini_array_sz = d[1]; break;
        }
    }
    LOG("loaded %-5s at %llx size %llx syms %llu\n", m->name, base, size, m->nsyms);
}

static Elf64Sym *sym(Module *m, uint64_t i) { return (Elf64Sym *)m->symtab + i; }
static const char *symname(Module *m, Elf64Sym *s) { return (const char *)(m->strtab + s->name); }

uint64_t loader_find_symbol(const char *name) {
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            if (s->shndx && !strcmp(symname(m, s), name)) return m->base + s->value;
        }
    }
    return 0;
}

const char *loader_symbol_at(uint64_t addr, uint64_t *offset) {
    const char *best = NULL;
    uint64_t best_addr = 0;
    for (int mi = 0; mi < g_nmodules; mi++) {
        Module *m = &g_modules[mi];
        if (addr < m->base || addr >= m->base + m->size) continue;
        for (uint64_t i = 1; i < m->nsyms; i++) {
            Elf64Sym *s = sym(m, i);
            uint64_t a = m->base + s->value;
            if (s->shndx && (s->info & 0xF) == 2 && a <= addr && a > best_addr) {
                best = symname(m, s);
                best_addr = a;
            }
        }
    }
    if (offset) *offset = addr - best_addr;
    return best;
}

static void rtld_lookup(Cpu *c) { c->x[0] = loader_find_symbol((const char *)(uintptr_t)c->x[0]); }

// ---- rtld's list of the loaded modules: the SDK's nn::ro::detail::g_pAutoLoadList (a RoModuleList *), which rtld
// sets on the console and nothing sets here (no exefs/rtld). Code mods walk it (Hakkun's
// hk::ro::RoUtil::initModuleList, its first act: a null list crashed it). Each module's RoModule is where its MOD0
// says (the module object field), filled as rtld does in SDK 19+ (Hakkun's __RTLD_PAST_19XX__ layout, which its Z-A
// builds use: m_IsPltRela before m_Base), linked into one list in load order. Only with code mods (main.c): the game
// alone keeps running as it always has here.
void loader_ro_modules(void) {
    uint64_t *list = hle_alloc(16);  // RoModuleList {front, back}; empty: both point at the list itself
    uint64_t head = (uint64_t)(uintptr_t)list, prev = head;
    list[0] = list[1] = head;
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        uint32_t mod0 = *(uint32_t *)(m->base + 4);
        uint64_t mo = m->base + mod0 + *(int32_t *)(m->base + mod0 + 0x18);  // (MOD0's module object, in its .bss)
        if (mo < m->base || mo + 0xC0 > m->base + m->size) continue;
        uint64_t *r = (uint64_t *)(uintptr_t)mo;
        memset(r, 0, 0xC0);  // (only the fields below: what follows in .bss may be the SDK's, already set)
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, hash = 0, strsz = 0, relacount = 0;
        for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
            if (d[0] == DT_RELA) rela = m->base + d[1];
            if (d[0] == DT_RELASZ) relasz = d[1];
            if (d[0] == DT_JMPREL) jmprel = m->base + d[1];
            if (d[0] == DT_PLTRELSZ) pltsz = d[1];
            if (d[0] == DT_HASH) hash = m->base + d[1];
            if (d[0] == 10) strsz = d[1];  // DT_STRSZ
            if (d[0] == 0x6FFFFFF9) relacount = d[1];  // DT_RELACOUNT
        }
        r[0x00 / 8] = head;  // next (the end of the list), prev
        r[0x08 / 8] = prev;
        r[0x10 / 8] = jmprel;  // m_pPlt, m_pDyn
        r[0x18 / 8] = rela;
        *(uint8_t *)(uintptr_t)(mo + 0x20) = 1;  // m_IsPltRela
        r[0x28 / 8] = m->base;
        r[0x30 / 8] = m->dynamic;
        r[0x38 / 8] = pltsz;
        r[0x40 / 8] = m->init, r[0x48 / 8] = m->fini;
        if (hash) r[0x50 / 8] = hash + 8, r[0x58 / 8] = hash + 8 + 4ull * *(uint32_t *)(uintptr_t)hash;  // buckets, chains
        r[0x60 / 8] = m->strtab, r[0x68 / 8] = m->symtab, r[0x70 / 8] = strsz;
        r[0x80 / 8] = relasz, r[0x98 / 8] = relacount;
        if (hash) r[0xA0 / 8] = *(uint32_t *)(uintptr_t)(hash + 4), r[0xA8 / 8] = *(uint32_t *)(uintptr_t)hash;  // nchain, nbucket
        r[0xB8 / 8] = m->size;  // (the image's size: nn::ro::LookupSymbol's "address in this module" check)
        ((uint64_t *)(uintptr_t)prev)[0] = mo;  // (the previous one's next; the list's front for the first)
        prev = mo;
    }
    list[1] = prev;  // back
    // (into every module's own definition: Hakkun mods define one of their own, which on the console comes first in
    // load order - subsdk before sdk - and is the one rtld fills)
    // (and the modules nn::ro loads later: none, an empty list - nn::ro::LookupSymbol walks it after this one, and read
    // through its null: Native Mouse's symbol lookup)
    uint64_t *manual = hle_alloc(16);
    manual[0] = manual[1] = (uint64_t)(uintptr_t)manual;
    int vars = 0;
    for (int i = 0; i < g_nmodules; i++) {
        Module *m = &g_modules[i];
        for (uint64_t k = 1; k < m->nsyms; k++) {
            Elf64Sym *s = sym(m, k);
            if (!s->shndx) continue;
            const char *nm = symname(m, s);
            if (!strcmp(nm, "_ZN2nn2ro6detail15g_pAutoLoadListE")) *(uint64_t *)(uintptr_t)(m->base + s->value) = head, vars++;
            if (!strcmp(nm, "_ZN2nn2ro6detail17g_pManualLoadListE"))
                *(uint64_t *)(uintptr_t)(m->base + s->value) = (uint64_t)(uintptr_t)manual;
        }
    }
    LOG("rtld module list: %d modules, in %d g_pAutoLoadList\n", g_nmodules, vars);
}

static uint64_t *lookup_cell;  // (g_LookupGlobalManualFunctionPointer: in the HLE heap, which a reboot hands out again)
RT_ON_RESET(lookup_cell_reset) { lookup_cell = NULL; }

static uint64_t resolve(Module *m, uint32_t symidx) {
    Elf64Sym *s = sym(m, symidx);
    const char *name = symname(m, s);
    // host implementation wins over the guest SDK's (also for the SDK's calls to itself through its PLT: fibers)
    HostFn impl = hle_lookup_impl(name);
    extern bool net_mod_only(const char *name);  // (hle/net_socket.c: the PC's network, for code mods' imports only)
    if (impl && m - g_modules < 2 && net_mod_only(name)) impl = NULL;
    if (impl) return hle_register(name, impl);
    // rtld's own variable (code mods import it: exlaunch / Hakkun look symbols up through it): a function pointer,
    // name -> address, here every module's exports (loader_find_symbol)
    if (!strcmp(name, "_ZN2nn2ro6detail35g_LookupGlobalManualFunctionPointerE")) {
        if (!lookup_cell) lookup_cell = hle_alloc(8);
        uint64_t *cell = lookup_cell;
        *cell = hle_register("<rtld lookup>", rtld_lookup);  // (every boot: a reboot wipes guest memory)
        return (uint64_t)(uintptr_t)cell;
    }
    if (s->shndx) return m->base + s->value;
    uint64_t a = loader_find_symbol(name);
    if (a) return a;
    if ((s->info >> 4) == 2) return 0;  // weak undefined
    return hle_register(name, NULL);
}

static void relocate(Module *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (uint64_t *d = (uint64_t *)m->dynamic; d[0] != DT_NULL; d += 2) {
        if (d[0] == DT_RELR) relr = m->base + d[1];
        if (d[0] == DT_RELRSZ) relrsz = d[1];
        if (d[0] == DT_RELA) rela = m->base + d[1];
        if (d[0] == DT_RELASZ) relasz = d[1];
        if (d[0] == DT_JMPREL) jmprel = m->base + d[1];
        if (d[0] == DT_PLTRELSZ) pltsz = d[1];
    }
    // RELR (newer SDKs, e.g. 19): packed relative relocations. An even entry is an address (relocated, then the
    // next word is the start of a bitmap run); an odd one a bitmap of the 63 words after that run's start.
    uint64_t *where = NULL;
    for (uint64_t *e = (uint64_t *)relr; e < (uint64_t *)(relr + relrsz); e++) {
        if (!(*e & 1)) {
            where = (uint64_t *)(m->base + *e);
            *where++ += m->base;
            continue;
        }
        for (int i = 1; i < 64; i++)
            if ((*e >> i) & 1) where[i - 1] += m->base;
        where += 63;
    }
    int unresolved = 0;
    for (int pass = 0; pass < 2; pass++) {
        Elf64Rela *r = (Elf64Rela *)(pass ? jmprel : rela);
        uint64_t n = (pass ? pltsz : relasz) / sizeof(Elf64Rela);
        for (uint64_t i = 0; i < n; i++) {
            uint64_t *where = (uint64_t *)(m->base + r[i].off);
            uint32_t type = (uint32_t)r[i].info, si = r[i].info >> 32;
            switch (type) {
            case R_RELATIVE: *where = m->base + r[i].addend; break;
            case R_ABS64:
            case R_GLOB_DAT:
            case R_JUMP_SLOT: {
                uint64_t s = resolve(m, si);
                *where = s ? s + r[i].addend : 0;
                if (s >= HLE_BASE && s < HLE_BASE + HLE_SIZE) unresolved++;
                break;
            }
            default: FATAL("%s: reloc type %u\n", m->name, type);
            }
        }
    }
    LOG("relocated %-5s (%d imports bound to host stubs)\n", m->name, unresolved);
}

// A code mod's NSO (runtime/codemods.c), after main and sdk: loaded at base, its imports bound to theirs.
Module *loader_load_mod(const char *path, uint64_t base, const char *name) {
    if (g_nmodules == (int)(sizeof g_modules / sizeof *g_modules)) return NULL;
    Module *m = &g_modules[g_nmodules++];
    m->name = name;
    load_nso(m, path, base);
    relocate(m);
    return m;
}

void loader_load(const char *exefs) {
    char path[1024];
    static const struct { const char *name; uint64_t base; } mods[] = {{"main", MAIN_BASE}, {"sdk", SDK_BASE}};
    for (unsigned i = 0; i < 2; i++) {
        Module *m = &g_modules[g_nmodules++];
        m->name = mods[i].name;
        snprintf(path, sizeof path, "%s/%s", exefs, m->name);
        load_nso(m, path, mods[i].base);
    }
    for (int i = 0; i < g_nmodules; i++) relocate(&g_modules[i]);
}

// exefs/rtld, just below the guest window, for code mods: exlaunch takes the memory map's 1st code module as rtld and
// the 2nd as the game (its mem_layout.hpp s_MainModuleIdx = 1) and reads each one's MOD0 and name. Without rtld there,
// Lumiose Online took itself for main ("main .text size 0x148000"), declined the game and installed no hooks. Never
// run nor relocated (rtld's own job here is loader.c's). Not inside the window: main starts at its bottom, and moving
// the window down isn't possible on macOS (rt.h) - there the address is taken and this is skipped, logged.
Module g_rtld;
#define RTLD_SPAN 0x10000ull  // (one Windows allocation granule; rtld is 0x5000)
void loader_map_rtld(const char *exefs) {
    static bool mapped;  // (a reboot keeps it: outside the window, nothing wipes it; loaded again over itself)
    uint64_t base = GUEST_LO - RTLD_SPAN;
    if (!mapped) {
#ifdef _WIN32
        mapped = VirtualAlloc((void *)(uintptr_t)base, RTLD_SPAN, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
        void *p = mmap((void *)(uintptr_t)base, RTLD_SPAN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED && p != (void *)(uintptr_t)base) munmap(p, RTLD_SPAN);
        mapped = p == (void *)(uintptr_t)base;
#endif
        if (!mapped) { LOG("rtld: %llx taken, not mapped (exlaunch mods will misread the game)\n", (unsigned long long)base); return; }
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/rtld", exefs);
    FILE *f = fopen(path, "rb");
    if (!f) { LOG("rtld: no %s (exlaunch mods will misread the game)\n", path); return; }
    fclose(f);
    load_nso(&g_rtld, path, base);
    g_rtld.name = "rtld";
    if (g_rtld.size > RTLD_SPAN) FATAL("rtld: %llx bytes, more than its %llx\n", (unsigned long long)g_rtld.size, RTLD_SPAN);
}
