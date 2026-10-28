// AMD FSR 2 (third_party/fsr2) for exec.c's temporal anti-aliasing (vk_fsr2_record): its context, made again when
// the sizes change, and one dispatch per frame recorded into a command buffer of the caller's (not the recorder's: the
// FSR 2 backend makes its vkCmd* calls itself). The game's picture is tone-mapped already (LDR input, no exposure
// texture); its depth is reversed with an infinite far plane (vk_fsr2 flags below).
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../../../third_party/fsr2/ffx_fsr2.h"
#include "../../../third_party/fsr2/vk/ffx_fsr2_vk.h"
#include "vk_fsr2.h"

static void LOG(const char *fmt, ...) {  // (as runtime/rt.h rt_log: one write per line)
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) fwrite(b, 1, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1, stderr);
}

static FfxFsr2Context ctx;
static bool have;
static void *scratch;
static uint32_t cur_w, cur_h, cur_ow, cur_oh;
static bool failed;  // (the context couldn't be made: not tried again until the sizes change)

static void message(FfxFsr2MsgType type, const wchar_t *m) {
    char b[512];
    size_t i = 0;
    for (; m && m[i] && i < sizeof b - 1; i++) b[i] = (char)m[i];
    b[i] = 0;
    static int n;
    if (n++ < 50) LOG("[fsr2] %s: %s\n", type == FFX_FSR2_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

void vk_fsr2_destroy(VkDevice dev) {
    if (!have) return;
    vkDeviceWaitIdle(dev);
    ffxFsr2ContextDestroy(&ctx);
    have = false;
}

void vk_fsr2_jitter(uint32_t index, uint32_t render_w, uint32_t out_w, float *jx, float *jy) {
    int32_t phases = ffxFsr2GetJitterPhaseCount((int32_t)render_w, (int32_t)out_w);
    if (phases < 1) phases = 1;
    *jx = *jy = 0;
    ffxFsr2GetJitterOffset(jx, jy, (int32_t)(index % (uint32_t)phases), phases);
}

bool vk_fsr2_record(VkPhysicalDevice phys, VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f) {
    bool sizes = f->w != cur_w || f->h != cur_h || f->ow != cur_ow || f->oh != cur_oh;
    if (failed && !sizes) return false;
    if (!have || sizes) {
        vk_fsr2_destroy(dev);
        cur_w = f->w, cur_h = f->h, cur_ow = f->ow, cur_oh = f->oh;
        failed = true;
        size_t sz = ffxFsr2GetScratchMemorySizeVK(phys);
        free(scratch);
        scratch = calloc(1, sz);
        FfxFsr2ContextDescription d = {};
        if (!scratch || ffxFsr2GetInterfaceVK(&d.callbacks, scratch, sz, phys, vkGetDeviceProcAddr) != FFX_OK) {
            LOG("[fsr2] no backend interface\n");
            return false;
        }
        d.flags = FFX_FSR2_ENABLE_DEPTH_INVERTED | FFX_FSR2_ENABLE_DEPTH_INFINITE;
        d.maxRenderSize = {f->w, f->h};
        d.displaySize = {f->ow, f->oh};
        d.device = ffxGetDeviceVK(dev);
        d.fpMessage = message;
        FfxErrorCode e = ffxFsr2ContextCreate(&ctx, &d);
        if (e != FFX_OK) { LOG("[fsr2] context failed: %d\n", (int)e); return false; }
        have = true, failed = false;
        LOG("[fsr2] context %ux%u -> %ux%u\n", f->w, f->h, f->ow, f->oh);
    }
    FfxFsr2DispatchDescription d = {};
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = ffxGetTextureResourceVK(&ctx, f->color, f->color_view, f->w, f->h, f->color_format, L"FSR2_InputColor");
    d.depth = ffxGetTextureResourceVK(&ctx, f->depth, f->depth_view, f->w, f->h, f->depth_format, L"FSR2_InputDepth");
    d.motionVectors = ffxGetTextureResourceVK(&ctx, f->mv, f->mv_view, f->w, f->h, VK_FORMAT_R16G16_SFLOAT, L"FSR2_InputMotionVectors");
    d.reactive = ffxGetTextureResourceVK(&ctx, f->reactive, f->reactive_view, f->w, f->h, VK_FORMAT_R8_UNORM, L"FSR2_InputReactiveMap");
    d.output = ffxGetTextureResourceVK(&ctx, f->out, f->out_view, f->ow, f->oh, f->out_format, L"FSR2_OutputUpscaledColor",
                                       FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f->jx, f->jy};
    d.motionVectorScale = {1, 1};  // (pixels: FSR 2 divides by the render size)
    d.renderSize = {f->w, f->h};
    d.enableSharpening = f->sharpness > 0;
    d.sharpness = f->sharpness;
    d.frameTimeDelta = f->dt_ms;
    d.preExposure = 1;
    d.reset = f->reset;
    d.cameraNear = f->near_z;
    d.cameraFar = FLT_MAX;
    d.cameraFovAngleVertical = f->fov_y;
    d.viewSpaceToMetersFactor = 1;
    FfxErrorCode e = ffxFsr2ContextDispatch(&ctx, &d);
    if (e != FFX_OK) {
        static int n;
        if (n++ < 10) LOG("[fsr2] dispatch failed: %d\n", (int)e);
        return false;
    }
    return true;
}
