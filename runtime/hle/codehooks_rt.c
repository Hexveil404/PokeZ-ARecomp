// Code mods' hooks in the running game (codehooks.h): the views the kernel recorded (kernel.h CodeAlias) handed to the
// hook logic (codehooks.c) with the runtime's own lookups, and the host side of a hook - a jump written over a lifted
// function's 5-byte entry pad (tools/gen.py FN_ATTR) to a small thunk (movabs rax, <hook>; jmp rax), within the
// 2 GB a rel32 reaches. hooks_capture runs after the code mods' start code (main.c rtld_init_sdk) and at every
// svcUnmapProcessMemory (exlaunch / Hakkun map a view per hook, write the branch, unmap it).
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "../hle.h"
#include "../kernel.h"
#include "../rt.h"
#include "codehooks.h"

HostFn hle_swap_impl(const char *name, HostFn fn);  // dispatch.c
const char *loader_symbol_at(uint64_t addr, uint64_t *offset);

static uint64_t identity(uint64_t host) { return host; }

static int mod_of(uint64_t a) {
    for (int i = 2; i < g_nmodules; i++)
        if (a >= g_modules[i].base && a < g_modules[i].base + g_modules[i].size) return i - 2;
    return -1;
}

static const char *hle_name(uint64_t a) {
    extern const char *hle_name_at(uint64_t pc);  // dispatch.c: a stand-in address (a host function's symbol)
    if (a >= HLE_BASE && a < HLE_BASE + HLE_SIZE) return a & 15 ? NULL : hle_name_at(a);
    if (a < g_modules[1].base || a >= g_modules[1].base + g_modules[1].size) return NULL;  // (the SDK's only)
    uint64_t off = 1;
    const char *s = loader_symbol_at(a, &off);
    extern bool net_mod_only(const char *name);  // (hle/net_socket.c: the SDK's own code for the game, not the host's)
    return s && !off && hle_lookup_impl(s) && !net_mod_only(s) ? s : NULL;
}

// What a boot's hooks changed outside guest memory, undone by a reboot (back to the start screen, then Play: the next
// boot's mods may differ, and their start code must run before any hook does): the host functions swapped, in order,
// and the lifted functions' entries patched.
static struct { char name[160]; GuestFn was; } swaps[256];
static int nswaps;
static uint8_t *patched[256];
static int npatched;

static GuestFn hle_swap(const char *name, GuestFn fn) {
    GuestFn was = hle_swap_impl(name, fn);
    if (nswaps < 256) snprintf(swaps[nswaps].name, sizeof swaps[nswaps].name, "%s", name), swaps[nswaps++].was = was;
    return was;
}

static void override(uint64_t addr, GuestFn fn) { hle_override(addr, fn, "<code mod trampoline>"); }

// ---- the entry pad: 0f 1f 44 00 08 (nopl 8(%rax,%rax,1)), as clang emits patchable_function_entry(5, 0)
static const uint8_t PAD[ENTRY_PAD] = {0x0f, 0x1f, 0x44, 0x00, 0x08};
static uint8_t *thunks;
static size_t thunks_used;
#define THUNK_PAGE 0x10000

