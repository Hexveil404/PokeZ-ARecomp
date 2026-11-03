// NVIDIA DLAA (DLSS at the scene's own size) for exec.c's temporal anti-aliasing (dlaa.c). Windows with an NVIDIA RTX
// GPU, and nvngx_dlss.dll next to lasrecomp.exe; anywhere else it is simply unavailable.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "vk_fsr2.h"  // (VkFsr2Frame: the same inputs)

// F1 > Video > Temporal anti-aliasing DLAA, as it was when the game started (NGX needs its extensions at device
// creation, so a change applies after a restart); LARECOMP_DLAA=0 / 1 overrides it
bool vk_dlaa_wanted(void);
// The instance / device extensions NGX asks for, added to ext (n of them so far, room for max)
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
// After the device is made: NGX brought up on it (logged)
void vk_dlaa_init(VkInstance inst, VkPhysicalDevice phys, VkDevice dev);
// NGX is up and DLSS can run on this GPU
bool vk_dlaa_available(void);
// Records DLAA into cmd (a command buffer the caller records itself): f->color resolved with its history into f->out
// (f->reactive unused); false: not done (logged)
bool vk_dlaa_record(VkDevice dev, VkCommandBuffer cmd, const VkFsr2Frame *f);
