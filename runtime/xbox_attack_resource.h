// On-disk metadata written by zaprompts; no player-specific values are compiled in.
#pragma once
#define XBOX_ATTACK_METADATA_SIZE 80u
#define XBOX_ATTACK_MAX_ARC (16u << 20)
#define XBOX_ATTACK_MAX_PACK (64u << 20)

typedef struct {
    uint64_t arc_size, arc_fnv, pack_offset, pack_size, pack_fnv, digits[4];
} XboxAttackResource;

static bool xbox_attack_resource(const uint8_t *bytes, size_t size, XboxAttackResource *out) {
    if (size != XBOX_ATTACK_METADATA_SIZE || memcmp(bytes, "ZAXBOX01", 8)) return false;
    uint64_t v[9] = {0};
    for (unsigned i = 0; i < 9; i++)
        for (unsigned j = 0; j < 8; j++) v[i] |= (uint64_t)bytes[8 + i * 8 + j] << (j * 8);
    if (!v[0] || v[0] > XBOX_ATTACK_MAX_ARC || !v[3] || v[3] > XBOX_ATTACK_MAX_PACK ||
        v[2] > UINT64_MAX - v[3]) return false;
    for (unsigned i = 5; i < 9; i++) {
        if (v[i] < 8 || v[i] >= v[0] || v[0] - v[i] < 2) return false;
        for (unsigned j = 5; j < i; j++) if (v[i] == v[j]) return false;
    }
    *out = (XboxAttackResource){v[0], v[1], v[2], v[3], v[4], {v[5], v[6], v[7], v[8]}};
    return true;
}
