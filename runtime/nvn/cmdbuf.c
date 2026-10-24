// NVN command buffers: commands are recorded into the guest-provided command memory as CmdRec
// records, so the game's own memory management and out-of-memory callbacks work as designed.
// A command handle is the guest address of a list's first record.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "nvn.h"

#define JUMP_RESERVE (sizeof(CmdRec) + 8)

static NvnCmdBuf *cb_of(uint64_t guest) { return HOST(NvnCmdBuf, guest); }

NVN(nvnCommandBufferInitialize) {
    NvnCmdBuf *cb = calloc(1, sizeof *cb);
    cb->guest = A(0);
    SET_HOST(A(0), cb);
    RET(1);
}
NVN(nvnCommandBufferFinalize) { free(cb_of(A(0))); SET_HOST(A(0), 0); }
NVN(nvnCommandBufferSetDebugLabel) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetDebugLabel\n"); }

static CmdRec *raw_rec(uint64_t at, uint32_t op, uint32_t size) {
    CmdRec *r = (CmdRec *)(uintptr_t)at;
    memset(r, 0, sizeof *r);
    r->op = op;
    r->size = size;
    return r;
}

// Host overflow chunks. Our records are far bigger than the hardware's commands (and some carry copied data), so a
// record can outgrow what the game's memory callback hands out: the list then goes on in host memory. A chunk is
// reached only through a jump from the guest chunk it continues (its owner), and lives until the game gives that
// guest memory to a command buffer again (the old list is gone then).
typedef struct Overflow { uint64_t owner; void *mem; struct Overflow *next; } Overflow;
static Overflow *overflows;
static pthread_mutex_t overflow_lock = PTHREAD_MUTEX_INITIALIZER;

static void overflow_free(uint64_t owner) {
    pthread_mutex_lock(&overflow_lock);
    for (Overflow **p = &overflows; *p;) {
        if ((*p)->owner == owner) {
            Overflow *o = *p;
            *p = o->next;
            free(o->mem);
            free(o);
        } else {
            p = &(*p)->next;
        }
    }
    pthread_mutex_unlock(&overflow_lock);
}

static void overflow_chunk(NvnCmdBuf *cb, uint32_t need) {
    uint64_t size = need + JUMP_RESERVE > 0x10000 ? need + JUMP_RESERVE : 0x10000;
    Overflow *o = malloc(sizeof *o);
    o->owner = cb->owner ? cb->owner : cb->mem;
    o->mem = malloc(size);
    pthread_mutex_lock(&overflow_lock);
    o->next = overflows;
    overflows = o;
    pthread_mutex_unlock(&overflow_lock);
    static int logged;
    if (logged++ < 5) LOG("[nvn] command memory overflow: %u bytes, list goes on in host memory\n", need);
    if (cb->mem) {
        CmdRec *j = raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = (uint64_t)(uintptr_t)o->mem;
    }
    cb->owner = o->owner;
    cb->mem = (uint64_t)(uintptr_t)o->mem;
    cb->size = size;
    cb->used = 0;
}

