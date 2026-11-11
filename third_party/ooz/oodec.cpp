// oodec: one Oodle-compressed block -> its bytes, for ZA Recomp's runtime (runtime/services/fs.c rt_za_oodle_call):
// when the game's own decompression of a block fails, the runtime runs this as a separate program on the same
// compressed bytes and hands the game what it decodes. GPLv3 like the rest of this folder (COPYING); ZA Recomp only
// runs it as its own process, as it does zaprompts. Built by tools/za/dist.sh / dist-linux.sh next to zaprompts.
//   oodec <compressed file> <output file> <decompressed size>   -> exit 0 when exactly that many bytes were decoded
#include "stdafx.h"
#include <stdio.h>
#include <stdlib.h>

int Kraken_Decompress(const byte *src, size_t src_len, byte *dst, size_t dst_len);

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: oodec <compressed file> <output file> <decompressed size>\n"); return 2; }
    size_t raw = (size_t)strtoull(argv[3], NULL, 10);
    FILE *f = fopen(argv[1], "rb");
    if (!f || !raw || raw > (256u << 20)) { fprintf(stderr, "oodec: bad input\n"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    // (ooz reads a little past both ends of its buffers: room on both)
    byte *src = (byte *)calloc((size_t)n + 64, 1), *dst = (byte *)calloc(raw + 64, 1);
    if (!src || !dst || n <= 0 || fread(src, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "oodec: read failed\n"); return 2; }
    fclose(f);
    int got = Kraken_Decompress(src, (size_t)n, dst, raw);
    if (got != (int)raw) { fprintf(stderr, "oodec: decoded %d of %zu bytes\n", got, raw); return 1; }
    FILE *o = fopen(argv[2], "wb");
    if (!o || fwrite(dst, 1, raw, o) != raw || fclose(o) != 0) { fprintf(stderr, "oodec: write failed\n"); return 2; }
    return 0;
}
