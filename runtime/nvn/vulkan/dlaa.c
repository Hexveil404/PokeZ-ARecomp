// NVIDIA DLAA for the temporal anti-aliasing (exec.c fsr2_copy, F1 > Video > Temporal anti-aliasing): DLSS at the scene's
// own size, fed what FSR 2 is fed - the jittered picture, the scene depth (reversed), the camera's motion vectors.
//
// No NVIDIA SDK code or header is compiled in. NGX is loaded at run time from the driver's NGX core (_nvngx.dll, found
// through HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore); the DLSS model (nvngx_dlss.dll) is loaded by NGX from the
// folder of lasrecomp.exe. The few NGX types used are declared here from the SDK's documented ABI. This follows
// totk-pc-recomp's runtime/vk_dlaa.c (valentderah, MIT licence), adapted to this backend.
//
// The core's parameter object is a C++ class whose virtual functions are called through the MSVC vtable: the Set
// overloads in slots 0-7 and the Get overloads in 8-15, each group in reverse declaration order. vtable_ok checks that
// layout with a set-then-get round trip before anything relies on it.
#include "vk_dlaa.h"
#include "../../rt.h"
#include "../../settings.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef int NgxResult;  // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
#define NGX_OK(r) (((unsigned)(r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
#define NGX_PERFQ_MAXQUALITY 2
#define NGX_PERFQ_DLAA 5
// NVSDK_NGX_DLSS_Feature_Flags: motion vectors at the render size, reversed depth (the game's picture is tone-mapped
// already: no IsHDR, no auto exposure)
#define NGX_FLAG_MV_LOWRES (1 << 1)
#define NGX_FLAG_DEPTH_INVERTED (1 << 3)

typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {  // NVSDK_NGX_ImageViewInfo_VK
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
} NgxImageView;
typedef struct {  // NVSDK_NGX_Resource_VK
    union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
    int type;  // 0 image view, 1 buffer
    _Bool rw;
} NgxResourceVK;

typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void *info);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_Params)(NgxParam **);
typedef NgxResult (*PFN_DestroyParams)(NgxParam *);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_Release)(NgxHandle *);

static struct {
    HMODULE core;
    PFN_Init init;
    PFN_ReqExt req_ext;
    PFN_Params caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    bool tried, loaded, available, failed;
    NgxParam *params;
    // DLSS features, each with its own history and size: [0] the scene's, [1] / [2] the clothing screen's character -
    // its colour and its alpha (exec.c pv_resolve)
    NgxHandle *feat[3];
    uint32_t fw[3], fh[3];
} N;

typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static bool p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static bool p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static bool p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static bool p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }

bool vk_dlaa_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) {
        const char *e = getenv("LARECOMP_DLAA");
        wanted = e && *e ? atoi(e) != 0 : g_settings.taa == TAA_DLAA;
    }
    return wanted;
}

static bool core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = true;
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ, NULL,
                     dir, &n) != ERROR_SUCCESS) {
        LOG("[dlaa] the driver has no NGX core (no NVIDIA RTX driver): DLAA off\n");
        return false;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    N.core = LoadLibraryA(path);
    if (!N.core) {
        LOG("[dlaa] cannot load %s (error %lu): DLAA off\n", path, GetLastError());
        return false;
    }
#define FN(f, name) N.f = (void *)GetProcAddress(N.core, name)
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.create || !N.evaluate || !N.release) {
        LOG("[dlaa] the NGX core %s lacks Vulkan entry points: DLAA off\n", path);
        return false;
    }
    N.loaded = true;
    LOG("[dlaa] NGX core %s\n", path);
    return true;
}

static bool listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return true;
    return false;
}

void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++)
        if (!listed(ext, *n, ie[i]) && *n < max) ext[(*n)++] = ie[i];
}

void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < nd; i++) {
        bool ok = false;
        for (uint32_t k = 0; k < nhave && !ok; k++) ok = !strcmp(have[k].extensionName, de[i]);
        if (!ok) LOG("[dlaa] the device lacks %s, which NGX asks for\n", de[i]);
        else if (!listed(ext, *n, de[i]) && *n < max) ext[(*n)++] = de[i];
    }
}