NVN(nvnCommandBufferAddCommandMemory) {
    NvnCmdBuf *cb = cb_of(A(0));
    uint64_t mem = HOST(NvnPool, A(1))->storage + A(2), size = A(3);
    overflow_free(mem);  // (records there from an earlier list are dead)
    cb->owner = 0;
    if (cb->recording && cb->mem) {
        // chain the list being recorded into the new chunk
        CmdRec *j = raw_rec(cb->mem + cb->used, CMD_JUMP, sizeof(CmdRec));
        j->x[0] = mem;
    }
    cb->mem = mem;
    cb->size = size;
    cb->used = 0;
}
NVN(nvnCommandBufferAddControlMemory) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferAddControlMemory\n"); }
NVN(nvnCommandBufferGetCommandMemorySize) { RET(cb_of(A(0))->size); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { RET(cb_of(A(0))->used); }
NVN(nvnCommandBufferGetCommandMemoryFree) {
    NvnCmdBuf *cb = cb_of(A(0));
    RET(cb->size > cb->used + JUMP_RESERVE ? cb->size - cb->used - JUMP_RESERVE : 0);
}
NVN(nvnCommandBufferGetControlMemorySize) { RET(0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { RET(0); }
NVN(nvnCommandBufferGetControlMemoryFree) { RET(0x10000); }
NVN(nvnCommandBufferSetMemoryCallback) { cb_of(A(0))->callback = A(1); }
NVN(nvnCommandBufferGetMemoryCallback) { RET(cb_of(A(0))->callback); }
NVN(nvnCommandBufferSetMemoryCallbackData) { cb_of(A(0))->callback_data = A(1); }
NVN(nvnCommandBufferGetMemoryCallbackData) { RET(cb_of(A(0))->callback_data); }
NVN(nvnCommandBufferSetCommandMemoryCallbackEnabled) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetCommandMemoryCallbackEnabled\n"); }

// Reserve a record. May call back into the guest for more memory, so callers snapshot their
// arguments first.
static CmdRec *rec(Cpu *c, NvnCmdBuf *cb, uint32_t op, uint32_t extra) {
    uint32_t size = (uint32_t)((sizeof(CmdRec) + extra + 7) & ~7u);
    for (int tries = 0; !cb->mem || cb->used + size + JUMP_RESERVE > cb->size; tries++) {
        if (!cb->callback || tries == 2) {
            if (!cb->mem) FATAL("nvn: out of command memory (need %u)\n", size);
            overflow_chunk(cb, size);
            break;
        }
        uint64_t saved[8];
        memcpy(saved, c->x, sizeof saved);
        c->x[0] = cb->guest;
        c->x[1] = 0;  // NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY
        c->x[2] = size + JUMP_RESERVE;
        c->x[3] = cb->callback_data;
        guest_call(c, cb->callback);
        memcpy(c->x, saved, sizeof saved);
    }
    CmdRec *r = raw_rec(cb->mem + cb->used, op, size);
    cb->used += size;
    return r;
}

NVN(nvnCommandBufferBeginRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    cb->recording = true;
    rec(c, cb, CMD_BARRIER, 0);  // makes sure memory exists; the list starts here
    cb->start = cb->mem + cb->used - sizeof(CmdRec);
}
NVN(nvnCommandBufferEndRecording) {
    NvnCmdBuf *cb = cb_of(A(0));
    rec(c, cb, CMD_END, 0);
    cb->recording = false;
    RET(cb->start);
}
NVN(nvnCommandBufferIsRecording) { RET(cb_of(A(0))->recording); }

// ---- generic recorders
typedef struct { uint64_t x[8]; float f[4]; } Snap;
static Snap snap(Cpu *c) {
    Snap s;
    memcpy(s.x, c->x, sizeof s.x);
    for (int i = 0; i < 4; i++) s.f[i] = *(float *)&c->v[i];
    return s;
}
static CmdRec *record(Cpu *c, uint32_t op, uint32_t extra, Snap *s) {
    CmdRec *r = rec(c, cb_of(s->x[0]), op, extra);
    memcpy(r->x, s->x + 1, sizeof r->x);
    memcpy(r->f, s->f, sizeof r->f);
    return r;
}
// record with a copy of `len` bytes from the guest pointer in argument `arg`
static CmdRec *record_copy(Cpu *c, uint32_t op, int arg, uint32_t len) {
    Snap s = snap(c);
    CmdRec *r = record(c, op, len, &s);
    if (s.x[arg] && len) memcpy(r->extra, (void *)(uintptr_t)s.x[arg], len);
    return r;
}
#define PLAIN(name, op) NVN(name) { Snap s = snap(c); record(c, op, 0, &s); }

// ---- state binding
NVN(nvnCommandBufferBindProgram) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_PROGRAM, 0, &s);
    r->x[0] = s.x[1] ? (uint64_t)(uintptr_t)HOST(NvnProgram, s.x[1]) : 0;  // host program (NULL unbinds: draws skip)
}
NVN(nvnCommandBufferBindBlendState) { record_copy(c, CMD_BIND_BLEND, 1, 8); }
NVN(nvnCommandBufferBindChannelMaskState) { record_copy(c, CMD_BIND_CHANNEL_MASK, 1, 4); }
NVN(nvnCommandBufferBindColorState) { record_copy(c, CMD_BIND_COLOR, 1, 4); }
NVN(nvnCommandBufferBindDepthStencilState) { record_copy(c, CMD_BIND_DEPTH_STENCIL, 1, 8); }
NVN(nvnCommandBufferBindMultisampleState) { record_copy(c, CMD_BIND_MULTISAMPLE, 1, 0x18); }
NVN(nvnCommandBufferBindPolygonState) { record_copy(c, CMD_BIND_POLYGON, 1, 4); }
NVN(nvnCommandBufferBindVertexAttribState) { record_copy(c, CMD_BIND_VERTEX_ATTRIB, 2, (uint32_t)A(1) * 4); }
NVN(nvnCommandBufferBindVertexStreamState) { record_copy(c, CMD_BIND_VERTEX_STREAM, 2, (uint32_t)A(1) * 8); }

// (cmd, first, count, const NVNbufferRange*) and the single-binding forms
NVN(nvnCommandBufferBindVertexBuffers) { record_copy(c, CMD_BIND_VERTEX_BUFFERS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferBindVertexBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_VERTEX_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = 1;
    memcpy(r->extra, &s.x[2], 16);
}
// (cmd, stage, first, count, const NVNbufferRange*)
NVN(nvnCommandBufferBindUniformBuffers) { record_copy(c, CMD_BIND_UNIFORM_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindUniformBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_UNIFORM_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
NVN(nvnCommandBufferBindStorageBuffers) { record_copy(c, CMD_BIND_STORAGE_BUFFERS, 4, (uint32_t)A(3) * 16); }
NVN(nvnCommandBufferBindStorageBuffer) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_STORAGE_BUFFERS, 16, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 16);
}
// (cmd, stage, first, count, const handle*)
NVN(nvnCommandBufferBindTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindImages) { record_copy(c, CMD_BIND_IMAGES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindImage) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_IMAGES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateTextures) { record_copy(c, CMD_BIND_TEXTURES, 4, (uint32_t)A(3) * 8); }
NVN(nvnCommandBufferBindSeparateTexture) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_BIND_TEXTURES, 8, &s);
    r->x[0] = s.x[1]; r->x[1] = s.x[2]; r->x[2] = 1;
    memcpy(r->extra, &s.x[3], 8);
}
NVN(nvnCommandBufferBindSeparateSamplers) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferBindSeparateSamplers\n"); }
NVN(nvnCommandBufferBindSeparateSampler) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferBindSeparateSampler\n"); }
// (buffers: first, count, NVNbufferRange* {address, size}) copied with the command
NVN(nvnCommandBufferBindTransformFeedbackBuffers) {
    Snap s = snap(c);
    uint32_t n = (uint32_t)s.x[2] < 4 ? (uint32_t)s.x[2] : 4;
    CmdRec *r = record(c, CMD_BIND_XFB_BUFFERS, n * 16, &s);
    if (n && s.x[3]) memcpy(r->extra, (const void *)(uintptr_t)s.x[3], n * 16);
}
PLAIN(nvnCommandBufferBindTransformFeedbackBuffer, CMD_BIND_XFB_BUFFER)  // (index, address, size)
NVN(nvnCommandBufferBindCoverageModulationTable) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferBindCoverageModulationTable\n"); }

