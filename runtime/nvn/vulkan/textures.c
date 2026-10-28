// Vulkan textures: NVN formats, images, change-detected uploads from guest memory, views and samplers
// (counterpart of the texture half of metal.m and the view helpers of renderer.m).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_timer.h>
#include <dirent.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../rqueue.h"
#include "vk.h"
#include "record.h"
#include "../astc.h"
#include "../tex_hash.h"
#include "../renderer.h"
#include "../texpatch.h"
#include "../../settings.h"

// ================================================================== formats
typedef struct { VkFormat vf; uint8_t expand_rgb, alpha_one; } VkFmt;

// DEPTH24_STENCIL8 as itself where the GPU has it for everything the game does with depth (NVIDIA, Intel): half the
// bytes of D32_SFLOAT_S8_UINT (4 instead of 8 per pixel) for every depth test, clear and copy - the game copies its
// full-size depth twice a frame. AMD has no D24S8 attachments: D32S8 there (and on Metal). LARECOMP_VK_D24=0: D32S8.
static VkFormat depth24_stencil8(void) {
    static VkFormat f;
    if (!f) {
        const char *e = ENV("LARECOMP_VK_D24");
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                    VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        bool ok = (p.optimalTilingFeatures & want) == want && !(e && !atoi(e));
        f = ok ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
        LOG("[vk] depth24 stencil8: %s\n", ok ? "D24_UNORM_S8_UINT" : "D32_SFLOAT_S8_UINT");
    }
    return f;
}

static VkFmt fmt_of(uint32_t f) {
    switch (f) {
    case 0x01: return (VkFmt){VK_FORMAT_R8_UNORM};
    case 0x02: return (VkFmt){VK_FORMAT_R8_SNORM};
    case 0x03: return (VkFmt){VK_FORMAT_R8_UINT};
    case 0x04: return (VkFmt){VK_FORMAT_R8_SINT};
    case 0x05: return (VkFmt){VK_FORMAT_R16_SFLOAT};
    case 0x06: return (VkFmt){VK_FORMAT_R16_UNORM};
    case 0x07: return (VkFmt){VK_FORMAT_R16_SNORM};
    case 0x08: return (VkFmt){VK_FORMAT_R16_UINT};
    case 0x09: return (VkFmt){VK_FORMAT_R16_SINT};
    case 0x0A: return (VkFmt){VK_FORMAT_R32_SFLOAT};
    case 0x0B: return (VkFmt){VK_FORMAT_R32_UINT};
    case 0x0C: return (VkFmt){VK_FORMAT_R32_SINT};
    case 0x0D: return (VkFmt){VK_FORMAT_R8G8_UNORM};
    case 0x0E: return (VkFmt){VK_FORMAT_R8G8_SNORM};
    case 0x0F: return (VkFmt){VK_FORMAT_R8G8_UINT};
    case 0x10: return (VkFmt){VK_FORMAT_R8G8_SINT};
    case 0x11: return (VkFmt){VK_FORMAT_R16G16_SFLOAT};
    case 0x12: return (VkFmt){VK_FORMAT_R16G16_UNORM};
    case 0x13: return (VkFmt){VK_FORMAT_R16G16_SNORM};
    case 0x14: return (VkFmt){VK_FORMAT_R16G16_UINT};
    case 0x15: return (VkFmt){VK_FORMAT_R16G16_SINT};
    case 0x16: return (VkFmt){VK_FORMAT_R32G32_SFLOAT};
    case 0x17: return (VkFmt){VK_FORMAT_R32G32_UINT};
    case 0x18: return (VkFmt){VK_FORMAT_R32G32_SINT};
    case 0x19: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 1};  // RGB8: expanded on upload
    case 0x25: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    case 0x26: return (VkFmt){VK_FORMAT_R8G8B8A8_SNORM};
    case 0x27: return (VkFmt){VK_FORMAT_R8G8B8A8_UINT};
    case 0x28: return (VkFmt){VK_FORMAT_R8G8B8A8_SINT};
    case 0x29: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT};
    case 0x2A: return (VkFmt){VK_FORMAT_R16G16B16A16_UNORM};
    case 0x2B: return (VkFmt){VK_FORMAT_R16G16B16A16_SNORM};
    case 0x2C: return (VkFmt){VK_FORMAT_R16G16B16A16_UINT};
    case 0x2D: return (VkFmt){VK_FORMAT_R16G16B16A16_SINT};
    case 0x2E: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT};
    case 0x2F: return (VkFmt){VK_FORMAT_R32G32B32A32_UINT};
    case 0x30: return (VkFmt){VK_FORMAT_R32G32B32A32_SINT};
    case 0x31: return (VkFmt){VK_FORMAT_S8_UINT};
    case 0x32: return (VkFmt){VK_FORMAT_D16_UNORM};
    case 0x33: return (VkFmt){VK_FORMAT_D32_SFLOAT};  // DEPTH24 (as on Metal: no 24-bit depth there)
    case 0x34: return (VkFmt){VK_FORMAT_D32_SFLOAT};
    case 0x35: return (VkFmt){depth24_stencil8()};
    case 0x36: return (VkFmt){VK_FORMAT_D32_SFLOAT_S8_UINT};
    case 0x37: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB, 0, 1};
    case 0x38: return (VkFmt){VK_FORMAT_R8G8B8A8_SRGB};
    case 0x39: return (VkFmt){VK_FORMAT_R4G4B4A4_UNORM_PACK16};   // Metal ABGR4Unorm
    case 0x3A: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};     // Metal B5G6R5Unorm
    case 0x3B: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};   // Metal A1BGR5Unorm
    case 0x3C: return (VkFmt){VK_FORMAT_R5G6B5_UNORM_PACK16};
    case 0x3D: return (VkFmt){VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    case 0x3E: return (VkFmt){VK_FORMAT_A2B10G10R10_UINT_PACK32};
    case 0x3F: return (VkFmt){VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    case 0x40: return (VkFmt){VK_FORMAT_E5B9G9R9_UFLOAT_PACK32};
    case 0x41: case 0x42: return (VkFmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK};
    case 0x43: return (VkFmt){VK_FORMAT_BC2_UNORM_BLOCK};
    case 0x44: return (VkFmt){VK_FORMAT_BC3_UNORM_BLOCK};
    case 0x45: case 0x46: return (VkFmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    case 0x47: return (VkFmt){VK_FORMAT_BC2_SRGB_BLOCK};
    case 0x48: return (VkFmt){VK_FORMAT_BC3_SRGB_BLOCK};
    case 0x49: return (VkFmt){VK_FORMAT_BC4_UNORM_BLOCK};
    case 0x4A: return (VkFmt){VK_FORMAT_BC4_SNORM_BLOCK};
    case 0x4B: return (VkFmt){VK_FORMAT_BC5_UNORM_BLOCK};
    // 0x4C = signed RGTC2; LARECOMP_BC5_UNORM=1 decodes it unsigned (see metal.m / notes/PLAN.md)
    case 0x4C: {
        static int unorm = -1;  // (per texture lookup: getenv scans the environment, notably on Windows)
        if (unorm < 0) unorm = getenv("LARECOMP_BC5_UNORM") != NULL;
        return (VkFmt){unorm ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};
    }
    case 0x4D: return (VkFmt){VK_FORMAT_BC7_UNORM_BLOCK};
    case 0x4E: return (VkFmt){VK_FORMAT_BC7_SRGB_BLOCK};
    case 0x4F: return (VkFmt){VK_FORMAT_BC6H_SFLOAT_BLOCK};
    case 0x50: return (VkFmt){VK_FORMAT_BC6H_UFLOAT_BLOCK};
    case 0x6D: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM, 0, 1};
    case 0x71: return (VkFmt){VK_FORMAT_R16G16B16A16_SFLOAT, 0, 1};
    case 0x76: return (VkFmt){VK_FORMAT_R32G32B32A32_SFLOAT, 0, 1};
    case 0x79: return (VkFmt){VK_FORMAT_ASTC_4x4_UNORM_BLOCK};
    case 0x7A: return (VkFmt){VK_FORMAT_ASTC_5x4_UNORM_BLOCK};
    case 0x7B: return (VkFmt){VK_FORMAT_ASTC_5x5_UNORM_BLOCK};
    case 0x7C: return (VkFmt){VK_FORMAT_ASTC_6x5_UNORM_BLOCK};
    case 0x7D: return (VkFmt){VK_FORMAT_ASTC_6x6_UNORM_BLOCK};
    case 0x7E: return (VkFmt){VK_FORMAT_ASTC_8x5_UNORM_BLOCK};
    case 0x7F: return (VkFmt){VK_FORMAT_ASTC_8x6_UNORM_BLOCK};
    case 0x80: return (VkFmt){VK_FORMAT_ASTC_8x8_UNORM_BLOCK};
    case 0x81: return (VkFmt){VK_FORMAT_ASTC_10x5_UNORM_BLOCK};
    case 0x82: return (VkFmt){VK_FORMAT_ASTC_10x6_UNORM_BLOCK};
    case 0x83: return (VkFmt){VK_FORMAT_ASTC_10x8_UNORM_BLOCK};
    case 0x84: return (VkFmt){VK_FORMAT_ASTC_10x10_UNORM_BLOCK};
    case 0x85: return (VkFmt){VK_FORMAT_ASTC_12x10_UNORM_BLOCK};
    case 0x86: return (VkFmt){VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
    case 0x87: return (VkFmt){VK_FORMAT_ASTC_4x4_SRGB_BLOCK};
    case 0x88: return (VkFmt){VK_FORMAT_ASTC_5x4_SRGB_BLOCK};
    case 0x89: return (VkFmt){VK_FORMAT_ASTC_5x5_SRGB_BLOCK};
    case 0x8A: return (VkFmt){VK_FORMAT_ASTC_6x5_SRGB_BLOCK};
    case 0x8B: return (VkFmt){VK_FORMAT_ASTC_6x6_SRGB_BLOCK};
    case 0x8C: return (VkFmt){VK_FORMAT_ASTC_8x5_SRGB_BLOCK};
    case 0x8D: return (VkFmt){VK_FORMAT_ASTC_8x6_SRGB_BLOCK};
    case 0x8E: return (VkFmt){VK_FORMAT_ASTC_8x8_SRGB_BLOCK};
    case 0x8F: return (VkFmt){VK_FORMAT_ASTC_10x5_SRGB_BLOCK};
    case 0x90: return (VkFmt){VK_FORMAT_ASTC_10x6_SRGB_BLOCK};
    case 0x91: return (VkFmt){VK_FORMAT_ASTC_10x8_SRGB_BLOCK};
    case 0x92: return (VkFmt){VK_FORMAT_ASTC_10x10_SRGB_BLOCK};
    case 0x93: return (VkFmt){VK_FORMAT_ASTC_12x10_SRGB_BLOCK};
    case 0x94: return (VkFmt){VK_FORMAT_ASTC_12x12_SRGB_BLOCK};
    case 0x95: case 0x96: case 0x97: return (VkFmt){VK_FORMAT_R5G5B5A1_UNORM_PACK16};
    case 0x98: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM, 0, 1};
    case 0x99: return (VkFmt){VK_FORMAT_B8G8R8A8_UNORM};
    case 0x9A: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB, 0, 1};
    case 0x9B: return (VkFmt){VK_FORMAT_B8G8R8A8_SRGB};
    default: return (VkFmt){VK_FORMAT_R8G8B8A8_UNORM};
    }
}

