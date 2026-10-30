// CPU-only battle presentation transform. No input state is modified.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "xbox_attack_resource.h"

#define XBOX_ATTACK_VIRTUAL "ui/data/btl_app/btl_app_waza_00.arc"
#define XBOX_ATTACK_BASE "mods/_XboxAttackLayout"
#define XBOX_ATTACK_ROOT XBOX_ATTACK_BASE "/romfs"
#define XBOX_ATTACK_HOST XBOX_ATTACK_ROOT "/" XBOX_ATTACK_VIRTUAL
#define XBOX_ATTACK_METADATA XBOX_ATTACK_BASE "/resource.bin"
#define XBOX_ATTACK_STATUS "mods/.XboxAttackLayout.prepared"

static uint64_t xbox_attack_fnv(const uint8_t *data, size_t size) {
    uint64_t h = UINT64_C(0xcbf29ce484222645);
    for (size_t i = 0; i < size; i++) h = (h ^ data[i]) * UINT64_C(0x100000001b3);
    return h;
}

// pad[] is Switch A,B,X,Y -> SDL physical south,east,west,north (0..3).
// Visual order in the verified layout is top,right,left,bottom; its logical
// identities are Switch X,A,Y,B. Renaming the parent routes the game's existing
// name-based text/state/animation updates to the destination's original template.
static bool xbox_attack_mapping(const int16_t pad[4], uint8_t visual[4]) {
    static const uint8_t slot[4] = {2, 0, 3, 1};
    static const uint8_t position[4] = {3, 1, 2, 0};
    unsigned seen = 0;
    uint8_t result[4];
    for (unsigned logical = 0; logical < 4; logical++) {
        int host = pad[slot[logical]];
        if (host < 0 || host > 3 || (seen & (1u << host))) return false;
        seen |= 1u << host;
        result[position[host]] = (uint8_t)logical;
    }
    memcpy(visual, result, sizeof result);
    return true;
}

// Fail closed before changing any byte: exact supported resource and four
// distinct face bindings only. A mismatch leaves both resource and input intact.
static bool xbox_attack_relabel(uint8_t *data, size_t size, const int16_t pad[4],
                                const XboxAttackResource *resource, const char **why) {
    uint8_t visual[4];
    if (!xbox_attack_mapping(pad, visual)) { *why = "bindings must use four distinct face buttons"; return false; }
    if (size != resource->arc_size || xbox_attack_fnv(data, size) != resource->arc_fnv) {
        *why = "unsupported battle resource"; return false;
    }
    for (unsigned i = 0; i < 4; i++) {
        uint64_t at = resource->digits[i];
        if (at < 8 || at >= size || size-at < 2 ||
            memcmp(data+at-8, "L_waza_0", 8) || data[at] != '0'+i || data[at+1]) {
            *why = "pane identity mismatch"; return false;
        }
    }
    for (unsigned i = 0; i < 4; i++) data[resource->digits[i]] = (uint8_t)('0'+visual[i]);
    *why = NULL;
    return true;
}