NVN(nvnCommandBufferSetTexturePool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_TEXTURE_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}
NVN(nvnCommandBufferSetSamplerPool) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SAMPLER_POOL, 0, &s);
    r->x[0] = (uint64_t)(uintptr_t)HOST(NvnDescPool, s.x[1]);
}

// SetRenderTargets(cmd, n, colors[], colorViews[], depth, depthView)
// extra: { u64 tex[8]; NvnTextureView views[8]; u8 has_view[8]; u64 depth; NvnTextureView dview; u8 has_dview }
typedef struct {
    uint64_t tex[8];
    NvnTextureView views[8];
    uint8_t has_view[8];
    uint64_t depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtRec;
NVN(nvnCommandBufferSetRenderTargets) {
    Snap s = snap(c);
    int n = (int)s.x[1];
    if (n > 8) n = 8;
    RtRec rt = {0};
    const uint64_t *colors = (const uint64_t *)(uintptr_t)s.x[2], *views = (const uint64_t *)(uintptr_t)s.x[3];
    for (int i = 0; i < n; i++) {
        rt.tex[i] = colors && colors[i] ? (uint64_t)(uintptr_t)HOST(NvnTexture, colors[i]) : 0;
        if (views && views[i]) { rt.has_view[i] = 1; memcpy(&rt.views[i], (void *)(uintptr_t)views[i], sizeof(NvnTextureView)); }
    }
    rt.depth = s.x[4] ? (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[4]) : 0;
    if (s.x[5]) { rt.has_dview = 1; memcpy(&rt.dview, (void *)(uintptr_t)s.x[5], sizeof(NvnTextureView)); }
    CmdRec *r = record(c, CMD_SET_RENDER_TARGETS, sizeof rt, &s);
    r->x[0] = (uint64_t)n;
    memcpy(r->extra, &rt, sizeof rt);
}

// viewports/scissors/depth ranges
NVN(nvnCommandBufferSetViewports) { record_copy(c, CMD_SET_VIEWPORTS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetViewport) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_VIEWPORTS, 16, &s);
    float v[4] = {(float)(int32_t)s.x[1], (float)(int32_t)s.x[2], (float)(int32_t)s.x[3], (float)(int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetScissors) { record_copy(c, CMD_SET_SCISSORS, 3, (uint32_t)A(2) * 16); }
NVN(nvnCommandBufferSetScissor) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_SCISSORS, 16, &s);
    int32_t v[4] = {(int32_t)s.x[1], (int32_t)s.x[2], (int32_t)s.x[3], (int32_t)s.x[4]};
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, v, 16);
}
NVN(nvnCommandBufferSetDepthRanges) { record_copy(c, CMD_SET_DEPTH_RANGES, 3, (uint32_t)A(2) * 8); }
NVN(nvnCommandBufferSetDepthRange) {
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_SET_DEPTH_RANGES, 8, &s);
    r->x[0] = 0; r->x[1] = 1;
    memcpy(r->extra, s.f, 8);
}
NVN(nvnCommandBufferSetBlendColor) { record_copy(c, CMD_SET_BLEND_COLOR, 1, 16); }
NVN(nvnCommandBufferSetStencilRef) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 0; }
NVN(nvnCommandBufferSetStencilMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 1; }
NVN(nvnCommandBufferSetStencilValueMask) { Snap s = snap(c); CmdRec *r = record(c, CMD_SET_STENCIL, 0, &s); r->x[6] = 2; }
PLAIN(nvnCommandBufferSetPrimitiveRestart, CMD_SET_PRIMITIVE_RESTART)
PLAIN(nvnCommandBufferSetAlphaRef, CMD_SET_ALPHA_REF)
PLAIN(nvnCommandBufferSetDepthBounds, CMD_SET_DEPTH_BOUNDS)
PLAIN(nvnCommandBufferSetPolygonOffsetClamp, CMD_SET_POLYGON_OFFSET)
PLAIN(nvnCommandBufferSetLineWidth, CMD_SET_LINE_WIDTH)
PLAIN(nvnCommandBufferSetPointSize, CMD_SET_POINT_SIZE)
PLAIN(nvnCommandBufferSetSampleMask, CMD_SET_SAMPLE_MASK)
PLAIN(nvnCommandBufferSetRenderEnable, CMD_SET_RENDER_ENABLE)
NVN(nvnCommandBufferSetRenderEnableConditional) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetRenderEnableConditional\n"); }
PLAIN(nvnCommandBufferSetDepthClamp, CMD_SET_DEPTH_CLAMP)  // (the game's clamp: geometry past the clip planes is clamped, not clipped)
PLAIN(nvnCommandBufferSetRasterizerDiscard, CMD_SET_RASTERIZER_DISCARD)
NVN(nvnCommandBufferSetConservativeRasterEnable) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetConservativeRasterEnable\n"); }
NVN(nvnCommandBufferSetConservativeRasterDilate) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetConservativeRasterDilate\n"); }
NVN(nvnCommandBufferSetSubpixelPrecisionBias) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetSubpixelPrecisionBias\n"); }
NVN(nvnCommandBufferSetViewportSwizzles) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetViewportSwizzles\n"); }
NVN(nvnCommandBufferSetStencilCullCriteria) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetStencilCullCriteria\n"); }
NVN(nvnCommandBufferSetColorReductionEnable) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetColorReductionEnable\n"); }
NVN(nvnCommandBufferSetColorReductionThresholds) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetColorReductionThresholds\n"); }
NVN(nvnCommandBufferSetTiledCacheAction) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetTiledCacheAction\n"); }
NVN(nvnCommandBufferSetTiledCacheTileSize) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetTiledCacheTileSize\n"); }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetShaderScratchMemory\n"); }
NVN(nvnCommandBufferSetPatchSize) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetPatchSize\n"); }
NVN(nvnCommandBufferSetInnerTessellationLevels) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetInnerTessellationLevels\n"); }
NVN(nvnCommandBufferSetOuterTessellationLevels) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetOuterTessellationLevels\n"); }
NVN(nvnCommandBufferSetProgramSubroutines) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSetProgramSubroutines\n"); }
NVN(nvnCommandBufferSaveZCullData) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferSaveZCullData\n"); }
NVN(nvnCommandBufferRestoreZCullData) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferRestoreZCullData\n"); }

