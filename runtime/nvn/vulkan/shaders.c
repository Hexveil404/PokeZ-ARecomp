// GLSL -> SPIR-V (shaderc), descriptor layouts from the SPIR-V itself, translated NVN programs, built-in shaders.
#include <pthread.h>
#include <shaderc/shaderc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "shaders.h"
#include "../screen_filter.h"

void vk_shaders_init(void) {  // (compilers are per thread: check shaderc works at all)
    shaderc_compiler_t c = shaderc_compiler_initialize();
    if (!c) FATAL("shaderc_compiler_initialize failed\n");
    shaderc_compiler_release(c);
}

static shaderc_shader_kind kind_of(int stage) {
    return stage == STAGE_VERTEX ? shaderc_vertex_shader : stage == STAGE_FRAGMENT ? shaderc_fragment_shader : shaderc_compute_shader;
}

// SPIR-V disk cache: cache/spirv/<hash of stage + source>.spv, so a known shader costs a file read instead of a
// GLSL compile (the driver's pipeline cache only covers the step after this one).
static uint64_t src_hash(const char *src, int stage) {
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;  // + version of the post-processing (no_contraction)
    if (g_vkc.api < VK_API_VERSION_1_3) h ^= (uint64_t)g_vkc.api << 32;  // (SPIR-V 1.5 for Vulkan 1.2: other files)
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static uint32_t *cache_load(uint64_t h, size_t *nwords) {
    char path[64];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *d = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d && d[0] != 0x07230203) { free(d); d = NULL; }
    if (d) *nwords = (size_t)n / 4;
    return d;
}

static void cache_store(uint64_t h, const uint32_t *spv, size_t nwords) {
    static int made;
    if (!made) { made = 1; mkdir("cache", 0755); mkdir("cache/spirv", 0755); }
    char path[64], tmp[80];
    snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)h);
    snprintf(tmp, sizeof tmp, "%s.%llx", path, (unsigned long long)(uintptr_t)&made ^ (unsigned long long)(uintptr_t)spv);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(spv, 4, nwords, f) == nwords;
    fclose(f);
    if (!ok || rename(tmp, path) != 0) remove(tmp);  // (another thread may have stored it first)
}

// Vertex shaders: every float add/sub/mul/div/fma gets NoContraction, so no compiler fuses or splits them. Depth
// pre-pass and colour pass are different shaders computing the same position, and the colour pass tests EQUAL:
// any difference in fusing leaves holes (foliage flickered black). GLSL's `precise gl_Position` only covers what
// glslang traces back from the position, which the translated register code defeats.
static bool float_arith(const uint32_t *w, uint32_t wc, uint32_t op) {
    switch (op) {
    case 127: case 129: case 131: case 133: case 136:  // FNegate FAdd FSub FMul FDiv
    case 142: case 143: case 144: case 145: case 146: case 148:  // vector/matrix products, Dot
        return true;
    case 12: return wc >= 5 && w[4] == 50;  // ExtInst GLSL.std.450 Fma
    default: return false;
    }
}

static uint32_t *no_contraction(uint32_t *spv, size_t *nwords) {
    enum { OP_DECORATE = 71, DEC_NO_CONTRACTION = 42 };
    size_t n = *nwords, insert = 0, count = 0;
    uint32_t bound = spv[3];
    uint8_t *done = calloc(bound, 1);  // already decorated (glslang does it for what it traces from precise)
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (!insert && op >= 19 && op <= 39) insert = i;  // first type declaration: end of the annotations
        if (op == OP_DECORATE && wc >= 3 && spv[i + 2] == DEC_NO_CONTRACTION && spv[i + 1] < bound) done[spv[i + 1]] = 1;
        i += wc;
    }
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) count++;
        i += wc;
    }
    if (!insert || !count) { free(done); return spv; }
    uint32_t *out = malloc((n + count * 3) * 4);
    memcpy(out, spv, insert * 4);
    size_t o = insert;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, op = spv[i] & 0xFFFF;
        if (!wc) break;
        if (float_arith(spv + i, wc, op) && spv[i + 2] < bound && !done[spv[i + 2]]) {
            done[spv[i + 2]] = 1;
            out[o++] = 3u << 16 | OP_DECORATE;
            out[o++] = spv[i + 2];  // result id
            out[o++] = DEC_NO_CONTRACTION;
        }
        i += wc;
    }
    memcpy(out + o, spv + insert, (n - insert) * 4);
    *nwords = o + (n - insert);
    free(done);
    free(spv);
    return out;
}

