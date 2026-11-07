// Exercise the actual parser/resolver/save path with isolated configuration files.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings.h"

bool bind_settings_load(const char *key, const char *val) { (void)key; (void)val; return false; }
void bind_settings_save(FILE *file) { (void)file; }

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "upscaler-auto")) {  // "upscaler = auto" / no upscaler line: the GPU's default
        settings_load();
        assert(g_settings.upscaler_auto);
        settings_resolve_defaults(true);
        assert(g_settings.upscaler == UPSCALE_BILINEAR && g_default_upscaler == UPSCALE_BILINEAR);
        settings_resolve_defaults(false);
        assert(g_settings.upscaler == UPSCALE_SHARP);
        settings_save();  // (still "auto")
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "res-auto")) {  // "resolution = auto" / none: Match display on every GPU (Dynamic is no default yet)
        settings_load();
        assert(g_settings.res_auto);
        settings_resolve_defaults(true);
        assert(g_settings.res_mode == RES_MATCH && g_default_res_mode == RES_MATCH && g_gpu_integrated);
        settings_resolve_defaults(false);
        assert(g_settings.res_mode == RES_MATCH && !g_gpu_integrated);
        settings_save();  // (still "auto")
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "res-chosen")) {  // a saved resolution is kept on an integrated GPU
        settings_load();
        assert(!g_settings.res_auto);
        int mode = g_settings.res_mode;
        settings_resolve_defaults(true);
        assert(g_settings.res_mode == mode);
        settings_save();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "upscaler-chosen")) {  // a choice is kept on an integrated GPU
        settings_load();
        assert(!g_settings.upscaler_auto);
        settings_resolve_defaults(true);
        assert(g_settings.upscaler == UPSCALE_SHARP);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "colours")) {  // Cosmetics colour rows: palette names, #RRGGBB, older names
        settings_load();
        assert(g_settings.tunic.pick == 3);                                          // Yellow (older tunic) -> Gold
        assert(g_settings.hearts.pick == 9);                                         // Pink
        assert(g_settings.hair.pick == COL_CUSTOM && g_settings.hair.rgb == 0x12AB34);
        assert(g_settings.shield.pick == COL_DEFAULT);                               // unknown -> Default
        assert(g_settings.skin.pick == COL_RANDOM);
        settings_save();
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "rando-options")) {  // the New seed options (an opaque seed ID), kept as saved
        settings_load();
        assert(!strcmp(g_settings.rando_options, argv[2]));
        settings_save();
        return 0;
    }
    assert(argc == 3);
    settings_load();
    assert(!g_settings.upscaler_auto);
    assert(!strcmp(settings_language(), argv[1]));
    assert(!strcmp(g_settings.rando_seed, argv[2]));
    assert(g_settings.aspect == ASPECT_ORIGINAL);
    assert(g_settings.upscaler == UPSCALE_BILINEAR);
    assert(g_settings.prompts == PROMPTS_XBOX);
    assert(g_settings.volume == 1.25f);
    settings_save();
    return 0;
}
// (ZA Recomp's settings.c sizes its recommended video settings by the machine)
int SDL_GetNumLogicalCPUCores(void) { return 8; }
int SDL_GetSystemRAM(void) { return 16384; }