static uint64_t copy_row_stride, copy_image_stride;
RT_ON_RESET(cmdbuf_reset) { copy_row_stride = copy_image_stride = 0; }
NVN(nvnCommandBufferSetCopyRowStride) {
    Snap s = snap(c);
    copy_row_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferSetCopyImageStride) {
    Snap s = snap(c);
    copy_image_stride = s.x[1];
    CmdRec *r = record(c, CMD_SET_COPY_STRIDES, 0, &s);
    r->x[0] = copy_row_stride; r->x[1] = copy_image_stride;
}
NVN(nvnCommandBufferGetCopyRowStride) { RET(copy_row_stride); }
NVN(nvnCommandBufferGetCopyImageStride) { RET(copy_image_stride); }

// ---- clears
NVN(nvnCommandBufferClearColor) { record_copy(c, CMD_CLEAR_COLOR, 2, 16); }
NVN(nvnCommandBufferClearColori) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 1; }
NVN(nvnCommandBufferClearColorui) { CmdRec *r = record_copy(c, CMD_CLEAR_COLOR, 2, 16); r->x[5] = 2; }
PLAIN(nvnCommandBufferClearDepthStencil, CMD_CLEAR_DEPTH_STENCIL)
// ClearTexture(cmd, texture, view, region, color, mask)
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } ClearTexRec;
static void clear_texture(Cpu *c, int kind) {
    Snap s = snap(c);
    ClearTexRec ct = {0};
    ct.tex = (uint64_t)(uintptr_t)HOST(NvnTexture, s.x[1]);
    if (s.x[2]) { ct.has_view = 1; memcpy(&ct.view, (void *)(uintptr_t)s.x[2], sizeof ct.view); }
    if (s.x[3]) memcpy(ct.region, (void *)(uintptr_t)s.x[3], sizeof ct.region);
    if (s.x[4]) memcpy(ct.color, (void *)(uintptr_t)s.x[4], 16);
    CmdRec *r = record(c, CMD_CLEAR_TEXTURE, sizeof ct, &s);
    r->x[6] = (uint64_t)kind;
    memcpy(r->extra, &ct, sizeof ct);
}
NVN(nvnCommandBufferClearTexture) { clear_texture(c, 0); }
NVN(nvnCommandBufferClearTexturei) { clear_texture(c, 1); }
NVN(nvnCommandBufferClearTextureui) { clear_texture(c, 2); }
PLAIN(nvnCommandBufferClearBuffer, CMD_CLEAR_BUFFER)
// DiscardColor(cmdbuf, index): the target's contents become undefined. On the Switch a discarded (compressed)
// target reads back cleared; areas the game then leaves undrawn (e.g. a pit after a floor tile falls away) stay
// black there. Treated as a clear to (0,0,0,0) (LARECOMP_KEEP_DISCARD=1: old no-op behaviour).
NVN(nvnCommandBufferDiscardColor) {
    static int keep = -1, logged;
    if (keep < 0) keep = ENV("LARECOMP_KEEP_DISCARD") != NULL;
    if (logged < 5) { logged++; LOG("[nvn] DiscardColor index %llu%s\n", (unsigned long long)c->x[1], keep ? " (kept)" : " -> clear"); }
    if (keep) return;
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_CLEAR_COLOR, 16, &s);
    memset(r->extra, 0, 16);
    r->x[2] = 0xF;  // all channels
    r->x[5] = 0;    // float clear
}
NVN(nvnCommandBufferDiscardDepthStencil) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferDiscardDepthStencil\n"); }