bool renderer_format_is_depth(uint32_t f) { return f >= 0x31 && f <= 0x36; }
bool renderer_format_has_stencil(uint32_t f) { return f == 0x31 || f == 0x35 || f == 0x36; }
VkFormat vk_format(uint32_t f) { return fmt_of(f).vf; }
bool vk_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }
bool renderer_format_alpha_one(uint32_t f) { return fmt_of(f).alpha_one; }

static VkImageAspectFlags aspect_of(uint32_t f) {
    if (f == 0x31) return VK_IMAGE_ASPECT_STENCIL_BIT;
    if (f == 0x35 || f == 0x36) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (renderer_format_is_depth(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

static bool format_supports(VkFormat f, VkFormatFeatureFlags want) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    return (p.optimalTilingFeatures & want) == want;
}

// (shader preparation) the format and aspects an image of this NVN format gets as a render target
// (create_backend_image: a format the device can't sample becomes RGBA8; compressed formats are never targets)
VkFormat vk_image_format(uint32_t nvn, VkImageAspectFlags *aspect) {
    VkFormat vf = vk_format(nvn);
    if (!format_supports(vf, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) vf = VK_FORMAT_R8G8B8A8_UNORM;
    if (aspect) *aspect = renderer_format_is_depth(nvn) ? aspect_of(nvn) : VK_IMAGE_ASPECT_COLOR_BIT;
    return vf;
}

// ================================================================== output resolution / per-frame clears
// Same rules as metal.m (see the comments there).
static float g_rs_x = 1, g_rs_y = 1;   // the render scale: what the frame is drawn at
static float g_al_x = 1, g_al_y = 1;   // the allocation scale: the size of scalable textures (Dynamic: the frame uses a part)
void renderer_get_scale(float *sx, float *sy) { *sx = g_rs_x; *sy = g_rs_y; }
void renderer_set_scale(float sx, float sy) { g_rs_x = sx; g_rs_y = sy; }
void renderer_set_alloc_scale(float ax, float ay) { g_al_x = ax; g_al_y = ay; }
// The part of a scalable texture its contents occupy (Dynamic: the render scale over the allocation scale; 1 otherwise).
void renderer_texture_fraction(NvnTexture *t, float *fx, float *fy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    *fx = *fy = 1;
    if (b && b->scalable && b->ax > 0 && b->ay > 0) { *fx = b->sx / b->ax; *fy = b->sy / b->ay; }
}
uint64_t renderer_present_count(void);

static bool tex_scalable(const NvnTexture *t) {
    static int off = -1;  // (per texture lookup: cached)
    if (off < 0) off = getenv("LARECOMP_NO_SCALE") != NULL;
    if (off) return false;
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;
    float a = (float)t->width / (float)t->height;
    return a > 1.74f && a < 1.81f;
}

// Shadow maps (square depth render targets, also arrays: Pokemon Z-A's cascades are 1024x1024 x 3) keep the game's size
// at any render scale, so their texels get bigger on screen as the image gets sharper: soft, blocky shadow edges at
// 1.5x-4x. F1 > Shadow resolution (shadow_resolution in the ini; LARECOMP_SHADOW_SCALE=<1-4> overrides it): they're
// drawn at that many times the size instead, from the next start (the size is taken once, before the first one is
// made). Shaders sample them with normalized coordinates (texel-based reads get the scale like the HD pack's textures:
// renderer_texture_scale), and their passes' viewports follow it as for the scaled targets. A colour target drawn in
// the same pass must have the same size (Z-A's: an 8-bit 1024x1024 x 3 next to the cascades - left as it was, only a
// quarter of the bigger map was drawn), so square render-target arrays of 2-4 layers are scaled too, depth or not.
// Z-A at 4x: 4096x4096 x 3, ~240 MB of GPU memory for the two (15 MB at the game's size); RTX 4060, 1.5x, the shadow
// passes 3.1-3.4 -> 3.7 ms a frame. 1 (the game's size) by default.
static float tex_shadow_scale(const NvnTexture *t) {
    static int k = -1;
    if (k < 0) {
        const char *e = getenv("LARECOMP_SHADOW_SCALE");
        k = e ? atoi(e) : g_settings.shadow_res;
        if (k < 1) k = 1;
        if (k > 4) k = 4;
    }
    if (k == 1 || !(t->flags & 8) || t->samples > 1 || t->width != t->height || t->width < 256 || t->width * k > 8192) return 1;
    bool array = t->target == 4 && t->depth >= 2 && t->depth <= 4;
    if (!array && !(t->target == 1 && renderer_format_is_depth(t->format))) return 1;
    static int nlog;
    if (nlog++ < 4) LOG("[vk] shadow map %s %ux%u (%u layers) drawn at %ux%u\n", renderer_format_is_depth(t->format) ? "depth" : "colour", t->width, t->height, t->target == 4 ? t->depth : 1,
                        t->width * k, t->height * k);
    return (float)k;
}

bool renderer_texture_first_write(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    extern uint64_t renderer_exec_frame(void);
    uint64_t total_frames = renderer_exec_frame();  // (the frame being translated)
    if (!b || !b->presented || (b->sx == b->sy) || b->touched_frame == total_frames + 1) return false;
    b->touched_frame = total_frames + 1;
    return true;
}

static uint32_t g_level_gen = 1;
void vk_level_change_check(void) {
    extern const char *game_level_name(void);
    static char last[64];
    const char *lv = game_level_name();
    if (strncmp(lv, last, sizeof last - 1) != 0) {
        strlcpy(last, lv, sizeof last);
        if (!strncmp(lv, "TelephoneBox", 12) || ENV("LARECOMP_CLEAR_ALL_LEVELS")) g_level_gen++;
    }
}

bool renderer_texture_frame_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    extern uint64_t renderer_exec_frame(void);
    uint64_t total_frames = renderer_exec_frame();  // (the frame being translated)
    if (!b || !b->scalable || b->frame_cleared == total_frames + 1) return false;
    // LARECOMP_NO_FRAME_CLEAR skips the clear only while the contents cover the whole target: below 1x a frame draws
    // into part of it, and the rest would show what an earlier frame left there (issue #205: the upper part flickered)
    // (Dynamic: the contents cover the whole target only at the factor 1 - its render scale is the allocation's)
    if (g_vk.no_frame_clear && b->sx >= 1.0f && b->sy >= 1.0f && b->sx >= b->ax && b->sy >= b->ay) return false;
    b->frame_cleared = total_frames + 1;
    return true;
}

bool renderer_texture_level_fresh(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    static int keep = -1;
    if (keep < 0) keep = ENV("LARECOMP_KEEP_TARGETS") != NULL;
    if (!b || keep || b->level_gen == g_level_gen) return false;
    if (!b->scalable) { b->level_gen = g_level_gen; return false; }
    bool fresh = b->level_gen != 0;
    b->level_gen = g_level_gen;
    return fresh;
}

void renderer_texture_mark_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b) b->presented = true;
}

bool renderer_texture_presented(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && b->presented;
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    if (b && (b->scalable || b->hd || b->shadow_k > 1)) { *sx = b->sx; *sy = b->sy; }
    else { *sx = 1; *sy = 1; }
}

// ================================================================== images
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL, .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &di);
}

// Images destroyed by the guest are kept until the submissions that may use them have completed.
typedef struct { VkImage img; VmaAllocation mem; VkImageView view; VkBuffer buf; VkDeviceMemory bmem; uint64_t seq; } Dead;
static Dead *dead;
static int ndead, cap_dead;
static void views_forget(TexBackend *b);

void vk_collect_dead(void) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < ndead; i++) {
        if (dead[i].seq > done) { dead[k++] = dead[i]; continue; }
        if (dead[i].view) vkDestroyImageView(g_vk.dev, dead[i].view, NULL);
        else if (dead[i].buf) { vkDestroyBuffer(g_vk.dev, dead[i].buf, NULL); vkFreeMemory(g_vk.dev, dead[i].bmem, NULL); }
        else vmaDestroyImage(g_vk.vma, dead[i].img, dead[i].mem);
    }
    ndead = k;
}

// The sRGB / UNORM counterpart of a colour format (VK_FORMAT_UNDEFINED: none). (g_vk.lean_usage format lists)
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
    case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
    case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
    case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
    default:
        if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK)  // (UNORM, SRGB alternate)
            return (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) & 1 ? f - 1 : f + 1;
        return VK_FORMAT_UNDEFINED;
    }
}

VkFormat vk_view_format(TexBackend *b, VkFormat want) {
    if (!b || !b->nview_fmt || want == b->format) return want;
    for (int i = 0; i < b->nview_fmt; i++) if (b->view_fmt[i] == want) return want;
    static int nlog;
    if (nlog++ < 10) LOG("[vk] lean usage: view format %d of an image of format %d refused (its own instead)\n", want, b->format);
    return b->format;
}

