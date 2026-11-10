// Code mods' places in guest memory (modlib.h).
#include "modlib.h"

#include <stdio.h>
#include <string.h>

#include "rt.h"

#define ALIGN_UP(v, a) (((v) + (a) - 1) & ~((a) - 1))

uint64_t mod_base(uint64_t main_size, int k, const uint64_t *sizes) {
    uint64_t at = ALIGN_UP(MAIN_BASE + main_size, MOD_ALIGN);
    for (int i = 0; i < k; i++) at = ALIGN_UP(at + sizes[i], MOD_ALIGN);
    return at + sizes[k] <= SDK_BASE ? at : 0;
}

uint64_t nso_image_size(const char *path) {
    unsigned char h[0x40];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    bool ok = fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "NSO0", 4);
    fclose(f);
    if (!ok) return 0;
    uint32_t data_mem, data_size, bss;  // (as loader.c's load_nso maps it)
    memcpy(&data_mem, h + 0x30 + 4, 4);
    memcpy(&data_size, h + 0x30 + 8, 4);
    memcpy(&bss, h + 0x3C, 4);
    return ((uint64_t)data_mem + data_size + bss + 0x3FFF) & ~0x3FFFull;
}
