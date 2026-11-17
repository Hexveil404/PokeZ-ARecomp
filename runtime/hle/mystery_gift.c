// Mystery Gift offline (Pokémon Z-A; g_settings.mystery_gift): Link Play > Mystery Gift without Nintendo's servers.
//
// Going online, the game runs a list of steps (the runner at main+0x1918aa8), each one a gflnet request polled through
// its vtable: the network environment (main+0x1914904, which also takes the account token: acc.c), then the login to
// the game servers (NPLN, main+0x191f284), then the gift download (BCAT: main+0x1910100, bcat.c). The login can't
// happen here - nothing speaks to Nintendo - so its request is put in the state it takes when the console is already
// logged in (2: complete with success) before its first update; the game's own code then marks it done and goes on to
// the next step. Nothing else is changed: the requests' results, the game's flags and the save are all the game's own.
//
// Receiving a gift stamps it with the network clock (nn::time::StandardNetworkSystemClock::GetCurrentTime, main
// +0x2dc69d0 on 2.0.2): a console that has never synced it with Nintendo's time server can't receive ("Something has
// gone wrong. We aren't able to retrieve that gift right now."). Here it is the PC's clock, like the user clock
// (services/time.c): the SDK's answer is logged once, then replaced with the host time and success.
//
// The login request's update is found by its code, on any game version (mg_login_find): the same function in 1.0.0
// (main+0x17d93f4) and 2.0.2 (main+0x191f284), instruction for instruction but for addresses. A version where it isn't
// found exactly once has no hook, and going online without it crashes the game at the login step (#316): there
// Mystery Gift stays off whatever the setting says (mystery_gift_supported, asked by misc.c nifm_online), so the
// game sees no connection, as it does with the setting off.
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "../hle.h"

static GuestFn g_login_orig;

// The login request's update, by its first 24 instructions with the addresses masked out (branch targets, adrp pages,
// load and store offsets, the frame size). Kept whole: ldr w8, [x19, #0x50]; cmp w8, #3; cmp w8, #2 - the state field
// and its values, which the hook relies on. 0 unless exactly one function matches.
static uint64_t g_login_found;

static void mg_login_scan(void) {
    static const uint32_t code[24] = {
        0xd10003ff, 0xa9087bfd, 0xf90003f5, 0xa90a4ff4, 0x910003fd, 0x90000008,
        0xaa0103f4, 0xf9400108, 0xb9400108, 0xd50339bf, 0x34000008, 0xaa0003f3,
        0x90000000, 0xf9400000, 0x94000000, 0x37000000, 0xb9405268, 0x71000d1f,
        0x54000000, 0x7100091f, 0x54000000, 0x35000008, 0x91000275, 0x94000000};
    static const uint32_t mask[24] = {
        0xffc003ff, 0xffffffff, 0xffc003ff, 0xffffffff, 0xffc003ff, 0x9f00001f,
        0xffffffff, 0xffc003ff, 0xffc003ff, 0xffffffff, 0xff00001f, 0xffffffff,
        0x9f00001f, 0xffc003ff, 0xfc000000, 0xfff8001f, 0xffffffff, 0xffffffff,
        0xff00001f, 0xffffffff, 0xff00001f, 0xff00001f, 0xffc003ff, 0xfc000000};
    const Module *m = &g_modules[0];
    const uint32_t *t = (const uint32_t *)(uintptr_t)(m->base + m->text_off);
    uint64_t n = m->text_size / 4, at = 0;
    int nat = 0;
    for (uint64_t i = 0; i + 24 <= n; i++) {
        if ((t[i] & mask[0]) != code[0] || (t[i + 1] & mask[1]) != code[1]) continue;
        int k = 2;
        while (k < 24 && (t[i + k] & mask[k]) == code[k]) k++;
        if (k == 24) at = i, nat++;
    }
    if (nat == 1) g_login_found = m->base + m->text_off + at * 4;
    LOG("[mystery-gift] login update: %d match%s in the game's code%s\n", nat, nat == 1 ? "" : "es",
        nat == 1 ? "" : " (not this version: Mystery Gift stays off)");
}

