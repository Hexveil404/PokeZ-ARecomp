// Maxwell (GM20B) shader translation to Metal Shading Language (macOS) or Vulkan GLSL (Windows; compiled to
// SPIR-V at runtime with shaderc). ShaderKey.glsl selects the dialect; the MSL output does not depend on it.
//
// GLSL contract (what the Vulkan renderer binds):
//  - `#version 460`, entry point `main`. Descriptor set: vertex 0, fragment 1, compute 0.
//  - hardware cbuf k -> binding k: `layout(std140, set=S, binding=k) uniform CB<k> { uvec4 cb<k>[N]; };`
//    (anonymous block). N = max(cbuf_size[k] / 16, highest constant-index vec4 + 1), and at least 4096 when
//    cbuf_dynamic has bit k (the renderer binds up to 64 KB). Register-indexed reads are clamped to N - 1.
//  - global memory (LDG/STG) -> binding GMEM_BUFFER_INDEX (17):
//    `layout(std430, set=S, binding=17) [readonly] buffer GMem { uint w[]; } gmem_;`, 32-bit words relative to
//    maxwell_gmem_base. `readonly` unless the shader stores (needs vertexPipeline/fragmentStoresAndAtomics then);
//    byte / short stores are atomicAnd + atomicOr on the containing word.
//  - rscale table -> binding 18: `uniform RScale { vec4 rscale[65]; };` (std140, the MSL buffer 18 contents).
//  - per-draw tint -> binding 19: `uniform Tint { vec4 ltint[4]; };` (fragment, when uses_tint).
//  - texture slot i (ShaderInfo.tex[i]) -> binding 32 + i, a combined image sampler: sampler1D/2D/3D/Cube/
//    1DArray/2DArray/CubeArray, or its *Shadow variant when tex[i].shadow.
//  - vertex inputs `layout(location=i) in vec4|ivec4|uvec4 a<i>;` (key->attr_kind); varyings: location i =
//    generic attribute i, `vec4 v<i>` (fragment side flat / noperspective as in MSL); `invariant gl_Position`;
//    gl_PointSize when writes_point_size, gl_Layer when writes_layer (GL_ARB_shader_viewport_layer_array:
//    shaderOutputLayer). Fragment outputs `layout(location=rt) out vec4|ivec4|uvec4 c<rt>;`,
//    gl_FragDepth when writes_depth.
//  - clip space exactly as the MSL output (key->flip_y / depth_minus_one epilogue unchanged): the renderer uses
//    a negative-height viewport so Vulkan matches Metal's y-up NDC, clip z in [0, 1].
//  - compute: `layout(local_size_x_id = 0, local_size_y_id = 1, local_size_z_id = 2) in;` (spec constants 0..2).
//  - extensions: GL_EXT_shader_explicit_arithmetic_types_int64 (64-bit carry / address math -> shaderInt64),
//    GL_KHR_shader_subgroup_basic/_shuffle/_shuffle_relative (SHFL, FSWZADD lane: fragment/compute subgroup
//    support), GL_EXT_texture_shadow_lod (explicit-LOD array / cube shadow lookups).
//  - not in GLSL: the maxwell_dbg_vs position dump (buffer 29) is MSL only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { STAGE_VERTEX = 0, STAGE_FRAGMENT = 1, STAGE_GEOMETRY = 2, STAGE_TESS_CTRL = 3, STAGE_TESS_EVAL = 4,
       STAGE_COMPUTE = 5 };

enum { TEXTYPE_1D, TEXTYPE_2D, TEXTYPE_3D, TEXTYPE_CUBE, TEXTYPE_2D_ARRAY, TEXTYPE_CUBE_ARRAY, TEXTYPE_1D_ARRAY,
       TEXTYPE_BUFFER, TEXTYPE_2D_MS };

typedef struct {
    uint8_t attr_kind[32];  // vertex inputs: 0 float, 1 signed int, 2 unsigned int
    uint8_t rt_kind[8];     // fragment outputs: 0 float, 1 signed int, 2 unsigned int
    bool depth_minus_one;   // NVN depth mode [-1,1]: remap clip z
    bool flip_y;            // lower-left window origin
    bool flip_x;            // mirror the image left to right (mirror mode's scene composite). (In the padding
                            // after flip_y: keys without it hash as before - shader caches stay valid.)
    uint32_t vs_out_mask;   // fragment: varyings the vertex shader provides
    uint8_t glsl;           // emit Vulkan GLSL (see the contract above) instead of MSL
    uint32_t stencil_slots; // (GLSL) texture handle slots ((handle_offset - 0x20) / 8) bound in stencil mode
                            // (NVN depth/stencil mode STENCIL) or with an unsigned integer format: a usampler,
                            // results read as uint bits (what Maxwell's texture fetches give for those)
    // (GLSL vertex) transform feedback: buffer b takes xfb_count[b] 32-bit output components, xfb_stride[b] bytes per
    // vertex. The written varying components go to the buffers in order (location, then x..w). Where (and whether)
    // per draw: rscale[66 + b] = (word offset of buffer b in global memory, -, on, -) as uint bits (binding 18).
    uint8_t xfb_count[4];
    uint16_t xfb_stride[4];
    uint32_t sint_slots;    // (GLSL) the slots with a signed integer format: an isampler, results read as int bits
} ShaderKey;

