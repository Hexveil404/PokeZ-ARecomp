// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0;
// builders and small state objects store their fields directly in guest memory.
#pragma once
#include "../hle.h"
#include "tegra.h"

#define NVN(name) HLE_FUNC(#name, hle_##name)

// argument helpers
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (*(float *)&c->v[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(f) do { c->v[0] = 0; *(float *)&c->v[0] = (f); } while (0)
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
#define SET_HOST(guest, p) (*(uint64_t *)(uintptr_t)(guest) = (uint64_t)(uintptr_t)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))

typedef struct NvnPool {
    uint64_t storage, size;
    uint32_t flags;
} NvnPool;

typedef struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of texel storage
    NvnPool *pool;
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    uint64_t debug_id;
    uint32_t pool_refs;  // texture pool entries that name it (cleared when it is finalized: runtime/nvn/objects.c)
    bool format_views;   // named through a view in a format the backend can only give with a flag (Metal: PixelFormatView)
    char label[64];
} NvnTexture;

// Also kept inside the guest's NVNsamplerBuilder (at +8, runtime/nvn/objects.c), which is 64 bytes: Pokémon Legends
// Z-A builds one on the stack right below its frame record. The enums fit a byte.
typedef struct {
    uint8_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
    uint32_t backend_gen;  // (Vulkan textures.c vk_sampler: the sampler settings backend was made with)
} NvnSamplerDesc;
_Static_assert(8 + sizeof(NvnSamplerDesc) <= 64, "NvnSamplerDesc must fit an NVNsamplerBuilder");

typedef struct {
    uint32_t setmask;  // which fields are overridden
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;  // our layout inside the guest NVNtextureView
_Static_assert(sizeof(NvnTextureView) <= 32, "NvnTextureView must fit the guest's");

// A view naming a texture (pool entry, render target, clear, copy): one in another format is told to the backend before
// the texture is used through it.
void backend_texture_view_format(NvnTexture *tex, uint32_t format);  // (runtime/nvn/metal.m, vulkan/textures.c)
static inline void nvn_texture_note_view(NvnTexture *t, const NvnTextureView *v) {
    if (t && v && (v->setmask & 4) && v->format != t->format) backend_texture_view_format(t, v->format);
}

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
    uint32_t dead_format;  // the texture registered here was finalized (NVN format + 1; 0 = no): tex is NULL
} NvnTexEntry;

// A draw named a texture pool slot whose texture was finalized (runtime/nvn/objects.c): logged, a few times
void nvn_dead_slot_sampled(const NvnTexEntry *e, uint32_t slot);

typedef struct NvnDescPool {
    uint64_t addr;
    int count;
    void *entries;  // NvnTexEntry[] or NvnSamplerDesc[]
} NvnDescPool;

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (== CPU) address of the code
    const uint8_t *control;   // control section
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    void *backend;
} NvnShaderStage;

typedef struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
} NvnProgram;

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
    uint64_t present_seq[8];  // GPU submission that last presented each texture (its acquire sync waits for it)
} NvnWindow;

typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
} NvnSync;

// ---- command recording (stored in guest command memory)
typedef struct {
    uint64_t mem;    // current command memory chunk (guest address)
    uint64_t size;   // chunk size
    uint64_t used;   // bytes used in chunk
    uint64_t start;  // first record of the list being recorded
    uint64_t callback, callback_data;
    bool recording;
    uint64_t guest;  // the guest NVNcommandBuffer*
    uint64_t owner;  // in a host overflow chunk (cmdbuf.c): the guest chunk whose list jumped there, else 0
} NvnCmdBuf;

// One recorded command: header, the call's integer and float arguments, then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
    uint8_t extra[];
} CmdRec;

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH,
    CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT, CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT,
    CMD_BEGIN_XFB, CMD_END_XFB, CMD_BIND_XFB_BUFFER, CMD_BIND_XFB_BUFFERS, CMD_SET_RASTERIZER_DISCARD, CMD_DRAW_XFB,
    CMD_SET_DEPTH_CLAMP,
    CMD_COUNT
};



// renderer backend (runtime/nvn/metal.m)
void backend_init(void);
void backend_execute(uint64_t handle);            // run a recorded command list
void backend_execute_batch(const uint64_t *handles, int n);  // the lists of one nvnQueueSubmitCommands
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);