static void *alloc_near(uintptr_t close_to) {
    uintptr_t base = close_to & ~(uintptr_t)(THUNK_PAGE - 1);
    for (uintptr_t d = THUNK_PAGE; d < 0x70000000u; d += THUNK_PAGE)
        for (int s = 0; s < 2; s++) {
            uintptr_t at = s ? base + d : base - d;
#ifdef _WIN32
            void *p = VirtualAlloc((void *)at, THUNK_PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
#else
            void *p = mmap((void *)at, THUNK_PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED) continue;
            if ((uintptr_t)p == at) return p;
            munmap(p, THUNK_PAGE);  // (the hint not taken)
#endif
        }
    return NULL;
}

static bool entry_store(uint8_t *p, const uint8_t five[ENTRY_PAD]) {
    uint64_t q;
    memcpy(&q, p, 8);  // (the pad and 3 bytes after it, one aligned word: one store)
    memcpy(&q, five, ENTRY_PAD);
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    InterlockedExchange64((volatile LONG64 *)p, (LONG64)q);
    VirtualProtect(p, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 8);
#else
    uintptr_t pg = (uintptr_t)p & ~(uintptr_t)(sysconf(_SC_PAGESIZE) - 1);
    if (mprotect((void *)pg, (uintptr_t)p + 8 - pg, PROT_READ | PROT_WRITE | PROT_EXEC)) return false;
    __atomic_store_n((uint64_t *)p, q, __ATOMIC_SEQ_CST);
    mprotect((void *)pg, (uintptr_t)p + 8 - pg, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + 8);
#endif
    return true;
}

// (false: no entry pad - game code built before code mods - or no thunk memory; the callers say so, once)
static bool patch_entry(GuestFn f, GuestFn to) {
    uint8_t *p = (uint8_t *)(void *)f;
    if (((uintptr_t)p & 7) || (memcmp(p, PAD, ENTRY_PAD) && p[0] != 0xE9)) return false;  // (0xE9: hooked before)
    if (!thunks || thunks_used + 16 > THUNK_PAGE) {
        thunks = alloc_near((uintptr_t)p), thunks_used = 0;
        if (!thunks) { LOG("[mods] no memory near the game code for a hook's thunk\n"); return false; }
    }
    uint8_t *t = thunks + thunks_used;
    int64_t rel = (int64_t)(t - (p + 5));
    if (rel != (int32_t)rel) { thunks = NULL; return patch_entry(f, to); }  // (a page too far: another near this one)
    thunks_used += 16;
    t[0] = 0x48, t[1] = 0xB8;  // movabs rax, to
    uint64_t dest = (uint64_t)(uintptr_t)(void *)to;
    memcpy(t + 2, &dest, 8);
    t[10] = 0xFF, t[11] = 0xE0;  // jmp rax (rax: scratch at a function's entry, rcx / rdi - the Cpu - untouched)
    uint8_t jmp[ENTRY_PAD] = {0xE9};
    int32_t r32 = (int32_t)rel;
    memcpy(jmp + 1, &r32, 4);
    if (!entry_store(p, jmp)) return false;
    bool seen = false;
    for (int i = 0; i < npatched; i++) seen |= patched[i] == p;
    if (!seen && npatched < 256) patched[npatched++] = p;
    return true;
}

RT_ON_RESET(codehooks_reset) {
    for (int i = npatched; i-- > 0;) entry_store(patched[i], PAD);
    for (int i = nswaps; i-- > 0;) hle_swap_impl(swaps[i].name, swaps[i].was);
    if (npatched || nswaps) LOG("[mods] reboot: %d entries and %d host functions restored\n", npatched, nswaps);
    npatched = nswaps = 0;
    hooks_reset();
}

// A runtime feature's own hook (not a mod's: Pokémon Z-A's ultrawide view, hle/widescreen.c): the lifted function at
// guest addr runs `to` from now on (until a reboot, codehooks_reset); `to` runs the original through the returned
// body (the function past its pad). NULL: no lifted function there, or no entry pad (game code built before code mods).
static pthread_mutex_t hooks_lock = PTHREAD_MUTEX_INITIALIZER;  // (the thunks and patched[]: one writer at a time)
GuestFn codehooks_entry(uint64_t addr, GuestFn to) {
    GuestFn f = dispatch_lookup(addr);
    pthread_mutex_lock(&hooks_lock);
    bool ok = f && patch_entry(f, to);
    pthread_mutex_unlock(&hooks_lock);
    return ok ? (GuestFn)(void *)((uint8_t *)(void *)f + ENTRY_PAD) : NULL;
}

static bool blank(uint64_t a) { return *(const uint32_t *)(uintptr_t)a == 0; }  // (the image: views are copies)
static const HookOps GAME_OPS = {identity, dispatch_lookup, mod_of, hle_name, hle_swap, patch_entry, override, rt_log, blank};

void hooks_capture(void) {
    if (g_nmodules <= 2) return;
    pthread_mutex_lock(&hooks_lock);  // (any thread's svcUnmapProcessMemory, and codehooks_entry: one at a time)
    CodeAlias al[MAX_CODE_ALIAS];
    int n = kernel_code_aliases(al, MAX_CODE_ALIAS);
    HookStats st;
    hooks_apply(al, n, &GAME_OPS, &st);
    pthread_mutex_unlock(&hooks_lock);
    if (st.applied || st.mid || st.unknown || st.stubs || st.failed || st.removed)
        LOG("[mods] hooks: applied %d, mid-function %d, unknown target %d, trampolines %d, not patched %d, taken out %d "
            "(%d views)\n", st.applied, st.mid, st.unknown, st.stubs, st.failed, st.removed, n);
}

// (the dispatch miss inside a code mod, before its FATAL: what the mod wrote there, for a stub hooks_apply didn't match)
void hooks_explain(uint64_t pc) {
    CodeAlias al[MAX_CODE_ALIAS];
    int n = kernel_code_aliases(al, MAX_CODE_ALIAS);
    for (int i = 0; i < n; i++) {
        if (pc < al[i].src || pc + 64 > al[i].src + al[i].size) continue;
        const uint32_t *w = (const uint32_t *)(uintptr_t)(al[i].dst + (pc - al[i].src));
        LOG("[mods] %llx as written (view %llx): %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
            (unsigned long long)pc, (unsigned long long)al[i].dst, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9],
            w[10], w[11]);
    }
}