static uint64_t mg_login_find(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (!g_nmodules) return 0;
    pthread_once(&once, mg_login_scan);  // (the game's threads and the F1 menu ask at once: one scan, the others wait)
    return g_login_found;
}

// Whether this build of the game can go online for Mystery Gift: 1 yes, 0 no, -1 not known yet (no game loaded).
int mystery_gift_support(void) { return !g_nmodules ? -1 : mg_login_find() != 0; }
bool mystery_gift_supported(void) { return mystery_gift_support() == 1; }

static void mg_login_update(Cpu *c) {
    uint64_t self = c->x[0];
    uint32_t *state = (uint32_t *)(uintptr_t)(self + 0x50);
    if (*state != 2) {
        LOG("[mystery-gift] game-server login: skipped (state %u -> 2, done)\n", *state);
        *state = 2;
    }
    g_login_orig(c);
}

// The main module's import slot (GOT) for a function, from its PLT relocations; 0 if it doesn't import it. Its PLT stub
// reads the slot at every call (only setjmp's are lifted specially: tools/gen.py), so a new value takes effect at
// once - and an HLE address there is never cached by the dispatcher.
static uint64_t *mg_import_slot(const char *name) {
    if (!g_nmodules) return NULL;
    Module *m = &g_modules[0];
    uint64_t jmprel = 0, pltsz = 0;
    for (const uint64_t *d = (const uint64_t *)(uintptr_t)m->dynamic; d[0]; d += 2) {
        if (d[0] == 23) jmprel = m->base + d[1];  // DT_JMPREL
        if (d[0] == 2) pltsz = d[1];              // DT_PLTRELSZ
    }
    for (uint64_t i = 0; jmprel && i < pltsz / 24; i++) {
        const uint64_t *r = (const uint64_t *)(uintptr_t)(jmprel + i * 24);  // Elf64_Rela {offset, info, addend}
        if ((uint32_t)r[1] != 1026) continue;                               // R_AARCH64_JUMP_SLOT
        uint32_t st_name = *(const uint32_t *)(uintptr_t)(m->symtab + (r[1] >> 32) * 24);
        if (!strcmp((const char *)(uintptr_t)(m->strtab + st_name), name)) return (uint64_t *)(uintptr_t)(m->base + r[0]);
    }
    return NULL;
}

static uint64_t g_netclock_orig;  // the SDK function (guest address)
static void mg_network_clock_now(Cpu *c) {
    uint64_t out = c->x[0];
    static int n;
    if (g_netclock_orig && n < 3) {  // what the console's own clock code says (logged, then replaced)
        int64_t was = out ? *(int64_t *)(uintptr_t)out : 0;
        guest_call(c, g_netclock_orig);
        LOG("[mystery-gift] network clock: the SDK said result %x (time %lld); answered with the PC's clock\n",
            (unsigned)c->x[0], out ? (long long)*(int64_t *)(uintptr_t)out : 0);
        if (out) *(int64_t *)(uintptr_t)out = was;
    }
    n++;
    if (out) *(int64_t *)(uintptr_t)out = (int64_t)time(NULL);
    c->x[0] = 0;  // ResultSuccess
}

void mystery_gift_hooks_install(void) {
    static int done;
    if (done++) return;
    // (any version: the import, by name)
    uint64_t *slot = mg_import_slot("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    if (slot) {
        g_netclock_orig = *slot;
        *slot = hle_register("mystery_gift_network_clock", mg_network_clock_now);
        LOG("[mystery-gift] network clock: the PC's clock (import slot %llx)\n", (unsigned long long)(uintptr_t)slot);
    }
    uint64_t addr = mg_login_find();
    if (!addr) return;  // (not reached on such a version: nifm_online is false there)
    g_login_orig = dispatch_lookup(addr);
    if (!g_login_orig) { LOG("[mystery-gift] login update not found at %llx\n", (unsigned long long)addr); return; }
    hle_override(addr, mg_login_update, "mystery_gift_login");
    LOG("[mystery-gift] hooks on (login skipped, gifts from mystery_gift/bcat)\n");
}