static bool vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return false;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "ZA.Test.I", -12345);
    p_setu(p, "ZA.Test.U", 4000000000u);
    p_setf(p, "ZA.Test.F", 1.25f);
    p_setp(p, "ZA.Test.P", (void *)p);
    bool ok = p_geti(p, "ZA.Test.I", &i) && i == -12345 && p_getu(p, "ZA.Test.U", &u) && u == 4000000000u &&
              p_getf(p, "ZA.Test.F", &f) && f == 1.25f && p_getp(p, "ZA.Test.P", &v) && v == (void *)p;
    if (N.destroy_params) N.destroy_params(p);
    if (!ok) LOG("[dlaa] NGX parameter layout check failed (i=%d u=%u f=%g p=%p): DLAA off\n", i, u, f, v);
    return ok;
}

void vk_dlaa_init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev) {
    if (!vk_dlaa_wanted()) return;
    // (DirectX 12: Dozen's device isn't NVIDIA's Vulkan one, NGX can't run on it - #169; the F1 row says so)
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(phys, &pp);
    if (!strncmp(pp.deviceName, "Microsoft Direct3D12", 20)) {
        LOG("[dlaa] Graphics API DirectX 12 (%s): DLAA needs Vulkan, off\n", pp.deviceName);
        return;
    }
    if (!core_load()) return;
    wchar_t wdir[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wdir, MAX_PATH);
    while (len && wdir[len - 1] != L'\\') len--;
    wdir[len] = 0;
    // 231313132: the application id NVIDIA's DLSS samples use for builds without an NVIDIA-issued id
    NgxResult r = N.init(231313132ull, wdir, inst, phys, dev, vkGetInstanceProcAddr, vkGetDeviceProcAddr, NGX_VERSION_API, NULL);
    if (!NGX_OK(r)) {
        LOG("[dlaa] NGX init failed (%08x): DLAA off\n", (unsigned)r);
        return;
    }
    if (!vtable_ok()) return;
    NgxParam *caps = NULL;
    int avail = 0, driver = 0;
    if (NGX_OK(N.caps(&caps)) && caps) {
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &driver);
    }
    N.available = avail != 0;
    LOG("[dlaa] NGX up; DLSS %s%s\n", avail ? "available" : "NOT available (is nvngx_dlss.dll next to lasrecomp.exe?)",
        driver ? "; the driver needs an update" : "");
}

bool vk_dlaa_available(void) { return vk_dlaa_wanted() && N.available && !N.failed; }

static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, uint32_t w, uint32_t h, VkImageAspectFlags a, bool rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img = (NgxImageView){v, img, {a, 0, 1, 0, 1}, f, w, h};
    r.type = 0;
    r.rw = rw;
    return r;
}

static bool fail(const char *what, NgxResult r) {
    LOG("[dlaa] %s failed (%08x): DLAA off for this run\n", what, (unsigned)r);
    N.failed = true;
    return false;
}