static void create_image(NvnTexture *t, TexBackend *b) {
    VkFmt mf = fmt_of(t->format);
    b->ax = b->scalable ? g_al_x : 1;
    b->ay = b->scalable ? g_al_y : 1;
    b->sx = b->scalable ? g_rs_x : 1;
    b->sy = b->scalable ? g_rs_y : 1;
    uint32_t tw = t->width ? t->width : 1, th = t->height ? t->height : 1;
    if (b->scalable) {
        tw = (uint32_t)lroundf(tw * b->ax); th = (uint32_t)lroundf(th * b->ay);
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    if (b->shadow_k > 1) {
        tw = (uint32_t)lroundf(tw * b->shadow_k); th = (uint32_t)lroundf(th * b->shadow_k);
        b->sx = b->sy = b->shadow_k;
    }
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t layers = 1, depth = 1;
    switch (t->target) {
    case 0: type = VK_IMAGE_TYPE_1D; th = 1; break;
    case 2: type = VK_IMAGE_TYPE_3D; depth = t->depth ? t->depth : 1; break;
    case 3: type = VK_IMAGE_TYPE_1D; layers = t->height ? t->height : 1; th = 1; break;
    case 4: case 6: layers = t->depth ? t->depth : 1; break;
    case 8: layers = 6; break;
    case 9: layers = t->depth ? t->depth : 6; break;
    default: break;
    }
    uint32_t levels = t->levels ? t->levels : 1;
    {   // never more levels than the size allows (scaled targets, odd guest values)
        uint32_t m = 1, sz = tw > th ? tw : th;
        if (depth > sz) sz = depth;
        while (sz > 1) { sz >>= 1; m++; }
        if (levels > m) levels = m;
    }
    uint32_t samples = t->samples > 1 ? t->samples : 1;
    if (samples > 1) levels = 1;
    VkFormat vf = mf.vf;
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    b->astc_bw = b->astc_bh = 0;
    if (!format_supports(vf, need)) {
        bool astc = vf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && vf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        const FormatInfo *fi = t->layout.fmt;
        if (astc && fi && fi->bpb == 16) {  // desktop GPUs: decoded on the CPU into an RGBA8 image of the same colour space
            b->astc_bw = fi->bw;
            b->astc_bh = fi->bh;
            b->astc_srgb = t->format >= 0x87;
            vf = b->astc_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        } else {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] format %d (nvn %#x) not supported: RGBA8 instead\n", vf, t->format);
            vf = VK_FORMAT_R8G8B8A8_UNORM;
        }
    }
    bool depthfmt = renderer_format_is_depth(t->format);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (depthfmt) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if (format_supports(vf, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateFlags flags = 0;
    if (!depthfmt) flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;  // format views (NVN texture views, raw sRGB views)
    if (type == VK_IMAGE_TYPE_2D && layers >= 6 && tw == th && samples == 1) flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    // 3D render targets (Pokémon Z-A's colour grading LUT, one slice per instance): rendered through 2D array views
    if (type == VK_IMAGE_TYPE_3D && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    // g_vk.lean_usage (integrated GPUs): colour images may only be viewed as their own format and its sRGB / UNORM
    // twin, listed in a VkImageFormatListCreateInfo (no twin: not mutable at all). Mesa's AMD / Intel drivers keep
    // DCC / CCS compression for such images; with MUTABLE_FORMAT and no list they turn it off. Views in any other
    // format fall back to the image's own (vk_view_format, logged).
    VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    b->nview_fmt = 0;
    if (!depthfmt && g_vk.lean_usage) {
        VkFormat twin = srgb_twin(vf);
        b->view_fmt[0] = vf;
        b->view_fmt[1] = twin;
        b->nview_fmt = twin != VK_FORMAT_UNDEFINED ? 2 : 1;
        if (b->nview_fmt == 1) flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        else fl.viewFormatCount = 2, fl.pViewFormats = b->view_fmt;
    }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = fl.viewFormatCount ? &fl : NULL,
                            .flags = flags, .imageType = type, .format = vf,
                            .extent = {tw, th, depth}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkResult r = vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL);
    if (r != VK_SUCCESS) {
        // (the Vulkan side too: a driver can refuse a format / usage / tiling combination with "out of device memory",
        // issue #45 - a GTX 1060 failing a 1920x1080 image with plenty of memory free)
        LOG("[vk] image creation failed (%d): nvn fmt %#x %ux%ux%u target %u - vk format %d, usage %#x, flags %#x, "
            "%u levels, %u layers, %u samples\n", r, t->format, t->width, t->height, t->depth, t->target, ci.format,
            ci.usage, ci.flags, ci.mipLevels, ci.arrayLayers, (unsigned)ci.samples);
        vk_log_memory("image creation failed");
        ci.format = vf = VK_FORMAT_R8G8B8A8_UNORM;
        ci.pNext = NULL;  // (g_vk.lean_usage: the fallback image is mutable as before, no format list)
        ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        b->nview_fmt = 0;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.flags &= ~VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
        depthfmt = false;
    }
    b->format = vf;
    b->width = tw; b->height = th; b->depth = depth; b->layers = layers; b->levels = levels; b->samples = samples;
    b->aspect = depthfmt ? aspect_of(t->format) : VK_IMAGE_ASPECT_COLOR_BIT;
    b->base_type = type == VK_IMAGE_TYPE_1D ? (layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D)
                 : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D
                 : t->target == 8 ? VK_IMAGE_VIEW_TYPE_CUBE : t->target == 9 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
                 : layers > 1 || t->target == 4 || t->target == 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vk_image_init_layout(vk_upload_cmd(), b->image, b->aspect);
    // New depth / stencil images start zeroed. The game builds its depth pyramid with depth-only copies, so the stencil
    // of those images was never written: whatever the memory held, different per image and per run, then failed or
    // passed the depth-of-field passes' stencil test in patches (flicker that came and went between runs, every other
    // frame, below 1x internal resolution).
    // Scalable colour targets too (as Metal's, metal.m clear_new_target): Dynamic resolution draws into their top-left
    // part only, and the game's blurs sample a few texels past it, where a new image holds whatever its memory held -
    // NaN / infinity in a float target among it, which the bloom chain spreads and keeps (a pure green band down the
    // right edge, issue #205; every window resize reallocates these images). Copies into them write only the contents'
    // part, and with LARECOMP_NO_FRAME_CLEAR the per-frame clear is skipped. LARECOMP_NEW_TARGET_FILL=<r>,<g>,<b>,<a>
    // (test): that colour instead of zeros.
    bool clear_color = !depthfmt && b->scalable && (ci.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (depthfmt || clear_color) {
        VkCommandBuffer cb = vk_upload_cmd();
        VkImageSubresourceRange rg = {b->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        if (depthfmt) {
            VkClearDepthStencilValue zero = {0.0f, 0};
            rec_CmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rg);
            if (levels > 1 && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                // Levels below 0 of a depth image (Pokémon Z-A's GPU culling tests small / distant objects against coarse
                // levels of its scene depth - a hierarchical-Z pyramid nothing here fills in): its depth is reversed, so
                // 0 is far and hides nothing (1.0 culled the gate, lamp posts and planters). LARECOMP_HIZ_FAR=<value>.
                static float far = -1;
                if (far < 0) far = ENV("LARECOMP_HIZ_FAR") ? (float)atof(ENV("LARECOMP_HIZ_FAR")) : 0.0f;
                VkClearDepthStencilValue fv = {far, 0};
                VkImageSubresourceRange r1 = {VK_IMAGE_ASPECT_DEPTH_BIT, 1, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
                rec_CmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &fv, 1, &r1);
            }
        } else {
            static int fill = -1;
            static VkClearColorValue fc;
            if (fill < 0) {
                const char *e = getenv("LARECOMP_NEW_TARGET_FILL");
                float *c = fc.float32;
                fill = e && sscanf(e, "%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3]) == 4;
            }
            VkClearColorValue zero = {{0}};
            rec_CmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, fill ? &fc : &zero, 1, &rg);
        }
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        rec_CmdPipelineBarrier2(cb, &di);
    }
}

static void defer(Dead d) {
    if (ndead == cap_dead) { cap_dead = cap_dead ? cap_dead * 2 : 256; dead = realloc(dead, cap_dead * sizeof *dead); }
    d.seq = vk_pending_seq() + VK_DEAD_MARGIN;
    dead[ndead++] = d;
}

void vk_defer_image(VkImage img, VmaAllocation mem) { defer((Dead){.img = img, .mem = mem}); }
void vk_defer_view(VkImageView v) { defer((Dead){.view = v}); }
void vk_defer_buffer(VkBuffer b, VkDeviceMemory m) { defer((Dead){.buf = b, .bmem = m}); }  // (raw memory, not VMA)

void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv);  // (exec.c: LARECOMP_VK_DEVLOST_TRACE)

static uint32_t level_w(const TexBackend *b, uint32_t lv) { uint32_t v = b->width >> lv; return v ? v : 1; }
static uint32_t level_h(const TexBackend *b, uint32_t lv) { uint32_t v = b->height >> lv; return v ? v : 1; }

static void destroy_image(TexBackend *b) {
    views_forget(b);
    if (b->image || b->mem) defer((Dead){.img = b->image, .mem = b->mem});  // (never the same allocation twice)
    b->image = VK_NULL_HANDLE;
    b->mem = VK_NULL_HANDLE;
}

// Live game textures: an in-place reboot (vk_textures_reset) destroys them all.
static NvnTexture **g_live;
static int g_nlive, g_cap_live;
static void live_add(NvnTexture *t, TexBackend *b) {
    if (g_nlive == g_cap_live) { g_cap_live = g_cap_live ? g_cap_live * 2 : 1024; g_live = realloc(g_live, g_cap_live * sizeof *g_live); }
    g_live[g_nlive++] = t;
    b->reg = g_nlive;
}
static void live_remove(TexBackend *b) {
    if (!b->reg) return;
    NvnTexture *last = g_live[--g_nlive];
    g_live[b->reg - 1] = last;
    if (last->backend) ((TexBackend *)last->backend)->reg = b->reg;
    b->reg = 0;
}

static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        if (b->scalable && (b->ax != g_al_x || b->ay != g_al_y)) {  // output resolution changed: reallocate
            destroy_image(b);
            create_image(t, b);
        }
        return b;
    }
    vk_collect_dead();
    b = calloc(1, sizeof *b);
    b->scalable = tex_scalable(t);
    b->shadow_k = tex_shadow_scale(t);
    create_image(t, b);
    t->backend = b;
    live_add(t, b);
    return b;
}

static void hd_forget(NvnTexture *t);  // (HD texture packs, below)
void backend_texture_destroyed(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
        extern void vk_alias_texture_destroyed(NvnTexture *t);  // (exec.c copy aliases: a source's contents handed over)
        vk_alias_texture_destroyed(t);
        extern void vk_exec_texture_destroyed(NvnTexture *t);  // (exec.c: bound render targets and snapshots forget it)
        vk_exec_texture_destroyed(t);
        extern void vk_device_texture_destroyed(NvnTexture *t);  // (device.c: the last presented texture)
        vk_device_texture_destroyed(t);
        hd_forget(t);
        live_remove(b);
        destroy_image(b);
        free(b->pending_rgba);
        free(b);
    }
    t->backend = NULL;
}

// Reboot (device.c backend_reset; GPU idle): every game texture and its views go.
void vk_textures_reset(void) {
    int n = g_nlive;
    extern void vk_alias_reset(void);
    vk_alias_reset();
    while (g_nlive > 0) backend_texture_destroyed(g_live[g_nlive - 1]);
    vk_collect_dead();
    LOG("[vk] reboot: %d textures released\n", n);
}


// ================================================================== HD texture packs
// A game texture is known by hash_full of its bytes as the game wrote them (deterministic: the same texture has the
// same name in every run). LARECOMP_TEX_DUMP=1 (or setting tex_dump) saves each 2D colour texture the game uploads, as
// it decodes (BCn / ASTC through the GPU), to textures/dump/<hash>.png - feed those to an upscaler (tools/za/
// upscale_textures.py) and put the results in textures/hd/<hash>.png: such a texture is then drawn from that image
// instead (any size; RGBA8, mipmaps made here), the shaders told its size over the game's as for render targets at the
// internal resolution (texel fetches and size queries stay in the game's units). Textures the game rewrites go back to
// its own data. Setting tex_hd = 0 / LARECOMP_TEX_HD=0: packs off; F1 > Textures switches them while playing.
// A pack texture can also be <hash>.dds (BC7 / BC1 / BC3 with its mip levels, as tools/za/hd_pack.py makes them):
// smaller in GPU memory and quicker to load than a PNG, and preferred when both are there.
// The same way, whatever the HD setting: the game's own button icons relabelled for the prompts' style
// (textures/prompts/<style>, runtime/hle/pl.c pl_prompt_set; made by tools/za/prompt_textures.py), ahead of the pack.
static int hd_on = -1, hd_dump = -1;
typedef struct { uint64_t *h; int n; char dir[48]; } HdSet;  // sorted hashes of <dir>/*.png, *.dds
static HdSet hd_sets[2];  // [0] button prompts, [1] HD pack
static int hd_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

