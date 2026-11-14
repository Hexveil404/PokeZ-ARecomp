// Function-entry profiler. LARECOMP_FNPROF=<from>:<to> counts guest function entries (every generated
// function calls FN_ENTER) between those presented frames, then writes build/fnprof.txt (addr count, most
// called first). Diffing two runs shows which game functions a change brings in; counts also show hot code.
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "platform.h"

volatile int g_fnprof;

#define NSLOT (1 << 18)
static struct { _Atomic uint64_t addr; _Atomic uint64_t count; } slots[NSLOT];

// LARECOMP_FNPROF_BT=<hex addr>[,...]: also print the host backtrace (guest functions are f_<addr>) the first time
// each listed function is entered inside the window - who calls it
static uint64_t bt_addrs[256];
static int bt_n = -1;
static _Atomic uint8_t bt_done[256];

uint64_t g_bt_first_x0;

static void bt_check(uint64_t a, uint64_t lr, uint64_t x0) {
    if (bt_n < 0) {
        bt_n = 0;
        const char *e = getenv("LARECOMP_FNPROF_BT");
        for (const char *p = e; p && *p && bt_n < 256;) {
            bt_addrs[bt_n++] = strtoull(p, (char **)&p, 16);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    for (int i = 0; i < bt_n; i++)
        if (bt_addrs[i] == a && !atomic_exchange(&bt_done[i], 1)) {
            if (i == 0) g_bt_first_x0 = x0;
            uint64_t vt = x0 >= 0x7000000000ull && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)x0 : 0;
            uint64_t v100 = vt >= 0x7000000000ull && vt < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(vt + 0x100) : 0;
            uint64_t p8 = x0 >= 0x7000000000ull && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(x0 + 8) : 0;
            fprintf(stderr, "[fnprof-bt] %llx entered from guest lr %llx x0 %llx (vtable %llx, +0x100 %llx, [x0+8] %llx):\n",
                    (unsigned long long)a, (unsigned long long)lr, (unsigned long long)x0, (unsigned long long)vt,
                    (unsigned long long)v100, (unsigned long long)p8);
            plat_print_backtrace();  // (execinfo is macOS / glibc only)
            if (i == 0 && getenv("LARECOMP_BT_WATCH")) {  // <hex size>: watch [x0, x0 + size) from here (dispatch.c)
                extern uint64_t g_watch_lo, g_watch_hi;
                extern void watch_arm(void);
                char spec[64];
                g_watch_lo = x0, g_watch_hi = x0 + strtoull(getenv("LARECOMP_BT_WATCH"), NULL, 16);
                snprintf(spec, sizeof spec, "LARECOMP_WATCH_ADDR=%llx,%llx", (unsigned long long)g_watch_lo, (unsigned long long)g_watch_hi);
                putenv(strdup(spec));
                watch_arm();
                fprintf(stderr, "[fnprof-bt] watching %llx-%llx\n", (unsigned long long)g_watch_lo, (unsigned long long)g_watch_hi);
            }
            {   // the guest's own call chain: lifted code keeps the frame records (x29 -> {previous x29, x30}) on the guest stack
                extern struct Cpu *kernel_current_cpu(void);
                const uint64_t *cp = (const uint64_t *)kernel_current_cpu();
                uint64_t fp = cp ? cp[29] : 0;
                fprintf(stderr, "  guest chain: %llx", (unsigned long long)lr);
                for (int k = 0; k < 24 && fp >= 0x7000000000ull && fp < 0x7800000000ull && !(fp & 7); k++) {
                    uint64_t nfp = *(const uint64_t *)(uintptr_t)fp, ra = *(const uint64_t *)(uintptr_t)(fp + 8);
                    fprintf(stderr, " <- %llx", (unsigned long long)ra);
                    if (nfp <= fp) break;
                    fp = nfp;
                }
                fprintf(stderr, "\n");
            }
        }
}

// Mystery Gift diagnostics (misc.c turns this and g_fnprof on when the game goes online for it): entries of the
// Mystery Gift code - its screens and gift list (main+0x2c60000..), its cards and downloads (main+0x2d9c000..), the
// network requests (main+0x1900000..) - with their callers, each pair once, to logs/mystery_gift_calls.log. The
// message window (main+0x2c6a808, x1 = {hash, label}) is logged every time, with the message's label.
volatile int g_mg_fn;
static FILE *g_mgf;
static uint64_t g_mg_seen[1 << 15][2];
static volatile int g_mg_reset;
void mg_calls_mark(const char *what) {  // (dispatch.c mg_trace_mark: a new phase - every pair is logged again)
    if (!g_mg_fn) return;
    if (!g_mgf) g_mgf = fopen("logs/mystery_gift_calls.log", "w");
    if (g_mgf) { fprintf(g_mgf, "==== %.80s\n", what); fflush(g_mgf); }
    g_mg_reset = 1;
}
static void mg_fn(uint64_t a, uint64_t lr) {
    static int lines;
    if (g_mg_reset) { memset(g_mg_seen, 0, sizeof g_mg_seen); g_mg_reset = 0; }
    uint64_t (*seen)[2] = g_mg_seen;
    FILE *f = g_mgf;
    if (!g_nmodules || lines >= 150000) return;
    uint64_t b = g_modules[0].base, off = a - b;
    // the async library's error makers (2.0.2): main+0xc57f50 (x1 = {hash, name}), main+0x2a1d3f0 (x0 = {hash, name},
    // x1 = result code) - every error the gift chain makes, by name
    bool err = off == 0xc57f50 || off == 0x2a1d3f0;
    if (a < b || !(err || (off >= 0x2c60000 && off < 0x2c78000) || (off >= 0x2d9c000 && off < 0x2dc8000) ||
                   (off >= 0x1900000 && off < 0x1920000)))
        return;
    Cpu *c = kernel_current_cpu();
    bool msg = off == 0x2c6a808 || err;
    if (!msg) {
        unsigned h = (unsigned)(((a >> 2) ^ (lr * 0x9E3779B1u)) * 2654435761u) & ((1 << 15) - 1);
        for (unsigned i = 0; i < 64; i++, h = (h + 1) & ((1 << 15) - 1)) {
            if (seen[h][0] == a && seen[h][1] == lr) return;
            if (!seen[h][0]) { seen[h][0] = a, seen[h][1] = lr; break; }
        }
    }
    if (!f && !(f = g_mgf = fopen("logs/mystery_gift_calls.log", "w"))) return;
    uint64_t e = b + g_modules[0].size;
    fprintf(f, "main+%llx <- %s%llx", (unsigned long long)off, lr >= b && lr < e ? "main+" : "",
            (unsigned long long)(lr >= b && lr < e ? lr - b : lr));
    if (c) fprintf(f, "  x0=%llx x1=%llx x2=%llx", (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                   (unsigned long long)c->x[2]);
    uint64_t named = !c ? 0 : off == 0x2a1d3f0 ? c->x[0] : c->x[1];
    if (msg && named >> 36 == 7) {  // the label / error name: a C string in the game's memory
        const char *label = *(const char *const *)(uintptr_t)(named + 8);
        if ((uint64_t)(uintptr_t)label >> 36 == 7) fprintf(f, "  %s %.48s", err ? "ERROR" : "MESSAGE", label);
    }
    fputc('\n', f);
    if (msg || ++lines % 32 == 0) fflush(f);
}

void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {
    if (g_mg_fn) { mg_fn(a, lr); if (bt_n <= 0) return; }
    if (bt_n) bt_check(a, lr, x0);
    {   // LARECOMP_FNPROF_X0=<fn hex>[,...] (#224): every entry of those functions inside the window, with its x0 and caller
        static uint64_t xf[16];
        static int nxf = -1;
        if (nxf < 0) {
            nxf = 0;
            for (const char *p = getenv("LARECOMP_FNPROF_X0"); p && *p && nxf < 16;) {
                xf[nxf++] = strtoull(p, (char **)&p, 16);
                if (*p == ',') p++; else break;
            }
        }
        for (int i = 0; i < nxf; i++)
            if (a == xf[i]) {
                extern uint64_t renderer_present_count(void);
                fprintf(stderr, "[x0] f%llu %llx x0 %llx lr %llx\n", (unsigned long long)renderer_present_count(),
                        (unsigned long long)a, (unsigned long long)x0, (unsigned long long)lr);
            }
    }
    {   // LARECOMP_FNPROF_VAR=<fn hex> (#224): each entry logs x0 and the {u64 hash, u64 value} record x1 points to
        static int64_t vf = -1;
        if (vf < 0) vf = getenv("LARECOMP_FNPROF_VAR") ? (int64_t)strtoull(getenv("LARECOMP_FNPROF_VAR"), NULL, 16) : 0;
        if (vf && a == (uint64_t)vf) {
            extern struct Cpu *kernel_current_cpu(void);
            const uint64_t *cp = (const uint64_t *)kernel_current_cpu();
            uint64_t x1 = cp ? cp[1] : 0;
            if (x1 >= 0x7000000000ull && x1 < 0x7800000000ull) {
                extern uint64_t renderer_present_count(void);
                fprintf(stderr, "[var] f%llu g %llx lr %llx hash %016llx val %016llx\n", (unsigned long long)renderer_present_count(),
                        (unsigned long long)x0, (unsigned long long)lr, (unsigned long long)*(uint64_t *)(uintptr_t)x1,
                        (unsigned long long)*(uint64_t *)(uintptr_t)(x1 + 8));
            }
        }
    }
    {   // LARECOMP_FNPROF_TL=<timeline update fn hex> (#224): each entry inside the window on a short timeline (end frame
        // +0x4c at most 2) logged with its state (+0x98), time (+0x90, 30 fps frames) and caller - where a step is skipped
        static int64_t tlfn = -1;
        if (tlfn < 0) tlfn = getenv("LARECOMP_FNPROF_TL") ? (int64_t)strtoull(getenv("LARECOMP_FNPROF_TL"), NULL, 16) : 0;
        if (tlfn && a == (uint64_t)tlfn && x0 > 0x7000000000ull && x0 < 0x7800000000ull) {
            int32_t end = *(volatile int32_t *)(uintptr_t)(x0 + 0x4c);
            if (end >= 0 && end <= 2) {
                extern uint64_t renderer_present_count(void);
                fprintf(stderr, "[tl] f%llu tl %llx lr %llx end %d state %u time %g\n", (unsigned long long)renderer_present_count(),
                        (unsigned long long)x0, (unsigned long long)lr, end, *(volatile uint32_t *)(uintptr_t)(x0 + 0x98),
                        (double)*(volatile float *)(uintptr_t)(x0 + 0x90));
            }
        }
    }
    uint32_t h = (uint32_t)((a >> 2) * 0x9E3779B1u) & (NSLOT - 1);
    for (int n = 0; n < NSLOT; n++, h = (h + 1) & (NSLOT - 1)) {
        uint64_t cur = atomic_load_explicit(&slots[h].addr, memory_order_relaxed);
        if (cur == a || (cur == 0 && atomic_compare_exchange_strong(&slots[h].addr, &cur, a)) || cur == a) {
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
    }
}

static int cmp(const void *x, const void *y) {
    uint64_t a = ((const uint64_t *)x)[1], b = ((const uint64_t *)y)[1];
    return a < b ? 1 : a > b ? -1 : 0;
}

#ifdef _WIN32
#include <windows.h>
// (debug, uncommitted) LARECOMP_SCAN=<frame>:<hex bytes>: at that frame, log every guest address holding the bytes
static void dbg_scan(uint64_t frame) {
    static long at = -2;
    static uint8_t pat[64];
    static int np;
    if (at == -2) {
        at = -1;
        const char *e = getenv("LARECOMP_SCAN");
        const char *c = e ? strchr(e, ':') : NULL;
        if (c) {
            at = atol(e);
            for (c++; c[0] && c[1] && np < 64; c += 2) { unsigned v; sscanf(c, "%2x", &v); pat[np++] = (uint8_t)v; }
        }
    }
    if (at < 0 || (long)frame != at) return;
    at = -1;
    int hits = 0;
    for (uint64_t a = 0x7000000000ull; a < 0x7800000000ull && hits < 50;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint64_t p = a; p + np <= end && hits < 50; p += 8)
                if (!memcmp((void *)(uintptr_t)p, pat, np)) {
                    LOG("[scan] frame %llu: %llx\n", (unsigned long long)frame, (unsigned long long)p);
                    if (!hits && getenv("LARECOMP_SCAN_WATCH")) {  // <hex delta>,<hex size>: watch [hit - delta, +size)
                        unsigned long long d = 0, sz = 0x48;
                        sscanf(getenv("LARECOMP_SCAN_WATCH"), "%llx,%llx", &d, &sz);
                        extern uint64_t g_watch_lo, g_watch_hi;
                        extern void watch_arm(void);
                        g_watch_lo = p - d, g_watch_hi = p - d + sz;
                        watch_arm();
                        LOG("[scan] watching %llx-%llx\n", (unsigned long long)g_watch_lo, (unsigned long long)g_watch_hi);
                    }
                    hits++;
                }
        a = end;
    }
    LOG("[scan] frame %llu done, %d hits\n", (unsigned long long)frame, hits);
}
#else
static void dbg_scan(uint64_t frame) { (void)frame; }
#endif

#ifdef _WIN32
// LARECOMP_SLOT_SCAN=<frame> (#224): at that frame, every guest address holding the first LARECOMP_FNPROF_BT function's
// x0 as a pointer is logged, and the first one outside that object is watched (who sets / clears the slot)
static uint64_t slot_at[8], slot_val[8];
static void slot_monitor(uint64_t frame) {  // (each frame after the scan: a slot's value when it changes)
    {   // LARECOMP_SLOT_DUMP=<from>:<to> (#224): the object slot 0 points to, 0x400 bytes, each frame -> build/st_<frame>.bin
        static long a0 = -2, a1;
        if (a0 == -2) { a0 = -1; const char *e = getenv("LARECOMP_SLOT_DUMP"); if (e) sscanf(e, "%ld:%ld", &a0, &a1); }
        uint64_t o = slot_at[0] ? *(volatile uint64_t *)(uintptr_t)slot_at[0] : 0;
        if (a0 >= 0 && (long)frame >= a0 && (long)frame <= a1 && o >= 0x7000000000ull && o < 0x7800000000ull) {
            char path[64];
            snprintf(path, sizeof path, "build/st_%llu.bin", (unsigned long long)frame);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(&o, 8, 1, f); fwrite((const void *)(uintptr_t)o, 1, 0x400, f); fclose(f); }
            uint64_t g = *(volatile uint64_t *)(uintptr_t)(o + 0x58);  // (the state's graph instance: its variables)
            if (g >= 0x7000000000ull && g < 0x7800000000ull) {
                snprintf(path, sizeof path, "build/gr_%llu.bin", (unsigned long long)frame);
                f = fopen(path, "wb");
                if (f) { fwrite(&g, 8, 1, f); fwrite((const void *)(uintptr_t)g, 1, 0x1000, f); fclose(f); }
            }
        }
    }
    for (int i = 0; i < 8 && slot_at[i]; i++) {
        uint64_t v = *(volatile uint64_t *)(uintptr_t)slot_at[i];
        if (v != slot_val[i]) {
            LOG("[slot] frame %llu: %llx %llx -> %llx\n", (unsigned long long)frame, (unsigned long long)slot_at[i],
                (unsigned long long)slot_val[i], (unsigned long long)v);
            slot_val[i] = v;
        }
    }
}
static int ok_ptr(uint64_t p) { return p >= 0x7000000000ull && p < 0x7800000000ull && !(p & 7); }
static void slot_scan(uint64_t frame) {
    slot_monitor(frame);
    {   // LARECOMP_INTENT_MON=<frame> (#224): from that frame, the BT function's x0 is the player's state manager M; the
        // entity E holds it at +0x2090; E's +0x2060 component C: mask +0xa8, list +0xa0 -> [0] -> +0x2c (move intent)
        static long im = -2;
        static uint64_t ent;
        static char last[160];
        if (im == -2) im = getenv("LARECOMP_INTENT_MON") ? atol(getenv("LARECOMP_INTENT_MON")) : -1;
        if (im >= 0 && (long)frame >= im && g_bt_first_x0 && !ent) {
            uint64_t m = g_bt_first_x0;
            for (uint64_t a = 0x7000000000ull; a < 0x7800000000ull && !ent;) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
                uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
                if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                    for (uint64_t p = a; p + 8 <= end && !ent; p += 8)
                        if (*(const uint64_t *)(uintptr_t)p == m && p >= a + 0x2090 && ok_ptr(*(const uint64_t *)(uintptr_t)(p - 0x2090 + 0x2060)))
                            ent = p - 0x2090;
                a = end;
            }
            LOG("[intent] frame %llu: manager %llx -> entity %llx\n", (unsigned long long)frame, (unsigned long long)m, (unsigned long long)ent);
        }
        if (ent) {
            uint64_t cc = *(volatile uint64_t *)(uintptr_t)(ent + 0x2060), mask = 0, l = 0, q = 0, r = 0;
            float s6 = -1;
            if (ok_ptr(cc)) {
                mask = *(volatile uint64_t *)(uintptr_t)(cc + 0xa8);
                l = *(volatile uint64_t *)(uintptr_t)(cc + 0xa0);
                if (ok_ptr(l)) q = *(volatile uint64_t *)(uintptr_t)l;
                if (ok_ptr(q)) r = *(volatile uint64_t *)(uintptr_t)q;
                if (ok_ptr(r)) s6 = *(volatile float *)(uintptr_t)(r + 0x2c);
            }
            char now[160];
            snprintf(now, sizeof now, "comp %llx mask %llx list %llx -> %llx -> %llx intent %g", (unsigned long long)cc,
                     (unsigned long long)mask, (unsigned long long)l, (unsigned long long)q, (unsigned long long)r, (double)s6);
            if (strcmp(now, last)) { LOG("[intent] frame %llu: %s\n", (unsigned long long)frame, now); strcpy(last, now); }
        }
    }
    {   // LARECOMP_COMP_WATCH=<frame> (#224): the BT function's x0 is an entity whose components are the pointer
        // vector [x0+0x60, x0+0x68); each component's +0x60 (its running sub-object) monitored from that frame
        static long cw = -2;
        if (cw == -2) cw = getenv("LARECOMP_COMP_WATCH") ? atol(getenv("LARECOMP_COMP_WATCH")) : -1;
        if (cw >= 0 && (long)frame >= cw && g_bt_first_x0) {
            uint64_t b = *(uint64_t *)(uintptr_t)(g_bt_first_x0 + 0x60), e = *(uint64_t *)(uintptr_t)(g_bt_first_x0 + 0x68);
            if (b < e) cw = -1;  // (none yet: tried again next frame)
            int k = 0;
            LOG("[comp] frame %llu: entity %llx, components %llx-%llx\n", (unsigned long long)frame,
                (unsigned long long)g_bt_first_x0, (unsigned long long)b, (unsigned long long)e);
            for (uint64_t p = b; p < e && k < 8; p += 8, k++) {
                uint64_t comp = *(uint64_t *)(uintptr_t)p;
                slot_at[k] = comp + 0x60, slot_val[k] = *(uint64_t *)(uintptr_t)(comp + 0x60);
                LOG("[comp] frame %llu: component %d at %llx (vtable %llx), +0x60 = %llx\n", (unsigned long long)frame, k,
                    (unsigned long long)comp, (unsigned long long)*(uint64_t *)(uintptr_t)comp, (unsigned long long)slot_val[k]);
            }
            if (getenv("LARECOMP_COMP_WATCH_SLOT0") && slot_at[0]) {  // (who writes component 0's running state)
                extern uint64_t g_watch_lo, g_watch_hi;
                extern void watch_arm(void);
                char spec[64];
                g_watch_lo = slot_at[0], g_watch_hi = slot_at[0] + 8;
                snprintf(spec, sizeof spec, "LARECOMP_WATCH_ADDR=%llx,%llx", (unsigned long long)g_watch_lo, (unsigned long long)g_watch_hi);
                putenv(strdup(spec));
                watch_arm();
            }
        }
    }
    static long at = -2;
    if (at == -2) at = getenv("LARECOMP_SLOT_SCAN") ? atol(getenv("LARECOMP_SLOT_SCAN")) : -1;
    if (at < 0 || (long)frame != at || !g_bt_first_x0) return;
    at = -1;
    uint64_t v = g_bt_first_x0, first = 0;
    int hits = 0;
    for (uint64_t a = 0x7000000000ull; a < 0x7800000000ull && hits < 40;) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint64_t p = a; p + 8 <= end && hits < 40; p += 8)
                if (*(const uint64_t *)(uintptr_t)p == v) {
                    LOG("[slot] frame %llu: %llx holds %llx\n", (unsigned long long)frame, (unsigned long long)p, (unsigned long long)v);
                    if (!first && (p < v || p >= v + 0x200) && hits >= (getenv("LARECOMP_SLOT_PICK") ? atoi(getenv("LARECOMP_SLOT_PICK")) : 0))
                        first = p;  // (LARECOMP_SLOT_PICK=<n>: watch the n-th hit)
                    if (hits < 8) slot_at[hits] = p, slot_val[hits] = v;
                    hits++;
                }
        a = end;
    }
    if (first) {
        extern uint64_t g_watch_lo, g_watch_hi;
        extern void watch_arm(void);
        char spec[64];
        g_watch_lo = first, g_watch_hi = first + 8;
        snprintf(spec, sizeof spec, "LARECOMP_WATCH_ADDR=%llx,%llx", (unsigned long long)first, (unsigned long long)first + 8);
        putenv(strdup(spec));
        watch_arm();
        LOG("[slot] watching %llx\n", (unsigned long long)first);
    }
}
#else
static void slot_scan(uint64_t frame) { (void)frame; }
#endif

