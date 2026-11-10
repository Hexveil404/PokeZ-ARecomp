// Code mods' hooks from their writes (codehooks.h): the logic, on the runtime through HookOps (codehooks_rt.c the
// game's; local-tests/codehooks_test.c fakes). What was written is found by comparing each view (kernel.h CodeAlias)
// with the code it copies; the lifted code itself never changes, so every pass starts from the original.
#include "codehooks.h"

#include <string.h>

#define LDR_X16_8 0x58000050u  // ldr x16, #8
#define LDR_X17_8 0x58000051u  // ldr x17, #8 (exlaunch)
#define BR_X16 0xd61f0200u
#define BR_X17 0xd61f0220u
#define NOP 0xd503201fu

static int far_jump(const uint32_t *w, int n, uint64_t *dest) {
    if (n < 4 || !((w[0] == LDR_X17_8 && w[1] == BR_X17) || (w[0] == LDR_X16_8 && w[1] == BR_X16))) return 0;
    *dest = (uint64_t)w[2] | (uint64_t)w[3] << 32;
    return 4;
}

int jump_at(const uint32_t *w, int nwords, uint64_t addr, uint64_t *dest) {
    if (nwords >= 1 && (w[0] & 0xFC000000u) == 0x14000000u) {  // b imm26
        int64_t imm = w[0] & 0x3FFFFFF;
        if (imm & 0x2000000) imm -= 0x4000000;
        *dest = addr + (uint64_t)(imm * 4);
        return 1;
    }
    return far_jump(w, nwords, dest);
}

int hook_decode(const uint32_t *old_w, const uint32_t *new_w, int nwords, uint64_t site, uint64_t *target) {
    (void)old_w;
    if (nwords >= 5 && new_w[0] == NOP && far_jump(new_w + 1, nwords - 1, target)) return 5;
    return jump_at(new_w, nwords, site, target);
}

// ---- what's in so far
typedef struct { uint64_t site; int words, mod; GuestFn target, orig; uint64_t view; } Hook;  // (view: the one it was seen in)
static Hook hooks[256];
static int nhooks;
static uint64_t stubs[512], mid_told[256];
static int nstubs, ntold;

void hooks_reset(void) { nhooks = nstubs = ntold = 0; }

static Hook *hook_of(uint64_t site, int mod) {
    for (int i = 0; i < nhooks; i++)
        if (hooks[i].site == site && hooks[i].mod == mod) return &hooks[i];
    return NULL;
}
static Hook *last_hook_at(uint64_t site) {
    for (int i = nhooks - 1; i >= 0; i--)
        if (hooks[i].site == site) return &hooks[i];
    return NULL;
}
static GuestFn past_pad(GuestFn f) { return (GuestFn)(void *)((const char *)(const void *)f + ENTRY_PAD); }
static bool told_once(uint64_t site) {  // (a pass runs at every unmap: each case logged once)
    for (int j = 0; j < ntold; j++)
        if (mid_told[j] == site) return true;
    if (ntold < (int)(sizeof mid_told / sizeof *mid_told)) mid_told[ntold++] = site;
    return false;
}

// What runs at a hooked site from now on: `to` through the entry pad, slot_to through the host function's slot.
static bool point_site(uint64_t site, GuestFn to, GuestFn slot_to, const HookOps *ops) {
    GuestFn f = ops->lifted(site);
    const char *hn = ops->hle_name(site);
    if (f && !ops->patch_entry(f, to)) return false;
    if (hn) ops->hle_swap(hn, slot_to);
    return true;
}

