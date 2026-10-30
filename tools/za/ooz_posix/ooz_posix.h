// MSVC spellings ooz (third_party/ooz) uses, for g++ / clang on Linux (tools/za/ooz_posix/README.md). Force-included
// ahead of its stdafx.h; the Windows headers it names are the empty or small files next to this one.
#pragma once
#include <stdint.h>
#include <time.h>
#include <x86intrin.h>  // (SSE2, _rotl, __rdtsc)

#define __forceinline inline __attribute__((always_inline))
#define __int64 long long

static inline unsigned char _BitScanForward(unsigned long *index, uint32_t mask) {  // (32-bit mask: MSVC's unsigned long)
    if (!mask) return 0;
    *index = (unsigned long)__builtin_ctz(mask);
    return 1;
}
static inline unsigned char _BitScanReverse(unsigned long *index, uint32_t mask) {
    if (!mask) return 0;
    *index = 31ul - (unsigned long)__builtin_clz(mask);
    return 1;
}
#define _byteswap_ushort(x) __builtin_bswap16((uint16_t)(x))
#define _byteswap_ulong(x) __builtin_bswap32((uint32_t)(x))
#define _byteswap_uint64(x) __builtin_bswap64((uint64_t)(x))

// (only its command-line benchmark times itself; the library export never calls these, nor the ones below)
typedef union { long long QuadPart; } LARGE_INTEGER;
static inline int QueryPerformanceCounter(LARGE_INTEGER *t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    return 1;
}
static inline int QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000ll; return 1; }

// (its benchmark also loads Oodle's own DLL to compare against: there is none here, so that path reports an error)
#define WINAPI
typedef void *HINSTANCE;
static inline HINSTANCE LoadLibraryA(const char *name) { (void)name; return 0; }
static inline void *GetProcAddress(HINSTANCE mod, const char *name) { (void)mod, (void)name; return 0; }
