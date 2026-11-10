// Code mods' hooks (runtime/hle/codehooks.c): what a mod wrote through its writable views of code (kernel.h CodeAlias)
// turned into host-side hooks. A branch written at a lifted function's start becomes a jump in that function's entry
// pad (ENTRY_PAD, tools/gen.py FN_ATTR); a call to the mod's "original" stub (its trampoline, in its own code) goes to
// the lifted function past the pad. Writes anywhere else (mid-function hooks, patched instructions) are counted and
// logged, not applied.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../cpu.h"
#include "../kernel.h"

// The words a hook at site took, with *target the branch's destination: 1 for `b imm26`, 4 for
// `ldr x17, #8; br x17; .quad T`, 5 for a `nop` then that (exlaunch's 8-byte alignment); 0 for anything else.
int hook_decode(const uint32_t *old_w, const uint32_t *new_w, int nwords, uint64_t site, uint64_t *target);
// A jump at w (guest address addr): its words (1 `b`, 4 `ldr x17, #8; br x17; .quad`), *dest its destination; 0: none.
int jump_at(const uint32_t *w, int nwords, uint64_t addr, uint64_t *dest);

// What the hook logic needs from the runtime (codehooks.c's own for the game; fakes in local-tests/codehooks_test.c).
typedef struct {
    uint64_t (*guest)(uint64_t host);                 // a view's address -> the guest address (the game: the same)
    GuestFn (*lifted)(uint64_t addr);                 // the lifted function starting at addr (any module); NULL: none
    int (*mod_of)(uint64_t addr);                     // the code mod whose image holds addr; -1: none (the game's)
    const char *(*hle_name)(uint64_t addr);           // the SDK function at addr is a host function: its name; NULL
    GuestFn (*hle_swap)(const char *name, GuestFn fn);  // that host function's slot runs fn from now on: the old one
    bool (*patch_entry)(GuestFn f, GuestFn to);       // f's entry pad jumps to `to`
    void (*override)(uint64_t addr, GuestFn fn);      // a call to guest address addr runs fn
    void (*log)(const char *fmt, ...);
    bool (*blank)(uint64_t addr);  // the original code at addr is 0 (udf #0): no real code, whatever was lifted there
} HookOps;

// (failed: its function's entry couldn't be patched - not recorded, tried again next pass; removed: taken out by the mod)
typedef struct { int applied, mid, unknown, stubs, failed, removed; } HookStats;

// Every view's writes, again from the start each time (only what's new is applied: a hook already in, a stub already
// registered are left as they are). st: totals of this pass.
void hooks_apply(const CodeAlias *al, int n, const HookOps *ops, HookStats *st);
void hooks_reset(void);  // (a reboot; tests)
