// Rounding controls must restore host state while preserving the guest's FPCR rules.
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include "../runtime/simd.h"
#if defined(__x86_64__) && defined(__SSE2__)
#include <xmmintrin.h>
#endif

static void *worker(void *argument) {
    const int host = *(int *)argument;
    assert(!fesetround(host));
    for (int repeat = 0; repeat < 100; repeat++) for (unsigned mode = 0; mode < 4; mode++) {
        Cpu c = {0}; c.fpcr = ((uint64_t)mode << 22) | FPCR_FZ;
        assert(fb_s(op_s(&c, FOP_ADD, 1.0f, 0x1p-24f, 0)) == (mode == 1 ? 0x3f800001u : 0x3f800000u));
        assert(fb_s(op_s(&c, FOP_ADD, -1.0f, -0x1p-24f, 0)) == (mode == 2 ? 0xbf800001u : 0xbf800000u));
        assert(fb_d(op_d(&c, FOP_ADD, 1.0, 0x1p-53, 0)) == (mode == 1 ? 0x3ff0000000000001ull : 0x3ff0000000000000ull));
        assert(fb_s(simd_d2s(&c, 1.0 + 0x1p-24)) == (mode == 1 ? 0x3f800001u : 0x3f800000u));
        assert(fb_s(fromint_s(&c, 16777217, 64, 1, 0)) == (mode == 1 ? 0x4b800001u : 0x4b800000u));
        assert(fb_d(fromint_d(&c, 9007199254740993ull, 64, 1, 0)) == (mode == 1 ? 0x4340000000000001ull : 0x4340000000000000ull));
        // (volatile: with FPCR.FZ alone ZA Recomp's simd.h takes the host's FTZ fast path, and two constants would be
        // multiplied by the compiler instead, without it - the game's operands are never compile-time constants)
        volatile float tiny = 0x1p-126f, half = 0.5f;
        assert(fb_s(op_s(&c, FOP_MUL, tiny, half, 0)) == 0);
        assert(simd_host_round() == host && fegetround() == host);
    }
    assert(!fesetround(FE_TONEAREST));
    return NULL;
}

int main(void) {
    int modes[4] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
#if defined(__x86_64__) && defined(__SSE2__)
    unsigned original = _mm_getcsr();
    for (unsigned controls = 0; controls < 4; controls++) {
        // Sticky exceptions and FTZ/DAZ belong to the host, not the rounding helper.
        unsigned before = original | _MM_EXCEPT_INVALID | _MM_EXCEPT_DIV_ZERO;
        before = (before & ~0x8040u) | ((controls & 1) ? 0x8000u : 0) | ((controls & 2) ? 0x40u : 0);
        _mm_setcsr(before);
        unsigned short x87_before, x87_after;
        __asm__ volatile("fnstcw %0" : "=m"(x87_before));
        for (int i = 0; i < 4; i++) {
            simd_set_host_round(modes[i]);
            assert(simd_host_round() == modes[i]);
            assert((_mm_getcsr() & ~_MM_ROUND_MASK) == (before & ~_MM_ROUND_MASK));
        }
        __asm__ volatile("fnstcw %0" : "=m"(x87_after));
        assert(x87_before == x87_after);
    }
    _mm_setcsr(original);
#endif
    {   // An FZ op, then the game writes FPCR = 0 (MSR FPCR lowers to c->fpcr = ...; simd_fpcr_sync): a denormal result
        // must come out as itself. Before simd_fpcr_sync the host's FTZ / DAZ stayed on after the FZ op and it read 0.
        Cpu c = {0};
        volatile float tiny = 0x1p-126f, half = 0.5f;
        c.fpcr = FPCR_FZ;
        simd_fpcr_sync(c.fpcr);
        assert(fb_s(op_s(&c, FOP_MUL, tiny, half, 0)) == 0);           // (FZ: flushed, as on the Switch)
        c.fpcr = 0;
        simd_fpcr_sync(c.fpcr);
        assert(fb_s(op_s(&c, FOP_MUL, tiny, half, 0)) == 0x00400000u);  // (0x1p-127: kept)
        vf32 a = {0x1p-126f, 0x1p-126f, 0x1p-126f, 0x1p-126f}, b = {0.5f, 0.5f, 0.5f, 0.5f};
        vf32 r = vop_s(&c, FOP_MUL, a, b, a);
        assert(fb_s(r[0]) == 0x00400000u && fb_s(r[1]) == 0x00400000u);  // (the vector path too)
#if defined(__x86_64__) && defined(__SSE2__)
        assert(!(_mm_getcsr() & 0x8040u));  // (FTZ / DAZ off with FPCR = 0)
#endif
    }
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, worker, &modes[i]));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    puts("PASS: SIMD guest rounding, host-state restoration, denormal controls and thread isolation");
}
