// Where a mod's romfs / exefs is (mod_layout.h). Names are matched without case (title ids come as 0100F43008C44000
// and 0100f43008c44000; folders made on Windows keep either).
#include "mod_layout.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define TITLE_ID "0100F43008C44000"  // Pokémon Legends: Z-A

static bool is_dir(const char *p) {
    struct stat sb;
    return !stat(p, &sb) && S_ISDIR(sb.st_mode);
}

// dir/name with name matched without case: out is the path as it is on disk
static bool find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir);
    if (!d) return false;
    bool found = false;
    for (struct dirent *e; !found && (e = readdir(d));)
        if (!strcasecmp(e->d_name, name)) found = snprintf(out, n, "%s/%s", dir, e->d_name) < (int)n;
    closedir(d);
    return found;
}

static bool at(const char *base, const char *const *parts, char *out, size_t n) {
    char cur[1100];
    snprintf(cur, sizeof cur, "%s", base);
    for (; *parts; parts++) {
        if (!find_ci(cur, *parts, out, n)) return false;
        snprintf(cur, sizeof cur, "%s", out);
    }
    return is_dir(cur);
}

static bool at_roots(const char *base, const char *leaf, char *out, size_t n) {
    const char *roots[][5] = {{leaf, NULL}, {"atmosphere", "contents", TITLE_ID, leaf, NULL},
                              {"contents", TITLE_ID, leaf, NULL}, {TITLE_ID, leaf, NULL}};
    for (size_t i = 0; i < sizeof roots / sizeof *roots; i++)
        if (at(base, roots[i], out, n)) return true;
    return false;
}

// leaf at one of the roots in mod_dir, else in its only subfolder (macOS zips' __MACOSX and dot folders don't count)
static bool find_leaf(const char *mod_dir, const char *leaf, char *out, size_t n) {
    if (at_roots(mod_dir, leaf, out, n)) return true;
    DIR *d = opendir(mod_dir);
    if (!d) return false;
    char only[1100] = "", p[1100];
    int subs = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "__MACOSX")) continue;
        snprintf(p, sizeof p, "%s/%s", mod_dir, e->d_name);
        if (is_dir(p) && subs++ == 0) snprintf(only, sizeof only, "%s", p);
    }
    closedir(d);
    return subs == 1 && at_roots(only, leaf, out, n);
}

bool mod_romfs_dir(const char *mod_dir, char *out, size_t n) { return find_leaf(mod_dir, "romfs", out, n); }
bool mod_exefs_dir(const char *mod_dir, char *out, size_t n) { return find_leaf(mod_dir, "exefs", out, n); }

bool mod_is_subsdk(const char *s) { return strlen(s) == 7 && !strncasecmp(s, "subsdk", 6) && s[6] >= '0' && s[6] <= '9'; }

bool mod_has_code(const char *mod_dir) {
    char ex[1100];
    if (!mod_exefs_dir(mod_dir, ex, sizeof ex)) return false;
    DIR *d = opendir(ex);
    if (!d) return false;
    bool code = false;
    for (struct dirent *e; !code && (e = readdir(d));) {
        const char *s = e->d_name;
        size_t k = strlen(s);
        code = mod_is_subsdk(s) || (k > 7 && !strcasecmp(s + k - 7, ".pchtxt"));
    }
    closedir(d);
    return code;
}

// One pass of pchtxt_apply: checks only (write false), or writes.
// (line: room for the longest line, the whole text - a line cut short would write part of its bytes, which
// tools/pchtxt.py writes in full)
static int pchtxt_pass(const char *text, uint8_t *image, uint64_t size, uint64_t keep_below, bool write, char *line) {
    long shift = 0;
    bool enabled = false;
    int written = 0;
    for (const char *p = text; *p;) {
        const char *eol = p + strcspn(p, "\r\n");
        size_t len = (size_t)(eol - p);
        memcpy(line, p, len), line[len] = 0;
        p = *eol ? eol + 1 : eol;
        char *c = strstr(line, "//");
        if (c) *c = 0;
        char *s = line, *e = line + strlen(line);
        while (*s == ' ' || *s == '\t') s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!*s || *s == '#') continue;
        if (!strncasecmp(s, "@flag offset_shift", 18)) shift = strtol(s + 18, NULL, 0);
        else if (!strcasecmp(s, "@enabled")) enabled = true;
        else if (!strcasecmp(s, "@disabled") || !strcasecmp(s, "@stop")) enabled = false;
        else if (*s == '@' || !enabled) continue;  // (@nsobid: gamebuild.c matched it; @flag print_values, credits)
        else {
            char *hex = s + strspn(s, "0123456789abcdefABCDEF");
            size_t olen = (size_t)(hex - s);
            char *bytes = hex + strspn(hex, " \t");
            size_t blen = strspn(bytes, "0123456789abcdefABCDEF");
            if (!olen || olen > 8 || bytes == hex || !blen || bytes[blen] || blen % 2) return -1;  // (string values...)
            int64_t off = (int64_t)strtoull(s, NULL, 16) + shift - 0x100;
            if (off < 0 || (uint64_t)off + blen / 2 > size) return -1;
            for (size_t i = 0; i < blen / 2; i++) {
                if ((uint64_t)off + i < keep_below) continue;
                char b[3] = {bytes[2 * i], bytes[2 * i + 1], 0};
                if (write) image[off + i] = (uint8_t)strtoul(b, NULL, 16);
                written++;
            }
        }
    }
    return written;
}

int pchtxt_apply(const char *text, uint8_t *image, uint64_t size, uint64_t keep_below) {
    char *line = malloc(strlen(text) + 1);
    if (!line) return -1;
    int r = pchtxt_pass(text, image, size, keep_below, false, line);
    if (r >= 0) r = pchtxt_pass(text, image, size, keep_below, true, line);
    free(line);
    return r;
}