typedef struct {
    uint32_t handle_offset;  // byte offset of the texture handle in the bound texture cbuf
    uint8_t type;            // TEXTYPE_*
    bool shadow, is_array, is_image, bindless;
    bool stencil;            // (GLSL) a stencil view or unsigned integer format: usampler (ShaderKey.stencil_slots)
    bool sint;               // (GLSL) a signed integer format: isampler (ShaderKey.sint_slots)
    uint32_t bindless_cbuf, bindless_offset;
} ShaderTexture;

typedef struct {
    int stage;
    char *msl;              // complete MSL or GLSL source (MSL entry "main0", GLSL "main"); NULL on failure
    uint32_t cbuf_mask;     // hardware constant buffers read
    uint32_t cbuf_size[18];
    uint32_t cbuf_dynamic;  // cbufs indexed with a register: bind the whole buffer
    int ntex;
    ShaderTexture tex[64];
    uint32_t attr_mask;     // vertex: input attributes; fragment: input varyings
    uint32_t out_mask;      // vertex: output varyings; fragment: render targets written
    bool writes_depth, uses_discard, writes_point_size;
    bool writes_layer;      // vertex: writes the render target layer (attribute 0x64)
    bool uses_gmem;         // global memory access: bind the guest heap at GMEM_BUFFER_INDEX
    bool uses_gmem_map;     // (GLSL compute only) gmem loads look their address up in GMap (uniform, binding
                            // GMAP_BUFFER_INDEX) and read GFast (storage, GFAST_BUFFER_INDEX) - a device-local copy of the
                            // dispatch's storage buffers - when found, stores too (copied back after the dispatch).
                            // runtime/nvn/vulkan/exec.c
    bool uses_gmem_store;   // writes global memory (STG)
    bool uses_tint;        // fragment: reads the per-draw tint (buffer 19, float4[4]) applied to color target 0
    uint16_t ssbo_read;     // storage buffer slots whose binding (address) the shader reads from the driver constants
                            // (c0 0x110 + 16 * i vertex, 0x510 + 16 * i fragment): the slots it actually uses
    bool uses_rscale;       // reads the resolution-scale table (fragment/compute buffer 18): float4 [0] = render
                            // target scale, [1 + i] = allocation scale of texture i
    uint32_t local_mem;     // bytes of local memory
    uint32_t num_insts;
    char error[256];        // set when translation fails
} ShaderInfo;

// Global memory (LDG/STG) is addressed through one buffer that wraps the guest heap.
#define GMEM_BUFFER_INDEX 17
// (MSL) Metal's sampler slots a stage. A shader with more textures takes its samplers as an array of all the slots
// and the slot of each texture from a table of uints at SMAP_BUFFER_INDEX (fragment, compute; [i] = texture i's slot).
#define MSL_MAX_SAMPLERS 16
#define SMAP_BUFFER_INDEX 22
// (GLSL compute, Vulkan) the fast copy of a dispatch's storage buffers: its address table and its data
#define GMAP_BUFFER_INDEX 20
#define GFAST_BUFFER_INDEX 21
// (vertex, Vulkan) transform feedback output: a device-local mirror of the buffers, or the guest heap (exec.c)
#define XFB_BUFFER_INDEX 22
#define GMAP_MAX 16  // ranges in the table (GMap { uvec4 gmap[GMAP_MAX + 1]; }: [i] = addr lo, addr hi, size, word offset;
                     // [GMAP_MAX].x = count)