// SPIR-V words of src, or NULL (error logged with the source). *nwords set on success. Thread-safe: one shaderc
// compiler per thread (a shared one behind a lock serialised all the pipeline workers).
// No spirv-opt by default: it rewrote the depth pre-pass and colour-pass vertex shaders differently enough that
// their depths stopped matching (EQUAL test: black holes in foliage), and it was most of the compile time; the
// driver optimises anyway. LARECOMP_VK_OPT=1 turns it back on (experiments).
uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords) {
    uint64_t h = src_hash(src, stage);
    static int nocache = -1;
    if (nocache < 0) nocache = ENV("LARECOMP_VK_NO_SPIRV_CACHE") != NULL;
    uint32_t *out = nocache ? NULL : cache_load(h, nwords);
    if (out) return out;
    static __thread shaderc_compiler_t comp;
    if (!comp) comp = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan,  // (SPIR-V 1.6 / 1.5: compat.c)
                                           g_vkc.api >= VK_API_VERSION_1_3 ? shaderc_env_version_vulkan_1_3 : shaderc_env_version_vulkan_1_2);
    shaderc_compile_options_set_optimization_level(o, ENV("LARECOMP_VK_OPT") ? shaderc_optimization_level_performance
                                                                                  : shaderc_optimization_level_zero);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp, src, strlen(src), kind_of(stage), name, "main", o);
    shaderc_compile_options_release(o);
    if (shaderc_result_get_compilation_status(r) != shaderc_compilation_status_success) {
        static int nlog;
        if (nlog++ < 20) LOG("[shader] %s: GLSL compile failed:\n%s\n%s\n", name, shaderc_result_get_error_message(r), src);
    } else {
        size_t n = shaderc_result_get_length(r);
        out = malloc(n);
        memcpy(out, shaderc_result_get_bytes(r), n);
        *nwords = n / 4;
        if (stage == STAGE_VERTEX) out = no_contraction(out, nwords);
        if (!nocache) cache_store(h, out, *nwords);
    }
    shaderc_result_release(r);
    return out;
}

VkShaderModule vk_module(const uint32_t *spv, size_t nwords) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nwords * 4, .pCode = spv};
    VkShaderModule m;
    return vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

VkShaderModule vk_compile_glsl(const char *src, int stage, const char *name) {
    size_t n;
    uint32_t *spv = vk_glsl_to_spirv(src, stage, name, &n);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    return m;
}

// ================================================================== reflection
// Just enough SPIR-V parsing to list the descriptor bindings a module declares (set, binding, type).
enum { OP_TYPE_IMAGE = 25, OP_TYPE_SAMPLER = 26, OP_TYPE_SAMPLED_IMAGE = 27, OP_TYPE_ARRAY = 28, OP_TYPE_RUNTIME_ARRAY = 29,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_VARIABLE = 59, OP_DECORATE = 71 };
enum { DEC_BLOCK = 2, DEC_BUFFER_BLOCK = 3, DEC_BINDING = 33, DEC_SET = 34 };
enum { SC_UNIFORM_CONSTANT = 0, SC_UNIFORM = 2, SC_STORAGE_BUFFER = 12 };