void fnprof_frame(uint64_t frame) {
    dbg_scan(frame);
    slot_scan(frame);
    {   // LARECOMP_DUMP=<frame>:<hex addr>:<hex size>[;...] (#224): guest memory written to build/dump_<frame>_<addr>.bin
        static char spec[512];
        static int init;
        if (!init) { init = 1; const char *e = getenv("LARECOMP_DUMP"); if (e) snprintf(spec, sizeof spec, "%s", e); }
        for (const char *p = spec; *p;) {
            unsigned long long fr = 0, ad = 0, sz = 0;
            char as[24] = "";
            bool ok = sscanf(p, "%llu:%23[^:]:%llx", &fr, as, &sz) == 3;
            ad = !strcmp(as, "bt") ? g_bt_first_x0 : strtoull(as, NULL, 16);  // (bt: the first LARECOMP_FNPROF_BT function's x0)
            if (ok && fr == frame && ad >= 0x7000000000ull && ad + sz < 0x7800000000ull) {
                char path[160];
                snprintf(path, sizeof path, "build/dump_%llu_%llx.bin", fr, ad);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite((const void *)(uintptr_t)ad, 1, sz, f); fclose(f); }
                LOG("[dump] frame %llu: %llx+%llx -> %s\n", fr, ad, sz, path);
            }
            p = strchr(p, ';');
            if (!p) break;
            p++;
        }
    }
    static long from = -2, to;
    if (from == -2) {
        from = -1;
        const char *e = getenv("LARECOMP_FNPROF");
        if (e && sscanf(e, "%ld:%ld", &from, &to) != 2) from = -1;
    }
    if (from < 0) return;
    if ((long)frame == from) { g_fnprof = 1; LOG("[fnprof] on at frame %llu\n", (unsigned long long)frame); }
    if ((long)frame != to) return;
    g_fnprof = 0;
    uint64_t (*rows)[2] = malloc(sizeof(uint64_t[2]) * NSLOT);
    int n = 0;
    for (int i = 0; i < NSLOT; i++)
        if (slots[i].addr) { rows[n][0] = slots[i].addr; rows[n][1] = slots[i].count; n++; }
    qsort(rows, n, sizeof rows[0], cmp);
    const char *out = getenv("LARECOMP_FNPROF_OUT") ? getenv("LARECOMP_FNPROF_OUT") : "build/fnprof.txt";  // (parallel runs)
    FILE *f = fopen(out, "w");
    for (int i = 0; f && i < n; i++) fprintf(f, "%llx %llu\n", (unsigned long long)rows[i][0], (unsigned long long)rows[i][1]);
    if (f) fclose(f);
    free(rows);
    LOG("[fnprof] %d functions, written %s (frames %ld-%ld)\n", n, out, from, to);
    from = -1;
}

// LARECOMP_FNPROF=0:<to>: profile from process start (threads the game starts before the first frame)
__attribute__((constructor)) static void fnprof_early(void) {
    const char *e = getenv("LARECOMP_FNPROF");
    if (e && e[0] == '0' && e[1] == ':') g_fnprof = 1;
}
