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
// Addresses by the main module's build ID; another version gets no hooks (and the login fails as before, cleanly).
#include <string.h>
#include <time.h>

#include "../game_addresses.h"
#include "../hle.h"

typedef struct {
    uint8_t id[8];
    uint64_t npln_login_update;  // the login request's update (vtable main+0x3c13..., state at +0x50: 2 = done, success)
} MgAddrs;

static const MgAddrs k_mg[] = {
    {{0xB1, 0xF1, 0x2F, 0xD9, 0x19, 0xEA, 0xE8, 0x6A}, 0x191f284},  // 2.0.2
};

static const MgAddrs *g_mg;
static GuestFn g_login_orig;

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
    for (size_t i = 0; i < sizeof k_mg / sizeof *k_mg; i++)
        if (!memcmp(g_main_build_id, k_mg[i].id, 8)) g_mg = &k_mg[i];
    if (!g_mg || !g_nmodules) {
        LOG("[mystery-gift] no hooks for this game version: the login to the game servers will fail\n");
        return;
    }
    uint64_t addr = g_modules[0].base + g_mg->npln_login_update;
    g_login_orig = dispatch_lookup(addr);
    if (!g_login_orig) { LOG("[mystery-gift] login update not found at %llx\n", (unsigned long long)addr); return; }
    hle_override(addr, mg_login_update, "mystery_gift_login");
    LOG("[mystery-gift] hooks on (login skipped, gifts from mystery_gift/bcat)\n");
}
