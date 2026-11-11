// tools/test_pack_names.py's helper: the name the runtime looks each texture up by (runtime/nvn/tex_hash.h
// tex_pack_name, as vulkan/textures.c upload_texture calls it), and the HD pack builder's file name for it
// (tools/za/hd_native.c pack_hash).
//   test-pack-names <prompt-names.txt> <texture files...>  ->  "<runtime name> <hd_pack name>" per file
#include <stdio.h>
#include <stdlib.h>

#include "za/hd_native.c"

static uint64_t listed[4096];
static int nlisted;
static bool prompt_listed(uint64_t h) {
    for (int i = 0; i < nlisted; i++)
        if (listed[i] == h) return true;
    return false;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "r");
    if (!f) return 2;
    unsigned long long v;
    while (nlisted < 4096 && fscanf(f, "%llx", &v) == 1) listed[nlisted++] = v;
    fclose(f);
    for (int i = 2; i < argc; i++) {
        FILE *t = fopen(argv[i], "rb");
        if (!t) return 2;
        static uint8_t buf[1 << 20];
        size_t n = fread(buf, 1, sizeof buf, t);
        fclose(t);
        uint64_t h = tex_hash_full(buf, n);  // (the upload check's hash, LARECOMP_TEX_FASTHASH on: the default)
        printf("%016llx %016llx\n", (unsigned long long)tex_pack_name(buf, n, h, true, prompt_listed),
               (unsigned long long)pack_hash(buf, n));
    }
    return 0;
}