static void hd_scan(HdSet *s, const char *dir) {
    snprintf(s->dir, sizeof s->dir, "%s", dir);
    DIR *d = opendir(dir);
    if (!d) return;
    int cap = 0;
    for (struct dirent *de; (de = readdir(d));) {
        unsigned long long h;
        char ext[8] = "";
        if (strlen(de->d_name) != 20 || sscanf(de->d_name, "%16llx.%4s", &h, ext) != 2 || (strcmp(ext, "png") && strcmp(ext, "dds")))
            continue;
        if (s->n == cap) { cap = cap ? cap * 2 : 1024; s->h = realloc(s->h, cap * sizeof *s->h); }
        s->h[s->n++] = h;
    }
    closedir(d);
    qsort(s->h, s->n, sizeof *s->h, hd_cmp);
    int k = 0;  // (a texture there as both .png and .dds: once)
    for (int i = 0; i < s->n; i++) if (!k || s->h[i] != s->h[k - 1]) s->h[k++] = s->h[i];
    s->n = k;
    LOG("[tex] %d textures in %s\n", s->n, dir);
}

static void hd_init(void) {
    if (hd_on >= 0) return;
    const char *e = ENV("LARECOMP_TEX_HD");
    hd_on = e ? atoi(e) != 0 : g_settings.tex_hd;
    e = ENV("LARECOMP_TEX_DUMP");
    hd_dump = e ? atoi(e) != 0 : g_settings.tex_dump;
    if (hd_dump) { SDL_CreateDirectory("textures"); SDL_CreateDirectory("textures/dump"); }
    extern const char *pl_prompt_set(void);
    const char *set = pl_prompt_set();
    char dir[48];
    snprintf(dir, sizeof dir, "textures/prompts/%s", set ? set : "");
    if (set) hd_scan(&hd_sets[0], dir);
    if (hd_on) hd_scan(&hd_sets[1], "textures/hd");
}

static const HdSet *hd_find(uint64_t h) {
    for (int i = 0; i < (hd_on > 0 ? 2 : 1); i++)
        if (hd_sets[i].n && bsearch(&h, hd_sets[i].h, hd_sets[i].n, sizeof h, hd_cmp)) return &hd_sets[i];
    return NULL;
}
static bool hd_has(uint64_t h) { return hd_find(h) != NULL; }

// plain 2D colour textures the game fills from the CPU (not render targets, video frames or linear / changing ones)
static bool hd_candidate(const NvnTexture *t, const TexBackend *b) {
    return (t->target == 1 || t->target == 5) && !b->scalable && !b->gpu_owned && !b->dynamic && !b->pending_rgba &&
           b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->depth == 1 && b->layers == 1 && b->samples == 1 &&
           t->width >= 16 && t->height >= 16 && !(t->flags & 0x10);
}

static bool hd_load_dds(NvnTexture *t, TexBackend *b, uint64_t h, const char *path);

static bool hd_replace(NvnTexture *t, TexBackend *b, uint64_t h) {
    char path[80];
    snprintf(path, sizeof path, "%s/%016llx.dds", hd_find(h)->dir, (unsigned long long)h);
    if (hd_load_dds(t, b, h, path)) return true;
    snprintf(path, sizeof path, "%s/%016llx.png", hd_find(h)->dir, (unsigned long long)h);
    SDL_Surface *s0 = SDL_LoadPNG(path);
    if (!s0) { LOG("[tex] %s: %s\n", path, SDL_GetError()); return false; }
    SDL_Surface *s = SDL_ConvertSurface(s0, SDL_PIXELFORMAT_ABGR8888);  // (R, G, B, A bytes)
    SDL_DestroySurface(s0);
    if (!s) return false;
    uint32_t w = (uint32_t)s->w, hh = (uint32_t)s->h, levels = 1;
    for (uint32_t m = w > hh ? w : hh; m > 1; m >>= 1) levels++;
    VkFormat vf = vk_format_is_srgb(b->format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D, .format = vf, .extent = {w, hh, 1}, .mipLevels = levels,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) { SDL_DestroySurface(s); return false; }
    destroy_image(b);
    b->image = img, b->mem = mem, b->format = vf;
    b->width = w, b->height = hh, b->depth = 1, b->layers = 1, b->levels = levels, b->samples = 1;
    b->aspect = VK_IMAGE_ASPECT_COLOR_BIT, b->base_type = VK_IMAGE_VIEW_TYPE_2D, b->astc_bw = b->astc_bh = 0;
    b->hd = true, b->sx = (float)w / (float)t->width, b->sy = (float)hh / (float)t->height;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkSpan sp = vk_transient((uint64_t)w * hh * 4, 16);
    for (uint32_t y = 0; y < hh; y++) memcpy((uint8_t *)sp.ptr + (uint64_t)y * w * 4, (const uint8_t *)s->pixels + (uint64_t)y * s->pitch, (size_t)w * 4);
    SDL_DestroySurface(s);
    VkBufferImageCopy rg = {.bufferOffset = sp.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, hh, 1}};
    rec_CmdCopyBufferToImage(cb, sp.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    for (uint32_t lv = 1; lv < levels; lv++) {  // each level a linear downsample of the one above
        rec_CmdPipelineBarrier2(cb, &di);
        int32_t sw = (int32_t)level_w(b, lv - 1), sh = (int32_t)level_h(b, lv - 1);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv - 1, 0, 1}, .srcOffsets = {{0, 0, 0}, {sw, sh, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1},
                          .dstOffsets = {{0, 0, 0}, {(int32_t)level_w(b, lv), (int32_t)level_h(b, lv), 1}}};
        rec_CmdBlitImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    }
    rec_CmdPipelineBarrier2(cb, &di);
    static int n;
    if (n++ < 20) LOG("[tex] HD %016llx: %ux%u for %ux%u\n", (unsigned long long)h, w, hh, t->width, t->height);
    return true;
}

// A .dds pack texture: block-compressed, every mip level in the file (DX10 header: BC7 / BC1 / BC3, sRGB or not; or
// the legacy DXT1 / DXT5 codes)
static bool hd_load_dds(NvnTexture *t, TexBackend *b, uint64_t h, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = len > 148 ? malloc(len) : NULL;
    bool ok = d && fread(d, 1, len, f) == (size_t)len && !memcmp(d, "DDS ", 4);
    fclose(f);
    if (!ok) { free(d); LOG("[tex] %s: not a DDS file\n", path); return false; }
    uint32_t hh, w, levels, fourcc, dxgi = 0;
    memcpy(&hh, d + 12, 4);
    memcpy(&w, d + 16, 4);
    memcpy(&levels, d + 28, 4);
    memcpy(&fourcc, d + 84, 4);
    size_t off = 128;
    if (!memcmp(&fourcc, "DX10", 4)) { memcpy(&dxgi, d + 128, 4); off = 148; }
    else if (!memcmp(&fourcc, "DXT1", 4)) dxgi = 71;
    else if (!memcmp(&fourcc, "DXT5", 4)) dxgi = 77;
    VkFormat vf;
    uint32_t bpb = 16;
    switch (dxgi) {
    case 98: vf = VK_FORMAT_BC7_UNORM_BLOCK; break;
    case 99: vf = VK_FORMAT_BC7_SRGB_BLOCK; break;
    case 71: vf = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; bpb = 8; break;
    case 72: vf = VK_FORMAT_BC1_RGBA_SRGB_BLOCK; bpb = 8; break;
    case 77: vf = VK_FORMAT_BC3_UNORM_BLOCK; break;
    case 78: vf = VK_FORMAT_BC3_SRGB_BLOCK; break;
    default: free(d); LOG("[tex] %s: unsupported DDS format %u\n", path, dxgi); return false;
    }
    if (!levels) levels = 1;
    uint64_t total = 0;
    for (uint32_t lv = 0; lv < levels; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = hh >> lv ? hh >> lv : 1;
        total += (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * bpb;
    }
    if (off + total > (uint64_t)len) { free(d); LOG("[tex] %s: truncated\n", path); return false; }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                            .imageType = VK_IMAGE_TYPE_2D, .format = vf, .extent = {w, hh, 1}, .mipLevels = levels,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage img;
    VmaAllocation mem;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) { free(d); return false; }
    destroy_image(b);
    b->image = img, b->mem = mem, b->format = vf;
    b->width = w, b->height = hh, b->depth = 1, b->layers = 1, b->levels = levels, b->samples = 1;
    b->aspect = VK_IMAGE_ASPECT_COLOR_BIT, b->base_type = VK_IMAGE_VIEW_TYPE_2D, b->astc_bw = b->astc_bh = 0;
    b->hd = true, b->sx = (float)w / (float)t->width, b->sy = (float)hh / (float)t->height;
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkSpan sp = vk_transient(total, 16);
    memcpy(sp.ptr, d + off, total);
    free(d);
    uint64_t at = 0;
    for (uint32_t lv = 0; lv < levels; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = hh >> lv ? hh >> lv : 1;
        VkBufferImageCopy rg = {.bufferOffset = sp.off + at, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, 1},
                                .imageExtent = {lw, lh, 1}};
        rec_CmdCopyBufferToImage(cb, sp.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        at += (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * bpb;
    }
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    static int n;
    if (n++ < 20) LOG("[tex] HD %016llx: %ux%u (%u levels) for %ux%u\n", (unsigned long long)h, w, hh, levels, t->width, t->height);
    return true;
}

// F1 > Textures (setting tex_hd) changed: between frames, every live texture goes back to the game's image or is
// looked up in the pack again (the next upload check rehashes it)
static void hd_switch(void) {
    static int last = -1;
    static long flip_at = -2;  // (tests: LARECOMP_TEX_HD_FLIP=<frame> switches the setting at that frame, as F1 would)
    if (flip_at == -2) { const char *e = ENV("LARECOMP_TEX_HD_FLIP"); flip_at = e ? atol(e) : -1; }
    if (flip_at >= 0 && vk_frame_counter() == (uint64_t)flip_at) g_settings.tex_hd = !g_settings.tex_hd;
    hd_init();
    if (last < 0) { last = g_settings.tex_hd; return; }
    if (g_settings.tex_hd == last) return;
    last = g_settings.tex_hd;
    if (last && !hd_sets[1].n) hd_scan(&hd_sets[1], "textures/hd");
    bool was = hd_on > 0;
    hd_on = last;
    int n = 0;
    for (int i = 0; i < g_nlive; i++) {
        NvnTexture *t = g_live[i];
        TexBackend *b = t->backend;
        if (!b) continue;
        if (was && !hd_on && b->hd && !(hd_sets[0].n && bsearch(&b->data_hash, hd_sets[0].h, hd_sets[0].n, sizeof b->data_hash, hd_cmp))) {
            destroy_image(b);
            b->hd = false;
            create_image(t, b);
            b->uploaded = false;
            n++;
        } else if (hd_on && !b->hd && !b->dynamic) {
            b->uploaded = false;
            n++;
        }
    }
    LOG("[tex] textures: %s (%d re-uploaded)\n", hd_on ? "HD" : "original", n);
}

// Dumps: queued at upload, saved between frames (vk_texdump_process, from the present: no pass to interrupt)
typedef struct { NvnTexture *t; uint64_t h; } HdDump;
static HdDump hd_q[4096];
static int hd_qn;
static uint64_t *hd_done;  // hashes already dumped this run
static int hd_done_n, hd_done_cap;

static void hd_queue_dump(NvnTexture *t, uint64_t h) {
    for (int i = 0; i < hd_done_n; i++) if (hd_done[i] == h) return;
    if (hd_qn == 4096) return;
    if (hd_done_n == hd_done_cap) { hd_done_cap = hd_done_cap ? hd_done_cap * 2 : 1024; hd_done = realloc(hd_done, hd_done_cap * sizeof *hd_done); }
    hd_done[hd_done_n++] = h;
    hd_q[hd_qn++] = (HdDump){t, h};
}

static void hd_forget(NvnTexture *t) {
    for (int i = 0; i < hd_qn; i++) if (hd_q[i].t == t) hd_q[i].t = NULL;
}

void renderer_save_texture_tex(TexBackend *b, const char *path);
void vk_hd_switch_frame(void) { hd_switch(); }  // (device.c backend_present_now, each frame)
void vk_texdump_process(void) {
    hd_switch();
    if (!hd_qn) return;
    int done = 0, k = 0;
    for (int i = 0; i < hd_qn; i++) {
        HdDump d = hd_q[i];
        TexBackend *b = d.t ? d.t->backend : NULL;
        if (!b || b->hd || b->data_hash != d.h) continue;  // (gone or changed)
        if (done >= 24) { hd_q[k++] = d; continue; }  // (a few per frame: each one waits for the GPU)
        char path[64];
        snprintf(path, sizeof path, "textures/dump/%016llx.png", (unsigned long long)d.h);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); continue; }  // (from an earlier run)
        renderer_save_texture_tex(b, path);
        done++;
    }
    hd_qn = k;
}

