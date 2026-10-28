// Vulkan backend for the NVN layer (Windows; macOS uses runtime/nvn/metal.m + renderer.m).
// Files: device.c (instance, window, swapchain, present, screenshots, ImGui hooks), textures.c (images, uploads,
// views), exec.c (command execution: passes, draws, clears, copies, compute), shaders.c (GLSL -> SPIR-V, programs,
// pipelines). Mirrors the Metal backend's behaviour; differences are noted where they happen.
//
// Synchronisation model: every image lives in VK_IMAGE_LAYOUT_GENERAL, and a full memory barrier separates
// render passes, copies and dispatches (Metal tracks these hazards itself). Submissions are numbered; a timeline
// semaphore signals each number, which is what NVN fences (renderer_fence_seq / renderer_wait_seq) wait for.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#if __has_include(<vma/vk_mem_alloc.h>)
#include <vma/vk_mem_alloc.h>
#else
#include <vk_mem_alloc.h>  // (Debian / Ubuntu libvulkan-memory-allocator-dev)
#endif

// The Vulkan 1.2 / 1.3 commands come from the driver (device.c, after vkCreateDevice), not from the loader's exports:
// a vulkan-1.dll older than 1.3 doesn't have them, and Windows then refuses to start the program ("The procedure entry
// point vkCmdBeginRendering could not be located") before it can say what is missing.
#define VK_DEVICE_COMMANDS_12(X) X(vkGetSemaphoreCounterValue) X(vkWaitSemaphores)
// Vulkan 1.3 commands: core, or the same command from its KHR / EXT extension on a Vulkan 1.2 driver; NULL when the
// driver has neither, and compat.c does without (g_vkc)
#define VK_DEVICE_COMMANDS_13(X)                                                                                      \
    X(vkCmdBeginRendering, KHR) X(vkCmdEndRendering, KHR) X(vkCmdPipelineBarrier2, KHR) X(vkCmdWriteTimestamp2, KHR)  \
    X(vkQueueSubmit2, KHR) X(vkCmdSetCullMode, EXT) X(vkCmdSetDepthCompareOp, EXT) X(vkCmdSetDepthTestEnable, EXT)    \
    X(vkCmdSetDepthWriteEnable, EXT) X(vkCmdSetFrontFace, EXT) X(vkCmdSetPrimitiveTopology, EXT)                      \
    X(vkCmdSetStencilOp, EXT) X(vkCmdSetStencilTestEnable, EXT) X(vkCmdSetDepthBiasEnable, EXT)                       \
    X(vkCmdSetPrimitiveRestartEnable, EXT)
#define VK_DECLARE_COMMAND(f) extern PFN_##f vkp_##f;
#define VK_DECLARE_COMMAND_13(f, ext) extern PFN_##f vkp_##f;
VK_DEVICE_COMMANDS_12(VK_DECLARE_COMMAND)
VK_DEVICE_COMMANDS_13(VK_DECLARE_COMMAND_13)
#undef VK_DECLARE_COMMAND
#undef VK_DECLARE_COMMAND_13
#define vkGetSemaphoreCounterValue vkp_vkGetSemaphoreCounterValue
#define vkWaitSemaphores vkp_vkWaitSemaphores

// ---- Vulkan 1.2 GPUs (compat.c): what the driver lacks of Vulkan 1.3 is done another way. LARECOMP_VK_COMPAT=1 takes
// those ways on a Vulkan 1.3 GPU too (testing them).
typedef struct {
    uint32_t api;  // the Vulkan version used: the device's, at most 1.3
    bool sync1;    // no synchronization2: barriers, submits and timestamps through the Vulkan 1.0 commands
    bool render_pass;  // no dynamic rendering: render passes and framebuffers (compat_rp.c)
    bool no_eds1;      // no extended dynamic state: cull mode, front face, topology, depth and stencil test state in
                       // the pipeline key (exec.c FixedState)
    bool no_eds2;      // no extended dynamic state 2: primitive restart in the pipeline key, depth bias always on in
                       // the pipeline (its values are dynamic and zero while the game has it off: the same)
} VkCompat;
extern VkCompat g_vkc;
void vkc_CmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *d);
VkResult vkc_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence fence);
void vkc_CmdWriteTimestamp2(VkCommandBuffer cb, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t query);
// (compat_rp.c)
void vkc_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri);
void vkc_CmdEndRendering(VkCommandBuffer cb);
VkResult vkc_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                     const VkAllocationCallbacks *al, VkPipeline *out);
VkResult vkc_CreateImageView(VkDevice dev, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *al, VkImageView *v);
void vkc_DestroyImageView(VkDevice dev, VkImageView v, const VkAllocationCallbacks *al);
VkResult vkc_vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai, VkImage *img,
                            VmaAllocation *mem, VmaAllocationInfo *info);
