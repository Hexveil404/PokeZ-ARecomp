// AMD FSR 2 for the Vulkan backend (fsr2.cpp; used by exec.c's temporal anti-aliasing).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// One frame: the game's jittered picture (color), its depth, our motion vectors (RG16F, pixels, current -> previous)
// and reactive mask (R8) at the render size w x h, and the output image (a storage image) at ow x oh. The picture and
// depth are images of ow x oh too (the most the render size can be) drawn in their top-left w x h. Images in
// VK_IMAGE_LAYOUT_GENERAL. jx, jy: the jitter the frame was drawn with, in render pixels. (The inputs any temporal
// upscaler takes - FSR 3.1's upscaler, DLSS: the same set; only vk_fsr2_record / vk_fsr2_jitter are FSR 2's own.)
typedef struct {
    VkImage color, depth, mv, reactive, out;
    VkImageView color_view, depth_view, mv_view, reactive_view, out_view;
    VkFormat color_format, depth_format, out_format;
    uint32_t w, h, ow, oh;
    float jx, jy, dt_ms, near_z, fov_y, sharpness;
    bool reset;
    int slot;  // (DLAA) which DLSS feature - history: 0 the scene's, 1 / 2 the clothing screen's character (exec.c pv_resolve)
} VkFsr2Frame;

// Records FSR 2 into cmd (a command buffer the caller records itself, not the recorder's); false: not done (logged)
bool vk_fsr2_record(VkPhysicalDevice phys, VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f);
// The jitter of frame `index` for this render / output width: FSR 2's own Halton sequence, in render pixels
void vk_fsr2_jitter(uint32_t index, uint32_t render_w, uint32_t out_w, float *jx, float *jy);
void vk_fsr2_destroy(VkDevice dev);
// AMD FSR 3.1's upscaler (fsr3.cpp), the same frame. available: its DLL loaded (Windows only) and its context hasn't
// failed (logged; FSR 2 from then on)
bool vk_fsr3_available(void);
bool vk_fsr3_record(VkPhysicalDevice phys, VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f);
// AMD FSR 4.1.2 on Linux, through a user-supplied libamd_fidelityfx_upscaler_vk.so (fsr4.cpp, FidelityFX SDK 2.4 ABI);
// the device must expose cooperative matrix and shader float8 (device.c g_vk.fsr4). False: FSR 2/3 stays.
bool vk_fsr4_wanted(void);
bool vk_fsr4_available(void);
bool vk_fsr4_record(VkPhysicalDevice phys, VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f);
// (fsr2.cpp, for both) a log line as runtime/rt.h rt_log; an SDK's message as "[tag] error / warning: ..." (the first 50)
void vk_fsr_log(const char *fmt, ...);
void vk_fsr_message(const char *tag, bool error, const wchar_t *m);

#ifdef __cplusplus
}
#endif
