// Texture upload checks' hashes (vulkan/textures.c and metal.m upload_texture), written for this project.
// LARECOMP_TEX_FASTHASH=0: each backend's previous hashes instead (A/B tests).
//
// tex_hash_full: eight independent lanes, each a chain of bijective steps (xor the next word, multiply by an odd
// constant, xor-shift), folded in order at the end. A change of any one 64-bit word always changes its lane, so it
// can't go unseen; the lanes keep the CPU's multipliers busy (M3 Max: 33 GB/s, against 19 for the 4-lane hash and
// 5 for the 1-lane one before; about memory speed). A hardware CRC32C was measured no faster here and keeps only 32
// bits per lane. Values are only compared within a run.
//
// tex_hash_sample: the change detector for textures not seen changing: the same 4096 strided places as before, but
// 8 bytes at each (more of the data seen) in four chains (the old byte-at-a-time chain waited on each multiply);
// textures up to 64 KB are hashed in full instead, which is quicker than the old 4096-step sample and misses nothing.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline uint64_t tex_hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h[8] = {0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull,
                     0x85EBCA77C2B2AE63ull, 0xFF51AFD7ED558CCDull, 0xC4CEB9FE1A85EC53ull, 0x94D049BB133111EBull};
    uint64_t i = 0;
    for (; i + 64 <= n; i += 64) {
        uint64_t v[8];
        memcpy(v, p + i, 64);
        for (int k = 0; k < 8; k++) {
            h[k] = (h[k] ^ v[k]) * 0x100000001B3ull;
            h[k] ^= h[k] >> 29;
        }
    }
    uint64_t r = 0;
    for (int k = 0; k < 8; k++) {
        r = (r ^ h[k]) * 0x9E3779B97F4A7C15ull;
        r ^= r >> 32;
    }
    for (; i < n; i++) r = (r ^ p[i]) * 0x100000001B3ull;
    return r ^ (r >> 31);
}

static inline uint64_t tex_hash_sample(const uint8_t *p, uint64_t n) {
    if (n <= 65536) return tex_hash_full(p, n);
    uint64_t step = n / 4096;  // (>= 16 here)
    uint64_t a = 1469598103934665603ull ^ n, b = 0xC2B2AE3D27D4EB4Full, c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 3 * step + 8 <= n; i += 4 * step) {
        uint64_t v0, v1, v2, v3;
        memcpy(&v0, p + i, 8);
        memcpy(&v1, p + i + step, 8);
        memcpy(&v2, p + i + 2 * step, 8);
        memcpy(&v3, p + i + 3 * step, 8);
        a = (a ^ v0) * 1099511628211ull;
        b = (b ^ v1) * 1099511628211ull;
        c = (c ^ v2) * 1099511628211ull;
        d = (d ^ v3) * 1099511628211ull;
    }
    for (; i < n; i += step) {  // (the last few places; the very end byte by byte)
        if (i + 8 <= n) {
            uint64_t v;
            memcpy(&v, p + i, 8);
            a = (a ^ v) * 1099511628211ull;
        } else
            for (uint64_t j = i; j < n; j++) a = (a ^ p[j]) * 1099511628211ull;
    }
    uint64_t r = a;
    r = (r ^ (r >> 29)) * 0x9E3779B97F4A7C15ull ^ b;
    r = (r ^ (r >> 29)) * 0x9E3779B97F4A7C15ull ^ c;
    r = (r ^ (r >> 29)) * 0x9E3779B97F4A7C15ull ^ d;
    return r ^ (r >> 31);
}

// The previous full hash (four lanes; the runtime's with LARECOMP_TEX_FASTHASH=0). Also the name the button prompt
// sets file their textures under: tools/za/prompt_textures.py hash_full computes it offline (tools/test_pack_names.py).
static inline uint64_t tex_hash_prompt(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// The name a texture's replacement is looked up by (vulkan/textures.c upload_texture). The two packs are named by
// different hashes: the button prompt sets by tex_hash_prompt (tools/za/prompt_textures.py), the HD pack by
// tex_hash_full (tools/za/hd_pack.py -> hd_native.c pack_hash). h is the upload check's hash (tex_hash_full, or
// tex_hash_prompt with LARECOMP_TEX_FASTHASH=0). Looking the prompts up by h found none of them: every button icon
// stayed the Switch's / Xbox's whatever the setting (#229). prompt_listed: whether the prompt set has that name.
static inline uint64_t tex_pack_name(const uint8_t *p, uint64_t n, uint64_t h, bool fast, bool (*prompt_listed)(uint64_t)) {
    if (!fast || !prompt_listed) return h;
    uint64_t old = tex_hash_prompt(p, n);
    return prompt_listed(old) ? old : h;
}

// LARECOMP_TEX_FASTHASH (default on)
static inline int tex_fast_hash(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_TEX_FASTHASH"); on = !e || atoi(e) != 0; }
    return on;
}