VkRenderPass vkc_render_pass_for(uint32_t ncolor, const VkFormat *color, VkFormat depth, VkFormat stencil,
                                 VkSampleCountFlagBits samples, VkImageLayout layout);
// (no extended dynamic state 1 / 2: exec.c bakes these into the pipeline instead)
static inline void vkc_CmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags m) {
    if (vkp_vkCmdSetCullMode) vkp_vkCmdSetCullMode(cb, m);
}
static inline void vkc_CmdSetFrontFace(VkCommandBuffer cb, VkFrontFace f) {
    if (vkp_vkCmdSetFrontFace) vkp_vkCmdSetFrontFace(cb, f);
}
static inline void vkc_CmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t) {
    if (vkp_vkCmdSetPrimitiveTopology) vkp_vkCmdSetPrimitiveTopology(cb, t);
}
static inline void vkc_CmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 on) {
    if (vkp_vkCmdSetDepthTestEnable) vkp_vkCmdSetDepthTestEnable(cb, on);
}
static inline void vkc_CmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 on) {
    if (vkp_vkCmdSetDepthWriteEnable) vkp_vkCmdSetDepthWriteEnable(cb, on);
}
static inline void vkc_CmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp op) {
    if (vkp_vkCmdSetDepthCompareOp) vkp_vkCmdSetDepthCompareOp(cb, op);
}
static inline void vkc_CmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 on) {
    if (vkp_vkCmdSetStencilTestEnable) vkp_vkCmdSetStencilTestEnable(cb, on);
}
static inline void vkc_CmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags face, VkStencilOp fail, VkStencilOp pass, VkStencilOp dfail,
                                      VkCompareOp cmp) {
    if (vkp_vkCmdSetStencilOp) vkp_vkCmdSetStencilOp(cb, face, fail, pass, dfail, cmp);
}
#define vkCmdSetCullMode vkc_CmdSetCullMode
#define vkCmdSetFrontFace vkc_CmdSetFrontFace
#define vkCmdSetPrimitiveTopology vkc_CmdSetPrimitiveTopology
#define vkCmdSetDepthTestEnable vkc_CmdSetDepthTestEnable
#define vkCmdSetDepthWriteEnable vkc_CmdSetDepthWriteEnable
#define vkCmdSetDepthCompareOp vkc_CmdSetDepthCompareOp
#define vkCmdSetStencilTestEnable vkc_CmdSetStencilTestEnable
#define vkCmdSetStencilOp vkc_CmdSetStencilOp
static inline void vkc_CmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 on) {
    if (vkp_vkCmdSetDepthBiasEnable) vkp_vkCmdSetDepthBiasEnable(cb, on);
}
static inline void vkc_CmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 on) {
    if (vkp_vkCmdSetPrimitiveRestartEnable) vkp_vkCmdSetPrimitiveRestartEnable(cb, on);
}
#define vkCmdSetDepthBiasEnable vkc_CmdSetDepthBiasEnable
#define vkCmdSetPrimitiveRestartEnable vkc_CmdSetPrimitiveRestartEnable
#define vkCmdBeginRendering vkc_CmdBeginRendering
#define vkCmdEndRendering vkc_CmdEndRendering
#define vkCreateGraphicsPipelines vkc_CreateGraphicsPipelines
#define vkCreateImageView vkc_CreateImageView
#define vkDestroyImageView vkc_DestroyImageView
#define vmaCreateImage vkc_vmaCreateImage
#define vkCmdPipelineBarrier2 vkc_CmdPipelineBarrier2
#define vkQueueSubmit2 vkc_QueueSubmit2
#define vkCmdWriteTimestamp2 vkc_CmdWriteTimestamp2

#include "../nvn.h"
#include "../maxwell/shader.h"