bool vk_dlaa_record(VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f) {
    if (!vk_dlaa_available()) return false;
    if (!N.params && !NGX_OK(N.alloc_params(&N.params))) return fail("parameter allocation", 0);
    NgxParam *p = N.params;
    bool reset = f->reset;
    int sl = f->slot > 0 && f->slot < 3 ? f->slot : 0;
    if (N.feat[sl] && (N.fw[sl] != f->w || N.fh[sl] != f->h)) {  // (another size: a new feature)
        vkDeviceWaitIdle(dev);
        N.release(N.feat[sl]);
        N.feat[sl] = NULL;
    }
    if (!N.feat[sl]) {
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", f->w);
        p_setu(p, "Height", f->h);
        p_setu(p, "OutWidth", f->ow);
        p_setu(p, "OutHeight", f->oh);
        p_seti(p, "DLSS.Feature.Create.Flags", NGX_FLAG_MV_LOWRES | NGX_FLAG_DEPTH_INVERTED);
        p_seti(p, "DLSS.Enable.Output.Subrects", 0);
        // the model: LARECOMP_DLAA_PRESET=<n> (NVSDK_NGX_DLSS_Hint_Render_Preset: 0 the DLL's default, 6 F, 10 J, 11 K
        // transformer...), as a hint for every quality value; unset: K (an older DLSS ignores what it doesn't know)
        {
            const char *e = getenv("LARECOMP_DLAA_PRESET");
            unsigned pr = e ? (unsigned)atoi(e) : 11u;
            static const char *const q[] = {"DLAA", "Quality", "Balanced", "Performance", "UltraPerformance", "UltraQuality"};
            char k[64];
            for (int i = 0; i < 6; i++) snprintf(k, sizeof k, "DLSS.Hint.Render.Preset.%s", q[i]), p_setu(p, k, pr);
        }
        // DLAA's quality value; an older DLSS (before it had one) gets Max Quality at the same size
        p_seti(p, "PerfQualityValue", NGX_PERFQ_DLAA);
        NgxResult r = N.create(cmd, NGX_FEATURE_SUPERSAMPLING, p, &N.feat[sl]);
        if (!NGX_OK(r) || !N.feat[sl]) {
            N.feat[sl] = NULL;
            p_seti(p, "PerfQualityValue", NGX_PERFQ_MAXQUALITY);
            r = N.create(cmd, NGX_FEATURE_SUPERSAMPLING, p, &N.feat[sl]);
            if (!NGX_OK(r) || !N.feat[sl]) {
                N.feat[sl] = NULL;
                if (sl) {  // (the clothing screen's character: only that one goes without - the scene's DLAA stays)
                    LOG("[dlaa] DLSS feature %d creation failed (%08x)\n", sl, (unsigned)r);
                    return false;
                }
                return fail("DLSS feature creation", r);
            }
            LOG("[dlaa] this DLSS has no DLAA quality value: Max Quality at the scene's size\n");
        }
        N.fw[sl] = f->w, N.fh[sl] = f->h;
        reset = true;
        if (sl) LOG("[dlaa] DLSS feature %d (clothing screen's character) created at %ux%u\n", sl, f->w, f->h);
        else LOG("[dlaa] DLSS feature created at %ux%u -> %ux%u\n", f->w, f->h, f->ow, f->oh);
    }
    NgxResourceVK rc = res_image(f->color_view, f->color, f->color_format, f->w, f->h, VK_IMAGE_ASPECT_COLOR_BIT, false);
    NgxResourceVK ro = res_image(f->out_view, f->out, f->out_format, f->ow, f->oh, VK_IMAGE_ASPECT_COLOR_BIT, true);
    NgxResourceVK rz = res_image(f->depth_view, f->depth, f->depth_format, f->w, f->h, VK_IMAGE_ASPECT_DEPTH_BIT, false);
    NgxResourceVK rm = res_image(f->mv_view, f->mv, VK_FORMAT_R16G16_SFLOAT, f->w, f->h, VK_IMAGE_ASPECT_COLOR_BIT, false);
    // LARECOMP_DLAA_JX / _JY = -1: the jitter handed over the other way on that axis (test)
    static float sx = 0, sy = 0;
    if (!sx) {
        const char *e = getenv("LARECOMP_DLAA_JX"), *g = getenv("LARECOMP_DLAA_JY");
        sx = e ? (float)atof(e) : 1, sy = g ? (float)atof(g) : 1;
    }
    p_setp(p, "Color", &rc);
    p_setp(p, "Output", &ro);
    p_setp(p, "Depth", &rz);
    p_setp(p, "MotionVectors", &rm);
    p_setf(p, "Jitter.Offset.X", f->jx * sx);
    p_setf(p, "Jitter.Offset.Y", f->jy * sy);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", reset);
    p_setf(p, "MV.Scale.X", 1.0f);  // (pixels, current -> previous, as FSR 2's)
    p_setf(p, "MV.Scale.Y", 1.0f);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", f->w);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", f->h);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    p_setf(p, "FrameTimeDeltaInMsec", f->dt_ms);
    NgxResult r = N.evaluate(cmd, N.feat[sl], p, NULL);
    if (!NGX_OK(r)) {
        if (sl) {
            static int nl;
            if (nl++ < 5) LOG("[dlaa] DLSS feature %d evaluation failed (%08x)\n", sl, (unsigned)r);
            return false;
        }
        return fail("DLSS evaluation", r);
    }
    return true;
}

#else  // (NGX is Windows-only)
bool vk_dlaa_wanted(void) { return false; }
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) { (void)ext, (void)n, (void)max; }
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max) {
    (void)have, (void)nhave, (void)ext, (void)n, (void)max;
}
void vk_dlaa_init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev) { (void)inst, (void)phys, (void)dev; }
bool vk_dlaa_available(void) { return false; }
bool vk_dlaa_record(VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f) { (void)dev, (void)cmd, (void)f; return false; }
#endif
