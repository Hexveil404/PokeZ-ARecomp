// AMD FSR 2 for the Vulkan backend (fsr2.cpp; used by exec.c's temporal anti-aliasing).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// One frame: the game's jittered picture (color), its depth, our motion vectors (RG16F, pixels, current -> previous)
// and reactive mask (R8) at the render size w x h, and the output image (a storage image) at ow x oh. Images in
// VK_IMAGE_LAYOUT_GENERAL. jx, jy: the jitter the frame was drawn with, in render pixels.
typedef struct {
    VkImage color, depth, mv, reactive, out;
    VkImageView color_view, depth_view, mv_view, reactive_view, out_view;
    VkFormat color_format, depth_format, out_format;
    uint32_t w, h, ow, oh;
    float jx, jy, dt_ms, near_z, fov_y, sharpness;
    bool reset;
} VkFsr2Frame;

// Records FSR 2 into cmd (a command buffer the caller records itself, not the recorder's); false: not done (logged)
bool vk_fsr2_record(VkPhysicalDevice phys, VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f);
// The jitter of frame `index` for this render / output width: FSR 2's own Halton sequence, in render pixels
void vk_fsr2_jitter(uint32_t index, uint32_t render_w, uint32_t out_w, float *jx, float *jy);
void vk_fsr2_destroy(VkDevice dev);

#ifdef __cplusplus
}
#endif