// ================================================================== uploads (same change detection as metal.m)
static uint64_t full_hash_budget, full_hash_budget_frame;

// Four independent lanes (one serial multiply chain ran at ~1.5 bytes/cycle; dynamic textures are hashed every
// frame). Values are only compared within a run.
static uint64_t hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

static uint64_t fnv_sample(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    uint64_t step = n > 4096 ? n / 4096 : 1;
    for (uint64_t i = 0; i < n; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    return h ^ n;
}

static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;

static void upload_pending(TexBackend *b) {
    pthread_mutex_lock(&pending_lock);
    void *px = b->pending_rgba;
    uint32_t w = b->pending_w, h = b->pending_h, stride = b->pending_stride, uv_off = b->pending_uv_off;
    bool nv12 = b->pending_nv12;
    b->pending_rgba = NULL;
    pthread_mutex_unlock(&pending_lock);
    if (!px) return;
    if (nv12) { vk_convert_nv12(b, px, w, h, stride, uv_off); free(px); return; }
    if (w > b->width) w = b->width;
    if (h > b->height) h = b->height;
    VkSpan s = vk_transient_copy(px, (uint64_t)stride * h, 16);
    free(px);
    VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = stride / 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    VkCommandBuffer cb = vk_upload_cmd();
    vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}

// ---- texture data unpacked on several threads
// A load's new textures (Pokémon Z-A starting a battle: 870 of them, 125 MB in one frame, 87 ms of the render thread)
// were each unpacked (block-linear -> linear rows) into the upload ring on the spot. The copy into the image is
// recorded right away but runs on the GPU only after its submission, so the unpacking only has to be done by then: the
// plain cases (no RGB expansion, ASTC decode or texture patch) are queued here, and vk_tex_unpack_finish (exec.c
// vk_submit_extra, before the submission) runs the queue on a few threads. LARECOMP_TEX_UNPACK_THREADS=<n>: how many
// (default half the CPU's, at most 6; 0: on the spot, as before).
typedef struct { const uint8_t *src; uint8_t *dst; uint32_t wb, hb, d, bpb, bh, bd, row, stride; bool linear; } Unpack;
static Unpack *up_q;
static size_t up_n, up_cap;
static uint64_t up_bytes;
static pthread_mutex_t up_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t up_go = PTHREAD_COND_INITIALIZER, up_done = PTHREAD_COND_INITIALIZER;
static _Atomic size_t up_next;
static size_t up_total;  // (up_lock; read by the helpers after they took it)
static int up_busy;
static uint32_t up_gen;

static int unpack_threads(void) {
    static int n = -1;
    if (n < 0) {
        const char *e = ENV("LARECOMP_TEX_UNPACK_THREADS");
        int cores = plat_cpu_count() > 0 ? plat_cpu_count() : SDL_GetNumLogicalCPUCores();
        n = e ? atoi(e) : cores / 2 < 6 ? cores / 2 : 6;
        if (n < 0) n = 0;
    }
    return n;
}

static void unpack_one(const Unpack *u) {
    if (u->linear) for (uint32_t y = 0; y < u->hb; y++) memcpy(u->dst + (uint64_t)y * u->row, u->src + (uint64_t)y * u->stride, u->row);
    else tegra_deswizzle(u->src, u->dst, u->wb, u->hb, u->d, u->bpb, u->bh, u->bd);
}

static void unpack_some(void) {  // (the queue's next items, until none are left)
    for (size_t i; (i = atomic_fetch_add(&up_next, 1)) < up_total;) unpack_one(&up_q[i]);
}

static void *unpack_helper(void *arg) {
    (void)arg;
    uint32_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&up_lock);
        while (up_gen == seen) pthread_cond_wait(&up_go, &up_lock);
        seen = up_gen;
        up_busy++;
        pthread_mutex_unlock(&up_lock);
        unpack_some();
        pthread_mutex_lock(&up_lock);
        if (--up_busy == 0) pthread_cond_signal(&up_done);
        pthread_mutex_unlock(&up_lock);
    }
    return NULL;
}

void vk_tex_unpack_finish(void) {
    if (!up_n) return;
    int n = unpack_threads();
    if (up_bytes < (2u << 20) || n < 2) {  // (a few small ones: not worth waking anyone)
        for (size_t i = 0; i < up_n; i++) unpack_one(&up_q[i]);
    } else {
        static bool started;
        if (!started) {
            started = true;
            for (int i = 0; i < n - 1; i++) { pthread_t t; if (!pthread_create(&t, NULL, unpack_helper, NULL)) pthread_detach(t); }
        }
        pthread_mutex_lock(&up_lock);
        up_total = up_n;
        atomic_store(&up_next, 0);
        up_gen++;
        pthread_cond_broadcast(&up_go);
        pthread_mutex_unlock(&up_lock);
        unpack_some();  // (this thread too)
        pthread_mutex_lock(&up_lock);
        while (up_busy) pthread_cond_wait(&up_done, &up_lock);
        up_total = 0;
        pthread_mutex_unlock(&up_lock);
    }
    up_n = 0, up_bytes = 0;
}

uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn, prof_tex_watch_skips;
uint64_t prof_tex_watch_written, prof_tex_watch_unknown, prof_tex_watch_gpu;  // (checks the page tracking couldn't skip: why)
static void upload_texture(NvnTexture *t, TexBackend *b) {
    if (!t->addr || t->target == 10) return;
    uint64_t frame = vk_frame_counter();
    if (b->uploaded && b->checked_frame == frame) return;
    // (a prefetched one: checked at its first use after that frame, not 1 frame in 8 - the game may have written its
    // data after registering it - and a change found then doesn't make it dynamic)
    if (b->uploaded && !b->dynamic && !b->prefetched && ((frame + ((uintptr_t)b >> 6)) & 7)) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb) return;
    VkFmt mf = fmt_of(t->format);
    uint64_t total = tegra_total_size(&t->layout);
    bool was_prefetched = b->prefetched;
    if (b->uploaded) b->prefetched = false;
    b->checked_frame = frame;
    prof_tex_checks++;
    if (t->flags & 0x10) b->dynamic = true;
    if (full_hash_budget_frame != frame) { full_hash_budget_frame = frame; full_hash_budget = 4 << 20; }
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    static int always_full = -1;
    if (always_full < 0) always_full = ENV("LARECOMP_TEX_FULLHASH") ? 1 : 0;
    bool full = b->dynamic || !b->uploaded || always_full;
    // (safety net: a full hash now and then. Every 600 frames - 10 s at 60 fps: since the page tracking covers the textures
    // outside the guest memory window too (exec.c pages_changed_outside), it rehashed up to 150 MB a second, finding
    // nothing. LARECOMP_TEX_RECHECK=<frames>: another interval)
    static long recheck = -1;
    if (recheck < 0) recheck = ENV("LARECOMP_TEX_RECHECK") ? atol(ENV("LARECOMP_TEX_RECHECK")) : 600;
    bool periodic = frame - b->full_frame >= (uint64_t)recheck && full_hash_budget >= total;
    if (!full && periodic) { full = true; full_hash_budget -= total; }
    bool identified_now = !b->tp;  // (button prompts / Hero Mode icon: re-uploaded when their look changes)
    if (identified_now) b->tp = texpatch_marker(t);
    bool restyle = b->tp > 0 && b->tp <= TEXPATCH_RECOLOUR_BASE && b->uploaded && b->tp_gen != texpatch_generation();
    // The OS's page write tracking (exec.c vk_pages_changed, shared with the vertex cache): texture memory the CPU
    // hasn't written since the last check is not hashed at all (dynamic textures were fully hashed every frame,
    // ~2.4 MB a frame in the overworld). The periodic full hash stays as a safety net. LARECOMP_VK_TEX_WATCH=0: off.
    static int watch = -1;
    if (watch < 0) { const char *e = ENV("LARECOMP_VK_TEX_WATCH"); watch = !e || atoi(e) != 0; }
    bool tracked_clean = false;  // (the tracking said unwritten: a full hash that finds a change means it missed one)
    if (watch && !always_full) {
        extern int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now);
        uint32_t now = 0;
        int ch = vk_pages_changed(t->addr, total, b->wgen, &now);
        if (ch > 0) prof_tex_watch_written++;
        else if (ch < 0) prof_tex_watch_unknown++, prof_tex_watch_gpu += ch == -2;
        if (ch >= 0) {
            b->wgen = now;
            if (!ch && b->uploaded && !restyle) {
                if (!periodic) { prof_tex_watch_skips++; return; }
                tracked_clean = true;
                if (b->dynamic) full_hash_budget -= total;  // (the others took theirs above)
            }
        }
    }
    bool fast = tex_fast_hash();  // (runtime/nvn/tex_hash.h; LARECOMP_TEX_FASTHASH=0: the previous hashes)
    if (!full && !restyle) {
        prof_tex_samples++;
        uint64_t sh = fast ? tex_hash_sample(mem, total) : fnv_sample(mem, total);
        if (sh == b->sample_hash) return;
    }
    prof_tex_full++; prof_tex_full_bytes += total; if (b->dynamic) prof_tex_dyn++;
    extern void slow_note_full_hash(void), slow_note_upload(uint64_t bytes);  // (slowframe.h)
    slow_note_full_hash();
    uint64_t h = fast ? tex_hash_full(mem, total) : hash_full(mem, total);
    b->full_frame = frame;
    // (the sample is only compared while the texture isn't dynamic, and a dynamic one stays so: none for those)
    if (!fast) b->sample_hash = fnv_sample(mem, total);
    else if (!b->dynamic && !(b->uploaded && h != b->data_hash)) b->sample_hash = tex_hash_sample(mem, total);
    if (b->uploaded && h == b->data_hash && !restyle) return;
    if (tracked_clean && h != b->data_hash) {
        static int nmiss;
        if (nmiss++ < 20)
            LOG("[tex] %llx+%llx changed although its pages show no write since the last check (the safety net caught it)\n",
                (unsigned long long)t->addr, (unsigned long long)total);
    }
    if (b->uploaded && h != b->data_hash && !was_prefetched) b->dynamic = true;    // new contents (the game reused the memory for another image, or the data arrived after the first check): which
    // known texture it is, if any, is decided again before this upload patches it
    if (!identified_now && h != b->data_hash) b->tp = texpatch_marker(t);
    if (b->tp > 0) b->tp_gen = texpatch_generation();
    b->data_hash = h;
    b->uploaded = true;
    hd_init();
    if (b->hd && (b->dynamic || !hd_has(h))) {  // an HD-replaced texture the game rewrote: back to its own image
        destroy_image(b);
        b->hd = false;
        create_image(t, b);
    } else if (b->hd) {
        return;
    }
    if (hd_candidate(t, b)) {
        if (hd_has(h) && hd_replace(t, b, h)) return;
        if (hd_dump) hd_queue_dump(t, h);
    }
    if (b->format != mf.vf && !b->astc_bw) return;  // unsupported format: nothing sensible to copy
    slow_note_upload(total);
    VkCommandBuffer cb = vk_upload_cmd();
    uint32_t layers = t->layout.layers ? t->layout.layers : 1;
    if (layers > b->layers && b->depth == 1) layers = b->layers;
    uint64_t layer_size = tegra_layer_size(&t->layout);
    bool linear = (t->flags & 0x10) != 0;
    uint32_t levels = t->layout.levels < b->levels ? t->layout.levels : b->levels;
    for (uint32_t layer = 0; layer < layers; layer++) {
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            if (b->astc_bw) {  // blocks -> linear block rows -> RGBA8 texels
                const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
                uint8_t *blocks = malloc((uint64_t)l.wb * l.hb * 16);
                if (linear) for (uint32_t y = 0; y < l.hb; y++) memcpy(blocks + (uint64_t)y * l.wb * 16, src + (uint64_t)y * t->stride, l.wb * 16);
                else tegra_deswizzle(src, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
                if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, blocks, l.w, l.h, l.wb * 16);
                VkSpan s = vk_transient((uint64_t)l.w * l.h * 4, 16);
                astc_decode_ldr(blocks, l.w, l.h, b->astc_bw, b->astc_bh, b->astc_srgb, s.ptr);
                free(blocks);
                VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.w, .bufferImageHeight = l.h,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                                        .imageExtent = {l.w < level_w(b, lv) ? l.w : level_w(b, lv), l.h < level_h(b, lv) ? l.h : level_h(b, lv), 1}};
                vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
                continue;
            }
            uint32_t row = l.wb * fi->bpb;
            uint64_t sz = (uint64_t)row * l.hb * l.d;
            bool expand = mf.expand_rgb && fi->bpb == 3;
            uint32_t out_row = expand ? l.wb * 4 : row;
            VkSpan s = vk_transient((uint64_t)out_row * l.hb * l.d, 16);
            uint8_t *lin = expand ? malloc(sz) : s.ptr;
            const uint8_t *src = (const uint8_t *)(uintptr_t)t->addr + layer * layer_size + l.offset;
            Unpack u = {src, lin, l.wb, l.hb, l.d, fi->bpb, l.block_height_log2, l.block_depth_log2, row, t->stride, linear};
            if (!expand && !(b->tp > 0 && layer == 0) && unpack_threads()) {  // (done before the submission: above)
                if (up_n == up_cap) up_q = realloc(up_q, sizeof *up_q * (up_cap = up_cap ? up_cap * 2 : 256));
                up_q[up_n++] = u;
                up_bytes += sz;
            } else unpack_one(&u);
            if (b->tp > 0 && layer == 0) texpatch_apply(b->tp - 1, lv, lin, l.w, l.h, row);
            if (expand) {  // RGB8 -> RGBA8
                uint8_t *o = s.ptr;
                uint64_t n = (uint64_t)l.wb * l.hb * l.d;
                for (uint64_t i = 0; i < n; i++) { o[i * 4] = lin[i * 3]; o[i * 4 + 1] = lin[i * 3 + 1]; o[i * 4 + 2] = lin[i * 3 + 2]; o[i * 4 + 3] = 255; }
                free(lin);
            }
            uint32_t w = l.w, hh = l.h, d = l.d;
            // the copy stays inside the image's level: a texture allocated at the internal resolution (16:9, render
            // target sized) can still be filled from the CPU - the save slots' screenshots on the title screen. Its
            // full-size data overran the scaled image (only the width was clamped): GPU writes past the allocation,
            // a device loss below 1x after a seed switch.
            if (w > level_w(b, lv)) w = level_w(b, lv);
            if (hh > level_h(b, lv)) hh = level_h(b, lv);
            VkBufferImageCopy rg = {.bufferOffset = s.off, .bufferRowLength = l.wb * fi->bw, .bufferImageHeight = l.hb * fi->bh,
                                    .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect, lv,
                                                         b->depth > 1 ? 0 : layer, 1},
                                    .imageExtent = {w, b->base_type == VK_IMAGE_VIEW_TYPE_1D || b->base_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? 1 : hh,
                                                    b->depth > 1 ? d : 1}};
            if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) continue;  // packed D/S: no CPU layout
            vk_dl_note_upload((void *)b->image, rg.imageExtent.width, rg.imageExtent.height, rg.imageSubresource.mipLevel);
                rec_CmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
        }
    }
}

TexBackend *vk_tex(NvnTexture *t, bool for_render) {
    TexBackend *b = tex_backend(t);
    if (b->pending_rgba) upload_pending(b);
    if (for_render) b->gpu_owned = true;
    else if (!b->gpu_owned) {
        extern uint64_t prof_up_ns;
        uint64_t t0 = prof_up_ns != ~0ull ? SDL_GetTicksNS() : 0;
        upload_texture(t, b);
        if (prof_up_ns != ~0ull) prof_up_ns += SDL_GetTicksNS() - t0;
    }
    return b;
}

// (render thread, in queue order: rqueue.c IT_PREFETCH, from nvnTexturePoolRegisterTexture) A texture the game just
// registered, uploaded now instead of at its first draw: Pokémon Z-A registers a battle's ~2000 textures as it starts
// loading it, ~5 s before the battle's first frame draws 870 of them (125 MB: a 135 ms frame), and meanwhile the
// render thread waits through the game's loading (now a 57 ms frame). Its first use checks it again (upload_texture:
// data the game wrote after registering it is uploaded then). Costs GPU memory for registered textures never drawn
// (Z-A: ~950 MB of ours instead of ~600). LARECOMP_TEX_PREFETCH=0: off.
void backend_prefetch_texture(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    if (b->uploaded || b->gpu_owned) return;
    vk_tex(t, false);
    if (b->uploaded) b->prefetched = true;
}

// A GPU write (render target, copy destination): the contents are at the current render scale from here on.
void renderer_mark_gpu_owned(NvnTexture *t) {
    TexBackend *b = tex_backend(t);
    b->gpu_owned = true;
    if (b->scalable) { b->sx = g_rs_x; b->sy = g_rs_y; }
}
bool renderer_is_gpu_owned(NvnTexture *t) { return tex_backend(t)->gpu_owned; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }

// Host-produced texel data (decoded video, any thread): applied on the render thread at the texture's next use.
static void upload_rgba_now(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    TexBackend *b = t->backend;
    if (!b) return;  // the game binds the texture before decoding into it; first frames may arrive earlier
    void *copy = malloc((size_t)stride * h);
    memcpy(copy, rgba, (size_t)stride * h);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->pending_nv12 = false;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
}

// Decoded video as NV12 (runtime/hle/movie.c): the planes (half the bytes of RGBA) go to the GPU, which converts them
// into the texture (exec.c vk_convert_nv12) - swscale plus the RGBA upload cost mid-range CPUs most of a frame at the
// movies' 60 fps. false: a texture format this doesn't handle (the caller converts on the CPU).
static bool nv12_format_ok(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}
static bool upload_nv12_now(NvnTexture *t, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off);
// Movie frames (runtime/hle/movie.c, the game thread): with the render thread on they are queued behind the lists
// already submitted (rqueue.c), as a copy.
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride) {
    if (!rq_on()) { upload_rgba_now(t, rgba, w, h, stride); return; }
    if (!t->backend) return;
    size_t n = (size_t)stride * h;
    void *copy = malloc(n);
    memcpy(copy, rgba, n);
    rq_upload(t, RQ_UPLOAD_RGBA, copy, w, h, stride, 0);
}
bool renderer_upload_nv12(NvnTexture *t, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    if (!rq_on()) return upload_nv12_now(t, nv12, w, h, stride, uv_off);
    if (!t->backend) return true;
    if (!nv12_format_ok(vk_format(t->format))) return false;
    size_t n = (size_t)uv_off + (size_t)stride * (h / 2);
    void *copy = malloc(n);
    memcpy(copy, nv12, n);
    rq_upload(t, RQ_UPLOAD_NV12, copy, w, h, stride, uv_off);
    return true;
}
void backend_upload_now(NvnTexture *t, int kind, void *data, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    if (kind == RQ_UPLOAD_RGBA) upload_rgba_now(t, data, w, h, stride);
    else upload_nv12_now(t, data, w, h, stride, uv_off);
    free(data);
}
static bool upload_nv12_now(NvnTexture *t, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off) {
    TexBackend *b = t->backend;
    if (!b) return true;  // (as renderer_upload_rgba: a frame before the texture is first bound is dropped)
    if (!nv12_format_ok(b->format)) return false;
    size_t n = (size_t)uv_off + (size_t)stride * (h / 2);
    void *copy = malloc(n);
    memcpy(copy, nv12, n);
    pthread_mutex_lock(&pending_lock);
    free(b->pending_rgba);
    b->pending_rgba = copy;
    b->pending_w = w; b->pending_h = h; b->pending_stride = stride;
    b->pending_nv12 = true;
    b->pending_uv_off = uv_off;
    b->gpu_owned = true;
    pthread_mutex_unlock(&pending_lock);
    return true;
}

// ================================================================== views
#define VIEW_TAB 16384
typedef struct { TexBackend *b; uint64_t k1, k2; VkImageView view; } ViewEnt;
static ViewEnt view_tab[VIEW_TAB];

// Views may still be referenced by submitted or pending work: destroyed once that has completed.
void vk_views_forget(TexBackend *b) { views_forget(b); }
uint32_t vk_view_gen;  // bumped whenever a cached view may be destroyed (exec.c's per-frame texture memo)
static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    fmt = vk_view_format(b, fmt);  // (g_vk.lean_usage: only the listed formats)
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}

