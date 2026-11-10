// AMD FSR 4.1.2's temporal upscaler on Linux (F1 > Rendering > Temporal anti-aliasing > AMD FSR 4.1.2).
//
// FSR 4 ships as signed Windows DLLs; a user-supplied Linux provider built from FidelityFX SDK 2.4.0 exposes the same
// ffx* API and runs the effect in-process on the caller's VkDevice (fsr4-vk style: libamd_fidelityfx_upscaler_vk.so
// next to the probe's fsr4-assets/). The provider is never redistributed with this project. The device features it
// needs (cooperative matrix, shader float8, 8/16-bit storage) are enabled by device.c when TAA_FSR4 is selected.
//
// The frame is the one FSR 2/3 take (vk_fsr2.h VkFsr2Frame): the jittered scene at render size, camera motion vectors
// (unjittered current -> previous, pixels), and the output at the shown size. The provider does not consume the
// reactive mask, so it is left null here; FSR 2/3 still use it if this falls back. Depth is null for now: PokeZ's
// depth is D32S8 and the provider would make a colour-aspect R32 view of it. Its neutral depth keeps FSR 4 correct
// (just without depth-based disocclusion), and a proper R32 depth copy can replace it later.
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include "vk_fsr2.h"
#include "../../settings.h"

bool vk_fsr4_wanted(void) {
    const char *e = getenv("LARECOMP_FSR4");
    if (e && *e) return atoi(e) != 0;
    return g_settings.taa == TAA_FSR4;
}

#if defined(__linux__)
#include <dlfcn.h>
#include "vk.h"
#include "../../platform.h"
#define FFX_API_ENTRY  // (the SDK header's Windows export attribute; the provider is dlopen'd on Linux)
#include "../../../third_party/ffx_api/ffx_api.h"
#include "../../../third_party/ffx_api/ffx_api_vk.h"
#include "../../../third_party/ffx_api/ffx_upscale.h"

#define LOG vk_fsr_log

static PfnFfxCreateContext p_create;
static PfnFfxDestroyContext p_destroy;
static PfnFfxQuery p_query;
static PfnFfxDispatch p_dispatch;
static void *module;
static bool tried, loaded, failed;  // (failed: the provider refused a context - FSR 2/3 for the rest of the session)
static ffxContext ctx;
static bool have;
static uint32_t cur_ow, cur_oh;

static bool load(void) {
    if (tried) return loaded;
    tried = true;
    const char *names[] = {"libamd_fidelityfx_upscaler_vk.so", "amd_fidelityfx_upscaler_vk.so"};
    const char *env = getenv("LARECOMP_FSR4_PATH");
    char path[4096];
    if (env && *env) {
        if (strstr(env, ".so")) {
            module = dlopen(env, RTLD_NOW | RTLD_LOCAL);
        } else {
            for (unsigned i = 0; i < sizeof names / sizeof *names && !module; i++) {
                snprintf(path, sizeof path, "%s/%s", env, names[i]);
                module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
            }
        }
    }
    if (!module) {  // next to the executable, where a user-supplied provider is normally staged
        char exe[4096];
        if (plat_exe_path(exe, sizeof exe)) {
            char *slash = strrchr(exe, '/');
            if (slash) {
                *slash = 0;
                for (unsigned i = 0; i < sizeof names / sizeof *names && !module; i++) {
                    snprintf(path, sizeof path, "%s/%s", exe, names[i]);
                    module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
                }
            }
        }
    }
    if (!module)  // the loader's own search path (LD_LIBRARY_PATH / cwd)
        for (unsigned i = 0; i < sizeof names / sizeof *names && !module; i++) module = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        LOG("[fsr4] no libamd_fidelityfx_upscaler_vk.so (put it next to lasrecomp, or set LARECOMP_FSR4_PATH): FSR 2/3 instead\n");
        return false;
    }
    p_create = (PfnFfxCreateContext)dlsym(module, "ffxCreateContext");
    p_destroy = (PfnFfxDestroyContext)dlsym(module, "ffxDestroyContext");
    p_query = (PfnFfxQuery)dlsym(module, "ffxQuery");
    p_dispatch = (PfnFfxDispatch)dlsym(module, "ffxDispatch");
    if (!p_create || !p_destroy || !p_query || !p_dispatch) {
        LOG("[fsr4] the provider lacks the FidelityFX entry points: FSR 2/3 instead\n");
        return false;
    }
    loaded = true;
    return true;
}