// Hooks taken out: the view a hook was seen in, still mapped, holds the original words there again (only that view -
// another one, a whole .text copy say, showing the original says nothing about writes made through others). The site
// runs what it ran before: the hook before it (another mod's), else the function. shortcut: only the last hook at a
// site (two mods on one function, the first taking its hook out: left in, logged); keep a chain if a mod needs it.
static void game_view_removed(const CodeAlias *a, const HookOps *ops, HookStats *st) {
    const uint32_t *now = (const uint32_t *)(uintptr_t)a->dst, *was = (const uint32_t *)(uintptr_t)a->src;
    uint64_t g0 = ops->guest(a->src);
    for (int i = nhooks - 1; i >= 0; i--) {
        Hook *h = &hooks[i];
        if (h->view != a->dst || h->site < g0 || h->site + 4ull * h->words > g0 + a->size) continue;
        uint64_t k = (h->site - g0) / 4;
        if (memcmp(now + k, was + k, 4u * h->words)) continue;
        if (last_hook_at(h->site) != h) {
            if (!told_once(h->site))
                ops->log("[mods] hook %llx taken out by mod %d: left in (another mod hooked it after)\n",
                         (unsigned long long)h->site, h->mod);
            continue;
        }
        Hook *prev = NULL;
        for (int j = 0; j < i; j++)
            if (hooks[j].site == h->site) prev = &hooks[j];
        GuestFn f = ops->lifted(h->site);
        if (!point_site(h->site, prev ? prev->target : f ? past_pad(f) : NULL, h->orig, ops)) continue;
        ops->log("[mods] hook %llx taken out (mod %d)\n", (unsigned long long)h->site, h->mod);
        st->removed++;
        memmove(h, h + 1, (size_t)(nhooks - i - 1) * sizeof *h);
        nhooks--;
    }
}

// the game's code: a branch written at a lifted function's start -> that function's pad jumps to the mod's callback
static void game_view(const CodeAlias *a, const HookOps *ops, HookStats *st) {
    const uint32_t *now = (const uint32_t *)(uintptr_t)a->dst, *was = (const uint32_t *)(uintptr_t)a->src;
    uint64_t g0 = ops->guest(a->src);
    int words = (int)(a->size / 4);
    for (int k = 0; k < words; k++) {
        if (now[k] == was[k]) continue;
        uint64_t site = g0 + 4ull * k, tgt;
        int w = hook_decode(was + k, now + k, words - k, site, &tgt);
        GuestFn f = w ? ops->lifted(site) : NULL;
        // (or a host function's stand-in address, no lifted code: what the loader resolves its symbol to - Lumiose
        // Online's far hooks on nn::nifm / nn::socket, one 16-byte slot each)
        const char *hn = w ? ops->hle_name(site) : NULL;
        if (f || hn) {
            // (not code the mod generates there while running: exlaunch's inline-hook thunks, in a pool of zeros the
            // generator still made a function of - running it hit the zeros' udf; written after the branch to it)
            GuestFn to = ops->lifted(tgt);  // (blank only asked of a lifted address: inside a loaded module)
            if (to && ops->blank(tgt)) to = NULL;
            int mod = ops->mod_of(tgt);
            Hook *h = to ? hook_of(site, mod) : NULL;
            if (!to) {
                st->unknown++;
                ops->log("[mods] hook at %llx -> %llx: no lifted function there, not applied\n", (unsigned long long)site,
                         (unsigned long long)tgt);
            } else if (h && h->target == to) {
                h->view = a->dst;  // (seen again: this view now holds it)
            } else if (h) {  // the mod moved its hook: the same original, another target
                if (last_hook_at(site) != h) {
                    if (!told_once(site))
                        ops->log("[mods] hook %llx moved by mod %d: left as it was (another mod hooked it after)\n",
                                 (unsigned long long)site, mod);
                } else if (point_site(site, to, to, ops)) {
                    h->target = to, h->words = w, h->view = a->dst;
                    st->applied++;
                    ops->log("[mods] hook %llx moved -> %llx (mod %d)\n", (unsigned long long)site, (unsigned long long)tgt, mod);
                }
            } else if (nhooks < (int)(sizeof hooks / sizeof *hooks)) {
                // the original: the hook before it (a second mod on the same function), else the function itself -
                // a host-implemented SDK function's host code, not its lifted code (which for nvnBootstrapLoader is
                // the console's own NVN: the Debug Menu's hook called it and NvOsDrvOpen aborted)
                Hook *prev = last_hook_at(site);
                if (f && !ops->patch_entry(f, to)) {  // (no pad, no memory for a thunk: not recorded, tried again)
                    st->failed++;
                    if (!told_once(site))
                        ops->log("[mods] hook %llx -> %llx: its function's entry couldn't be patched, not applied\n",
                                 (unsigned long long)site, (unsigned long long)tgt);
                } else {
                    GuestFn was_slot = hn ? ops->hle_swap(hn, to) : NULL;  // (the game's calls to it go through its slot)
                    hooks[nhooks++] = (Hook){site, w, mod, to, prev ? prev->target : was_slot ? was_slot : past_pad(f), a->dst};
                    st->applied++;
                    ops->log("[mods] hook %llx -> %llx (mod %d, %d words%s)\n", (unsigned long long)site,
                             (unsigned long long)tgt, mod, w, hn ? ", a host function's slot too" : "");
                }
            }
            k += w - 1;
            continue;
        }
        st->mid++;
        if (!told_once(site))
            ops->log("[mods] write at %llx (%08x -> %08x): mid-function or not a hook, not applied\n",
                     (unsigned long long)site, was[k], now[k]);
        while (k + 1 < words && now[k + 1] != was[k + 1]) k++;
    }
}