int vk_reflect(const uint32_t *spv, size_t n, VkBindingInfo *out, int max) {
    if (n < 5 || spv[0] != 0x07230203) return 0;
    uint32_t bound = spv[3];
    int32_t *binding = malloc(bound * sizeof *binding), *set = malloc(bound * sizeof *set);
    uint8_t *flags = calloc(bound, 1);    // 1 Block, 2 BufferBlock
    uint32_t *op = calloc(bound, sizeof *op), *ref = calloc(bound, sizeof *ref), *sc = calloc(bound, sizeof *sc);
    for (uint32_t i = 0; i < bound; i++) binding[i] = set[i] = -1;
    for (size_t i = 5; i < n;) {
        uint32_t wc = spv[i] >> 16, o = spv[i] & 0xFFFF;
        if (!wc) break;
        const uint32_t *w = spv + i;
        if (o == OP_DECORATE && wc >= 3 && w[1] < bound) {
            if (w[2] == DEC_BINDING && wc >= 4) binding[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_SET && wc >= 4) set[w[1]] = (int32_t)w[3];
            else if (w[2] == DEC_BLOCK) flags[w[1]] |= 1;
            else if (w[2] == DEC_BUFFER_BLOCK) flags[w[1]] |= 2;
        } else if ((o == OP_TYPE_IMAGE || o == OP_TYPE_SAMPLER || o == OP_TYPE_SAMPLED_IMAGE || o == OP_TYPE_STRUCT) && wc >= 2 && w[1] < bound) {
            op[w[1]] = o;
        } else if ((o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) && wc >= 3 && w[1] < bound) {
            op[w[1]] = o;
            ref[w[1]] = w[2];
        } else if (o == OP_TYPE_POINTER && wc >= 4 && w[1] < bound) {
            op[w[1]] = o;
            sc[w[1]] = w[2];
            ref[w[1]] = w[3];
        } else if (o == OP_VARIABLE && wc >= 4 && w[2] < bound && w[1] < bound) {
            op[w[2]] = o;
            ref[w[2]] = w[1];  // pointer type
            sc[w[2]] = w[3];
        }
        i += wc;
    }
    int cnt = 0;
    for (uint32_t id = 0; id < bound && cnt < max; id++) {
        if (op[id] != OP_VARIABLE || binding[id] < 0) continue;
        uint32_t t = ref[ref[id]];  // pointee
        while (t < bound && (op[t] == OP_TYPE_ARRAY || op[t] == OP_TYPE_RUNTIME_ARRAY)) t = ref[t];
        VkDescriptorType dt;
        if (t >= bound) continue;
        if (op[t] == OP_TYPE_SAMPLED_IMAGE) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (op[t] == OP_TYPE_IMAGE) dt = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (op[t] == OP_TYPE_SAMPLER) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (op[t] == OP_TYPE_STRUCT && (sc[id] == SC_STORAGE_BUFFER || (flags[t] & 2))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (op[t] == OP_TYPE_STRUCT && sc[id] == SC_UNIFORM) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else continue;
        out[cnt++] = (VkBindingInfo){(uint32_t)(set[id] < 0 ? 0 : set[id]), (uint32_t)binding[id], dt};
    }
    free(binding); free(set); free(flags); free(op); free(ref); free(sc);
    return cnt;
}

VkDescriptorSetLayout vk_set_layout(const VkBindingInfo *b, int n, uint32_t set, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding lb[96];
    uint32_t k = 0;
    for (int i = 0; i < n && k < 96; i++)
        if (b[i].set == set) lb[k++] = (VkDescriptorSetLayoutBinding){b[i].binding, b[i].type, 1, stages, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = k, .pBindings = lb};
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== built-in shaders
static const char *builtin_src(const char *name, int *stage) {
    if (!strcmp(name, "present.vert")) {
        *stage = STAGE_VERTEX;
        // pv[0].x: flip (lower-left origin), pv[1]: uv crop (origin, size). Fullscreen triangle; Vulkan NDC y points down.
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 a; vec4 crop; } pc;\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  vec2 u = vec2(p.x, pc.a.x > 0.5 ? 1.0 - p.y : p.y);\n"
               "  uv = pc.crop.xy + u * pc.crop.zw;\n"
               "}\n";
    }
    if (!strcmp(name, "present.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform F { layout(offset = 32) vec4 pf; };\n"  // (the screen filter)
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec3 c = texture(t, uv).rgb;\n"
               SCREEN_FILTER_SRC("vec3")
               "  o = vec4(c, 1.0);\n"
               "}\n";
    }
    // ---- decoded video, NV12 -> RGBA (exec.c vk_convert_nv12): limited range BT.601, as swscale did (the movies
    // declare no matrix). Fullscreen triangle; Vulkan NDC y points down, so uv = p.
    if (!strcmp(name, "nv12.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(location = 0) out vec2 uv;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "  uv = p;\n"
               "}\n";
    }
    if (!strcmp(name, "nv12.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D ty;\n"
               "layout(set = 0, binding = 1) uniform sampler2D tuv;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  float y = (texture(ty, uv).r - 16.0 / 255.0) * (255.0 / 219.0);\n"
               "  vec2 c = (texture(tuv, uv).rg - 128.0 / 255.0) * (255.0 / 224.0);\n"
               "  o = vec4(clamp(vec3(y + 1.402 * c.y, y - 0.344136 * c.x - 0.714136 * c.y, y + 1.772 * c.x), 0.0, 1.0), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "texcopy.frag")) {  // (texture dumps: alpha kept - foliage, fences, decals are cut out by it)
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(location = 0) in vec2 uv;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() { o = texture(t, uv); }\n";
    }
    // ---- post passes before the present (Video tab "Anti-aliasing" / "Upscaler"): exec.c vk_present_draw.
    // Each draws a fullscreen triangle into an image the size of its output; push constants: two vec4.
    if (!strcmp(name, "post.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
               "}\n";
    }
    // SMAA 1x (third_party/smaa, runtime/nvn/smaa_data.c): the reference SMAA.hlsl in its GLSL mode, High preset,
    // with an entry point per pass. a = SMAA_RT_METRICS (1 / size, size). The vertex-stage offset helpers run per
    // pixel here. Luma edges (t0 = colour) -> blending weights (t0 = edges, t1 = area, t2 = search) -> neighbourhood
    // blending (t0 = colour, t1 = weights). Vulkan's texture origin is top-left, as SMAA expects (no flips).
    if (!strncmp(name, "smaa_", 5)) {
        *stage = STAGE_FRAGMENT;
        static char *src[3];
        int k = !strcmp(name, "smaa_edge.frag") ? 0 : !strcmp(name, "smaa_weight.frag") ? 1 : !strcmp(name, "smaa_blend.frag") ? 2 : -1;
        if (k < 0) return NULL;
        if (!src[k]) {
            extern const char smaa_hlsl_src[];
            static const char *head =
                "#version 450\n"
                "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
                "#define SMAA_RT_METRICS pc.a\n"
                "#define SMAA_GLSL_4\n"
                "#define SMAA_PRESET_HIGH\n";
            static const char *mains[3] = {
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off[3];\n"
                "  SMAAEdgeDetectionVS(uv, off);\n"
                "  o = vec4(SMAALumaEdgeDetectionPS(uv, off, t0), 0.0, 0.0);\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(set = 0, binding = 2) uniform sampler2D t2;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy, pix;\n"
                "  vec4 off[3];\n"
                "  SMAABlendingWeightCalculationVS(uv, pix, off);\n"
                "  o = SMAABlendingWeightCalculationPS(uv, pix, off, t0, t1, t2, vec4(0.0));\n"
                "}\n",
                "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
                "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
                "layout(location = 0) out vec4 o;\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy * SMAA_RT_METRICS.xy;\n"
                "  vec4 off;\n"
                "  SMAANeighborhoodBlendingVS(uv, off);\n"
                "  o = vec4(SMAANeighborhoodBlendingPS(uv, off, t0, t1).rgb, 1.0);\n"
                "}\n"};
            size_t n = strlen(head) + strlen(smaa_hlsl_src) + strlen(mains[k]) + 1;
            src[k] = malloc(n);
            snprintf(src[k], n, "%s%s%s", head, smaa_hlsl_src, mains[k]);
        }
        return src[k];
    }
    // AMD FidelityFX Super Resolution 1 (MIT), EASU: edge-adaptive upscale. a = (in / out scale xy, offset zw),
    // b.xy = input size. 12 taps around the source pixel, direction / edge length from luma, Lanczos-2-like
    // weights stretched along the edge, clamped to the nearest 4 (no ringing).
    if (!strcmp(name, "easu.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void setf(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n"
               "  float dx = lD - lB, dy = lE - lA;\n"
               "  float lx = clamp(abs(dx) / max(max(abs(lD - lC), abs(lC - lB)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  float ly = clamp(abs(dy) / max(max(abs(lE - lC), abs(lC - lA)), 1.0 / 65536.0), 0.0, 1.0);\n"
               "  dir += vec2(dx, dy) * w;\n"
               "  len += (lx * lx + ly * ly) * w;\n"
               "}\n"
               "void tapf(inout vec3 ac, inout float aw, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {\n"
               "  vec2 v = vec2(dot(off, dir), dot(off, vec2(-dir.y, dir.x))) * len2;\n"
               "  float d2 = min(dot(v, v), clp);\n"
               "  float wb = 0.4 * d2 - 1.0, wa = lob * d2 - 1.0;\n"
               "  wb *= wb; wa *= wa;\n"
               "  float w = (1.5625 * wb - 0.5625) * wa;\n"
               "  ac += c * w; aw += w;\n"
               "}\n"
               "void main() {\n"
               "  vec2 pp = floor(gl_FragCoord.xy) * pc.a.xy + pc.a.zw;\n"
               "  vec2 fpf = floor(pp); pp -= fpf;\n"
               "  ivec2 fp = ivec2(fpf);\n"
               "  //    b c\n"
               "  //  e f g h\n"
               "  //  i j k l\n"
               "  //    n o\n"
               "  vec3 bC = tap(fp + ivec2(0, -1)), cC = tap(fp + ivec2(1, -1));\n"
               "  vec3 eC = tap(fp + ivec2(-1, 0)), fC = tap(fp), gC = tap(fp + ivec2(1, 0)), hC = tap(fp + ivec2(2, 0));\n"
               "  vec3 iC = tap(fp + ivec2(-1, 1)), jC = tap(fp + ivec2(0, 1)), kC = tap(fp + ivec2(1, 1)), lC = tap(fp + ivec2(2, 1));\n"
               "  vec3 nC = tap(fp + ivec2(0, 2)), oC = tap(fp + ivec2(1, 2));\n"
               "  float bL = lum(bC), cL = lum(cC), eL = lum(eC), fL = lum(fC), gL = lum(gC), hL = lum(hC);\n"
               "  float iL = lum(iC), jL = lum(jC), kL = lum(kC), lL = lum(lC), nL = lum(nC), oL = lum(oC);\n"
               "  vec2 dir = vec2(0.0);\n"
               "  float len = 0.0;\n"
               "  setf(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n"
               "  setf(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n"
               "  setf(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n"
               "  setf(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n"
               "  float dr = dot(dir, dir);\n"
               "  bool zro = dr < 1.0 / 32768.0;\n"
               "  dir = zro ? vec2(1.0, 0.0) : dir * inversesqrt(dr);\n"
               "  len = len * 0.5; len *= len;\n"
               "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n"
               "  vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n"
               "  float lob = 0.5 - 0.29 * len, clp = 1.0 / lob;\n"
               "  vec3 mn = min(min(fC, gC), min(jC, kC)), mx = max(max(fC, gC), max(jC, kC));\n"
               "  vec3 ac = vec3(0.0);\n"
               "  float aw = 0.0;\n"
               "  tapf(ac, aw, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, bC);\n"
               "  tapf(ac, aw, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, cC);\n"
               "  tapf(ac, aw, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);\n"
               "  tapf(ac, aw, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, jC);\n"
               "  tapf(ac, aw, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, fC);\n"
               "  tapf(ac, aw, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);\n"
               "  tapf(ac, aw, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, kC);\n"
               "  tapf(ac, aw, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, lC);\n"
               "  tapf(ac, aw, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, hC);\n"
               "  tapf(ac, aw, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, gC);\n"
               "  tapf(ac, aw, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, oC);\n"
               "  tapf(ac, aw, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, nC);\n"
               "  o = vec4(min(mx, max(mn, ac / aw)), 1.0);\n"
               "}\n";
    }
    // FSR 1 RCAS: contrast-adaptive sharpening of the EASU output (same size). a.x = sharpness (exp2(-stops)),
    // b.xy = image size. The lobe is limited so the result stays inside the neighbours' range; noise is damped.
    // Box downscale (exec.c post_process): the image is larger than its place in the window (internal resolution
    // above the window's). a = source texels per output pixel (x, y), source size. Every source texel under the
    // output pixel counts, weighted by how much of it is covered (non-integer scales); at most 9 x 9 of them.
    if (!strcmp(name, "boxdown.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "void main() {\n"
               "  vec2 lo = floor(gl_FragCoord.xy) * pc.a.xy, hi = lo + pc.a.xy;\n"
               "  ivec2 i0 = ivec2(floor(lo)), i1 = min(ivec2(ceil(hi)) - 1, i0 + 8), sz = ivec2(pc.a.zw) - 1;\n"
               "  vec3 s = vec3(0.0);\n"
               "  float n = 0.0;\n"
               "  for (int y = i0.y; y <= i1.y; y++) {\n"
               "    float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));\n"
               "    for (int x = i0.x; x <= i1.x; x++) {\n"
               "      float w = wy * (min(hi.x, float(x + 1)) - max(lo.x, float(x)));\n"
               "      s += texelFetch(t, clamp(ivec2(x, y), ivec2(0), sz), 0).rgb * w;\n"
               "      n += w;\n"
               "    }\n"
               "  }\n"
               "  o = vec4(s / max(n, 1e-6), 1.0);\n"
               "}\n";
    }
    if (!strcmp(name, "rcas.frag")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\n"
               "layout(set = 0, binding = 0) uniform sampler2D t;\n"
               "layout(push_constant) uniform P { vec4 a; vec4 b; } pc;\n"
               "layout(location = 0) out vec4 o;\n"
               "vec3 tap(ivec2 p) { return texelFetch(t, clamp(p, ivec2(0), ivec2(pc.b.xy) - 1), 0).rgb; }\n"
               "float lum(vec3 c) { return c.g + 0.5 * (c.r + c.b); }\n"
               "void main() {\n"
               "  ivec2 ip = ivec2(gl_FragCoord.xy);\n"
               "  //    b\n"
               "  //  d e f\n"
               "  //    h\n"
               "  vec3 b = tap(ip + ivec2(0, -1)), d = tap(ip + ivec2(-1, 0)), e = tap(ip), f = tap(ip + ivec2(1, 0)), h = tap(ip + ivec2(0, 1));\n"
               "  float bL = lum(b), dL = lum(d), eL = lum(e), fL = lum(f), hL = lum(h);\n"
               "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n"
               "  float rng = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);\n"
               "  nz = 1.0 - 0.5 * clamp(abs(nz) / max(rng, 1.0 / 65536.0), 0.0, 1.0);\n"
               "  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));\n"
               "  vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));\n"
               "  vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));\n"
               "  vec3 lobeRGB = max(-hitMin, hitMax);\n"
               "  float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.a.x * nz;\n"
               "  o = vec4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 1.0);\n"
               "}\n";
    }
    // GTAO (exec.c gtao_*): ambient occlusion from Pokémon Z-A's scene depth, after XeGTAO (Intel, MIT licence). The
    // push constants (all three passes): m0..m3 the game's own reconstruction - world position (homogeneous) =
    // ndc.x * m0 + ndc.y * m1 + depth * m2 + m3 -, cam the camera's world position, prm (radius in metres, intensity,
    // ndc y sign, frame), sz (AO image size, depth image size), prj (tan of the half field of view x / y).
    static const char gtao_common[] =
        "#version 450\n"
        "layout(push_constant) uniform P { vec4 m0, m1, m2, m3, cam, prm, sz, prj; } pc;\n"
        "layout(set = 0, binding = 0) uniform sampler2D t0;\n"
        "layout(set = 0, binding = 1) uniform sampler2D t1;\n"
        "layout(location = 0) out vec4 o;\n"
        "const float PI = 3.14159265, HALF_PI = 1.57079633;\n"
        // depth at a uv of the full-resolution depth image (nearest texel: depth isn't filtered)
        "float depth_at(sampler2D d, vec2 uv) {\n"
        "  ivec2 s = ivec2(pc.sz.zw);\n"
        "  return texelFetch(d, clamp(ivec2(uv * pc.sz.zw), ivec2(0), s - 1), 0).r;\n"
        "}\n"
        "vec4 wpos4(vec2 uv, float d) {\n"
        "  vec2 n = vec2(uv.x * 2.0 - 1.0, (uv.y * 2.0 - 1.0) * pc.prm.z);\n"
        "  return n.x * pc.m0 + n.y * pc.m1 + d * pc.m2 + pc.m3;\n"
        "}\n"
        "vec3 wpos(vec2 uv, float d) { vec4 p = wpos4(uv, d); return p.xyz / p.w; }\n";
    if (!strcmp(name, "gtao.frag")) {  // t0 = the scene depth; out .r = visibility (1 = open)
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "vec3 pos_at(vec2 uv) { return wpos(uv, depth_at(t0, uv)); }\n"
                "float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }\n"
                "float facos(float x) { float r = -0.156583 * abs(x) + HALF_PI; r *= sqrt(1.0 - abs(x)); return x >= 0.0 ? r : PI - r; }\n"
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.xy;\n"
                "  float d = depth_at(t0, uv);\n"
                "  vec4 P4 = wpos4(uv, d);\n"
                "  if (abs(P4.w) < 1e-7) { o = vec4(1.0); return; }\n"
                "  vec3 P = P4.xyz / P4.w;\n"
                "  float dist = length(pc.cam.xyz - P);\n"
                "  if (!(dist > 0.05 && dist < 400.0)) { o = vec4(1.0); return; }\n"  // (sky, far: open)
                "  vec3 V = (pc.cam.xyz - P) / dist;\n"
                // the normal from the depth: on each axis the neighbour closer in depth (no smearing across edges)
                "  vec2 px = 1.0 / pc.sz.zw;\n"
                "  vec3 Pl = pos_at(uv - vec2(px.x * 2.0, 0.0)), Pr = pos_at(uv + vec2(px.x * 2.0, 0.0));\n"
                "  vec3 Pu = pos_at(uv - vec2(0.0, px.y * 2.0)), Pd = pos_at(uv + vec2(0.0, px.y * 2.0));\n"
                "  vec3 dx = length(Pr - P) < length(P - Pl) ? Pr - P : P - Pl;\n"
                "  vec3 dy = length(Pd - P) < length(P - Pu) ? Pd - P : P - Pu;\n"
                "  vec3 N = normalize(cross(dy, dx));\n"
                "  if (dot(N, V) < 0.0) N = -N;\n"
                "  if (pc.prj.z == 2.0) { o = vec4(N * 0.5 + 0.5, 1.0); return; }\n"  // (debug: the normals)
                "  if (pc.prj.z == 3.0) { o = vec4(vec3(fract(dist / 5.0)), 1.0); return; }\n"  // (debug: 5 m bands)
                "  float radius = pc.prm.x;\n"
                "  vec2 rUV = 0.5 * radius / (dist * pc.prj.xy);\n"  // (the radius in uv at that distance)
                "  if (rUV.x * pc.sz.x < 1.0) { o = vec4(1.0); return; }\n"
                "  float falloffRange = 0.615 * radius, falloffFrom = radius * (1.0 - 0.615);\n"
                "  float falloffMul = -1.0 / falloffRange, falloffAdd = falloffFrom / falloffRange + 1.0;\n"
                "  float n0 = ign(gl_FragCoord.xy + pc.prm.w * 5.588238), n1 = ign(gl_FragCoord.yx * 1.37 + 17.0 + pc.prm.w * 3.1);\n"
                "  int SLICES = int(pc.prj.w), STEPS = SLICES * 2;\n"  // (quality: 2 / 3 / 4 slices)
                "  float vis = 0.0;\n"
                "  for (int s = 0; s < SLICES; s++) {\n"
                "    float phi = (float(s) + n0) * PI / float(SLICES);\n"
                "    vec2 dir = vec2(cos(phi), sin(phi));\n"
                // the slice's plane holds the view ray and the screen direction: its 3D direction from a point at the
                // same depth along that screen direction
                "    vec3 D3 = wpos(uv + dir * rUV * 0.25, d) - P;\n"
                "    vec3 orthoDir = D3 - V * dot(D3, V);\n"
                "    if (dot(orthoDir, orthoDir) < 1e-12) continue;\n"
                "    orthoDir = normalize(orthoDir);\n"
                "    vec3 axis = normalize(cross(orthoDir, V));\n"
                "    vec3 projN = N - axis * dot(N, axis);\n"
                "    float projNLen = length(projN);\n"
                "    float signN = sign(dot(orthoDir, projN));\n"
                "    float cosN = clamp(dot(projN, V) / max(projNLen, 1e-6), 0.0, 1.0);\n"
                "    float n = signN * facos(cosN);\n"
                "    float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);\n"
                "    float hc0 = low0, hc1 = low1;\n"
                "    for (int j = 0; j < STEPS; j++) {\n"
                "      float t = (float(j) + fract(n1 + float(j) * 0.618034)) / float(STEPS);\n"
                "      t = t * t;\n"
                "      vec2 off = dir * (rUV * t + 1.0 / pc.sz.xy);\n"
                "      vec3 d0 = pos_at(uv + off) - P, d1 = pos_at(uv - off) - P;\n"
                // (XeGTAO's thin occluder compensation: a sample far in front of the point - a grass blade, a railing -
                // counts as farther away along the view axis, so it occludes less: thin things aren't solid walls)
                "      float l0 = length(d0 + V * dot(d0, V) * 2.0), l1 = length(d1 + V * dot(d1, V) * 2.0);\n"
                "      float sh0 = dot(normalize(d0 + 1e-9), V), sh1 = dot(normalize(d1 + 1e-9), V);\n"  // (the true direction)
                "      float w0 = clamp(l0 * falloffMul + falloffAdd, 0.0, 1.0), w1 = clamp(l1 * falloffMul + falloffAdd, 0.0, 1.0);\n"
                "      hc0 = max(hc0, mix(low0, sh0 - 0.1, w0));\n"
                "      hc1 = max(hc1, mix(low1, sh1 - 0.1, w1));\n"
                "    }\n"
                "    float h0 = -facos(hc1), h1 = facos(hc0);\n"
                "    h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);\n"
                "    h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);\n"
                "    float sinN = sin(n);\n"
                "    float a0 = (cosN + 2.0 * h0 * sinN - cos(2.0 * h0 - n)) * 0.25;\n"
                "    float a1 = (cosN + 2.0 * h1 * sinN - cos(2.0 * h1 - n)) * 0.25;\n"
                "    vis += projNLen * (a0 + a1);\n"
                "  }\n"
                "  vis = clamp(vis / float(SLICES), 0.0, 1.0);\n"
                "  vis = pow(vis, 1.6);\n"  // (XeGTAO's final power, softened: the game's lighting is soft already)
                "  o = vec4(vis, 0.0, 0.0, 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    if (!strcmp(name, "gtao_blur.frag")) {  // t0 = raw AO, t1 = the scene depth: a 5x5 depth-aware average
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.xy;\n"
                "  vec4 c4 = wpos4(uv, depth_at(t1, uv));\n"
                "  if (abs(c4.w) < 1e-7) { o = vec4(1.0); return; }\n"
                "  float cd = length(c4.xyz / c4.w - pc.cam.xyz);\n"
                "  float sum = 0.0, wsum = 0.0;\n"
                "  for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
                "    vec2 u = uv + vec2(x, y) / pc.sz.xy;\n"
                "    vec4 p4 = wpos4(u, depth_at(t1, u));\n"
                "    float sd = abs(p4.w) < 1e-7 ? 1e9 : length(p4.xyz / p4.w - pc.cam.xyz);\n"
                "    float w = exp(-abs(sd - cd) / (0.03 * cd + 0.01)) * (1.0 / (1.0 + float(x * x + y * y) * 0.25));\n"
                "    sum += texelFetch(t0, clamp(ivec2(gl_FragCoord.xy) + ivec2(x, y), ivec2(0), ivec2(pc.sz.xy) - 1), 0).r * w;\n"
                "    wsum += w;\n"
                "  }\n"
                "  o = vec4(sum / max(wsum, 1e-6), 0.0, 0.0, 1.0);\n"
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    if (!strcmp(name, "gtao_apply.frag")) {  // t0 = the filtered AO (half size, bilinear): drawn with a multiply blend
        *stage = STAGE_FRAGMENT;
        static char *src;
        if (!src) {
            static const char body[] =
                "void main() {\n"
                "  vec2 uv = gl_FragCoord.xy / pc.sz.zw;\n"  // (sz.zw: the lit image's size here)
                // depth-aware upsampling: of the 4 half-size AO texels around, those at this pixel's distance count
                // (a background texel's AO doesn't spill onto a character's arm in front of it)
                "  vec4 c4 = wpos4(uv, depth_at(t1, uv));\n"
                "  float cd = abs(c4.w) < 1e-7 ? 1e9 : length(c4.xyz / c4.w - pc.cam.xyz);\n"
                "  vec2 hp = uv * pc.sz.xy - 0.5, b = floor(hp), fr = hp - b;\n"
                "  float v = 0.0, ws = 0.0;\n"
                "  for (int k = 0; k < 4; k++) {\n"
                "    vec2 o2 = vec2(k & 1, k >> 1), tc = b + o2;\n"
                "    vec2 tuv = (tc + 0.5) / pc.sz.xy;\n"
                "    vec4 t4 = wpos4(tuv, depth_at(t1, tuv));\n"
                "    float td = abs(t4.w) < 1e-7 ? 1e9 : length(t4.xyz / t4.w - pc.cam.xyz);\n"
                "    float bw = (o2.x > 0.5 ? fr.x : 1.0 - fr.x) * (o2.y > 0.5 ? fr.y : 1.0 - fr.y) + 1e-3;\n"
                "    float w = bw * exp(-abs(td - cd) / (0.02 * cd + 0.02));\n"
                "    v += w * texelFetch(t0, clamp(ivec2(tc), ivec2(0), ivec2(pc.sz.xy) - 1), 0).r; ws += w;\n"
                "  }\n"
                "  v = ws > 1e-5 ? v / ws : texture(t0, uv).r;\n"
                "  if (pc.prj.z >= 2.0) { o = vec4(texture(t0, uv).rgb, 1.0); return; }\n"
                "  o = pc.prj.z != 0.0 ? vec4(vec3(v), 1.0) : vec4(vec3(max(mix(1.0, v, pc.prm.y), 0.0)), 1.0);\n"  // (strength above 1: a stronger darkening, never below black)
                "}\n";
            size_t n = strlen(gtao_common) + strlen(body) + 1;
            src = malloc(n);
            snprintf(src, n, "%s%s", gtao_common, body);
        }
        return src;
    }
    if (!strcmp(name, "clear.vert")) {
        *stage = STAGE_VERTEX;
        return "#version 450\n"
               "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
               "void main() {\n"
               "  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
               "  gl_Position = vec4(p * 2.0 - 1.0, pc.d.x, 1.0);\n"
               "}\n";
    }
    // clear.frag.<f|i|u><index>: writes the push-constant colour to colour target <index>
    if (!strncmp(name, "clear.frag.", 11)) {
        *stage = STAGE_FRAGMENT;
        static __thread char buf[512];
        char k = name[11];
        int idx = atoi(name + 12);
        const char *ty = k == 'i' ? "ivec4" : k == 'u' ? "uvec4" : "vec4";
        const char *cv = k == 'i' ? "floatBitsToInt(pc.c)" : k == 'u' ? "floatBitsToUint(pc.c)" : "pc.c";
        snprintf(buf, sizeof buf,
                 "#version 450\n"
                 "layout(push_constant) uniform P { vec4 c; vec4 d; } pc;\n"
                 "layout(location = %d) out %s o;\n"
                 "void main() { o = %s; }\n", idx, ty, cv);
        return buf;
    }
    if (!strcmp(name, "clear.frag.none")) {
        *stage = STAGE_FRAGMENT;
        return "#version 450\nvoid main() {}\n";
    }
    // (exec.c gfast_finish) a storing dispatch's device-local copy back to the guest heap: only the words the dispatch
    // changed (copy f vs its untouched twin o), so the CPU's writes to the rest of the buffer meanwhile survive
    if (!strcmp(name, "gfast_merge.comp")) {
        *stage = STAGE_COMPUTE;
        return "#version 450\n"
               "layout(local_size_x = 256) in;\n"
               "layout(std430, set = 0, binding = 0) buffer H { uint h[]; };\n"
               "layout(std430, set = 0, binding = 1) readonly buffer F { uint f[]; };\n"
               "layout(std430, set = 0, binding = 2) readonly buffer O { uint o[]; };\n"
               "layout(push_constant) uniform P { uint src; uint dst; uint n; } pc;\n"
               "void main() {\n"
               "  uint i = gl_GlobalInvocationID.x;\n"
               "  if (i >= pc.n) return;\n"
               "  uint v = f[pc.src + i];\n"
               "  if (v != o[pc.src + i]) h[pc.dst + i] = v;\n"
               "}\n";
    }
    return NULL;
}

VkShaderModule vk_builtin_shader(const char *name) {
    static struct { char name[32]; VkShaderModule m; } cache[64];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) return cache[i].m;
    int stage = 0;
    const char *src = builtin_src(name, &stage);
    if (!src) FATAL("no built-in shader %s\n", name);
    VkShaderModule m = vk_compile_glsl(src, stage, name);
    if (!m) FATAL("built-in shader %s failed to compile\n", name);
    if (n < 64) { snprintf(cache[n].name, sizeof cache[n].name, "%s", name); cache[n++].m = m; }
    return m;
}