extern uint64_t maxwell_gmem_base, maxwell_gmem_size;
// (GLSL / Vulkan) The heap reaches shaders in chunks of 1 << maxwell_gmem_chunk_shift bytes in two cases:
// - windows of one import where a storage buffer can't span the heap (issue #8) and lazy chunks aren't possible (or
//   LARECOMP_VK_GMEM_LAZY=0): one binding per window (GMEM_CHUNK_BINDING + k in set GMEM_CHUNK_SET) and plain
//   branches, each word picking its own window (a wide access over a window's end reads both);
// - chunks imported as the game binds them (AMD APUs, Intel: vulkan/exec.c gmem_lazy; maxwell_gmem_lazy): the shader takes
//   the chunk's device address and size from a table (GChunks, binding 0) and goes through a buffer reference after
//   one bounds check per LDG / STG (an aligned 8 or 16 byte access never crosses a chunk). A chunk not imported has
//   size 0: the access reads 0 / drops the store and counts in GMiss (binding 1), with the lowest word index hit.
//   (A per-access switch over constant descriptors, tried first, took the NVIDIA driver ~40 s to compile for the
//   chain chomps' compute shader with 87 access sites, and Intel ran it as a loop over the wave.)
// 0: one buffer.
extern uint32_t maxwell_gmem_chunk_shift;
extern bool maxwell_gmem_lazy;
// The straight-line form of forward-only branches (glsl_flatten) unless LARECOMP_SHADER_FLAT says otherwise: the
// runtime sets it before the first translation (Vulkan: off on AMD GPUs, exec.c vk_exec_init).
extern bool maxwell_flat_default;
extern bool maxwell_quad_ops;
// Lazy mode, fragment: a shader without discard / depth output / stores declares early fragment tests (its GMiss
// atomics would turn them off); one with discard / depth output (no stores) counts no misses - the
// runtime imports the bound storage buffers' chunks before the draw. true (LARECOMP_VK_GMEM_FS_MISS=1): every shader
// counts, none declares early tests.
extern bool maxwell_gmem_fs_miss;
// Lazy mode, vertex / fragment: the chunks under the draw's storage buffers (where the heap reads are) come as push
// constants, GDirect { uvec4 gd[GMEM_DIRECT_MAX]; } (x = first word, y = words, 0: unused, z, w = device address), and
// as storage buffers GDir<k> at GMEM_DIRECT_BINDING + k of the chunk set: an access in one is a compare and a plain
// storage buffer load (not a buffer reference: on Intel those miss the bindless surface path and the translator's
// dispatch loop can't hoist them), no table read and no miss path (exec.c gmem_direct_pick). The table after them.
#define GMEM_DIRECT_MAX 4
#define GMEM_DIRECT_BINDING 2  // lazy: GDir<k> at 2 + k (k < GMEM_DIRECT_MAX; unused: a small zero buffer)
#define GMEM_CHUNKS_MAX 256  // chunks at most (HEAP_MAX in 16 MB chunks is 208)
#define GMEM_CHUNK_SET(stage) ((stage) == STAGE_COMPUTE ? 1 : 2)
#define GMEM_CHUNK_BINDING 0  // windows: GMem<k> at binding k; lazy: the GChunks table
#define GMEM_MISS_BINDING 1   // lazy: GMiss { uint n[GMEM_CHUNKS_MAX]; uint w[GMEM_CHUNKS_MAX]; } (accesses, lowest word hit)
static inline int maxwell_gmem_chunks(void) {
    return maxwell_gmem_chunk_shift ? (int)((maxwell_gmem_size + (1ull << maxwell_gmem_chunk_shift) - 1) >> maxwell_gmem_chunk_shift) : 1;
}
// Debug: when translating the shader whose code starts at maxwell_dbg_code, output register
// maxwell_dbg_reg as captured after the instruction at maxwell_dbg_pc (greyscale, all targets).
extern const uint8_t *maxwell_dbg_code;
extern uint32_t maxwell_dbg_pc;
extern int maxwell_dbg_reg;
// Debug: the vertex shader at maxwell_dbg_vs writes each vertex's position to buffer 18 (slot iid*8+vid).
extern const uint8_t *maxwell_dbg_vs;
#define DBG_VS_BUFFER_INDEX 29
// translations kept on disk (cache/maxwell, translate.c): set by the runtime; off for the tools and tests
extern bool maxwell_disk_cache;

// code: shader binary (with the 0x50-byte SPH header for graphics stages)
ShaderInfo *maxwell_translate(const uint8_t *code, size_t max_size, int stage, const ShaderKey *key);
// A fragment stage that writes nothing: programs with only a vertex shader (depth-only passes: shadow maps, prepass)
ShaderInfo *maxwell_empty_fragment(void);
void maxwell_free(ShaderInfo *s);
// Disassemble for debugging (writes text to f).
void maxwell_disassemble(const uint8_t *code, size_t max_size, int stage, void *file);