// ---- draws (all integer arguments fit in x1..x7)
NVN(nvnCommandBufferDrawArrays) { Snap s = snap(c); CmdRec *r = record(c, CMD_DRAW_ARRAYS, 0, &s); r->x[3] = 0; r->x[4] = 1; }
NVN(nvnCommandBufferDrawArraysInstanced) { Snap s = snap(c); record(c, CMD_DRAW_ARRAYS, 0, &s); }
// Debug: LARECOMP_TRACE_COUNT=<n> logs the guest call chain of indexed draws with n indices.
static void trace_draw(Cpu *c, uint64_t count) {
    static long want = -2;
    if (want == -2) want = ENV("LARECOMP_TRACE_COUNT") ? atol(ENV("LARECOMP_TRACE_COUNT")) : -1;
    if (want < 0 || (long)count != want) return;
    static int n;
    if (n++ > 40) return;
    LOG("[trace] draw count %llu lr %llx chain:", (unsigned long long)count, (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 14 && fp >= 0x7000000000ull && fp < 0x8000000000ull && (fp & 7) == 0; i++) {
        uint64_t next = *(uint64_t *)(uintptr_t)fp, lr = *(uint64_t *)(uintptr_t)(fp + 8);
        LOG(" %llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    LOG("\n");
}

NVN(nvnCommandBufferDrawElements) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[4] = 0; r->x[5] = 0; r->x[6] = 1;  // base vertex, base instance, instances
}
NVN(nvnCommandBufferDrawElementsBaseVertex) {
    trace_draw(c, c->x[3]);
    Snap s = snap(c);
    CmdRec *r = record(c, CMD_DRAW_ELEMENTS, 0, &s);
    r->x[5] = 0; r->x[6] = 1;
}
NVN(nvnCommandBufferDrawElementsInstanced) { trace_draw(c, c->x[3]); Snap s = snap(c); record(c, CMD_DRAW_ELEMENTS, 0, &s); }
PLAIN(nvnCommandBufferDrawArraysIndirect, CMD_DRAW_ARRAYS_INDIRECT)
PLAIN(nvnCommandBufferDrawElementsIndirect, CMD_DRAW_ELEMENTS_INDIRECT)
PLAIN(nvnCommandBufferMultiDrawArraysIndirectCount, CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT)
PLAIN(nvnCommandBufferMultiDrawElementsIndirectCount, CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT)
PLAIN(nvnCommandBufferDrawTransformFeedback, CMD_DRAW_XFB)  // (mode, control buffer)
NVN(nvnCommandBufferDrawTexture) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferDrawTexture\n"); }
PLAIN(nvnCommandBufferDispatchCompute, CMD_DISPATCH)
PLAIN(nvnCommandBufferDispatchComputeIndirect, CMD_DISPATCH_INDIRECT)
PLAIN(nvnCommandBufferBeginTransformFeedback, CMD_BEGIN_XFB)  // (control buffer)
PLAIN(nvnCommandBufferEndTransformFeedback, CMD_END_XFB)
NVN(nvnCommandBufferPauseTransformFeedback) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPauseTransformFeedback\n"); }
NVN(nvnCommandBufferResumeTransformFeedback) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferResumeTransformFeedback\n"); }

// ---- copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRef;
static void tex_ref(TexRef *t, uint64_t tex, uint64_t view, uint64_t region) {
    t->tex = tex ? (uint64_t)(uintptr_t)HOST(NvnTexture, tex) : 0;
    if (view) { t->has_view = 1; memcpy(&t->view, (void *)(uintptr_t)view, sizeof t->view); }
    if (region) memcpy(t->region, (void *)(uintptr_t)region, sizeof t->region);
}
NVN(nvnCommandBufferCopyBufferToTexture) {  // (cmd, src, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[2], s.x[3], s.x[4]);
    CmdRec *r = record(c, CMD_COPY_BUFFER_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToBuffer) {  // (cmd, src, srcView, srcRegion, dst, flags)
    Snap s = snap(c);
    TexRef t = {0};
    tex_ref(&t, s.x[1], s.x[2], s.x[3]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_BUFFER, sizeof t, &s);
    memcpy(r->extra, &t, sizeof t);
}
NVN(nvnCommandBufferCopyTextureToTexture) {  // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags)
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], s.x[3]);
    tex_ref(&t[1], s.x[4], s.x[5], s.x[6]);
    CmdRec *r = record(c, CMD_COPY_TEXTURE_TO_TEXTURE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
PLAIN(nvnCommandBufferCopyBufferToBuffer, CMD_COPY_BUFFER_TO_BUFFER)
NVN(nvnCommandBufferDownsample) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], 0, 0);
    tex_ref(&t[1], s.x[2], 0, 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferDownsampleTextureView) {
    Snap s = snap(c);
    TexRef t[2] = {{0}};
    tex_ref(&t[0], s.x[1], s.x[2], 0);
    tex_ref(&t[1], s.x[3], s.x[4], 0);
    CmdRec *r = record(c, CMD_DOWNSAMPLE, sizeof t, &s);
    memcpy(r->extra, t, sizeof t);
}
NVN(nvnCommandBufferTiledDownsample) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferTiledDownsample\n"); }
NVN(nvnCommandBufferTiledDownsampleTextureView) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferTiledDownsampleTextureView\n"); }
NVN(nvnCommandBufferResolveDepthBuffer) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferResolveDepthBuffer\n"); }
// UpdateUniformBuffer(cmd, buffer, size, offset, dataSize, data)
NVN(nvnCommandBufferUpdateUniformBuffer) { record_copy(c, CMD_UPDATE_UNIFORM_BUFFER, 5, (uint32_t)A(4)); }

// ---- synchronization and misc
NVN(nvnCommandBufferFenceSync) {
    // work executes at submit time, so the sync is satisfied once this list has run
    Snap s = snap(c);
    record(c, CMD_FENCE_SYNC, 0, &s);
}
PLAIN(nvnCommandBufferWaitSync, CMD_WAIT_SYNC)
PLAIN(nvnCommandBufferBarrier, CMD_BARRIER)
PLAIN(nvnCommandBufferSignalEvent, CMD_SIGNAL_EVENT)
PLAIN(nvnCommandBufferWaitEvent, CMD_WAIT_EVENT)
PLAIN(nvnCommandBufferReportCounter, CMD_REPORT_COUNTER)
NVN(nvnCommandBufferReportValue) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferReportValue\n"); }
NVN(nvnCommandBufferResetCounter) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferResetCounter\n"); }
NVN(nvnCommandBufferCallCommands) { record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferCopyCommands) { record_copy(c, CMD_CALL, 2, (uint32_t)A(1) * 8); }
NVN(nvnCommandBufferInsertDebugMarker) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferInsertDebugMarker\n"); }
NVN(nvnCommandBufferInsertDebugMarkerDynamic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferInsertDebugMarkerDynamic\n"); }
NVN(nvnCommandBufferInsertDebugMarkerStatic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferInsertDebugMarkerStatic\n"); }
NVN(nvnCommandBufferPushDebugGroup) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPushDebugGroup\n"); }
NVN(nvnCommandBufferPushDebugGroupDynamic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPushDebugGroupDynamic\n"); }
NVN(nvnCommandBufferPushDebugGroupStatic) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPushDebugGroupStatic\n"); }
NVN(nvnCommandBufferPopDebugGroup) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPopDebugGroup\n"); }
NVN(nvnCommandBufferPopDebugGroupId) { (void)c; static int seen_; if (!seen_++ && ENV("LARECOMP_LOG_NOOP")) LOG("[nvn] no-op called: nvnCommandBufferPopDebugGroupId\n"); }