void vk_device_fault_report(void);
void vk_checkpoint(VkCommandBuffer cb, uint64_t mark);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { if (r_ == VK_ERROR_DEVICE_LOST) vk_device_fault_report(); \
    FATAL("%s failed: %d (%s:%d)\n", #x, r_, __FILE__, __LINE__); } } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    uint32_t ts_bits;          // the queue's timestampValidBits (0: no timestamps)
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;      // value = last completed submission number
    VkCommandPool pool;        // render thread only
    bool custom_border, divisor, host_import, dmabuf;
    // VK_EXT_vertex_input_dynamic_state (exec.c): the vertex layout is set per draw (vkCmdSetVertexInputEXT), not
    // baked into each pipeline - fewer pipelines. Off: no extension / feature, or LARECOMP_VK_DYN_VERTEX=0.
    bool vids;
    PFN_vkCmdSetVertexInputEXT set_vertex_input;
    // LARECOMP_VK_GPL_PROBE=1 only (exec.c gpl_probe, measurement): VK_EXT_graphics_pipeline_library enabled
    bool gpl_probe;
    bool map_placed, unmap_reserve;  // VK_EXT_map_memory_placed: driver memory mapped at a given address (exec.c guest_heap_dmabuf)
    bool ssbo_chunks;  // stores and atomics from fragment and vertex shaders (exec.c: the heap in chunks counts misses)
    bool lazy_heap, bda;  // the heap imported chunk by chunk (device.c lazy_heap_predicted), buffer device addresses enabled for it
    bool heap_probe;      // one import tried at the start, lazy chunks if it runs out of memory (exec.c whole_import_probe_oom)
    // on by default on integrated GPUs only (device.c; LARECOMP_VK_LEAN_USAGE / LARECOMP_VK_SMAA_STENCIL /
    // LARECOMP_NO_FRAME_CLEAR =0 / =1 override):
    bool lean_usage;      // colour images list their view formats instead of a bare MUTABLE_FORMAT (textures.c create_image)
    bool smaa_stencil;    // SMAA's weights pass only where the edge pass found edges (exec.c post_process)
    bool no_frame_clear;  // scene targets not cleared at their first use in each frame (textures.c renderer_texture_frame_fresh)
    // window
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
    // HDR output (F1 > Video > HDR; device.c create_swapchain): the window's own images; sc_images / sc_views are then
    // SDR images everything draws into, turned into HDR at vk_frame_end (exec.c vk_hdr_encode)
    bool hdr, hdr_capable;  // (on; the display offers it now)
    VkFormat hdr_format;
    VkColorSpaceKHR hdr_space;
    VkImage hdr_images[8];
    VkImageView hdr_views[8];
    VmaAllocation sdr_mem[8];
    VkImage hdr_base;  // the window image before the overlay (vk_hdr_mark_base): what ImGui drew is told apart from it
    VmaAllocation hdr_base_mem;
    VkImageView hdr_base_view;
} VkG;
void vk_log_memory(const char *why);  // (device.c: per-heap budget and use, at start and on allocation failures)
extern VkG g_vk;

// ---- submissions (exec.c)
// Work is recorded into the current command buffer; uploads go into a separate one that runs first.
VkCommandBuffer vk_cmd(void);         // current command buffer (outside any rendering unless exec.c began one)
VkCommandBuffer vk_upload_cmd(void);  // upload command buffer of the pending submission
uint64_t vk_pending_seq(void);        // number the pending submission will get
// Resources the guest is done with are destroyed this many submissions after the last one that may use them: a
// destroyed buffer / image still read by the GPU is a page fault and a lost device on AMD (#22), NVIDIA reads on
#define VK_DEAD_MARGIN 2
uint64_t vk_completed_seq(void);
void vk_wait_seq(uint64_t seq);
void vk_full_barrier(VkCommandBuffer cb);
void vk_end_pass(void);
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra);  // submit + one more cb (present)
void vk_tex_unpack_finish(void);  // (textures.c) the queued texture data unpacked into the upload ring: before a submission
// GPU timer (exec.c gpu_timer_add): the window image's submission starts its counted work here (after its acquire wait)
void vk_gpu_timer_window_start(void);
bool vk_gpu_split_ns(uint64_t out[4]);  // cumulative: game GPU ns, window GPU ns, frame spans' ns, frames; false: no timer
uint64_t vk_cum_wait_ns(void);          // cumulative vk_wait_seq time
// (device.c) CPU time blocked presenting, cumulative ns: [0] frame slot's fence, [1] window image acquire, [2] present;
// [3]: frames presented
extern uint64_t g_vk_frame_waits[4];

// Transient GPU-visible memory that lives until the submission it was allocated for completes.
typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;
VkSpan vk_transient(uint64_t size, uint64_t align);
VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align);
VkSpan vk_readback(uint64_t size);  // host-cached GPU -> CPU staging (read right after the copy completes)
VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout);