static VkImageViewType view_type_for(int textype) {
    switch (textype) {
    case TEXTYPE_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEXTYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEXTYPE_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEXTYPE_2D_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEXTYPE_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    case TEXTYPE_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static const VkComponentMapping IDENTITY = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

// Format views of a VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT image must stay in the image format's compatibility class:
// ordinary colour formats of the same texel size, a compressed format only with its own class (BCn / ASTC block size:
// unorm, snorm and sRGB twins). Anything else is invalid usage (the Metal side aborted on it: a texture pool slot whose
// texture was finalized and the address reused by another format, runtime/nvn/objects.c).
static int vk_color_bytes(VkFormat f) {  // texel size of an ordinary colour format, 0 = not one
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_SRGB:
        return 1;
    case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
    case VK_FORMAT_B5G5R5A1_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        return 2;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16G16_UNORM:
    case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        return 4;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SNORM: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 0;
    }
}
static int vk_block_class(VkFormat f) {  // compressed formats: one id per compatibility class, 0 = not compressed
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) return 1 + (f - VK_FORMAT_BC1_RGB_UNORM_BLOCK) / 2;
    if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return 100 + (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2;
    return 0;
}
static bool vk_view_compatible(VkFormat image, VkFormat view) {
    if (image == view) return true;
    int ib = vk_color_bytes(image), vb = vk_color_bytes(view);
    if (ib || vb) return ib == vb;
    int ic = vk_block_class(image);
    return ic && ic == vk_block_class(view);
}

// Attachment view: one level; every layer of the view's range (array targets) or every slice of that level (3D
// targets, as a 2D array). *layers (if given): the layer count, for VkRenderingInfo.layerCount.
VkImageView vk_render_view(NvnTexture *t, const NvnTextureView *v, bool has_view, uint32_t *layers) {
    TexBackend *b = vk_tex(t, true);
    if (!has_view) v = NULL;
    if (layers) *layers = 1;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;  // (fallback images keep their own format)
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    bool is3d = b->base_type == VK_IMAGE_VIEW_TYPE_3D;
    if (is3d && b->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return VK_NULL_HANDLE;  // (2D array views: colour images only)
    uint32_t total = is3d ? (b->depth >> lv0 ? b->depth >> lv0 : 1) : b->layers;
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    if (ly0 >= total) ly0 = total - 1;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : total - ly0;
    if (ly0 + nly > total) nly = total - ly0;
    if (layers) *layers = nly;
    VkImageViewType type = is3d || nly > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    return vk_image_view(b, type, fmt, lv0, 1, ly0, nly, IDENTITY, b->aspect);
}

// render: attachment view (vk_render_view). Otherwise a sampled view of type want_textype (TEXTYPE_*),
// with the NVN component swizzle, converting between compatible types the way renderer.m does.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype) {
    if (render) return vk_render_view(t, v, has_view, NULL);
    TexBackend *b = vk_tex(t, render);
    if (!has_view) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format && vf != fmt) {  // (fallback images keep their own format)
            if (vk_view_compatible(fmt, vf)) fmt = vf;
            else {
                static int nlog;
                if (!nlog++)
                    LOG("[vk] texture %ux%u fmt %#x (vk %d) can't be viewed as nvn %#x (vk %d): its own format instead "
                        "(logged once)\n", t->width, t->height, t->format, b->format, v->format, vf);
            }
        }
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    VkImageAspectFlags aspect = b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect;
    // NVN depth/stencil mode STENCIL (view, else texture): the stencil values (the shader declares a usampler)
    if ((b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (v && (v->setmask & 16) ? v->ds_mode : t->ds_mode) == 1)
        aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkComponentMapping sw = IDENTITY;
    if (!(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && b->samples == 1) {
        const uint8_t *s = v && (v->setmask & 8) ? v->swizzle : t->swizzle;
        uint8_t c[4];
        for (int i = 0; i < 4; i++) c[i] = s[i] <= 5 ? s[i] : (uint8_t)(2 + i);
        if (c[0] || c[1] || c[2] || c[3]) {  // never set: identity
            if (fmt_of(t->format).alpha_one)
                for (int i = 0; i < 4; i++) if (c[i] == 5) c[i] = 1;
            // NVN swizzle 0 zero, 1 one, 2..5 r,g,b,a == VkComponentSwizzle - 1
            sw = (VkComponentMapping){(VkComponentSwizzle)(c[0] + 1), (VkComponentSwizzle)(c[1] + 1), (VkComponentSwizzle)(c[2] + 1),
                                      (VkComponentSwizzle)(c[3] + 1)};
        } else if (fmt_of(t->format).alpha_one) {
            sw.a = VK_COMPONENT_SWIZZLE_ONE;
        }
    }
    VkImageViewType want = view_type_for(want_textype);
    VkImageViewType have = b->base_type;
    if (b->samples > 1) return vk_dummy_view(want_textype);  // multisampled images can't be sampled as 2D
    if (want == have) {
        if (have == VK_IMAGE_VIEW_TYPE_CUBE) nly = 6;
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, nly, sw, aspect);
    }
    if (want == VK_IMAGE_VIEW_TYPE_2D && (have == VK_IMAGE_VIEW_TYPE_2D_ARRAY || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_2D_ARRAY && (have == VK_IMAGE_VIEW_TYPE_2D || have == VK_IMAGE_VIEW_TYPE_CUBE))
        return vk_image_view(b, want, fmt, lv0, nlv, ly0, have == VK_IMAGE_VIEW_TYPE_CUBE ? nly : 1, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_CUBE && have == VK_IMAGE_VIEW_TYPE_2D_ARRAY && b->layers >= 6 && b->width == b->height)
        return vk_image_view(b, want, fmt, lv0, nlv, ly0 + 6 <= b->layers ? ly0 : 0, 6, sw, aspect);
    if (want == VK_IMAGE_VIEW_TYPE_1D_ARRAY && have == VK_IMAGE_VIEW_TYPE_1D)
        return vk_image_view(b, want, fmt, lv0, nlv, 0, 1, sw, aspect);
    static int nlog;
    if (nlog++ < 30) LOG("[tex] type mismatch: want %d have %d (%ux%ux%u target %u fmt %#x layers %u)\n", want, have, t->width,
                         t->height, t->depth, t->target, t->format, b->layers);
    return vk_dummy_view(want_textype);
}

// 1x1 black textures for unbound slots, per type
VkImageView vk_dummy_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    TexBackend *b = &dummies[textype];
    VkImageViewType vt = view_type_for(textype);
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = vt == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                                       : vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {1, 1, 1}, .mipLevels = 1, .arrayLayers = cube ? 6 : 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL));
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_COLOR_BIT);
    float dv = ENV("LARECOMP_DUMMY_WHITE") ? 1.0f : 0.0f;  // (debug: white dummies)
    VkClearColorValue zero = {{dv, dv, dv, dv}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, cube ? 6 : 1};
    rec_CmdClearColorImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    b->format = VK_FORMAT_R8G8B8A8_UNORM;
    b->levels = 1;
    b->layers = cube ? 6 : 1;
    views[textype] = vk_image_view(b, vt, b->format, 0, 1, 0, cube ? 6 : 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
    return views[textype];
}

// Depth compare (sampler*Shadow) needs a view whose format has SAMPLED_IMAGE_DEPTH_COMPARISON: the RGBA8 dummy above
// has not (VUID-vkCmdDrawIndexed-None-06479). Shadow slots with nothing usable bound get a 1x1 depth image instead,
// cleared to 1.0: the farthest depth, so a less / less-equal compare reads it as lit (no shadow), as an empty shadow
// map would. D32_SFLOAT where it can be compared, else D16_UNORM. 3D (no shadow form) and 1D (depth 1D images are not
// required) types keep the colour dummy.
bool vk_format_depth_compare(VkFormat f) {
    static VkFormat fs[16];
    static int8_t ok[16];
    static int n;
    for (int i = 0; i < n; i++) if (fs[i] == f) return ok[i];
    VkFormatProperties3 p3 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
    VkFormatProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, .pNext = &p3};
    vkGetPhysicalDeviceFormatProperties2(g_vk.phys, f, &p2);
    bool r = (p3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT) != 0;
    if (n < 16) { fs[n] = f; ok[n++] = r; }
    return r;
}