// a mod's own code: its trampolines ("call the original"), each a few instructions ending in a jump back to a hooked
// site past the words its hook took -> a call to the stub runs the original (the lifted function past its pad, or the
// hook before it)
static void mod_view(const CodeAlias *a, int mod, const HookOps *ops, HookStats *st) {
    const uint32_t *now = (const uint32_t *)(uintptr_t)a->dst, *was = (const uint32_t *)(uintptr_t)a->src;
    uint64_t g0 = ops->guest(a->src);
    int words = (int)(a->size / 4), start = -1, floor = 0;  // (floor: past the stub before)
    for (int k = 0; k < words; k++) {
        if (now[k] == was[k]) { start = -1; continue; }
        if (start < 0) start = k;
        uint64_t dest, at = g0 + 4ull * k;
        int w = jump_at(now + k, words - k, at, &dest);
        if (!w) continue;
        if (dest > at && dest <= at + 4 * 32) { k += w - 1; continue; }  // (a branch inside the stub: over a relocated
        // instruction's literal - exlaunch's `ldr x16, #8; b +12; .quad page` for a hooked adrp - not its end)
        for (int i = 0; i < nhooks; i++) {
            if (hooks[i].mod != mod || dest != hooks[i].site + 4ull * hooks[i].words) continue;
            // (it holds at least the words the hook replaced, relocated: those can equal the pool's zeros - a host
            // function's stand-in slot is zeros - so the stub may start before the first word that changed)
            int first = start < k - hooks[i].words ? start : k - hooks[i].words;
            uint64_t stub = g0 + 4ull * (first > floor ? first : floor);
            bool known = false;
            for (int j = 0; j < nstubs && !known; j++) known = stubs[j] == stub;
            if (!known && nstubs < (int)(sizeof stubs / sizeof *stubs)) {
                stubs[nstubs++] = stub;
                ops->override(stub, hooks[i].orig);
                st->stubs++;
            }
            break;
        }
        k += w - 1;
        start = -1;
        floor = k + 1;
    }
}

void hooks_apply(const CodeAlias *al, int n, const HookOps *ops, HookStats *st) {
    memset(st, 0, sizeof *st);
    for (int i = 0; i < n; i++)
        if (ops->mod_of(ops->guest(al[i].src)) < 0) game_view_removed(&al[i], ops, st), game_view(&al[i], ops, st);
    for (int i = 0; i < n; i++) {
        int mod = ops->mod_of(ops->guest(al[i].src));
        if (mod >= 0) mod_view(&al[i], mod, ops, st);
    }
}