// ---- textures (textures.c)
typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
    bool prefetched;  // (uploaded at its registration, not drawn yet: textures.c backend_prefetch_texture)
    uint64_t data_hash, sample_hash, checked_frame, full_frame;
    uint32_t wgen;  // page write tracking generation at the last upload check (textures.c)
    bool scalable, presented;
    uint64_t touched_frame, frame_cleared;
    uint32_t level_gen;
    float sx, sy;        // the scale the contents were drawn at (scalable textures)
    float ax, ay;        // the allocation scale (the image's size; Dynamic: the contents use a part of it)
    void *pending_rgba;  // host-produced pixels (video) waiting to be uploaded on the render thread
    uint32_t pending_w, pending_h, pending_stride;
    bool pending_nv12;        // pending_rgba holds NV12 (Y, then UV at pending_uv_off): converted on the GPU
    uint32_t pending_uv_off;
    uint8_t astc_bw, astc_bh;  // ASTC without GPU support: decoded to RGBA8 on upload (runtime/nvn/astc.c)
    bool astc_srgb;
    int tp;              // runtime/nvn/texpatch.c: 0 not checked yet, -1 none, else known UI texture + 1
    uint32_t tp_gen;     // texpatch_generation() it was last uploaded with
    int reg;  // slot in textures.c's live list + 1 (a reboot destroys them all)
    bool hd;  // replaced by an HD texture pack's image (textures.c): sx / sy = its size over the game's
    float shadow_k;  // a shadow map drawn at shadow_k times the game's size (textures.c tex_shadow_scale): 0 / 1 = as is
    // (g_vk.lean_usage) the only formats views of this image may have: nview_fmt 0 = any compatible one
    VkFormat view_fmt[2];
    uint8_t nview_fmt;
} TexBackend;

static inline bool vk_format_is_srgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return true;
    default: return f >= VK_FORMAT_ASTC_4x4_SRGB_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 == 1;
    }
}

TexBackend *vk_tex(NvnTexture *t, bool for_render);  // backend image (uploads CPU contents unless for_render)
VkFormat vk_format(uint32_t nvn_format);
VkFormat vk_image_format(uint32_t nvn_format, VkImageAspectFlags *aspect);  // (shader preparation)
void vk_pipeline_cache_save(void);
bool vk_format_alpha_one(uint32_t nvn_format);
// View of t (optionally through an NVN texture view) for rendering or sampling.
VkImageView vk_view(NvnTexture *t, const NvnTextureView *v, bool has_view, bool render, int want_textype);
// Attachment view of every layer in range (3D targets: slices, as a 2D array); *layers = its layer count.
VkImageView vk_render_view(NvnTexture *t, const NvnTextureView *v, bool has_view, uint32_t *layers);
VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkImageView vk_dummy_view(int textype);
VkImageView vk_dummy_depth_view(int textype);  // shadow (depth compare) slots: 1x1 depth, cleared to 1.0
bool vk_format_depth_compare(VkFormat f);  // SAMPLED_IMAGE_DEPTH_COMPARISON
bool vk_tex_depth_compare(NvnTexture *t);  // t's image can be sampled with a depth compare
VkFormat vk_view_format(TexBackend *b, VkFormat want);  // want, or the image's own format where want isn't allowed
void vk_convert_nv12(TexBackend *b, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off);  // exec.c
VkImageView vk_dummy_color_view(uint32_t w, uint32_t h, uint32_t samples);
VkSampler vk_sampler(NvnSamplerDesc *d);
VkSampler vk_default_sampler(bool linear);
VkSampler vk_sampler_nearest(const NvnSamplerDesc *d);  // (integer formats: unfiltered)
int vk_view_int_kind(NvnTexture *t, const NvnTextureView *v, bool has_view);  // 0 float, 1 uint, 2 sint
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect);
// Destruction once every submission that may use the object has completed.
void vk_defer_image(VkImage img, VmaAllocation mem);
void vk_defer_view(VkImageView v);
void vk_defer_buffer(VkBuffer b, VkDeviceMemory m);  // raw memory (an import); destroyed once the pending submission is done
void vk_views_forget(TexBackend *b);  // drop (deferred) the cached views of an image being replaced
uint64_t vk_frame_counter(void);

// ---- shaders / pipelines (shaders.c)
VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name);  // NULL on failure (logged)
VkShaderModule vk_builtin_shader(const char *name);  // internal shaders (clear, copy, present)

// ---- device / window (device.c)
void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h);