static void message(uint32_t type, const wchar_t *m) { vk_fsr_message("fsr4", type == FFX_API_MESSAGE_TYPE_ERROR, m); }

bool vk_fsr4_available(void) { return g_vk.fsr4 && !failed && load(); }

static void destroy(VkDevice dev) {
    if (!have) return;
    vkDeviceWaitIdle(dev);
    p_destroy(&ctx, NULL);
    have = false;
}

// (images stay in VK_IMAGE_LAYOUT_GENERAL: GENERIC_READ / UNORDERED_ACCESS are the states the SDK's Vulkan backend
// maps to it, and it restores the state it was given)
static FfxApiResource res(VkImage img, VkFormat fmt, uint32_t w, uint32_t h, uint32_t state, uint32_t usage) {
    FfxApiResourceDescription d = {};
    d.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    d.format = ffxApiGetSurfaceFormatVK(fmt);
    d.width = w, d.height = h, d.depth = 1, d.mipCount = 1;
    d.usage = usage | (ffxApiIsDepthFormat(fmt) ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0);
    return ffxApiGetResourceVK((void *)img, d, state);
}

bool vk_fsr4_record(VkPhysicalDevice phys, VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f) {
    if (!vk_fsr4_available()) return false;
    if (!have || f->ow != cur_ow || f->oh != cur_oh) {
        destroy(dev);
        cur_ow = f->ow, cur_oh = f->oh;
        ffxCreateBackendVKDesc be = {};
        be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        be.deviceContext = {dev, phys, vkGetDeviceProcAddr};
        ffxCreateContextDescUpscale d = {};
        d.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        d.header.pNext = &be.header;
        // (the motion vectors are unjittered already: no FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION)
        d.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
        d.maxRenderSize = {f->ow, f->oh};
        d.maxUpscaleSize = {f->ow, f->oh};
        d.fpMessage = message;
        ffxReturnCode_t e = p_create(&ctx, &d.header, NULL);
        if (e != FFX_API_RETURN_OK) {
            LOG("[fsr4] context %ux%u failed: %u (FSR 2/3 instead)\n", f->ow, f->oh, e);
            failed = true;
            return false;
        }
        have = true;
        ffxQueryGetProviderVersion v = {};
        v.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        p_query(&ctx, &v.header);
        LOG("[fsr4] context %ux%u: %s%s\n", f->ow, f->oh, v.versionName ? v.versionName : "provider",
            v.versionName && strstr(v.versionName, "4.") ? "" : " (not FSR 4)");
    }
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void *)cmd;
    const uint32_t rd = FFX_API_RESOURCE_STATE_GENERIC_READ;
    d.color = res(f->color, f->color_format, f->w, f->h, rd, FFX_API_RESOURCE_USAGE_RENDERTARGET);
    d.depth = (FfxApiResource){};  // (see the header: a real R32 colour copy can replace this later)
    d.motionVectors = res(f->mv, VK_FORMAT_R16G16_SFLOAT, f->w, f->h, rd, FFX_API_RESOURCE_USAGE_RENDERTARGET);
    d.reactive = (FfxApiResource){};  // (the provider ignores it; FSR 2/3 use it on fallback)
    d.output = res(f->out, f->out_format, f->ow, f->oh, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset = {f->jx, f->jy};
    d.motionVectorScale = {1, 1};  // (pixels)
    d.renderSize = {f->w, f->h};
    d.upscaleSize = {f->ow, f->oh};
    d.enableSharpening = f->sharpness > 0;
    d.sharpness = f->sharpness;
    d.frameTimeDelta = f->dt_ms;
    d.preExposure = 1;
    d.reset = f->reset;
    d.cameraNear = f->near_z;
    d.cameraFar = FLT_MAX;
    d.cameraFovAngleVertical = f->fov_y;
    d.viewSpaceToMetersFactor = 1;
    ffxReturnCode_t e = p_dispatch(&ctx, &d.header);
    if (e != FFX_API_RETURN_OK) {
        static int n;
        if (n++ < 10) LOG("[fsr4] dispatch failed: %u\n", e);
        return false;
    }
    return true;
}
#else
bool vk_fsr4_available(void) { return false; }
bool vk_fsr4_record(VkPhysicalDevice, VkDevice, VkCommandBuffer, const VkFsr2Frame *) { return false; }
#endif
