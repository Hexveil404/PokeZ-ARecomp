// Helpers for tools/za/hd_pack.py (built by it: gcc -O2 -shared -o hd_native.dll hd_native.c): the runtime's texture
// hashes and the Tegra block-linear -> linear deswizzle, at C speed.
#include <stdint.h>
#include <string.h>

#include "../../runtime/nvn/tex_hash.h"

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

// The name the runtime looks a pack texture up by (runtime/nvn/vulkan/textures.c, since v0.1.10)
EXPORT uint64_t pack_hash(const uint8_t *p, uint64_t n) { return tex_hash_full(p, n); }

// The previous one (up to v0.1.9; still the runtime's with LARECOMP_TEX_FASTHASH=0)
EXPORT uint64_t hash_full(const uint8_t *p, uint64_t n) {
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

// rows of `row` bytes, hb of them, from GOBs (64 bytes x 8 rows) stacked 2^bh_log2 high
EXPORT void deswizzle(const uint8_t *src, uint64_t src_len, uint8_t *dst, uint32_t row, uint32_t hb, uint32_t bh_log2) {
    uint32_t gob_h = 8u << bh_log2, gobs_x = (row + 63) / 64;
    for (uint32_t y = 0; y < hb; y++) {
        uint32_t by = y / gob_h, byo = y % gob_h, gy = byo / 8, gyo = byo % 8;
        uint64_t base = (uint64_t)by * gobs_x * gob_h * 64 + (uint64_t)gy * 512 + (gyo / 2) * 64 + (gyo % 2) * 16;
        for (uint32_t x = 0; x < row; x += 16) {  // (16-byte runs are contiguous)
            uint32_t gx = x / 64, gxo = x % 64;
            uint64_t o = base + (uint64_t)gx * gob_h * 64 + (gxo / 32) * 256 + ((gxo % 32) / 16) * 32;
            uint32_t n = row - x < 16 ? row - x : 16;
            if (o + n <= src_len) memcpy(dst + (uint64_t)y * row + x, src + o, n);
            else memset(dst + (uint64_t)y * row + x, 0, n);
        }
    }
}