// t's sampled image can be read with a depth compare (a depth image of a comparable format)
bool vk_tex_depth_compare(NvnTexture *t) {
    TexBackend *b = t ? (TexBackend *)t->backend : NULL;
    return b && (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && vk_format_depth_compare(b->format);
}

VkImageView vk_dummy_depth_view(int textype) {
    static TexBackend dummies[10];
    static VkImageView views[10];
    if (textype < 0 || textype >= 10) textype = TEXTYPE_2D;
    if (views[textype]) return views[textype];
    VkImageViewType vt = view_type_for(textype);
    if (vt != VK_IMAGE_VIEW_TYPE_2D && vt != VK_IMAGE_VIEW_TYPE_2D_ARRAY && vt != VK_IMAGE_VIEW_TYPE_CUBE &&
        vt != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
        return vk_dummy_view(textype);
    static VkFormat df;
    if (!df) {
        df = vk_format_depth_compare(VK_FORMAT_D32_SFLOAT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;
        LOG("[vk] shadow slot dummy: 1x1 %s cleared to 1.0\n", df == VK_FORMAT_D32_SFLOAT ? "D32_SFLOAT" : "D16_UNORM");
    }
    bool cube = vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    uint32_t layers = cube ? 6 : 1;
    TexBackend *b = &dummies[textype];
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = VK_IMAGE_TYPE_2D, .format = df, .extent = {1, 1, 1}, .mipLevels = 1,
                            .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &b->image, &b->mem, NULL) != VK_SUCCESS) {
        LOG("[vk] shadow slot dummy: depth image creation failed, the colour dummy instead\n");
        return views[textype] = vk_dummy_view(textype);
    }
    VkCommandBuffer cb = vk_upload_cmd();
    vk_image_init_layout(cb, b->image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkClearDepthStencilValue one = {1.0f, 0};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
    rec_CmdClearDepthStencilImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &all);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(cb, &di);
    b->format = df;
    b->levels = 1;
    b->layers = layers;
    b->aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    views[textype] = vk_image_view(b, vt, df, 0, 1, 0, layers, IDENTITY, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!views[textype]) views[textype] = vk_dummy_view(textype);
    return views[textype];
}

// Throwaway color attachment for passes that have none (exec.c begin_pass, Intel only): R8, at least w x h (the pass's
// render area: the attachments' level extent, which Dynamic resolution's sub-rect lies inside), never sampled; its
// contents are don't-care. One image per sample count, grown to the largest render area seen (the outgrown one is
// destroyed once the submissions that may use it are done), so changing render scales don't pile up images. Only the
// image is kept: its view comes from the view table on each use, like a game target's (a cached handle could be
// evicted from that table and destroyed under us). Called by the translating thread only (the render thread, or the
// game's GPU thread with LARECOMP_RENDER_THREAD=0), as vk_dummy_view and the view table are.
VkImageView vk_dummy_color_view(uint32_t w, uint32_t h, uint32_t samples) {
    static TexBackend imgs[5];  // samples 1, 2, 4, 8, 16
    int k = 0;
    while (k < 4 && (1u << k) < samples) k++;
    TexBackend *b = &imgs[k];
    if (!b->image || b->width < w || b->height < h || b->samples != samples) {
        if (b->width > w) w = b->width;
        if (b->height > h) h = b->height;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkImage img;
        VmaAllocation mem;
        if (vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
        if (b->image) { views_forget(b); defer((Dead){.img = b->image, .mem = b->mem}); }
        vk_image_init_layout(vk_upload_cmd(), img, VK_IMAGE_ASPECT_COLOR_BIT);
        *b = (TexBackend){.image = img, .mem = mem, .format = VK_FORMAT_R8_UNORM, .base_type = VK_IMAGE_VIEW_TYPE_2D,
                          .width = w, .height = h, .depth = 1, .layers = 1, .levels = 1, .samples = samples,
                          .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .gpu_owned = true};
        LOG("[vk] placeholder color attachment %ux%u (samples %u) for passes without one\n", w, h, samples);
    }
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, b->format, 0, 1, 0, 1, IDENTITY, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ================================================================== samplers
static VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

// Sampler settings that change while playing - F1's Texture filtering, the render scale - make every sampler again at
// its next use (NvnSamplerDesc.backend_gen), through a table by contents: the samplers made before stay alive for the
// work in flight, and come back when the settings do.
//  - Texture filtering High (g_settings.aniso16): 16x anisotropic, and linear between mip levels where the game asks
//    for the nearest level (trilinear: no visible bands where the ground's detail steps down).
//  - A frame drawn smaller than its place in the window (25-75%, Dynamic's lower steps, upscaled by FSR or bilinear):
//    the textures' mip bias by log2 of the ratio, in half steps down to -2, so their detail is the window's, as
//    upscalers ask. Only the game's material samplers (sample_bias_ok). LARECOMP_NO_LOD_BIAS=1: off.
static uint32_t samp_gen = 1;
static float samp_bias;  // (<= 0)
static bool samp_high;

// (render thread, each presented frame: device.c) ratio = the frame's lines per window line of its image
void vk_sampler_settings(double ratio) {
    static int off = -1;
    if (off < 0) off = ENV("LARECOMP_NO_LOD_BIAS") != NULL;
    float bias = off || !(ratio > 0 && ratio < 1) ? 0 : fmaxf(-2.0f, roundf((float)log2(ratio) * 2) / 2);
    if (bias == samp_bias && g_settings.aniso16 == samp_high) return;
    LOG("[vk] samplers: mip bias %.1f, texture filtering %s\n", bias, g_settings.aniso16 ? "high" : "game's");
    samp_bias = bias, samp_high = g_settings.aniso16;
    samp_gen++;
}

static VkSampler sampler_make(const NvnSamplerDesc *d);
VkSampler vk_sampler(NvnSamplerDesc *d) {
    if (d->backend && d->backend_gen == samp_gen) return (VkSampler)d->backend;
    typedef struct { NvnSamplerDesc k; float bias; bool high; VkSampler s; } Ent;
    static Ent tab[4096];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    NvnSamplerDesc k;
    memset(&k, 0, sizeof k);
    memcpy(&k, d, offsetof(NvnSamplerDesc, backend));  // (the contents: not the cache fields, nor padding)
    uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(samp_bias * -2) ^ (uint64_t)samp_high << 4;
    for (size_t i = 0; i < offsetof(NvnSamplerDesc, backend); i++) { h ^= ((const uint8_t *)&k)[i]; h *= 0x100000001B3ull; }
    pthread_mutex_lock(&lock);
    VkSampler s = VK_NULL_HANDLE;
    for (uint32_t n = 0, i = (uint32_t)h & 4095; n < 4096 && !s; n++, i = (i + 1) & 4095) {
        if (!tab[i].s) tab[i] = (Ent){k, samp_bias, samp_high, s = sampler_make(&k)};
        else if (tab[i].bias == samp_bias && tab[i].high == samp_high && !memcmp(&tab[i].k, &k, offsetof(NvnSamplerDesc, backend)))
            s = tab[i].s;
    }
    if (!s) s = sampler_make(&k);  // shortcut: a full table makes uncached samplers (thousands - not seen); grow it then
    pthread_mutex_unlock(&lock);
    d->backend = (void *)s;
    d->backend_gen = samp_gen;
    return s;
}

// The game's own material textures: mipmapped, linearly filtered, tiled (repeat / mirrored repeat in u or v). Not the
// post effects', the depth pyramid's or the UI's, which clamp at their edges - the first two read chosen levels
// (textureLod), which a sampler's bias moves too. (Pokémon Z-A asks for no anisotropy anywhere: that can't tell them.)
static bool sample_bias_ok(const NvnSamplerDesc *d) {
    bool tiled = d->wrap[0] == 1 || d->wrap[0] == 6 || d->wrap[1] == 1 || d->wrap[1] == 6;
    return (d->min_filter == 3 || d->min_filter == 5) && tiled && !d->compare_mode && !d->reduction;
}

static VkSampler sampler_make(const NvnSamplerDesc *d) {
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = d->mag_filter == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    uint32_t mf = d->min_filter;
    if (samp_high && mf == 3 && !d->compare_mode && !d->reduction) mf = 5;  // (High: trilinear)
    si.minFilter = (mf == 1 || mf == 3 || mf == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = mf == 4 || mf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode modes[3];
    bool border = false;
    for (int i = 0; i < 3; i++) {
        switch (d->wrap[i]) {
        case 0: case 7: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; break;
        case 1: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT; break;
        case 2: case 3: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; break;
        case 4: case 5: modes[i] = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; border = true; break;
        case 6: modes[i] = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        default: modes[i] = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        }
    }
    si.addressModeU = modes[0]; si.addressModeV = modes[1]; si.addressModeW = modes[2];
    si.minLod = d->lod_min;
    si.maxLod = d->lod_max > 1000 ? VK_LOD_CLAMP_NONE : d->lod_max;
    if (mf <= 1) si.maxLod = si.minLod + 0.25f;  // not mipmapped: stay on the base level
    if (si.maxLod < si.minLod) si.maxLod = si.minLod;
    si.mipLodBias = d->lod_bias + (sample_bias_ok(d) ? samp_bias : 0);
    if (d->max_aniso > 1) { si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = fminf(d->max_aniso, 16); }
    // (setting aniso16: every mipmapped, linearly filtered sampler at 16x - ground and walls stay sharp at grazing
    // angles; the Switch games mostly ask for 1-4x to save bandwidth. Not shadow / depth-compare samplers.)
    if (samp_high && mf == 5 && !d->compare_mode && g_vk.props.limits.maxSamplerAnisotropy >= 16) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = 16;
    }
    static float lodmin = -1;
    if (lodmin < 0) lodmin = ENV("LARECOMP_LOD_MIN") ? (float)atof(ENV("LARECOMP_LOD_MIN")) : 0;
    if (lodmin > 0 && mf > 1) si.minLod = fmaxf(si.minLod, lodmin);
    static int stest = -1;
    if (stest < 0) stest = ENV("LARECOMP_SAMPLER_TEST") != NULL;
    if (stest && mf > 1) { si.minLod = 0; si.maxLod = VK_LOD_CLAMP_NONE; si.anisotropyEnable = VK_TRUE; si.maxAnisotropy = 16; }
    if (d->compare_mode) {
        si.compareEnable = VK_TRUE;
        si.compareOp = ENV("LARECOMP_NO_SHADOW") ? VK_COMPARE_OP_ALWAYS : compare_op(d->compare_func);
    }
    VkSamplerCustomBorderColorCreateInfoEXT cb = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border) {
        const float *c = d->border;
        if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0) si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        else if (c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        else if (c[0] == 1 && c[1] == 1 && c[2] == 1 && c[3] == 1) si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else if (g_vk.custom_border) {
            memcpy(cb.customBorderColor.float32, c, 16);
            si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            si.pNext = &cb;
        } else si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    // NVN reduction filter (AVERAGE 0, MIN 1, MAX 2 == VkSamplerReductionMode): Pokémon Z-A builds its hierarchical-Z
    // pyramid with a MIN-filtered linear fetch (the farthest of 4 reversed depths) - averaged, the coarse levels came
    // out too near and its GPU culling hid small props (planters) at mid range.
    VkSamplerReductionModeCreateInfo rm = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    if (d->reduction == 1 || d->reduction == 2) {
        rm.reductionMode = d->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        rm.pNext = si.pNext;
        si.pNext = &rm;
        static int logged;
        if (!logged++) LOG("[vk] sampler reduction filter %u (min filter %u)\n", d->reduction, d->min_filter);
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s));
    if (ENV("LARECOMP_LOG_SAMPLERS")) {  // (diagnostics: the samplers made, and the mip bias each got)
        static int n;
        if (n++ < 400) LOG("[smp] min %u mag %u aniso %.0f bias %.2f lod %.1f-%.1f cmp %u red %u wrap %u%u -> extra %.1f\n",
                           d->min_filter, d->mag_filter, d->max_aniso, d->lod_bias, d->lod_min, d->lod_max, d->compare_mode,
                           d->reduction, d->wrap[0], d->wrap[1], sample_bias_ok(d) ? samp_bias : 0);
    }
    return s;
}

// The sampler for a texture with an integer format (exec.c texture_info): d with nearest filtering and no anisotropy -
// Vulkan can't filter integer formats linearly (Maxwell fetches them unfiltered). Cached by contents.
VkSampler vk_sampler_nearest(const NvnSamplerDesc *d) {
    NvnSamplerDesc n = *d;
    n.backend = NULL;
    n.mag_filter = 0;
    n.min_filter = d->min_filter <= 1 ? 0 : 2;  // (mipmapped: nearest level, as linear between levels is filtering too)
    n.max_aniso = 0;
    if (n.mag_filter == d->mag_filter && n.min_filter == d->min_filter && d->max_aniso <= 1) return vk_sampler((NvnSamplerDesc *)d);
    static NvnSamplerDesc tab[256];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < offsetof(NvnSamplerDesc, backend); i++) { h ^= ((const uint8_t *)&n)[i]; h *= 0x100000001B3ull; }
    pthread_mutex_lock(&lock);
    VkSampler s = VK_NULL_HANDLE;
    for (uint32_t k = 0, i = (uint32_t)h & 255; k < 256; k++, i = (i + 1) & 255) {
        if (!tab[i].backend) { tab[i] = n; s = vk_sampler(&tab[i]); break; }
        if (!memcmp(&tab[i], &n, offsetof(NvnSamplerDesc, backend))) { s = vk_sampler(&tab[i]); break; }
    }
    pthread_mutex_unlock(&lock);
    return s ? s : vk_default_sampler(false);  // (table full: hundreds of integer samplers - not seen)
}

// The numeric type of a sampled view of t (the format vk_view picks): 0 float / normalized, 1 unsigned integer, 2
// signed integer. The shader declares a usampler / isampler for those (exec.c, ShaderKey.stencil_slots / sint_slots).
int vk_view_int_kind(NvnTexture *t, const NvnTextureView *v, bool has_view) {
    VkFormat fmt = vk_format(t->format);
    TexBackend *b = (TexBackend *)t->backend;
    if (b && b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return 0;  // (stencil: ds_mode)
    if (b && b->format != fmt) fmt = b->format;  // (fallback images keep their own format)
    else if (has_view && v && (v->setmask & 4)) fmt = fmt_of(v->format).vf;
    switch (fmt) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_S8_UINT:
        return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32: case VK_FORMAT_A2R10G10B10_SINT_PACK32:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        return 2;
    default: return 0;
    }
}

VkSampler vk_default_sampler(bool linear) {
    static VkSampler s[2];
    if (!s[linear]) {
        VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                  .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                  .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                  .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.25f};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &s[linear]));
    }
    return s[linear];
}

void backend_texture_view_format(NvnTexture *t, uint32_t format) { (void)t; (void)format; }  // (images are made mutable-format)
